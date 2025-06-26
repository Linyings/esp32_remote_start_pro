#include "web_server/web_server.h"
#include "wifi_manager/wifi_manager.h"
#include "pc_monitor/pc_monitor.h"
#include "servo_control/servo_control.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include <string.h>
#include <fcntl.h>

static const char *TAG = "web_server";

// Web服务器句柄
static httpd_handle_t s_server = NULL;

// WebSocket客户端列表
#define MAX_WS_CLIENTS 4
static int s_ws_client_fds[MAX_WS_CLIENTS] = {-1, -1, -1, -1};

// 添加WebSocket客户端
static void add_ws_client(int fd)
{
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_client_fds[i] == -1) {
            s_ws_client_fds[i] = fd;
            break;
        }
    }
}

// 删除WebSocket客户端
static void remove_ws_client(int fd)
{
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_client_fds[i] == fd) {
            s_ws_client_fds[i] = -1;
            break;
        }
    }
}

// 广播PC状态到WebSocket客户端
static void broadcast_pc_state(pc_state_t state)
{
    // 构建JSON消息
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event", "pc_state");
    cJSON_AddBoolToObject(root, "is_on", state == PC_STATE_ON);
    
    char *json_str = cJSON_Print(root);
    if (json_str == NULL) {
        ESP_LOGE(TAG, "创建JSON字符串失败");
        cJSON_Delete(root);
        return;
    }
    
    // 发送到所有连接的客户端
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_client_fds[i] != -1) {
            httpd_ws_frame_t ws_pkt;
            memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
            ws_pkt.payload = (uint8_t *)json_str;
            ws_pkt.len = strlen(json_str);
            ws_pkt.type = HTTPD_WS_TYPE_TEXT;
            
            esp_err_t ret = httpd_ws_send_frame_async(s_server, s_ws_client_fds[i], &ws_pkt);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "发送WebSocket消息失败: %d", ret);
            }
        }
    }
    
    free(json_str);
    cJSON_Delete(root);
}

// PC状态变化回调
static void pc_state_changed_cb(pc_state_t new_state)
{
    // 广播新状态给WebSocket客户端
    broadcast_pc_state(new_state);
}

// 初始化SPIFFS文件系统
static esp_err_t init_fs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/web",
        .partition_label = "web_data",
        .max_files = 5,
        .format_if_mount_failed = false
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "无法挂载或格式化文件系统");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "找不到指定的分区");
        } else {
            ESP_LOGE(TAG, "文件系统挂载失败: %s", esp_err_to_name(ret));
        }
        return ret;
    }
    
    size_t total = 0, used = 0;
    ret = esp_spiffs_info("web_data", &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "获取SPIFFS分区信息失败");
        return ret;
    }
    
    ESP_LOGI(TAG, "分区总大小: %d, 已使用: %d", total, used);
    return ESP_OK;
}

// 从SPIFFS读取文件并发送
static esp_err_t send_file(httpd_req_t *req, const char *filepath)
{
    FILE *file = fopen(filepath, "r");
    if (!file) {
        ESP_LOGE(TAG, "无法打开文件: %s", filepath);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    
    // 确定文件类型
    const char *content_type;
    if (strstr(filepath, ".html") != NULL) {
        content_type = "text/html";
    } else if (strstr(filepath, ".css") != NULL) {
        content_type = "text/css";
    } else if (strstr(filepath, ".js") != NULL) {
        content_type = "application/javascript";
    } else if (strstr(filepath, ".ico") != NULL) {
        content_type = "image/x-icon";
    } else {
        content_type = "text/plain";
    }
    
    httpd_resp_set_type(req, content_type);
    
    // 分块读取并发送文件内容
    char buffer[1024];
    size_t read_bytes;
    
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (httpd_resp_send_chunk(req, buffer, read_bytes) != ESP_OK) {
            fclose(file);
            ESP_LOGE(TAG, "发送文件失败: %s", filepath);
            return ESP_FAIL;
        }
    }
    
    // 发送空块表示结束
    httpd_resp_send_chunk(req, NULL, 0);
    fclose(file);
    
    return ESP_OK;
}

