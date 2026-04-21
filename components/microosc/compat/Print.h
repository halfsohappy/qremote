#pragma once
// Arduino Print-interface shim. MicroOsc writes outbound OSC bytes through
// this abstraction; on ESP-IDF we subclass it to capture bytes into our own
// UDP send buffer (see osc_bridge.cpp).
#include <stdint.h>
#include <stddef.h>
#include <string.h>

class Print {
public:
    virtual ~Print() {}
    virtual size_t write(uint8_t b) = 0;
    virtual size_t write(const uint8_t *buf, size_t n) {
        size_t total = 0;
        for (size_t i = 0; i < n; ++i) total += write(buf[i]);
        return total;
    }
    size_t print(const char *s) {
        return write(reinterpret_cast<const uint8_t *>(s), strlen(s));
    }
};
