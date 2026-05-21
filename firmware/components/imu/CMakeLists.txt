# PagerOS IMU driver stub — FW-012.
#
# Probes the BHI260AP on the shared I2C bus and exposes a raw register
# read passthrough. Firmware upload + sensor event dispatch are deferred
# to v2 per FW-012's spec.

idf_component_register(
    SRCS    "src/imu.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_common log freertos
)