// 根URL处理函数（主页）
static esp_err_t root_get_handler(httpd_req_t *req)
{
    // 直接检查请求的主机名和当前WiFi模式
    const char *host = NULL;
    size_t host_len = httpd_req_get_hdr_value_len(req, "Host");
    bool is_ap_access = false;
    
    if (host_len > 0) {
        char *host_buf = malloc(host_len + 1);
        if (host_buf) {
            if (httpd_req_get_hdr_value_str(req, "Host", host_buf, host_len + 1) == ESP_OK) {
                ESP_LOGI(TAG, "请求的Host: %s", host_buf);
                // 检查是否是AP IP地址 (192.168.4.1)
                if (strncmp(host_buf, "192.168.4.1", 11) == 0) {
                    is_ap_access = true;
                }
            }
            free(host_buf);
        }
    }
    
    // 如果无法从Host头判断，则使用WiFi模式
    if (!is_ap_access && wifi_manager_get_mode() == WIFI_MANAGER_MODE_AP) {
        is_ap_access = true;
    }
    
    // 如果是通过AP访问，重定向到配网页面
    if (is_ap_access) {
        ESP_LOGI(TAG, "通过AP访问，重定向到配网页面");
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/setup");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    
    // 通过STA模式（家庭WiFi）访问，返回主控制页面
    ESP_LOGI(TAG, "通过STA访问，显示控制页面");
    return send_file(req, "/web/index.html");
}

// 设置页面处理函数
static esp_err_t setup_get_handler(httpd_req_t *req)
{
    return send_file(req, "/web/setup.html");
}

// 获取PC状态API
static esp_err_t status_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    
    // 获取当前PC状态
    pc_state_t state = pc_monitor_get_state();
    
    // 构建JSON响应
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "is_on", state == PC_STATE_ON);
    
    char *json_str = cJSON_Print(root);
    httpd_resp_sendstr(req, json_str);
    
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// 开机操作API
static esp_err_t power_post_handler(httpd_req_t *req)
{
    // 获取当前PC状态
    pc_state_t state = pc_monitor_get_state();
    
    // 如果已经开机，则返回错误
    if (state == PC_STATE_ON) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"PC已开机\"}");
        return ESP_OK;
    }
    
    // 执行开机动作
    esp_err_t ret = servo_press_power_button();
    
    // 返回JSON响应
    httpd_resp_set_type(req, "application/json");
    
    if (ret == ESP_OK) {
        httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"操作成功\"}");
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"操作失败\"}");
    }
    
    return ESP_OK;
}

// WiFi扫描API
static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    
    // 创建一个临时的JSON对象用于错误响应
    cJSON *error_json = NULL;
    char *error_str = NULL;
    
    // 记录当前WiFi模式
    wifi_mode_t current_mode;
    esp_wifi_get_mode(&current_mode);
    ESP_LOGI(TAG, "扫描开始时的WiFi模式: %d", current_mode);
    
    uint16_t ap_count = 0;
    wifi_ap_record_t *ap_records = wifi_manager_scan_networks(&ap_count);
    
    // 检查扫描结果
    cJSON *root = cJSON_CreateObject();
    
    if (ap_records == NULL) {
        // 主要扫描方法失败，作为备用，尝试直接返回一个空列表
        // 而不是再尝试扫描，以避免潜在的更多问题
        ESP_LOGW(TAG, "WiFi扫描失败，返回空列表");
        
        cJSON_AddBoolToObject(root, "success", true);
        cJSON *networks = cJSON_AddArrayToObject(root, "networks");
        // 添加一条提示消息
        cJSON_AddStringToObject(root, "message", "扫描暂时不可用，请稍后重试");
        
        char *json_str = cJSON_Print(root);
        httpd_resp_sendstr(req, json_str);
        
        free(json_str);
        cJSON_Delete(root);
        return ESP_OK;
    }
    
    // 成功获取扫描结果，正常返回
    cJSON_AddBoolToObject(root, "success", true);
    
    cJSON *networks = cJSON_AddArrayToObject(root, "networks");
    
    // 只有当ap_count > 0时才添加网络
    for (int i = 0; i < ap_count; i++) {
        cJSON *network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "ssid", (char *)ap_records[i].ssid);
        cJSON_AddNumberToObject(network, "rssi", ap_records[i].rssi);
        cJSON_AddNumberToObject(network, "channel", ap_records[i].primary);
        cJSON_AddItemToArray(networks, network);
    }
    
    char *json_str = cJSON_Print(root);
    httpd_resp_sendstr(req, json_str);
    
    free(json_str);
    cJSON_Delete(root);
    free(ap_records);
    
    return ESP_OK;
}

