idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES 
        nvs_flash
        esp_netif
        wifi_manager
        pc_monitor
        servo_control
        web_server
)

# 禁用格式截断警告
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
