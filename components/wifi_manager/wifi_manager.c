#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include <errno.h>

#include "wifi_manager/wifi_manager.h"

static const char *TAG = "wifi_manager";

#define MAX_SSID_LEN 32
#define MAX_PASSWORD_LEN 64
#define MAX_CONNECTION_RETRIES 5
#define WIFI_NVS_NAMESPACE "wifi_config"
#define WIFI_NVS_SSID_KEY "ssid"
#define WIFI_NVS_PASS_KEY "password"

// AP模式配置
#define DEFAULT_AP_SSID "ESP32开机助手"
#define DEFAULT_AP_PASSWORD "12345678"
#define DEFAULT_AP_CHANNEL 1
#define DEFAULT_AP_MAX_CONNECTIONS 4

// Captive Portal DNS服务器相关配置
#define DNS_PORT 53
#define DNS_TASK_STACK_SIZE 4096
#define DNS_TASK_PRIORITY 5

// 事件组位
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// WiFi事件组
static EventGroupHandle_t s_wifi_event_group;
// 当前工作模式
static wifi_working_mode_t s_current_mode = WIFI_MANAGER_MODE_AP;
// WiFi事件回调
static wifi_event_callback_t s_event_callback = NULL;
static void *s_event_callback_arg = NULL;
// 重连尝试次数
static int s_retry_num = 0;

// ESP-NETIF实例
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;

// DNS服务器任务句柄
static TaskHandle_t s_dns_server_task_handle = NULL;
static bool s_dns_server_running = false;

// WiFi连接任务的参数结构
typedef struct {
    char ssid[MAX_SSID_LEN];
    char password[MAX_PASSWORD_LEN];
} wifi_connect_params_t;

// WiFi连接任务
static void wifi_connect_task(void *pvParameters)
{
    wifi_connect_params_t *params = (wifi_connect_params_t *)pvParameters;
    
    ESP_LOGI(TAG, "开始连接到保存的WiFi: %s", params->ssid);
    esp_err_t ret = wifi_manager_start_sta(params->ssid, params->password);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "连接到保存的WiFi失败: %s", esp_err_to_name(ret));
    }
    
    // 释放参数内存
    free(params);
    vTaskDelete(NULL);
}

// 处理DNS查询包
static void dns_process_query(uint8_t *packet, size_t len, struct sockaddr_in *from_addr, int sockfd)
{
    // 定义DNS头部结构
    typedef struct {
        uint16_t id;
        uint16_t flags;
        uint16_t qdcount;
        uint16_t ancount;
        uint16_t nscount;
        uint16_t arcount;
    } __attribute__((packed)) dns_header_t;

    // 确保长度至少包含DNS头部
    if (len < sizeof(dns_header_t)) {
        return;
    }

    dns_header_t *header = (dns_header_t *)packet;
    
    // 转换为主机字节序
    uint16_t id = ntohs(header->id);
    uint16_t flags = ntohs(header->flags);
    uint16_t qdcount = ntohs(header->qdcount);

    // 只处理查询包
    if ((flags & 0x8000) == 0 && qdcount > 0) {
        ESP_LOGD(TAG, "收到DNS查询请求");
        
        // 构建响应包
        uint8_t response[512];
        memcpy(response, packet, len);
        
        // 修改响应标志 (应答 + 授权回答)
        header = (dns_header_t *)response;
        flags = 0x8400; // 10000100 00000000 (应答 + 授权回答)
        header->flags = htons(flags);
        header->ancount = htons(1); // 一个回答记录
        
        // 找到查询问题的结尾位置
        size_t qname_len = 0;
        size_t i = sizeof(dns_header_t);
        while (i < len && packet[i] != 0) {
            qname_len += packet[i] + 1;
            i += packet[i] + 1;
        }
        
        // 跳过qname和qtype/qclass (qname + 1字节0结束符 + 2字节qtype + 2字节qclass)
        size_t answer_start = sizeof(dns_header_t) + qname_len + 1 + 4;
        
        // 添加应答记录
        // 设置域名指针 (指向查询问题的域名)
        response[answer_start] = 0xC0; // 指针标记
        response[answer_start + 1] = sizeof(dns_header_t); // 指向问题部分的域名
        
        // 类型: A记录
        response[answer_start + 2] = 0;
        response[answer_start + 3] = 1;
        
        // 类: IN
        response[answer_start + 4] = 0;
        response[answer_start + 5] = 1;
        
        // TTL: 300秒
        response[answer_start + 6] = 0;
        response[answer_start + 7] = 0;
        response[answer_start + 8] = 1;
        response[answer_start + 9] = 44;
        
        // 数据长度: 4字节 (IPv4地址)
        response[answer_start + 10] = 0;
        response[answer_start + 11] = 4;
        
        // IPv4地址 (192.168.4.1)
        response[answer_start + 12] = 192;
        response[answer_start + 13] = 168;
        response[answer_start + 14] = 4;
        response[answer_start + 15] = 1;
        
        // 发送响应
        size_t response_len = answer_start + 16;
        sendto(sockfd, response, response_len, 0, (struct sockaddr *)from_addr, sizeof(*from_addr));
        
        ESP_LOGD(TAG, "已发送DNS响应，将所有域名指向192.168.4.1");
    }
}

