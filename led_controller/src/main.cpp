#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "board_pins.h"
#include "light_protocol.h"

// Receiving half of the qremote system. Listens for light_cmd_t packets
// over ESP-NOW (sent by the ../src dongle's OSC bridge, driven by QLab
// Network cues) and drives 4x RGBW 24V LED strips through a PCA9685
// (16 channels, 4 per strip) plus a solenoid prop effect.
//
// Strips are wired W,B,G,R on channels 4n+0..4n+3.
// This unit's screen/buzzer are dead, so status is USB serial (when
// tethered) plus a boot flash / no-signal heartbeat on the strips
// themselves (see stepHeartbeat()) for confidence with nothing plugged in.
//
// Serial commands (newline terminated) — bench/debug only, independent of
// ESP-NOW reception:
//   p              pause/resume the auto test sequence
//   t              toggle test-sequence mode (off by default; runs live)
//   c <ch> <val>   set channel 0-15 to 0-4095 directly
//   s <n> r g b w  set strip 0-3 to four 0-4095 values directly
//   a <val>        set all channels
//   x              all off (LEDs + solenoid)
//   v <0|1>        solenoid off/on (held until changed)
//   v p <ms>       pulse solenoid for <ms> milliseconds
//   i              re-scan I2C bus

// ---- config ----
#define PCA_ADDR_DEFAULT 0x40
#define PWM_FREQ_HZ      1000
#define INVERT_OUTPUT    0      // set 1 if strips are ON at value 0 (inverting MOSFET driver)
#define SIGNAL_TIMEOUT_MS 5000  // no /light command for this long -> "waiting for signal" heartbeat

// ---- PCA9685 registers ----
#define REG_MODE1    0x00
#define REG_MODE2    0x01
#define REG_LED0     0x06
#define REG_PRESCALE 0xFE

// channel offset within a group for each colour (physical wiring: W,B,G,R)
enum { CH_W = 0, CH_B = 1, CH_G = 2, CH_R = 3 };
// display/test order (R,G,B,W) independent of the physical wiring offsets above
static const uint8_t CH_ORDER[4] = { CH_R, CH_G, CH_B, CH_W };
static const char *CH_ORDER_NAME[4] = { "R", "G", "B", "W" };
static uint8_t pcaAddr = 0;
static bool paused = false;
static bool testMode = false;

// ---------------------------------------------------------------- PCA9685 --

static void solenoid(bool on) {
    digitalWrite(PIN_SOLENOID, on ? HIGH : LOW);
    Serial.printf("solenoid %s\n", on ? "ON" : "off");
}

