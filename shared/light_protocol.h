#pragma once
#include <stdint.h>

// Wire protocol sent over ESP-NOW from the qremote OSC dongle (sender) to
// the LED strip controller (receiver). Both sides include this header and
// must agree on the struct layout byte-for-byte — this is a raw send/recv
// of the packed struct, no serialization step.
//
// Fixed Wi-Fi channel lets both boards use ESP-NOW without either joining
// an access point (the dongle's Wi-Fi radio is otherwise unused — its link
// to the Mac is USB-NCM, not Wi-Fi).

#ifdef __cplusplus
extern "C" {
#endif

#define LIGHT_ESPNOW_CHANNEL   1

typedef enum {
    LIGHT_CMD_STRIP    = 1,  // one strip's RGBW: strip, r,g,b,w, fade_ms
    LIGHT_CMD_ALL      = 2,  // all strips to the same RGBW: r,g,b,w, fade_ms
    LIGHT_CMD_OFF      = 3,  // blackout all strips: fade_ms
    LIGHT_CMD_SOLENOID = 4,  // pulse the solenoid: fade_ms doubles as pulse duration (ms)
} light_cmd_kind_t;

typedef struct __attribute__((packed)) {
    uint8_t  cmd;       // light_cmd_kind_t
    uint8_t  strip;     // 0-3; ignored for ALL / OFF / SOLENOID
    uint8_t  r, g, b, w;    // 0-255
    uint16_t fade_ms;   // 0 = snap; solenoid pulse duration for LIGHT_CMD_SOLENOID
    uint16_t seq;       // increments per send; lets the receiver log drops/reorders
} light_cmd_t;

#ifdef __cplusplus
}
#endif
