cmake_minimum_required(VERSION 3.16)

idf_component_register(
    SRCS        "publisher_main.c"
    REQUIRES    espnow-mqtt esp_wifi nvs_flash
)