static bool i2cWrite8(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(pcaAddr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static int i2cRead8(uint8_t reg) {
    Wire.beginTransmission(pcaAddr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return -1;
    if (Wire.requestFrom(pcaAddr, (uint8_t)1) != 1) return -1;
    return Wire.read();
}

// val 0..4095
static void setChannel(uint8_t ch, uint16_t val) {
    if (ch > 15) return;
    if (val > 4095) val = 4095;
#if INVERT_OUTPUT
    val = 4095 - val;
#endif
    uint16_t on = 0, off = val;
    if (val >= 4095) { on = 4096; off = 0; }      // full on bit
    else if (val == 0) { on = 0; off = 4096; }    // full off bit
    Wire.beginTransmission(pcaAddr);
    Wire.write(REG_LED0 + 4 * ch);
    Wire.write(on & 0xFF);
    Wire.write(on >> 8);
    Wire.write(off & 0xFF);
    Wire.write(off >> 8);
    Wire.endTransmission();
}

static void allOff() {
    for (uint8_t ch = 0; ch < 16; ch++) setChannel(ch, 0);
}

static void scanBus() {
    Serial.println("I2C scan:");
    uint8_t found = 0;
    for (uint8_t a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            Serial.printf("  0x%02X", a);
            if (a == I2C_ADDR_QMI8658) Serial.print("  (QMI8658 IMU)");
            else if (a == I2C_ADDR_PCF85063) Serial.print("  (PCF85063 RTC)");
            else if (a == 0x70) Serial.print("  (PCA9685 all-call)");
            else if (a >= 0x40 && a <= 0x7F) Serial.print("  (PCA9685?)");
            Serial.println();
            if (a >= 0x40 && a != 0x70 && a != I2C_ADDR_PCF85063 && a != I2C_ADDR_QMI8658 && !found) {
                pcaAddr = a;
                int m1 = i2cRead8(REG_MODE1), ps = i2cRead8(REG_PRESCALE);
                Serial.printf("      probe 0x%02X: MODE1=%d PRESCALE=%d\n", a, m1, ps);
                if (m1 >= 0 && ps >= 0) found = a;
                pcaAddr = 0;
            }
        }
    }
    if (found) {
        pcaAddr = found;
        Serial.printf("using PCA9685 at 0x%02X\n", pcaAddr);
    } else {
        pcaAddr = 0;
        Serial.println("!! no PCA9685 found (check wiring / pull-ups / address jumpers)");
    }
}

static bool pcaInit() {
    if (!pcaAddr) return false;
    if (!i2cWrite8(REG_MODE1, 0x00)) return false;   // clear any state
    delay(5);
    uint8_t prescale = (uint8_t)lround(25000000.0 / (4096.0 * PWM_FREQ_HZ)) - 1;
    if (prescale < 3) prescale = 3;
    i2cWrite8(REG_MODE1, 0x10);                      // sleep (required to change prescale)
    i2cWrite8(REG_PRESCALE, prescale);
    i2cWrite8(REG_MODE2, 0x04);                      // OUTDRV = totem pole, non-inverted
    i2cWrite8(REG_MODE1, 0x20);                      // wake, auto-increment
    delay(5);
    i2cWrite8(REG_MODE1, 0xA0);                      // restart + auto-increment
    int m1 = i2cRead8(REG_MODE1), ps = i2cRead8(REG_PRESCALE);
    Serial.printf("PCA9685 init: MODE1=0x%02X PRESCALE=%d (~%d Hz)\n", m1, ps, 25000000 / (4096 * (ps + 1)));
    allOff();
    return m1 >= 0;
}

// ------------------------------------------------------------- fade engine --
// Software-tracked crossfade per channel, stepped from loop(). Replaces the
// old fixed-step ramp() with one driven by whatever fade time a /light
// command specifies. s_chanVal is our source of truth for "current value"
// since the PCA9685 can't be read back cheaply.

struct ChanFade { uint16_t from, to; uint32_t startMs, durMs; bool active; };
static ChanFade s_fade[16];
static uint16_t s_chanVal[16];   // last value written to hardware (0-4095)
static uint32_t s_lastPacketMs = 0;
static uint16_t s_lastSeq = 0;
static bool s_gotFirstPacket = false;

static void startFade(uint8_t ch, uint8_t val8, uint16_t durMs) {
    if (ch > 15) return;
    uint16_t to12 = (uint16_t)((uint32_t)val8 * 4095 / 255);
    s_fade[ch] = { s_chanVal[ch], to12, millis(), durMs, true };
}

static void stepFades() {
    uint32_t now = millis();
    for (uint8_t ch = 0; ch < 16; ch++) {
        ChanFade &f = s_fade[ch];
        if (!f.active) continue;
        uint16_t val;
        if (f.durMs == 0 || now - f.startMs >= f.durMs) {
            val = f.to;
            f.active = false;
        } else {
            uint32_t elapsed = now - f.startMs;
            val = f.from + (int32_t)(f.to - f.from) * (int32_t)elapsed / (int32_t)f.durMs;
        }
        if (val != s_chanVal[ch]) {
            setChannel(ch, val);
            s_chanVal[ch] = val;
        }
    }
}

static void applyStrip(uint8_t strip, uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint16_t fadeMs) {
    if (strip > 3) return;
    startFade(strip * 4 + CH_R, r, fadeMs);
    startFade(strip * 4 + CH_G, g, fadeMs);
    startFade(strip * 4 + CH_B, b, fadeMs);
    startFade(strip * 4 + CH_W, w, fadeMs);
}

// ---------------------------------------------------------------- solenoid --
// Non-blocking pulse so it doesn't stall fade stepping.

static uint32_t s_solenoidOffAt = 0; // 0 = not pulsing

static void solenoidPulse(uint16_t ms) {
    if (ms == 0) return;
    solenoid(true);
    s_solenoidOffAt = millis() + ms;
}

static void stepSolenoid() {
    if (s_solenoidOffAt && (int32_t)(millis() - s_solenoidOffAt) >= 0) {
        solenoid(false);
        s_solenoidOffAt = 0;
    }
}

// --------------------------------------------------------------- heartbeat --
// No screen/buzzer to say "I'm alive" — use the strips themselves.

static void bootFlash() {
    // Quick white blip across strips 0..3: board came up, ESP-NOW is ready.
    for (uint8_t s = 0; s < 4; s++) {
        setChannel(s * 4 + CH_W, 4095);
        delay(80);
        setChannel(s * 4 + CH_W, 0);
    }
}

static void stepHeartbeat() {
    if (testMode) return;
    uint32_t sinceLast = s_gotFirstPacket ? millis() - s_lastPacketMs : millis();
    if (sinceLast < SIGNAL_TIMEOUT_MS) return;
    static uint32_t lastBlink = 0;
    static bool on = false;
    if (millis() - lastBlink < 900) return;
    lastBlink = millis();
    on = !on;
    // Faint blip on strip 0's W channel only — cosmetic "waiting for
    // signal" indicator, deliberately dim so it doesn't read as a cue.
    setChannel(0 * 4 + CH_W, on ? 200 : 0);
}

// ---------------------------------------------------------------- ESP-NOW --

static light_cmd_t s_pending;
static volatile bool s_havePending = false;
// The ESP-NOW recv callback and loop() can run on different cores, so
// noInterrupts()/interrupts() (which only mask the calling core) wouldn't
// protect s_pending — use a real cross-core spinlock instead.
static portMUX_TYPE s_pendingMux = portMUX_INITIALIZER_UNLOCKED;

static void handleLightCmd(const light_cmd_t &cmd) {
    s_gotFirstPacket = true;
    s_lastPacketMs = millis();
    if (cmd.seq != (uint16_t)(s_lastSeq + 1) && s_lastSeq != 0) {
        Serial.printf("[espnow] seq gap: expected %u got %u\n", (uint16_t)(s_lastSeq + 1), cmd.seq);
    }
    s_lastSeq = cmd.seq;

    switch (cmd.cmd) {
        case LIGHT_CMD_STRIP:
            applyStrip(cmd.strip, cmd.r, cmd.g, cmd.b, cmd.w, cmd.fade_ms);
            break;
        case LIGHT_CMD_ALL:
            for (uint8_t s = 0; s < 4; s++) applyStrip(s, cmd.r, cmd.g, cmd.b, cmd.w, cmd.fade_ms);
            break;
        case LIGHT_CMD_OFF:
            for (uint8_t s = 0; s < 4; s++) applyStrip(s, 0, 0, 0, 0, cmd.fade_ms);
            break;
        case LIGHT_CMD_SOLENOID:
            solenoidPulse(cmd.fade_ms);
            break;
        default:
            Serial.printf("[espnow] unknown cmd %u\n", cmd.cmd);
    }
}

// Keep the recv callback minimal (runs in the Wi-Fi task) — just latch the
// most recent packet; loop() picks it up. Last-write-wins is fine for a
// light show: a dropped/superseded intermediate frame doesn't matter.
#if defined(ESP_ARDUINO_VERSION) && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
static void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    (void)info;
    if (len != sizeof(light_cmd_t)) return;
    portENTER_CRITICAL(&s_pendingMux);
    memcpy((void *)&s_pending, data, sizeof(light_cmd_t));
    s_havePending = true;
    portEXIT_CRITICAL(&s_pendingMux);
}
#else
static void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
    (void)mac;
    if (len != sizeof(light_cmd_t)) return;
    portENTER_CRITICAL(&s_pendingMux);
    memcpy((void *)&s_pending, data, sizeof(light_cmd_t));
    s_havePending = true;
    portEXIT_CRITICAL(&s_pendingMux);
}
#endif

