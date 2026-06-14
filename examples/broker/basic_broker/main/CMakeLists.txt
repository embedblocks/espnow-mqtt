cmake_minimum_required(VERSION 3.16)

idf_component_register(
    SRCS        "broker_main.c"
    INCLUDE_DIRS "."
    REQUIRES    espnow-mqtt esp_wifi nvs_flash
)