// DNS服务器任务
static void dns_server_task(void *pvParameters)
{
    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        ESP_LOGE(TAG, "无法创建DNS服务器socket: %d", errno);
        vTaskDelete(NULL);
        return;
    }
    
    // 绑定socket到DNS端口
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(DNS_PORT);
    
    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS服务器绑定失败: %d", errno);
        close(sockfd);
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "DNS服务器已启动，处理所有DNS查询");
    
    uint8_t rx_buffer[512];
    s_dns_server_running = true;
    
    // 等待并处理DNS查询
    while (s_dns_server_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int len = recvfrom(sockfd, rx_buffer, sizeof(rx_buffer), 0, 
                           (struct sockaddr *)&client_addr, &addr_len);
                           
        if (len > 0) {
            dns_process_query(rx_buffer, len, &client_addr, sockfd);
        }
    }
    
    // 关闭socket并删除任务
    close(sockfd);
    s_dns_server_running = false;
    s_dns_server_task_handle = NULL;
    vTaskDelete(NULL);
}

// 启动DNS服务器
static void start_dns_server(void)
{
    if (s_dns_server_task_handle != NULL) {
        ESP_LOGI(TAG, "DNS服务器已经在运行");
        return;
    }
    
    // 创建DNS服务器任务
    xTaskCreate(dns_server_task, "dns_server", DNS_TASK_STACK_SIZE, NULL, 
                DNS_TASK_PRIORITY, &s_dns_server_task_handle);
                
    if (s_dns_server_task_handle == NULL) {
        ESP_LOGE(TAG, "创建DNS服务器任务失败");
    }
}

// 停止DNS服务器
static void stop_dns_server(void)
{
    if (s_dns_server_running) {
        s_dns_server_running = false;
        // 等待任务自行结束
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// WiFi事件处理函数
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            ESP_LOGI(TAG, "STA模式启动，尝试连接到AP");
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "WiFi断开连接，原因: %d", event->reason);
            
            if (s_retry_num < MAX_CONNECTION_RETRIES) {
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGI(TAG, "重新连接到AP，尝试次数: %d/%d", s_retry_num, MAX_CONNECTION_RETRIES);
            } else {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                
                // 不需要切换到纯AP模式，保持APSTA模式使配网和STA可以同时工作
                ESP_LOGI(TAG, "连接AP失败 (已尝试%d次)，维持APSTA模式等待配网", MAX_CONNECTION_RETRIES);
                
                // 重置重试计数器，允许后续尝试重新连接
                vTaskDelay(pdMS_TO_TICKS(10000)); // 等待10秒后重置
                s_retry_num = 0;
            }
        } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
            ESP_LOGI(TAG, "客户端 %02x:%02x:%02x:%02x:%02x:%02x 连接到AP", 
                     event->mac[0], event->mac[1], event->mac[2], 
                     event->mac[3], event->mac[4], event->mac[5]);
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
            ESP_LOGI(TAG, "客户端 %02x:%02x:%02x:%02x:%02x:%02x 断开连接", 
                     event->mac[0], event->mac[1], event->mac[2], 
                     event->mac[3], event->mac[4], event->mac[5]);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "获取IP地址:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
    
    // 调用用户注册的回调
    if (s_event_callback) {
        s_event_callback(s_event_callback_arg, event_base, event_id, event_data);
    }
}

