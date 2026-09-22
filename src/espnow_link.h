#pragma once
// ESP-NOW transport to the LED strip controller. Wi-Fi is otherwise idle on
// this board (the host link is USB-NCM), so this just brings the radio up
// in STA mode on a fixed channel and broadcasts — no pairing/AP needed for
// a single receiver.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void espnow_link_init(void);
void espnow_link_send(const void *data, size_t len);

#ifdef __cplusplus
}
#endif
