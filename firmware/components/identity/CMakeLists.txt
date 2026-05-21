# PagerOS device identity — FW-014.
#
# Generates and persists the device's Ed25519 keypair (seed in NVS,
# pubkey derived via the `crypto` component which vendors Monocypher).

idf_component_register(
    SRCS
        "src/identity.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_common log esp_hw_support crypto
)
