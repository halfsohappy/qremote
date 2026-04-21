#pragma once
// AMOLED status display. No-op on builds without CONFIG_QREMOTE_HAS_DISPLAY,
// so main.c can call these unconditionally.

#define OSC_LOG_LINE_MAX 256

#ifdef __cplusplus
extern "C" {
#endif

void display_init(void);
void display_set_status(const char *msg);
void display_set_host(const char *ip);               // "" to clear
void display_log_osc(const char *addr,
                     const char *tags,
                     const char *summary);

#ifdef __cplusplus
}
#endif