// WiFi连接API
static esp_err_t wifi_connect_handler(httpd_req_t *req)
{
    // 读取请求体
    char content[100];
    int ret, remaining = req->content_len;
    
    if (remaining > sizeof(content) - 1) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"请求太大\"}");
        return ESP_OK;
    }
    
    int received = 0;
    while (remaining > 0) {
        ret = httpd_req_recv(req, content + received, remaining);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "接收失败");
            return ESP_FAIL;
        }
        received += ret;
        remaining -= ret;
    }
    content[received] = '\0';
    
    // 解析JSON
    cJSON *root = cJSON_Parse(content);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"无效的JSON\"}");
        return ESP_OK;
    }
    
    cJSON *ssid_json = cJSON_GetObjectItem(root, "ssid");
    cJSON *password_json = cJSON_GetObjectItem(root, "password");
    
    if (!ssid_json || !cJSON_IsString(ssid_json)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"SSID必须提供\"}");
        return ESP_OK;
    }
    
    const char *ssid = ssid_json->valuestring;
    const char *password = NULL;
    
    if (password_json && cJSON_IsString(password_json)) {
        password = password_json->valuestring;
    }
    
    // 准备响应
    httpd_resp_set_type(req, "application/json");
    
    // 先尝试连接，如果连接成功再保存凭证
    ESP_LOGI(TAG, "尝试连接到WiFi: %s", ssid);
    esp_err_t connect_ret = wifi_manager_start_sta(ssid, password);
    
    if (connect_ret == ESP_OK) {
        // 连接成功后才保存WiFi凭证
        esp_err_t save_ret = wifi_manager_save_credentials(ssid, password);
        if (save_ret != ESP_OK) {
            ESP_LOGW(TAG, "保存WiFi凭证失败: %s", esp_err_to_name(save_ret));
            // 连接成功但保存失败，继续处理
        }
        
        // 获取IP地址
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_get_ip_info(netif, &ip_info);
        
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "success", true);
        char ip_str[16];
        sprintf(ip_str, IPSTR, IP2STR(&ip_info.ip));
        cJSON_AddStringToObject(resp, "ip", ip_str);
        cJSON_AddStringToObject(resp, "message", "连接成功");
        
        char *json_str = cJSON_Print(resp);
        httpd_resp_sendstr(req, json_str);
        
        free(json_str);
        cJSON_Delete(resp);
    } else {
        // 连接失败，提供详细的错误信息
        const char *error_msg;
        if (connect_ret == ESP_FAIL) {
            error_msg = "连接失败，请检查WiFi密码是否正确";
        } else if (connect_ret == ESP_ERR_TIMEOUT) {
            error_msg = "连接超时，请确认WiFi网络可用";
        } else {
            error_msg = "连接出错，请重试";
        }
        
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "success", false);
        cJSON_AddStringToObject(resp, "message", error_msg);
        cJSON_AddNumberToObject(resp, "error_code", connect_ret);
        
        char *json_str = cJSON_Print(resp);
        httpd_resp_sendstr(req, json_str);
        
        free(json_str);
        cJSON_Delete(resp);
    }
    
    cJSON_Delete(root);
    return ESP_OK;
}

