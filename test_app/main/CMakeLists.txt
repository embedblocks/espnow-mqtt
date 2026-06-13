cmake_minimum_required(VERSION 3.16)

idf_component_register(
    SRCS
        "test_app_main.c"
    REQUIRES
        unity
        espnow_mqtt_test
)
