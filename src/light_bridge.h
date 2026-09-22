#pragma once
// Translates the `/light/...` OSC namespace (QLab Network cues) into
// light_cmd_t packets sent over ESP-NOW. See shared/light_protocol.h for
// the wire format and README notes on the OSC addresses.

#include "osc_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

void light_bridge_handle(const osc_parsed_t *msg);

#ifdef __cplusplus
}
#endif
