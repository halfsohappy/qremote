#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "lwip/inet.h"
#include "mdns.h"
#include "tinyusb.h"
#include "tinyusb_net.h"
#include "tusb_cdc_acm.h"
#include "tusb_console.h"

#include "config.h"

static const char *TAG = "qremote";

// esp_netif handle for our USB-NCM interface — referenced by the USB rx callback
// (pushes frames up into lwIP) and by the transmit driver (lwIP pushes down to USB).
static esp_netif_t *s_netif;

// ───────────────────────────── netif ↔ USB glue ─────────────────────────────

// lwIP has a frame to send → hand it to TinyUSB's NCM driver.
static esp_err_t netif_transmit(void *handle, void *buffer, size_t len)
{
    if (tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(100)) != ESP_OK) {
        ESP_LOGW(TAG, "tx dropped (%u B)", (unsigned)len);
    }
    return ESP_OK;
}

// lwIP is done with an rx buffer we handed it → free our copy.
static void netif_free_rx_buffer(void *handle, void *buffer)
{
    free(buffer);
}

// USB received a frame → copy and push into lwIP via s_netif.
static esp_err_t usb_recv_cb(void *buffer, uint16_t len, void *ctx)
{
    void *copy = malloc(len);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, buffer, len);

    if (esp_netif_receive(s_netif, copy, len, NULL) != ESP_OK) {
        free(copy);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// TinyUSB finished with a tx buffer — nothing to free; tinyusb_net_send_sync owns it.
static void usb_free_tx_cb(void *buffer, void *ctx) { (void)buffer; (void)ctx; }

// ─────────────────────────────── setup steps ────────────────────────────────

static void setup_netif(void)
{
    // CRITICAL: leave gw = 0 so the DHCP server does NOT advertise a default
    // gateway. If we advertised 172.30.42.1 as gateway, macOS would install
    // a default route through our USB interface, hijacking the user's entire
    // internet connection (and breaking DNS, since we'd also be the only
    // nameserver reachable via that route). The device is a point-to-point
    // link, not a router.
    esp_netif_ip_info_t ip_info = {
        .ip      = { .addr = ipaddr_addr(NCM_DEVICE_IP) },
        .gw      = { .addr = 0 },
        .netmask = { .addr = ipaddr_addr(NCM_NETMASK)   },
    };

    // route_prio = 0: never compete with WiFi/Ethernet for the default route
    // on the device side either.
    esp_netif_inherent_config_t base_cfg = {
        .flags      = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP,
        .ip_info    = &ip_info,
        .if_key     = "USB_NCM",
        .if_desc    = "usbnet",
        .route_prio = 0,
    };

    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle                = (void *)1,   // non-null sentinel — required
        .transmit              = netif_transmit,
        .driver_free_rx_buffer = netif_free_rx_buffer,
    };

    esp_netif_config_t cfg = {
        .base   = &base_cfg,
        .driver = &driver_cfg,
        .stack  = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };

    s_netif = esp_netif_new(&cfg);
    assert(s_netif);

    // Stable MAC — derive from factory efuse so re-flashing keeps the same identity
    // and macOS reuses its cached DHCP lease. ESP_MAC_ETH is unique per chip.
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_ETH));
    ESP_ERROR_CHECK(esp_netif_set_mac(s_netif, mac));

    // Bring netif up; the ESP_NETIF_DHCP_SERVER flag causes esp_netif to start
    // the DHCP server automatically. Default pool = subnet starting at
    // device_ip+1, so macOS gets NCM_HOST_IP (172.30.42.2).
    esp_netif_action_start(s_netif, NULL, 0, NULL);
    // Ethernet-style drivers emit ETHERNET_EVENT_CONNECTED; we're a custom
    // driver with no events, so signal "L2 link live" to esp_netif ourselves.
    // Without this, the host's NCM driver shows media: none.
    esp_netif_action_connected(s_netif, NULL, 0, NULL);

    ESP_LOGI(TAG, "netif up: device=%s, netmask=%s", NCM_DEVICE_IP, NCM_NETMASK);
}