// Favicon处理函数
static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    // 如果没有favicon.ico文件，返回204 No Content
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// WebSocket处理函数
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket握手");
        
        // 添加客户端
        add_ws_client(httpd_req_to_sockfd(req));
        
        // 初始发送PC状态
        pc_state_t state = pc_monitor_get_state();
        
        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "event", "pc_state");
        cJSON_AddBoolToObject(root, "is_on", state == PC_STATE_ON);
        
        char *json_str = cJSON_Print(root);
        
        ws_pkt.payload = (uint8_t *)json_str;
        ws_pkt.len = strlen(json_str);
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;
        
        esp_err_t ret = httpd_ws_send_frame(req, &ws_pkt);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "发送WebSocket消息失败: %d", ret);
        }
        
        free(json_str);
        cJSON_Delete(root);
    }
    
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    // 设置最大负载长度
    ws_pkt.len = 128;
    
    // 创建缓冲区
    uint8_t *buf = malloc(ws_pkt.len + 1);
    if (buf == NULL) {
        ESP_LOGE(TAG, "无法分配内存");
        return ESP_ERR_NO_MEM;
    }
    
    ws_pkt.payload = buf;
    
    // 接收数据
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket接收失败: %d", ret);
        free(buf);
        
        // 特殊处理临时错误 - 通常是超时或连接暂时不可用
        if (ret == ESP_FAIL) {
            ESP_LOGI(TAG, "WebSocket接收遇到临时错误，尝试保持连接");
            
            // 发送心跳包来保持连接活跃
            httpd_ws_frame_t ping_pkt;
            memset(&ping_pkt, 0, sizeof(httpd_ws_frame_t));
            ping_pkt.type = HTTPD_WS_TYPE_PING;
            ping_pkt.len = 0;
            ping_pkt.payload = NULL;
            
            esp_err_t ping_ret = httpd_ws_send_frame(req, &ping_pkt);
            if (ping_ret != ESP_OK) {
                ESP_LOGW(TAG, "发送心跳包失败: %d", ping_ret);
            }
            
            return ESP_OK;
        }
        
        // 处理连接关闭
        if (ret == ESP_ERR_HTTPD_INVALID_REQ) {
            remove_ws_client(httpd_req_to_sockfd(req));
            ESP_LOGI(TAG, "WebSocket客户端断开连接");
        }
        
        return ret;
    }
    
    // 确保字符串结束
    if (ws_pkt.len > 0) {
        buf[ws_pkt.len] = 0;
    } else {
        buf[0] = 0;
    }
    
    // 处理接收的消息
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        ESP_LOGI(TAG, "接收WebSocket消息: %s", ws_pkt.payload);
        
        // 检查是否是心跳包
        if (ws_pkt.len > 0 && strstr((char*)ws_pkt.payload, "ping") != NULL) {
            // 回复pong心跳响应
            memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
            char *pong_str = "{\"type\":\"pong\"}";
            ws_pkt.payload = (uint8_t *)pong_str;
            ws_pkt.len = strlen(pong_str);
            ws_pkt.type = HTTPD_WS_TYPE_TEXT;
            
            ret = httpd_ws_send_frame(req, &ws_pkt);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "发送pong响应失败: %d", ret);
            }
        }
        
        // 这里可以处理其他客户端发来的消息
        // ...
    } else if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
        // 自动回复PONG
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.type = HTTPD_WS_TYPE_PONG;
        ws_pkt.len = 0;
        
        ret = httpd_ws_send_frame(req, &ws_pkt);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "发送PONG响应失败: %d", ret);
        }
    }
    
    free(buf);
    return ESP_OK;
}