void wifi_manager_init(void)
{
    ESP_LOGI(TAG, "初始化WiFi管理器 - 统一APSTA模式");

    // 创建WiFi事件组
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL) {
            ESP_LOGE(TAG, "创建WiFi事件组失败");
            return;
        }
    }

    // 创建默认netif实例
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    // 初始化WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi初始化失败: %s", esp_err_to_name(err));
        return;
    }

    // 注册事件处理函数
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    // 尝试加载WiFi凭证
    char ssid[MAX_SSID_LEN] = {0};
    char password[MAX_PASSWORD_LEN] = {0};
    bool has_credentials = false;

    if (wifi_manager_load_credentials(ssid, password) == ESP_OK && strlen(ssid) > 0) {
        has_credentials = true;
        ESP_LOGI(TAG, "找到保存的WiFi凭证，将尝试连接到 %s", ssid);
    } else {
        ESP_LOGI(TAG, "未找到保存的WiFi凭证");
    }

    // 统一使用APSTA模式启动
    ESP_LOGI(TAG, "统一使用APSTA模式启动，AP始终可用");

    // 配置AP
    wifi_config_t ap_config = {
        .ap = {
            .ssid = DEFAULT_AP_SSID,
            .ssid_len = strlen(DEFAULT_AP_SSID),
            .channel = DEFAULT_AP_CHANNEL,
            .password = DEFAULT_AP_PASSWORD,
            .max_connection = DEFAULT_AP_MAX_CONNECTIONS,
            .authmode = strlen(DEFAULT_AP_PASSWORD) > 0 ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN
        },
    };

    // 配置STA（如果有保存的凭证）
    wifi_config_t sta_config = {0};
    if (has_credentials) {
        strlcpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
        strlcpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password));
        ESP_LOGI(TAG, "配置STA连接到: %s", ssid);
    } else {
        ESP_LOGI(TAG, "无保存凭证，STA配置为空");
    }

    // 设置为APSTA模式
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 启动DNS服务器，实现Captive Portal功能
    start_dns_server();

    // 如果有保存的凭证，尝试连接
    if (has_credentials) {
        ESP_LOGI(TAG, "尝试连接到保存的WiFi: %s", ssid);
        esp_wifi_connect();
        s_current_mode = WIFI_MANAGER_MODE_STA; // 标记为STA模式（实际是APSTA）
    } else {
        s_current_mode = WIFI_MANAGER_MODE_AP; // 标记为AP模式（实际是APSTA）
    }

    ESP_LOGI(TAG, "WiFi管理器初始化完成 - APSTA模式运行，AP: %s", DEFAULT_AP_SSID);
}

