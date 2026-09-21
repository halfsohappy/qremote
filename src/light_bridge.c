#include "light_bridge.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

#include "espnow_link.h"
#include "light_protocol.h"

static const char *TAG = "qremote.light";
static uint16_t s_seq;

static uint8_t clamp255(int32_t v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static bool have_ints(const osc_parsed_t *m, uint8_t n)
{
    if (m->nints < n) {
        ESP_LOGW(TAG, "%s: expected %u int arg(s), got %u", m->address, n, m->nints);
        return false;
    }
    return true;
}

// QLab-facing OSC addresses (drive these from Network cues, all-int args):
//   /light/strip    <strip 0-3> <r> <g> <b> <w> <fadeMs>
//   /light/all      <r> <g> <b> <w> <fadeMs>
//   /light/off      [fadeMs]              -- fadeMs defaults to 0 (snap)
//   /light/solenoid <pulseMs>
void light_bridge_handle(const osc_parsed_t *m)
{
    if (!m->address) return;

    light_cmd_t cmd = { 0 };

    if (!strcmp(m->address, "/light/strip")) {
        if (!have_ints(m, 6)) return;
        cmd.cmd     = LIGHT_CMD_STRIP;
        cmd.strip   = (uint8_t)m->ints[0];
        cmd.r       = clamp255(m->ints[1]);
        cmd.g       = clamp255(m->ints[2]);
        cmd.b       = clamp255(m->ints[3]);
        cmd.w       = clamp255(m->ints[4]);
        cmd.fade_ms = (uint16_t)m->ints[5];
    } else if (!strcmp(m->address, "/light/all")) {
        if (!have_ints(m, 5)) return;
        cmd.cmd     = LIGHT_CMD_ALL;
        cmd.r       = clamp255(m->ints[0]);
        cmd.g       = clamp255(m->ints[1]);
        cmd.b       = clamp255(m->ints[2]);
        cmd.w       = clamp255(m->ints[3]);
        cmd.fade_ms = (uint16_t)m->ints[4];
    } else if (!strcmp(m->address, "/light/off")) {
        cmd.cmd     = LIGHT_CMD_OFF;
        cmd.fade_ms = m->nints >= 1 ? (uint16_t)m->ints[0] : 0;
    } else if (!strcmp(m->address, "/light/solenoid")) {
        if (!have_ints(m, 1)) return;
        cmd.cmd     = LIGHT_CMD_SOLENOID;
        cmd.fade_ms = (uint16_t)m->ints[0];
    } else {
        return; // not an address we handle
    }

    cmd.seq = s_seq++;
    espnow_link_send(&cmd, sizeof(cmd));
}
