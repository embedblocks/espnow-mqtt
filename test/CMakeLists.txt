cmake_minimum_required(VERSION 3.16)

set(srcs
    "test_wire_protocol.c"
    "test_topic_validation.c"
    "test_topic_id_negotiation.c"
    "test_seq_tracking.c"
    "mock_transport.c"
)

# Phase 6: HMAC tests compiled only when PAYLOAD_HMAC is enabled.
# test_app/sdkconfig.defaults_hmac enables this for the HMAC test build.
if(CONFIG_ESPNOW_MQTT_PAYLOAD_HMAC)
    list(APPEND srcs "test_hmac_integrity.c")
endif()

idf_component_register(
    SRCS
        ${srcs}
    INCLUDE_DIRS
        "."
    REQUIRES
        espnow_mqtt
        unity
)