esp_err_t wifi_manager_start_ap(void)
{
    ESP_LOGI(TAG, "启动APSTA模式（AP功能）");

    wifi_config_t ap_config = {
        .ap = {
            .ssid = DEFAULT_AP_SSID,
            .ssid_len = strlen(DEFAULT_AP_SSID),
            .channel = DEFAULT_AP_CHANNEL,
            .password = DEFAULT_AP_PASSWORD,
            .max_connection = DEFAULT_AP_MAX_CONNECTIONS,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    if (strlen(DEFAULT_AP_PASSWORD) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    // 清空STA配置
    wifi_config_t sta_config = {0};

    // 设置为APSTA模式
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_current_mode = WIFI_MANAGER_MODE_AP;
    ESP_LOGI(TAG, "APSTA模式启动，AP: %s, 密码: %s", DEFAULT_AP_SSID, DEFAULT_AP_PASSWORD);

    // 启动DNS服务器，实现Captive Portal功能
    start_dns_server();

    return ESP_OK;
}

esp_err_t wifi_manager_start_sta(const char *ssid, const char *password)
{
    if (ssid == NULL || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "连接到WiFi: %s (保持APSTA模式)", ssid);

    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));

    if (password != NULL) {
        strlcpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password));
    }

    s_retry_num = 0;

    // 检查WiFi是否已初始化
    esp_err_t err;
    wifi_mode_t mode;

    // 先检查WiFi状态
    err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "获取WiFi模式失败: %s", esp_err_to_name(err));
        if (err == ESP_ERR_WIFI_NOT_INIT) {
            ESP_LOGI(TAG, "WiFi未初始化，重新初始化WiFi");
            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            err = esp_wifi_init(&cfg);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "WiFi初始化失败: %s", esp_err_to_name(err));
                return err;
            }
        } else {
            return err;
        }
    }

    // 确保为APSTA模式
    if (mode != WIFI_MODE_APSTA) {
        ESP_LOGI(TAG, "当前模式不是APSTA，切换到APSTA模式");
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "设置APSTA模式失败: %s", esp_err_to_name(err));
            return err;
        }
    }

    // 设置STA配置
    err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置STA配置失败: %s", esp_err_to_name(err));
        return err;
    }

    // 确保WiFi已启动
    bool wifi_started = false;
    err = esp_wifi_get_mode(&mode);
    if (err == ESP_OK) {
        if (mode != WIFI_MODE_NULL) {
            // 检查WiFi是否已启动
            uint8_t mac[6];
            if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
                wifi_started = true;
                ESP_LOGI(TAG, "WiFi已启动，STA MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            }
        }
    }

    // 如果WiFi尚未启动，则启动它
    if (!wifi_started) {
        ESP_LOGI(TAG, "启动APSTA模式WiFi...");
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "启动WiFi失败: %s", esp_err_to_name(err));
            return err;
        }

        // 启动DNS服务器（如果尚未启动）
        if (s_dns_server_task_handle == NULL) {
            start_dns_server();
        }

        // 添加短暂延迟，确保WiFi完全启动
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // 尝试连接到AP
    ESP_LOGI(TAG, "尝试连接到WiFi: %s", ssid);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi连接失败: %s", esp_err_to_name(err));
        return err;
    }

    s_current_mode = WIFI_MANAGER_MODE_STA; // 标记为STA模式（实际运行APSTA）
    ESP_LOGI(TAG, "APSTA模式运行，尝试连接到: %s", ssid);

    // 等待连接结果（带超时）
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                          WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                          pdFALSE,
                                          pdFALSE,
                                          pdMS_TO_TICKS(15000)); // 增加超时时间

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "成功连接到WiFi: %s (AP仍然可用)", ssid);
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "连接到WiFi: %s 失败 (AP仍然可用)", ssid);
        return ESP_FAIL;
    } else {
        ESP_LOGI(TAG, "连接超时 (AP仍然可用)");
        return ESP_ERR_TIMEOUT;
    }
}

void wifi_manager_register_callback(wifi_event_callback_t callback, void *arg)
{
    s_event_callback = callback;
    s_event_callback_arg = arg;
}

wifi_working_mode_t wifi_manager_get_mode(void)
{
    return s_current_mode;
}

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    err = nvs_set_str(nvs_handle, WIFI_NVS_SSID_KEY, ssid);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }
    
    if (password != NULL) {
        err = nvs_set_str(nvs_handle, WIFI_NVS_PASS_KEY, password);
        if (err != ESP_OK) {
            nvs_close(nvs_handle);
            return err;
        }
    }
    
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    return err;
}

esp_err_t wifi_manager_load_credentials(char *ssid, char *password)
{
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    size_t ssid_len = MAX_SSID_LEN;
    err = nvs_get_str(nvs_handle, WIFI_NVS_SSID_KEY, ssid, &ssid_len);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }
    
    if (password != NULL) {
        size_t pass_len = MAX_PASSWORD_LEN;
        err = nvs_get_str(nvs_handle, WIFI_NVS_PASS_KEY, password, &pass_len);
        // 密码读取失败不算错误，可能没有密码
        if (err != ESP_OK) {
            password[0] = '\0';
        }
    }
    
    nvs_close(nvs_handle);
    return ESP_OK;
}

