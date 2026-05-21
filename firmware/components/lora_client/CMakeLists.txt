# PagerOS LoRa transport client — FW-026.
#
# Envelope codec + fragmentation + high-level request/response over the
# SX1262 driver (FW-008).

idf_component_register(
    SRCS
        "src/lora_envelope.c"
        "src/lora_fragment.c"
        "src/lora_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common lora
    PRIV_REQUIRES log esp_timer
)
