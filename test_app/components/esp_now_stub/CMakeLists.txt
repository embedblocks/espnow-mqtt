cmake_minimum_required(VERSION 3.16)

# esp_now_stub — Component Replacement for the real esp_now component.
# Provides the same public symbols but routes all calls through mock_transport.c.
# Visible only to test_app — never shipped as part of espnow_mqtt itself.
#
# The real esp_now component is excluded from test_app via the COMPONENTS list
# in test_app/CMakeLists.txt. The linker then resolves all esp_now_* symbols
# from this stub instead.

idf_component_register(
    SRCS
        "esp_now_stub.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        # The test component provides mock_transport.h
        espnow_mqtt_test
)
