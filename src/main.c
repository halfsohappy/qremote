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
#include "dhcpserver/dhcpserver.h"
#include "dhcpserver/dhcpserver_options.h"

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
    esp_netif_ip_info_t ip_info = {
        .ip      = { .addr = ipaddr_addr(NCM_DEVICE_IP) },
        .gw      = { .addr = ipaddr_addr(NCM_DEVICE_IP) },
        .netmask = { .addr = ipaddr_addr(NCM_NETMASK)   },
    };

    esp_netif_inherent_config_t base_cfg = {
        .flags      = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP,
        .ip_info    = &ip_info,
        .if_key     = "USB_NCM",
        .if_desc    = "usbnet",
        .route_prio = 10,
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

    // Constrain DHCP to a single-address pool: macOS always gets NCM_HOST_IP.
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_netif));
    dhcps_lease_t lease = {
        .enable   = true,
        .start_ip = { .addr = ipaddr_addr(NCM_HOST_IP) },
        .end_ip   = { .addr = ipaddr_addr(NCM_HOST_IP) },
    };
    ESP_ERROR_CHECK(esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET,
                                           ESP_NETIF_REQUESTED_IP_ADDRESS,
                                           &lease, sizeof(lease)));

    esp_netif_action_start(s_netif, NULL, 0, NULL);
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_netif));

    ESP_LOGI(TAG, "netif up: device=%s, host-lease=%s/%s",
             NCM_DEVICE_IP, NCM_HOST_IP, NCM_NETMASK);
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
    ESP_LOGI(TAG, "USB-NCM device started");
}

static void setup_mdns(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(MDNS_HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set("qremote OSC bridge"));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_osc", "_udp", QLAB_OSC_PORT, NULL, 0));
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
    setup_mdns();

    xTaskCreate(udp_task, "udp", 4096, NULL, 5, NULL);
}