static void setup_usb(void)
{
    const tinyusb_config_t tusb_cfg = { 0 };    // defaults: our own descriptors not needed
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    // Give the USB-side MAC a distinct last-byte from the netif MAC so host and
    // device aren't identical on the wire. Keeps Wireshark / netstat honest.
    uint8_t usb_mac[6];
    ESP_ERROR_CHECK(esp_read_mac(usb_mac, ESP_MAC_ETH));
    usb_mac[5] ^= 0x01;

    tinyusb_net_config_t net_cfg = {
        .on_recv_callback = usb_recv_cb,
        .free_tx_buffer   = usb_free_tx_cb,
        .user_context     = NULL,
    };
    memcpy(net_cfg.mac_addr, usb_mac, sizeof(usb_mac));

    ESP_ERROR_CHECK(tinyusb_net_init(TINYUSB_USBDEV_0, &net_cfg));

    // Add a CDC-ACM interface alongside NCM — composite device, one USB cable.
    const tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev          = TINYUSB_USBDEV_0,
        .cdc_port         = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = 64,
        .callback_rx                   = NULL,
        .callback_rx_wanted_char       = NULL,
        .callback_line_state_changed   = NULL,
        .callback_line_coding_changed  = NULL,
    };
    ESP_ERROR_CHECK(tusb_cdc_acm_init(&acm_cfg));

    // Redirect stdout/stderr (ESP_LOG) to the CDC interface. After this,
    // `pio device monitor` on the USB-C port shows live logs.
    ESP_ERROR_CHECK(esp_tusb_init_console(TINYUSB_CDC_ACM_0));

    ESP_LOGI(TAG, "USB composite up: CDC (logs) + NCM (network)");
}

// mDNS is a nice-to-have — failures here must not abort the firmware because
// that would kill the UDP/OSC path too. Log and keep going.
#define MDNS_TRY(call) do {                                                  \
    esp_err_t _e = (call);                                                   \
    if (_e != ESP_OK) {                                                      \
        ESP_LOGW(TAG, "mdns: %s failed: %s", #call, esp_err_to_name(_e));    \
    }                                                                        \
} while (0)

static void setup_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed: %s — skipping mDNS", esp_err_to_name(err));
        return;
    }
    MDNS_TRY(mdns_hostname_set(MDNS_HOSTNAME));
    MDNS_TRY(mdns_instance_name_set("qremote OSC bridge"));

    // Our USB-NCM netif isn't a default-tracked interface (WiFi STA/AP or
    // built-in Ethernet), so mdns won't listen on it unless we register it.
    MDNS_TRY(mdns_register_netif(s_netif));
    MDNS_TRY(mdns_netif_action(s_netif,
        MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4));

    MDNS_TRY(mdns_service_add(NULL, "_osc", "_udp", QLAB_OSC_PORT, NULL, 0));
    ESP_LOGI(TAG, "mDNS: %s.local  _osc._udp:%u", MDNS_HOSTNAME, QLAB_OSC_PORT);
}

// ───────────────────────────── UDP / OSC task ───────────────────────────────

static void udp_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket: %d", errno);
        vTaskDelete(NULL);
    }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(UDP_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind :%u failed: %d", UDP_PORT, errno);
        close(sock);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "UDP listening on %s:%u", NCM_DEVICE_IP, UDP_PORT);

    uint8_t buf[BUF_SIZE];
    while (1) {
        struct sockaddr_in src;
        socklen_t srclen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&src, &srclen);
        if (n < 0) {
            ESP_LOGW(TAG, "recvfrom: %d", errno);
            continue;
        }
        ESP_LOGI(TAG, "rx %d B from %s:%u",
                 n, inet_ntoa(src.sin_addr), ntohs(src.sin_port));

        // Echo back (placeholder — replace with OSC parse + dispatch).
        sendto(sock, buf, n, 0, (struct sockaddr *)&src, srclen);
    }
}

// ───────────────────────────────── entry ────────────────────────────────────

void app_main(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    setup_netif();
    setup_usb();

    // Start UDP task before mdns — if mdns has a problem, OSC still works.
    xTaskCreate(udp_task, "udp", 4096, NULL, 5, NULL);

    vTaskDelay(pdMS_TO_TICKS(500));   // let the netif settle before mdns binds
    setup_mdns();
}
