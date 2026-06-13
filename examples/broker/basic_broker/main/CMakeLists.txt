cmake_minimum_required(VERSION 3.16)

idf_component_register(
    SRCS        "broker_main.c"
    REQUIRES    espnow_mqtt esp_wifi esp_now nvs_flash
)
