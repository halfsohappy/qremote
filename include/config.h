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

// ─── Waveshare ESP32-S3-Touch-AMOLED-1.64 display pinout ────────────────────
// 280×456 SH8601 panel driven over QSPI. Only relevant to the
// waveshare_amoled env (guarded by CONFIG_QREMOTE_HAS_DISPLAY).
#define AMOLED_W            280
#define AMOLED_H            456
#define AMOLED_PIN_CS       9
#define AMOLED_PIN_SCLK     10
#define AMOLED_PIN_D0       11
#define AMOLED_PIN_D1       12
#define AMOLED_PIN_D2       13
#define AMOLED_PIN_D3       14
#define AMOLED_PIN_RESET    21
#define AMOLED_SPI_HOST     SPI2_HOST