// 检查并打印WiFi状态
static void wifi_manager_print_status(void)
{
    wifi_mode_t mode;
    esp_err_t mode_err = esp_wifi_get_mode(&mode);
    
    if (mode_err != ESP_OK) {
        ESP_LOGE(TAG, "获取WiFi模式失败: %s", esp_err_to_name(mode_err));
        return;
    }
    
    ESP_LOGI(TAG, "当前WiFi模式: %s", 
             mode == WIFI_MODE_NULL ? "NULL" : 
             mode == WIFI_MODE_STA ? "STA" : 
             mode == WIFI_MODE_AP ? "AP" : 
             mode == WIFI_MODE_APSTA ? "APSTA" : "未知");
    
    // 检查STA模式连接状态
    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
        wifi_ap_record_t ap_info;
        esp_err_t sta_err = esp_wifi_sta_get_ap_info(&ap_info);
        
        if (sta_err == ESP_OK) {
            ESP_LOGI(TAG, "STA已连接到AP: %s, RSSI: %d", ap_info.ssid, ap_info.rssi);
        } else {
            ESP_LOGI(TAG, "STA未连接到AP: %s", esp_err_to_name(sta_err));
        }
    }
    
    // 检查扫描状态 - 这里无法直接检查扫描状态，只能尝试获取扫描结果
    uint16_t ap_count = 0;
    esp_err_t scan_err = esp_wifi_scan_get_ap_num(&ap_count);
    if (scan_err == ESP_OK) {
        ESP_LOGI(TAG, "上次扫描发现的AP数量: %d", ap_count);
    } else if (scan_err == ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGI(TAG, "WiFi未初始化，无法获取扫描状态");
    } else if (scan_err == ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGI(TAG, "WiFi未启动，无法获取扫描状态");
    } else if (scan_err == ESP_ERR_WIFI_NOT_STOPPED) {
        ESP_LOGI(TAG, "WiFi扫描正在进行中");
    } else {
        ESP_LOGI(TAG, "获取扫描状态失败: %s", esp_err_to_name(scan_err));
    }
    
    // 检查电源管理模式
    wifi_ps_type_t ps_type;
    esp_err_t ps_err = esp_wifi_get_ps(&ps_type);
    
    if (ps_err == ESP_OK) {
        ESP_LOGI(TAG, "电源管理模式: %s", 
                 ps_type == WIFI_PS_NONE ? "NONE" : 
                 ps_type == WIFI_PS_MIN_MODEM ? "MIN_MODEM" : 
                 ps_type == WIFI_PS_MAX_MODEM ? "MAX_MODEM" : "未知");
    } else {
        ESP_LOGE(TAG, "获取电源管理模式失败: %s", esp_err_to_name(ps_err));
    }
}

// 比较函数，用于按信号强度排序（从强到弱）
static int compare_rssi(const void *a, const void *b) {
    wifi_ap_record_t *ap_a = (wifi_ap_record_t *)a;
    wifi_ap_record_t *ap_b = (wifi_ap_record_t *)b;
    return ap_b->rssi - ap_a->rssi; // 降序排列
}

// 去重函数，移除相同SSID的重复项，保留信号最强的
static uint16_t remove_duplicates(wifi_ap_record_t *ap_records, uint16_t count) {
    if (count <= 1) return count;

    uint16_t unique_count = 0;
    for (uint16_t i = 0; i < count; i++) {
        bool is_duplicate = false;

        // 检查是否已经存在相同的SSID
        for (uint16_t j = 0; j < unique_count; j++) {
            if (strcmp((char*)ap_records[i].ssid, (char*)ap_records[j].ssid) == 0) {
                is_duplicate = true;
                break;
            }
        }

        // 如果不是重复项，添加到结果中
        if (!is_duplicate) {
            if (unique_count != i) {
                memcpy(&ap_records[unique_count], &ap_records[i], sizeof(wifi_ap_record_t));
            }
            unique_count++;
        }
    }

    return unique_count;
}