// Captive Portal请求处理函数（处理各种设备的连接测试URL）
static esp_err_t captive_portal_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Captive Portal请求: %s", req->uri);
    
    // 检查WiFi模式
    wifi_working_mode_t mode = wifi_manager_get_mode();
    bool redirect_to_setup = false;
    
    // 检查Host头部信息
    size_t host_len = httpd_req_get_hdr_value_len(req, "Host");
    if (host_len > 0) {
        char *host_buf = malloc(host_len + 1);
        if (host_buf) {
            if (httpd_req_get_hdr_value_str(req, "Host", host_buf, host_len + 1) == ESP_OK) {
                // 如果访问的不是设备自己的IP，需要重定向
                if (strncmp(host_buf, "192.168.4.1", 11) != 0 && 
                    strncmp(host_buf, "esp32.local", 11) != 0) {
                    redirect_to_setup = true;
                }
            }
            free(host_buf);
        }
    }
    
    // 对于纯AP模式或来自外部域名的请求，重定向到设置页面
    if (mode == WIFI_MANAGER_MODE_AP || redirect_to_setup) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/setup");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    
    // 其他情况返回到主页
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// 获取网络信息API
static esp_err_t network_info_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    
    // 创建JSON响应
    cJSON *root = cJSON_CreateObject();
    
    // 获取IP地址
    esp_netif_ip_info_t ip_info;
    char ip_str[16] = "未知";
    char netmask_str[16] = "255.255.255.0";
    
    // 尝试获取STA模式的IP地址（如果已连接到WiFi）
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        // 检查IP是否有效（不是0.0.0.0）
        if (ip_info.ip.addr != 0) {
            sprintf(ip_str, IPSTR, IP2STR(&ip_info.ip));
            sprintf(netmask_str, IPSTR, IP2STR(&ip_info.netmask));
            cJSON_AddStringToObject(root, "mode", "sta");
        }
    }
    
    // 如果STA模式未连接，尝试AP模式的IP
    if (ip_str[0] == '未') {
        netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            sprintf(ip_str, IPSTR, IP2STR(&ip_info.ip));
            sprintf(netmask_str, IPSTR, IP2STR(&ip_info.netmask));
            cJSON_AddStringToObject(root, "mode", "ap");
        }
    }
    
    // 获取MAC地址
    uint8_t mac[6];
    char mac_str[18] = "";
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        sprintf(mac_str, "%02X:%02X:%02X:%02X:%02X:%02X", 
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    
    // 获取当前WiFi模式
    wifi_working_mode_t current_mode = wifi_manager_get_mode();
    
    // 获取RSSI（如果在STA模式下）
    int8_t rssi = 0;
    wifi_ap_record_t ap_info;
    bool rssi_available = false;
    
    if (current_mode == WIFI_MANAGER_MODE_STA && 
        esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
        rssi_available = true;
    }
    
    // 添加信息到JSON对象
    cJSON_AddStringToObject(root, "ip", ip_str);
    cJSON_AddStringToObject(root, "netmask", netmask_str);
    cJSON_AddStringToObject(root, "mac", mac_str);
    
    if (rssi_available) {
        cJSON_AddNumberToObject(root, "rssi", rssi);
    }
    
    // 发送JSON响应
    char *json_str = cJSON_Print(root);
    httpd_resp_sendstr(req, json_str);
    
    // 释放内存
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// 注册URL处理程序
static void register_handlers(httpd_handle_t server)
{
    httpd_uri_t root = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = root_get_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &root);
    
    httpd_uri_t favicon = {
        .uri       = "/favicon.ico",
        .method    = HTTP_GET,
        .handler   = favicon_get_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &favicon);
    
    httpd_uri_t setup = {
        .uri       = "/setup",
        .method    = HTTP_GET,
        .handler   = setup_get_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &setup);
    
    httpd_uri_t status = {
        .uri       = "/api/status",
        .method    = HTTP_GET,
        .handler   = status_get_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &status);
    
    httpd_uri_t power = {
        .uri       = "/api/power",
        .method    = HTTP_POST,
        .handler   = power_post_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &power);
    
    httpd_uri_t wifi_scan = {
        .uri       = "/api/wifi/scan",
        .method    = HTTP_GET,
        .handler   = wifi_scan_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &wifi_scan);
    
    httpd_uri_t wifi_connect = {
        .uri       = "/api/wifi/connect",
        .method    = HTTP_POST,
        .handler   = wifi_connect_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &wifi_connect);
    
    httpd_uri_t network_info = {
        .uri       = "/api/network/info",
        .method    = HTTP_GET,
        .handler   = network_info_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &network_info);
    
    httpd_uri_t ws = {
        .uri       = "/ws",
        .method    = HTTP_GET,
        .handler   = ws_handler,
        .user_ctx  = NULL,
        .is_websocket = true
    };
    httpd_register_uri_handler(server, &ws);
    
    // 注册Captive Portal检测URL处理函数
    
    // Android/Chrome OS
    httpd_uri_t generate_204 = {
        .uri       = "/generate_204",
        .method    = HTTP_GET,
        .handler   = captive_portal_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &generate_204);
    
    // iOS/macOS
    httpd_uri_t hotspot_detect = {
        .uri       = "/hotspot-detect.html",
        .method    = HTTP_GET,
        .handler   = captive_portal_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &hotspot_detect);
    
    // Windows
    httpd_uri_t ncsi = {
        .uri       = "/ncsi.txt",
        .method    = HTTP_GET,
        .handler   = captive_portal_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &ncsi);
    
    // 通用
    httpd_uri_t connecttest = {
        .uri       = "/connecttest.txt",
        .method    = HTTP_GET,
        .handler   = captive_portal_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &connecttest);
}

esp_err_t web_server_init(void)
{
    // 初始化为NULL，以防止多次初始化
    if (s_server != NULL) {
        ESP_LOGI(TAG, "Web服务器已经在运行");
        return ESP_OK;
    }
    
    // 初始化文件系统
    esp_err_t ret = init_fs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "文件系统初始化失败");
        return ret;
    }
    
    // 注册PC状态变化回调
    pc_monitor_register_callback(pc_state_changed_cb);
    
    // 配置服务器
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 15;
    
    // 增加超时设置，解决WebSocket超时问题
    config.recv_wait_timeout = 30;      // 增加到30秒
    config.send_wait_timeout = 30;      // 增加到30秒
    config.keep_alive_enable = true;    // 确保启用keep-alive
    config.keep_alive_idle = 30;        // 空闲时间30秒
    config.keep_alive_interval = 5;     // 保活间隔5秒
    config.keep_alive_count = 3;        // 尝试3次
    
    // 启动服务器
    ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动Web服务器失败: %d", ret);
        return ret;
    }
    
    // 注册URI处理函数
    register_handlers(s_server);
    
    ESP_LOGI(TAG, "Web服务器启动成功");
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }
    
    esp_err_t ret = httpd_stop(s_server);
    s_server = NULL;
    
    // 重置WebSocket客户端列表
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        s_ws_client_fds[i] = -1;
    }
    
    return ret;
}

httpd_handle_t web_server_get_handle(void)
{
    return s_server;
} 