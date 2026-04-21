idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "." "../include"
    REQUIRES esp_netif esp_event lwip
    PRIV_REQUIRES esp_tinyusb mdns
)