// 简化的WiFi扫描函数，不切换模式
static wifi_ap_record_t *simple_wifi_scan(uint16_t *ap_count) {
    if (ap_count == NULL) {
        return NULL;
    }

    *ap_count = 0;
    ESP_LOGI(TAG, "使用简化扫描模式");

    // 停止任何正在进行的扫描
    esp_wifi_scan_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    // 配置简单的扫描参数
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 200,
    };

    // 开始扫描
    esp_err_t scan_result = esp_wifi_scan_start(&scan_config, true);
    if (scan_result != ESP_OK) {
        ESP_LOGE(TAG, "简化扫描失败: %s", esp_err_to_name(scan_result));
        return NULL;
    }

    // 获取扫描结果
    esp_err_t get_num_result = esp_wifi_scan_get_ap_num(ap_count);
    if (get_num_result != ESP_OK || *ap_count == 0) {
        ESP_LOGW(TAG, "简化扫描未找到网络");
        return NULL;
    }

    wifi_ap_record_t *ap_records = (wifi_ap_record_t *)malloc(*ap_count * sizeof(wifi_ap_record_t));
    if (ap_records == NULL) {
        ESP_LOGE(TAG, "简化扫描内存分配失败");
        *ap_count = 0;
        return NULL;
    }

    esp_err_t get_ap_result = esp_wifi_scan_get_ap_records(ap_count, ap_records);
    if (get_ap_result != ESP_OK) {
        ESP_LOGE(TAG, "简化扫描获取记录失败");
        free(ap_records);
        *ap_count = 0;
        return NULL;
    }

    ESP_LOGI(TAG, "简化扫描成功，找到 %d 个网络", *ap_count);
    return ap_records;
}

