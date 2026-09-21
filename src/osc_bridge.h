#pragma once
// C-callable wrapper around MicroOsc (C++ library from halfsohappy/MicroOsc).
// Parses a UDP payload — message or bundle — into a simple struct suitable
// for display/logging, and invokes a callback per message.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OSC_SUMMARY_MAX 160
#define OSC_MAX_INT_ARGS 8

typedef struct {
    const char *address;     // pointer into caller's buffer; valid during callback
    const char *typetags;    // ditto
    char summary[OSC_SUMMARY_MAX];  // pretty-printed args, e.g. `"hello" 42 1.5`
    int32_t ints[OSC_MAX_INT_ARGS];  // values of 'i'-tagged args, in order (args past capacity are dropped)
    uint8_t nints;
} osc_parsed_t;

typedef void (*osc_handler_t)(const osc_parsed_t *msg, void *user);

// Parse buffer in-place; calls `cb` for each message found (≥1 for bundles).
// `buffer` is mutated temporarily by MicroOsc but is NOT retained.
void osc_parse(uint8_t *buffer, size_t len, osc_handler_t cb, void *user);

#ifdef __cplusplus
}
#endif
