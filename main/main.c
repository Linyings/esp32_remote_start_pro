#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <dirent.h>
#include <sys/stat.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "wifi_manager/wifi_manager.h"
#include "pc_monitor/pc_monitor.h"
#include "servo_control/servo_control.h"
#include "web_server/web_server.h"

static const char *TAG = "main";

// 任务句柄
TaskHandle_t pc_monitor_task_handle = NULL;

void app_main(void)
{
    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    ESP_LOGI(TAG, "ESP32远程开机助手启动");
    
    // 初始化netif
    ESP_ERROR_CHECK(esp_netif_init());
    
    // 创建事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // 初始化WiFi管理器
    wifi_manager_init();
    
    // 初始化PC状态监控
    pc_monitor_init();
    
    // 初始化舵机控制
    servo_control_init();
    
    // 启动Web服务器
    web_server_init();
    
    // 创建PC监控任务
    xTaskCreate(pc_monitor_task, "pc_monitor_task", 4096, NULL, 5, &pc_monitor_task_handle);
    
    ESP_LOGI(TAG, "所有组件初始化完成");
} 