wifi_ap_record_t *wifi_manager_scan_networks(uint16_t *ap_count)
{
    if (ap_count == NULL) {
        ESP_LOGE(TAG, "ap_count参数为NULL");
        return NULL;
    }

    *ap_count = 0;

    // 打印当前WiFi状态
    wifi_manager_print_status();

    // 确保WiFi已初始化
    wifi_mode_t current_mode;
    esp_err_t mode_err = esp_wifi_get_mode(&current_mode);
    if (mode_err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi未初始化或获取模式失败: %s", esp_err_to_name(mode_err));
        return NULL;
    }

    ESP_LOGI(TAG, "开始WiFi扫描，当前模式: %s (%d)",
             current_mode == WIFI_MODE_NULL ? "NULL" :
             current_mode == WIFI_MODE_STA ? "STA" :
             current_mode == WIFI_MODE_AP ? "AP" :
             current_mode == WIFI_MODE_APSTA ? "APSTA" : "未知", current_mode);

    // 统一APSTA模式下，无需模式切换，直接扫描
    if (current_mode != WIFI_MODE_APSTA && current_mode != WIFI_MODE_STA) {
        ESP_LOGE(TAG, "当前模式不支持扫描: %d", current_mode);
        return NULL;
    }

    ESP_LOGI(TAG, "APSTA模式下直接扫描，无需切换模式");

    // 停止任何正在进行的扫描
    ESP_LOGI(TAG, "停止之前的扫描...");
    esp_wifi_scan_stop();
    vTaskDelay(pdMS_TO_TICKS(300));

    // 配置扫描参数
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,  // 扫描所有信道
        .show_hidden = false,  // 不显示隐藏网络
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,   // 优化扫描时间
        .scan_time.active.max = 300,   // 优化扫描时间
    };

    // 开始扫描（阻塞模式）
    ESP_LOGI(TAG, "开始WiFi扫描（阻塞模式）...");
    esp_err_t scan_result = esp_wifi_scan_start(&scan_config, true);

    if (scan_result != ESP_OK) {
        ESP_LOGE(TAG, "WiFi扫描失败: %s", esp_err_to_name(scan_result));

        // 尝试简化扫描作为备用方案
        ESP_LOGW(TAG, "尝试简化扫描作为备用方案");
        return simple_wifi_scan(ap_count);
    }
    
    ESP_LOGI(TAG, "WiFi扫描完成，获取结果...");

    // 获取扫描到的AP数量
    esp_err_t get_num_result = esp_wifi_scan_get_ap_num(ap_count);
    if (get_num_result != ESP_OK) {
        ESP_LOGE(TAG, "获取AP数量失败: %s", esp_err_to_name(get_num_result));
        *ap_count = 0;
        return NULL;
    }

    ESP_LOGI(TAG, "扫描到 %d 个WiFi网络", *ap_count);

    if (*ap_count == 0) {
        ESP_LOGW(TAG, "未找到任何WiFi网络");
        // 返回空数组而不是NULL，让上层知道扫描成功但没有结果
        wifi_ap_record_t *empty_records = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t));
        if (empty_records != NULL) {
            *ap_count = 0;
            return empty_records;
        }
        return NULL;
    }

    // 分配内存存储扫描结果
    wifi_ap_record_t *ap_records = (wifi_ap_record_t *)malloc(*ap_count * sizeof(wifi_ap_record_t));
    if (ap_records == NULL) {
        ESP_LOGE(TAG, "内存分配失败，需要 %d 字节", *ap_count * sizeof(wifi_ap_record_t));
        *ap_count = 0;
        return NULL;
    }

    // 获取扫描结果
    uint16_t actual_count = *ap_count;
    esp_err_t get_ap_result = esp_wifi_scan_get_ap_records(&actual_count, ap_records);
    if (get_ap_result != ESP_OK) {
        ESP_LOGE(TAG, "获取AP记录失败: %s", esp_err_to_name(get_ap_result));
        free(ap_records);
        *ap_count = 0;
        return NULL;
    }

    *ap_count = actual_count;
    ESP_LOGI(TAG, "成功获取 %d 个AP记录", *ap_count);

    ESP_LOGI(TAG, "原始扫描结果: %d 个网络", *ap_count);

    // 过滤掉信号太弱的网络（RSSI < -90dBm）
    uint16_t filtered_count = 0;
    for (uint16_t i = 0; i < *ap_count; i++) {
        if (ap_records[i].rssi >= -90 && strlen((char*)ap_records[i].ssid) > 0) {
            if (filtered_count != i) {
                memcpy(&ap_records[filtered_count], &ap_records[i], sizeof(wifi_ap_record_t));
            }
            filtered_count++;
        }
    }
    *ap_count = filtered_count;

    if (*ap_count == 0) {
        ESP_LOGW(TAG, "过滤后没有可用的WiFi网络");
        free(ap_records);
        // 返回空数组而不是NULL
        wifi_ap_record_t *empty_records = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t));
        if (empty_records != NULL) {
            *ap_count = 0;
            return empty_records;
        }
        return NULL;
    }

    ESP_LOGI(TAG, "过滤后: %d 个网络", *ap_count);

    // 按信号强度排序（从强到弱）
    qsort(ap_records, *ap_count, sizeof(wifi_ap_record_t), compare_rssi);

    // 去除重复的SSID，保留信号最强的
    *ap_count = remove_duplicates(ap_records, *ap_count);

    ESP_LOGI(TAG, "去重后: %d 个网络", *ap_count);

    // 限制返回的网络数量为前10个
    const uint16_t MAX_NETWORKS = 10;
    if (*ap_count > MAX_NETWORKS) {
        *ap_count = MAX_NETWORKS;
        ESP_LOGI(TAG, "限制为前 %d 个信号最强的网络", MAX_NETWORKS);
    }

    // 重新分配内存以节省空间
    wifi_ap_record_t *final_records = (wifi_ap_record_t *)malloc(*ap_count * sizeof(wifi_ap_record_t));
    if (final_records == NULL) {
        ESP_LOGE(TAG, "重新分配内存失败");
        free(ap_records);
        *ap_count = 0;
        return NULL;
    }

    memcpy(final_records, ap_records, *ap_count * sizeof(wifi_ap_record_t));
    free(ap_records);

    // APSTA模式下无需恢复模式，保持当前状态
    ESP_LOGI(TAG, "WiFi扫描完成，保持APSTA模式");

    // 打印扫描结果
    ESP_LOGI(TAG, "WiFi扫描成功完成，返回 %d 个网络:", *ap_count);
    for (uint16_t i = 0; i < *ap_count; i++) {
        ESP_LOGI(TAG, "  %d. SSID: %s, RSSI: %d dBm, 信道: %d",
                 i + 1, final_records[i].ssid, final_records[i].rssi, final_records[i].primary);
    }

    return final_records;
} 