set(qremote_srcs "main.c" "display.c" "osc_bridge.cpp")

set(qremote_priv_reqs esp_tinyusb mdns microosc)
if(CONFIG_QREMOTE_HAS_DISPLAY)
    # Only link the display stack when the AMOLED env requests it. The deps
    # are listed unconditionally in idf_component.yml so they stay downloaded,
    # but they only get pulled into the link here.
    list(APPEND qremote_priv_reqs
        esp_lcd
        esp_lvgl_port
        esp_lcd_sh8601
        lvgl
        driver)
endif()

idf_component_register(
    SRCS ${qremote_srcs}
    INCLUDE_DIRS "." "../include"
    REQUIRES esp_netif esp_event lwip
    PRIV_REQUIRES ${qremote_priv_reqs}
)
