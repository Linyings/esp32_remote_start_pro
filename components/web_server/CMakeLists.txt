idf_component_register(
    SRCS "web_server_fixed.c"
    INCLUDE_DIRS "include"
    REQUIRES 
        esp_http_server
        json
        pc_monitor
        servo_control
        wifi_manager
        spiffs
) 