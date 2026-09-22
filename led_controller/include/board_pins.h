#pragma once
// Waveshare ESP32-S3-LCD-1.69 pinout (see ../README.md), plus a solenoid
// added for a prop effect. Reconstructed from the board's pin table —
// double-check against the original board_pins.h before flashing if you
// have it.

#define PIN_LCD_SCLK    6
#define PIN_LCD_MOSI    7
#define PIN_LCD_CS      5
#define PIN_LCD_DC      4
#define PIN_LCD_RST     8
#define PIN_LCD_BL      15

#define PIN_I2C_SDA     11
#define PIN_I2C_SCL     10
#define PIN_IMU_INT1    38
#define PIN_RTC_INT     39
#define PIN_BUZZER      42
#define PIN_BATT_ADC    1
#define PIN_SYS_OUT     40
#define PIN_SYS_EN      41
#define PIN_BOOT        0

#define I2C_ADDR_QMI8658  0x6B
#define I2C_ADDR_PCF85063 0x51

// Added for the LED-strip receiver: solenoid driver MOSFET gate.
#define PIN_SOLENOID    16
