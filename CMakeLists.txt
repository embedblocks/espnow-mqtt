cmake_minimum_required(VERSION 3.16)

# Build the SRCS list conditionally based on role.
# Entire .c files are excluded for role-split builds — not just guarded with #if.
# espnow_mqtt.c (common layer) is always compiled; it contains narrow #if guards
# in recv_cb and send_cb dispatch paths only.

set(srcs "src/espnow_mqtt.c")

# Publisher sources: included when role is BOTH or PUBLISHER (i.e. not BROKER-only)
if(NOT CONFIG_ESPNOW_MQTT_ROLE_BROKER)
    list(APPEND srcs "src/espnow_mqtt_publisher.c")
endif()

# Broker sources: included when role is BOTH or BROKER (i.e. not PUBLISHER-only)
if(NOT CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER)
    list(APPEND srcs "src/espnow_mqtt_broker.c")
endif()

# Optional HMAC integrity checking (Phase 6)
if(CONFIG_ESPNOW_MQTT_PAYLOAD_HMAC)
    list(APPEND srcs "src/espnow_mqtt_hmac.c")
endif()

# mbedtls is only required when HMAC is enabled
set(optional_requires "")
if(CONFIG_ESPNOW_MQTT_PAYLOAD_HMAC)
    list(APPEND optional_requires mbedtls)
endif()

idf_component_register(
    SRCS
        ${srcs}
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        esp_now
        esp_wifi
        nvs_flash
        freertos
        esp_timer
        ${optional_requires}
)
