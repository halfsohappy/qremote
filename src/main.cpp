#include <Arduino.h>
#include <USB.h>
#include <USBNCM.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include "config.h"

// USB-NCM interface — exposes the ESP32-S3 as a USB ethernet adapter to the host.
// Host sees a new network interface; assign NCM_GW_IP on that interface, then
// send UDP to NCM_LOCAL_IP:UDP_PORT to reach this device.
USBNCM ncm;

static int g_sock = -1;

static void udp_open() {
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock < 0) {
        Serial.printf("[udp] socket() failed: %d\n", errno);
        return;
    }

    // Non-blocking so loop() stays responsive
    int flags = fcntl(g_sock, F_GETFL, 0);
    fcntl(g_sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(UDP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(g_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        Serial.printf("[udp] bind() failed: %d\n", errno);
        close(g_sock);
        g_sock = -1;
        return;
    }

    Serial.printf("[udp] listening on %s:%u\n", NCM_LOCAL_IP, UDP_PORT);
}

static void udp_poll() {
    if (g_sock < 0) return;

    static uint8_t buf[BUF_SIZE];
    struct sockaddr_in src{};
    socklen_t srclen = sizeof(src);

    int len = recvfrom(g_sock, buf, sizeof(buf) - 1, 0,
                       reinterpret_cast<sockaddr*>(&src), &srclen);
    if (len < 0) return;  // EAGAIN or error

    buf[len] = '\0';
    Serial.printf("[udp] rx %d B from %s:%u: %.*s\n",
                  len, inet_ntoa(src.sin_addr), ntohs(src.sin_port),
                  len, buf);

    // Echo the packet back to sender
    sendto(g_sock, buf, len, 0,
           reinterpret_cast<sockaddr*>(&src), srclen);
}

void setup() {
    // UART0 via the devkit's USB-serial bridge — independent of native USB
    Serial.begin(115200);
    delay(500);
    Serial.println("[init] qremote starting");

    // Static IP for the NCM interface
    ncm.setLocalIP(NCM_LOCAL_IP);
    ncm.setSubnetMask(NCM_NETMASK);
    ncm.setGateway(NCM_GW_IP);

    ncm.begin();
    USB.begin();

    Serial.println("[ncm] waiting for host to connect...");
    while (!ncm.connected()) {
        delay(100);
    }
    Serial.printf("[ncm] up — device %s, host %s\n", NCM_LOCAL_IP, NCM_GW_IP);

    udp_open();
}

void loop() {
    udp_poll();

    // Re-open socket if NCM link drops and comes back
    if (ncm.connected() && g_sock < 0) {
        Serial.println("[ncm] reconnected, reopening UDP socket");
        udp_open();
    } else if (!ncm.connected() && g_sock >= 0) {
        Serial.println("[ncm] link down, closing socket");
        close(g_sock);
        g_sock = -1;
    }

    delay(1);
}
