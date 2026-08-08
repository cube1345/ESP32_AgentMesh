set(ESPAGENT_CORE_SRCS
    "cJSON_upstream.c"
    "espagent.c"
    "app/espagent_app.c"
    "bus/message_bus.c"
    "time_sync/time_sync.c"
    "wifi/wifi_manager.c"
    "net/net_guard.c"
    "device/device_registry.c"
)

set(ESPAGENT_AGENT_SRCS
    "agent/agent_loop.c"
    "agent/agent_state_machine.c"
    "agent/context_builder.c"
    "agent/slash_command.c"
    "capability/capability_registry.c"
    "capability/role_capability_profile.c"
    "events/espagent_event.c"
    "dynamic/dynamic_extension.c"
    "llm/llm_proxy.c"
    "lua/espagent_lua_runtime.c"
    "cache/cache_store.c"
    "memory/memory_store.c"
    "memory/memory_v2.c"
    "memory/session_mgr.c"
    "skills/skill_loader.c"
    "skills/skill_runtime.c"
)

set(ESPAGENT_MESH_ROLE_SRCS
    "mesh/mesh_protocol.c"
    "mesh/mesh_auth.c"
    "mesh/task_registry.c"
    "node/node_profile.c"
    "roles/role_config.c"
    "roles/coordinator_node.c"
    "roles/sensor_node.c"
    "roles/control_node.c"
    "roles/display_node.c"
    "roles/guardian_node.c"
    "guardian/approval_queue.c"
    "control/command_queue.c"
    "control/ir_emergency_stop.c"
    "sensors/sensor_history.c"
    "sensors/sensor_mqtt.c"
    "espnow/espnow_sender.c"
)

set(ESPAGENT_CHANNEL_SRCS
    "channels/feishu/feishu_bot.c"
    "gateway/ws_server.c"
    "cli/serial_cli.c"
    "proxy/http_proxy.c"
    "onboard/wifi_onboard.c"
)

set(ESPAGENT_TOOL_SRCS
    "tools/tool_registry.c"
    "tools/tool_sandbox.c"
    "tools/tool_amap_weather.c"
    "tools/tool_web_search.c"
    "tools/tool_get_time.c"
    "tools/tool_lua.c"
    "tools/tool_files.c"
    "tools/tool_mesh_command.c"
    "tools/tool_virtual_device.c"
    "tools/tool_subagent.c"
    "tools/tool_sensor_history.c"
    "tools/gpio_policy.c"
    "tools/tool_gpio.c"
    "tools/tool_servo.c"
    "tools/tool_max98357.c"
    "tools/tool_environment.c"
    "tools/tool_aht10.c"
    "tools/tool_hc_sr05.c"
    "tools/tool_sgp30.c"
    "tools/tool_bh1750.c"
    "tools/tool_gree_ac.c"
)

set(ESPAGENT_DRIVER_SRCS
    "drivers/aht10.c"
    "drivers/max98357.c"
    "drivers/sgp30.c"
    "drivers/bh1750.c"
    "drivers/gree_ir.c"
)

idf_component_register(
    SRCS
        ${ESPAGENT_CORE_SRCS}
        ${ESPAGENT_AGENT_SRCS}
        ${ESPAGENT_MESH_ROLE_SRCS}
        ${ESPAGENT_CHANNEL_SRCS}
        ${ESPAGENT_TOOL_SRCS}
        ${ESPAGENT_DRIVER_SRCS}
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash esp_wifi esp_netif esp_http_client esp_http_server
        esp_event spiffs console vfs esp-tls
        esp_timer esp_websocket_client esp_driver_gpio esp_driver_rmt esp_driver_i2c
        esp_driver_spi esp_adc
        esp_driver_ledc esp_driver_uart esp_driver_i2s
)