static void setupEspNow() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    if (esp_wifi_set_channel(LIGHT_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        Serial.println("!! esp_wifi_set_channel failed");
    }

    if (esp_now_init() != ESP_OK) {
        Serial.println("!! esp_now_init failed");
        return;
    }
    esp_now_register_recv_cb(onEspNowRecv);
    Serial.printf("ESP-NOW ready on channel %d, MAC %s\n",
                  LIGHT_ESPNOW_CHANNEL, WiFi.macAddress().c_str());
}

// -------------------------------------------------------- bench test mode --
// Original bring-up sequence, gated behind 't' so it doesn't fight live
// ESP-NOW control. Blocking by design (bench use only, USB tethered).

static void handleSerial();
static bool waitMs(uint32_t ms) {
    uint32_t t0 = millis();
    while (millis() - t0 < ms) {
        handleSerial();
        if (paused || !testMode) return false;
        delay(2);
    }
    return true;
}

static bool ramp(uint8_t ch, uint16_t from, uint16_t to, uint32_t ms) {
    const int steps = 40;
    for (int i = 0; i <= steps; i++) {
        setChannel(ch, from + (int32_t)(to - from) * i / steps);
        if (!waitMs(ms / steps)) return false;
    }
    return true;
}

static void autoSequence() {
    for (uint8_t s = 0; s < 4; s++) {
        for (uint8_t c = 0; c < 4; c++) {
            uint8_t ch = s * 4 + CH_ORDER[c];
            allOff();
            Serial.printf("[single] strip %d  %s  (ch %2d)  ", s, CH_ORDER_NAME[c], ch);
            Serial.flush();
            if (!ramp(ch, 0, 4095, 800)) return;
            Serial.print("FULL ");
            if (!waitMs(1200)) return;
            if (!ramp(ch, 4095, 0, 500)) return;
            Serial.println("ok");
        }
    }
    for (uint8_t s = 0; s < 4; s++) {
        for (uint8_t c = 0; c < 5; c++) {
            allOff();
            if (c < 4) {
                Serial.printf("[strip %d] %s\n", s, CH_ORDER_NAME[c]);
                setChannel(s * 4 + CH_ORDER[c], 4095);
            } else {
                Serial.printf("[strip %d] R+G+B+W\n", s);
                for (uint8_t k = 0; k < 4; k++) setChannel(s * 4 + k, 4095);
            }
            if (!waitMs(700)) return;
        }
    }
    Serial.println("[all] 16 channels full");
    for (uint8_t ch = 0; ch < 16; ch++) setChannel(ch, 4095);
    if (!waitMs(2000)) return;
    allOff();
    Serial.print("[solenoid] ");
    solenoid(true);
    bool cont = waitMs(300);
    solenoid(false);
    if (!cont) return;
    if (!waitMs(1000)) return;
    Serial.println("---- sequence complete, restarting in 2 s ----");
    waitMs(2000);
}

