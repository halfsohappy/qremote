#pragma once

// USB-NCM point-to-point link between host (Mac running QLab) and device (ESP32-S3).
// Subnet choice rationale:
//   - RFC1918 private space, /24 for macOS/Windows network-settings friendliness
//   - 172.16.0.0/12 block: far less common on home & show networks than
//     192.168.x.x (home routers, ETC consoles) or 10.x.x.x (corporate, Dante)
//   - .42.x is memorable and unlikely to collide with anything
static constexpr const char* NCM_LOCAL_IP  = "172.30.42.1";   // ESP32-S3 (device)
static constexpr const char* NCM_GW_IP     = "172.30.42.2";   // host (Mac / QLab)
static constexpr const char* NCM_NETMASK   = "255.255.255.0";

// OSC / QLab ports — 53000 is QLab's default OSC-in.
// We listen on 53000 for cues coming from QLab, and send to host:53000
// to drive QLab (e.g. /cue/1/go). QLab replies come back to our sender port.
static constexpr uint16_t QLAB_OSC_PORT = 53000;
static constexpr uint16_t UDP_PORT      = QLAB_OSC_PORT;
static constexpr size_t   BUF_SIZE      = 1024;   // OSC bundles can exceed 512 B
