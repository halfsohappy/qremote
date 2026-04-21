#pragma once
// Minimal Arduino.h shim so upstream MicroOsc compiles under ESP-IDF. Only
// pulled in for the parse/write-buffer paths we use; nothing Arduino-specific
// is referenced beyond what standard C headers already provide.
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
