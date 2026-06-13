cmake_minimum_required(VERSION 3.16)

# test_app — Unity test runner application.
#
# Flash this to the target to get the interactive Unity test selection menu
# on UART. Each test case is individually selectable.
#
# Build system strategy:
#   1. EXTRA_COMPONENT_DIRS adds:
#      - ../../   (the espnow_mqtt component itself)
#      - ../../test/  (the test component with Unity test sources)
#      - components/  (the esp_now_stub Component Replacement)
#   2. The COMPONENTS list explicitly enumerates what to build.
#      The real 'esp_now' component is NOT in the list — the linker sees
#      only esp_now_stub, which provides all esp_now_* symbols.
#
# Note: component names in ESP-IDF CMake correspond to the directory name
# or the name in idf_component_register COMPONENT_NAME. The test component
# directory is 'test' so its component name is 'espnow_mqtt_test' (set via
# COMPONENT_NAME below — see test/CMakeLists.txt).

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/../..        # espnow_mqtt component
    ${CMAKE_CURRENT_LIST_DIR}/../../test   # Unity test sources
    ${CMAKE_CURRENT_LIST_DIR}/components   # esp_now_stub
)

# Enumerate components explicitly to exclude the real esp_now.
# All other standard IDF components are still pulled in via their
# normal dependency resolution.
set(COMPONENTS
    main
    espnow_mqtt
    espnow_mqtt_test   # the test/ directory component
    esp_now_stub
    unity
    freertos
    esp_wifi
    nvs_flash
    esp_timer
    esp_log
    esp_common
    newlib
)

project(espnow_mqtt_test_app)