static void handleSerial() {
    static char buf[64];
    static uint8_t len = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\r') continue;
        if (c != '\n') { if (len < sizeof(buf) - 1) buf[len++] = c; continue; }
        buf[len] = 0; len = 0;
        char cmd = buf[0];
        int a = 0, b = 0, d = 0, e = 0, f = 0;
        switch (cmd) {
            case 'p':
                paused = !paused;
                Serial.println(paused ? "paused" : "resumed");
                break;
            case 't':
                testMode = !testMode;
                if (!testMode) allOff();
                Serial.printf("test mode %s\n", testMode ? "ON (live ESP-NOW commands ignored)" : "off (live)");
                break;
            case 'c':
                if (sscanf(buf + 1, "%d %d", &a, &b) == 2) {
                    testMode = true; paused = true; setChannel(a, b); s_chanVal[a] = b;
                    Serial.printf("ch %d = %d\n", a, b);
                }
                break;
            case 's':
                if (sscanf(buf + 1, "%d %d %d %d %d", &a, &b, &d, &e, &f) == 5 && a >= 0 && a < 4) {
                    testMode = true; paused = true;
                    setChannel(a * 4 + CH_R, b); setChannel(a * 4 + CH_G, d);
                    setChannel(a * 4 + CH_B, e); setChannel(a * 4 + CH_W, f);
                    Serial.printf("strip %d = %d %d %d %d\n", a, b, d, e, f);
                }
                break;
            case 'a':
                if (sscanf(buf + 1, "%d", &a) == 1) {
                    testMode = true; paused = true;
                    for (uint8_t ch = 0; ch < 16; ch++) setChannel(ch, a);
                    Serial.printf("all = %d\n", a);
                }
                break;
            case 'x':
                testMode = true; paused = true; allOff(); solenoid(false); Serial.println("all off");
                break;
            case 'v':
                testMode = true; paused = true;
                if (sscanf(buf + 1, " p %d", &a) == 1) {
                    solenoid(true); delay(constrain(a, 1, 5000)); solenoid(false);
                } else if (sscanf(buf + 1, "%d", &a) == 1) {
                    solenoid(a != 0);
                }
                break;
            case 'i':
                testMode = true; paused = true; scanBus(); pcaInit();
                break;
            case 0:
                break;
            default:
                Serial.println("cmds: p | t | c <ch> <val> | s <strip> r g b w | a <val> | x | v <0|1> | v p <ms> | i");
        }
    }
}

// --------------------------------------------------------------------------

void setup() {
    pinMode(PIN_SOLENOID, OUTPUT);       // first thing: make sure the solenoid is off
    digitalWrite(PIN_SOLENOID, LOW);
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 3000) delay(10);
    delay(200);
    Serial.println("\n=== qremote LED controller ===");
    Serial.printf("I2C SDA=%d SCL=%d, solenoid GPIO%d\n", PIN_I2C_SDA, PIN_I2C_SCL, PIN_SOLENOID);

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
    scanBus();
    if (!pcaInit()) {
        Serial.println("!! PCA9685 init failed; type 'i' to retry after fixing wiring");
    } else {
        bootFlash();
    }

    setupEspNow();
}

void loop() {
    handleSerial();

    if (s_havePending) {
        light_cmd_t cmd;
        portENTER_CRITICAL(&s_pendingMux);
        cmd = s_pending;
        s_havePending = false;
        portEXIT_CRITICAL(&s_pendingMux);
        handleLightCmd(cmd);
    }

    stepFades();
    stepSolenoid();
    stepHeartbeat();

    if (testMode && !paused && pcaAddr) autoSequence();
    else delay(2);
}
