#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_wifi.h"
#include "esp_err.h"

// 工作模式枚举
typedef enum {
    WIFI_MANAGER_MODE_AP,     // 热点模式（配置模式）
    WIFI_MANAGER_MODE_STA     // 客户端模式（服务模式）
} wifi_working_mode_t;

// WiFi事件回调类型
typedef void (*wifi_event_callback_t)(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

// 初始化WiFi管理器
void wifi_manager_init(void);

// 开始AP模式
esp_err_t wifi_manager_start_ap(void);

// 开始STA模式
esp_err_t wifi_manager_start_sta(const char *ssid, const char *password);

// 注册WiFi事件回调
void wifi_manager_register_callback(wifi_event_callback_t callback, void *arg);

// 获取当前模式
wifi_working_mode_t wifi_manager_get_mode(void);

// 保存WiFi凭证到NVS
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);

// 从NVS加载WiFi凭证
esp_err_t wifi_manager_load_credentials(char *ssid, char *password);

// 扫描可用的WiFi网络
wifi_ap_record_t *wifi_manager_scan_networks(uint16_t *ap_count);

#endif /* WIFI_MANAGER_H */ 