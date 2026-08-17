# PagerOS on-device AI agent harness.
#
# Conversational tool-use loop that talks to an OpenAI-compatible
# endpoint (default: OpenRouter) and dispatches calls into a tool
# registry covering every hardware subsystem on the device.

idf_component_register(
    SRCS
        "src/agent_config.c"
        "src/agent_messages.c"
        "src/agent_tools.c"
        "src/agent_http.c"
        "src/agent_core.c"
        "src/tools_register.c"
        "src/tool_wifi.c"
        "src/tool_gps.c"
        "src/tool_lora.c"
        "src/tool_nfc.c"
        "src/tool_fs.c"
        "src/tool_net.c"
        "src/tool_sys.c"
    INCLUDE_DIRS "include"
    REQUIRES
        network
        storage
        json
        esp_wifi
        esp_event
        esp_http_client
        freertos
        gps
        lora
        nfc
        audio
        imu
        power
        identity
        widgets
        fonts
    PRIV_REQUIRES log esp_timer
)
