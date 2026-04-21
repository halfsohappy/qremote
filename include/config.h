#pragma once

// USB-NCM network addresses — ESP32-S3 is the "device" end
static constexpr const char* NCM_LOCAL_IP  = "192.168.7.1";
static constexpr const char* NCM_GW_IP     = "192.168.7.2";   // host-side address
static constexpr const char* NCM_NETMASK   = "255.255.255.0";

// UDP listener
static constexpr uint16_t UDP_PORT  = 4210;
static constexpr size_t   BUF_SIZE  = 512;
