#pragma once

// USB-NCM point-to-point link between host (Mac running QLab) and device (ESP32-S3).
// Subnet choice rationale:
//   - RFC1918 private space, /24 for macOS/Windows network-settings friendliness
//   - 172.16.0.0/12 block: far less common on home & show networks than
//     192.168.x.x (home routers, ETC consoles) or 10.x.x.x (corporate, Dante)
//   - .42.x is memorable and unlikely to collide with anything
#define NCM_DEVICE_IP   "172.30.42.1"   /* ESP32-S3 (this device, DHCP server) */
#define NCM_HOST_IP     "172.30.42.2"   /* Mac / QLab (DHCP client, single lease) */
#define NCM_NETMASK     "255.255.255.0"

// mDNS — host can reach the device at <hostname>.local without knowing the IP.
#define MDNS_HOSTNAME   "qremote"

// OSC / QLab ports — 53000 is QLab's default OSC-in.
#define QLAB_OSC_PORT   53000
#define UDP_PORT        QLAB_OSC_PORT
#define BUF_SIZE        1024    /* OSC bundles can exceed 512 B */
