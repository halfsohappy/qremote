#pragma once
// MicroOsc.cpp unconditionally includes <Udp.h> (Arduino UDP class) at the
// top even when we only use the base MicroOsc class. We don't use
// MicroOscUdp on ESP-IDF — our transport is native lwIP sockets — so this
// is just an empty stub to satisfy the include.
