#include "web_server/web_server.h"
#include "wifi_manager/wifi_manager.h"
#include "pc_monitor/pc_monitor.h"
#include "servo_control/servo_control.h"

// AP模式配置常量（与wifi_manager.c保持一致）
#define DEFAULT_AP_SSID "ESP32开机助手"

// Session管理
#define SESSION_TOKEN_LENGTH 32
#define SESSION_TIMEOUT_SECONDS 3600  // 1小时超时

static char current_session_token[SESSION_TOKEN_LENGTH + 1] = {0};
static time_t session_created_time = 0;
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include <string.h>
#include <fcntl.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>
#include "nvs.h"

// 定义MIN宏
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static const char *TAG = "web_server";

// Session管理函数
static void generate_session_token(char *token, size_t length) {
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (size_t i = 0; i < length - 1; i++) {
        token[i] = charset[esp_random() % (sizeof(charset) - 1)];
    }
    token[length - 1] = '\0';
}

static bool is_session_valid(void) {
    if (strlen(current_session_token) == 0) {
        return false;
    }

    time_t current_time;
    time(&current_time);

    if (current_time - session_created_time > SESSION_TIMEOUT_SECONDS) {
        // Session过期，清除token
        memset(current_session_token, 0, sizeof(current_session_token));
        session_created_time = 0;
        return false;
    }

    return true;
}

static void create_new_session(void) {
    generate_session_token(current_session_token, SESSION_TOKEN_LENGTH + 1);
    time(&session_created_time);
    ESP_LOGI(TAG, "创建新session: %s", current_session_token);
}

static bool validate_session_token(const char *token) {
    if (!token || strlen(token) == 0) {
        return false;
    }

    if (!is_session_valid()) {
        return false;
    }

    return strcmp(token, current_session_token) == 0;
}

// 认证中间件 - 检查请求是否已认证
static bool check_authentication(httpd_req_t *req) {
    // 从Cookie或Header中获取session token
    size_t cookie_len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (cookie_len > 0) {
        char *cookie_buf = malloc(cookie_len + 1);
        if (cookie_buf) {
            if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_buf, cookie_len + 1) == ESP_OK) {
                // 查找session token
                char *token_start = strstr(cookie_buf, "session_token=");
                if (token_start) {
                    token_start += strlen("session_token=");
                    char *token_end = strchr(token_start, ';');
                    if (token_end) {
                        *token_end = '\0';
                    }

                    bool valid = validate_session_token(token_start);
                    free(cookie_buf);
                    return valid;
                }
            }
            free(cookie_buf);
        }
    }

    // 也检查Authorization header
    size_t auth_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (auth_len > 0) {
        char *auth_buf = malloc(auth_len + 1);
        if (auth_buf) {
            if (httpd_req_get_hdr_value_str(req, "Authorization", auth_buf, auth_len + 1) == ESP_OK) {
                // 检查Bearer token格式
                if (strncmp(auth_buf, "Bearer ", 7) == 0) {
                    char *token = auth_buf + 7;
                    bool valid = validate_session_token(token);
                    free(auth_buf);
                    return valid;
                }
            }
            free(auth_buf);
        }
    }

    return false;
}

// Web服务器句柄
static httpd_handle_t s_server = NULL;

// WebSocket客户端列表
#define MAX_WS_CLIENTS 4
static int s_ws_client_fds[MAX_WS_CLIENTS] = {-1, -1, -1, -1};

// 用户认证相关常量
#define AUTH_NVS_NAMESPACE "auth_config"
#define AUTH_NVS_USERNAME_KEY "username"
#define AUTH_NVS_PASSWORD_KEY "password"
#define DEFAULT_USERNAME "admin"
#define DEFAULT_PASSWORD "admin"

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

    // 统计活跃的WebSocket客户端数量
    int active_clients = 0;

    // 发送到所有连接的客户端
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_client_fds[i] != -1) {
            active_clients++;
            httpd_ws_frame_t ws_pkt;
            memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
            ws_pkt.payload = (uint8_t *)json_str;
            ws_pkt.len = strlen(json_str);
            ws_pkt.type = HTTPD_WS_TYPE_TEXT;

            esp_err_t ret = httpd_ws_send_frame_async(s_server, s_ws_client_fds[i], &ws_pkt);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "发送WebSocket消息到客户端 %d 失败: %d", i, ret);
                // 发送失败，可能客户端已断开，清除该客户端
                s_ws_client_fds[i] = -1;
                active_clients--;
            }
        }
    }

    ESP_LOGI(TAG, "状态变化广播完成，发送到 %d 个WebSocket客户端", active_clients);

    free(json_str);
    cJSON_Delete(root);
}

// PC状态变化回调（只在状态真正变化时被调用）
static void pc_state_changed_cb(pc_state_t new_state)
{
    ESP_LOGI(TAG, "PC状态发生变化，主动推送到WebSocket客户端: %s",
             new_state == PC_STATE_ON ? "开机" : "关机");

    // 广播新状态给WebSocket客户端
    broadcast_pc_state(new_state);
}

// 初始化SPIFFS文件系统
static esp_err_t init_fs(void)
{
    ESP_LOGI(TAG, "开始初始化SPIFFS文件系统...");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/web",
        .partition_label = "web_data",
        .max_files = 5,
        .format_if_mount_failed = true  // 改为true，如果挂载失败则格式化
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "无法挂载或格式化文件系统");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "找不到指定的分区 'web_data'");
        } else {
            ESP_LOGE(TAG, "文件系统挂载失败: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info("web_data", &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "获取SPIFFS分区信息失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SPIFFS初始化成功 - 分区总大小: %d bytes, 已使用: %d bytes", total, used);

    // 检查关键文件是否存在
    FILE *test_file = fopen("/web/setup.html", "r");
    if (test_file) {
        ESP_LOGI(TAG, "setup.html 文件存在");
        fclose(test_file);
    } else {
        ESP_LOGW(TAG, "setup.html 文件不存在，可能需要烧录SPIFFS分区");
    }

    test_file = fopen("/web/index.html", "r");
    if (test_file) {
        ESP_LOGI(TAG, "index.html 文件存在");
        fclose(test_file);
    } else {
        ESP_LOGW(TAG, "index.html 文件不存在，可能需要烧录SPIFFS分区");
    }

    return ESP_OK;
}

// 备用的完整配网页面HTML（包含WiFi扫描功能）
static const char* backup_setup_html =
"<!DOCTYPE html>"
"<html><head><meta charset='UTF-8'><title>ESP32配网</title>"
"<style>"
"body{font-family:Arial;margin:20px;background:#f5f5f5}"
".container{max-width:500px;margin:0 auto;background:white;padding:30px;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,0.1)}"
"h1{color:#333;text-align:center;margin-bottom:30px}"
"h2{color:#495057;margin-bottom:15px;font-size:18px}"
"input,button{width:100%;padding:12px;margin:8px 0;border:1px solid #ddd;border-radius:5px;box-sizing:border-box}"
"button{background:#007bff;color:white;border:none;cursor:pointer;font-size:14px}"
"button:hover{background:#0056b3}"
"button:disabled{background:#6c757d;cursor:not-allowed}"
".status{margin:10px 0;padding:10px;border-radius:5px;display:none}"
".success{background:#d4edda;color:#155724;border:1px solid #c3e6cb}"
".error{background:#f8d7da;color:#721c24;border:1px solid #f5c6cb}"
".wifi-list{border:1px solid #ddd;border-radius:5px;margin:10px 0;max-height:300px;overflow-y:auto}"
".wifi-item{padding:12px;border-bottom:1px solid #eee;cursor:pointer;display:flex;justify-content:space-between;align-items:center}"
".wifi-item:hover{background:#f8f9fa}"
".wifi-item:last-child{border-bottom:none}"
".wifi-item.selected{background:#e3f2fd;border-left:4px solid #2196f3}"
".wifi-name{font-weight:500}"
".wifi-signal{font-size:12px;color:#6c757d}"
".loading{text-align:center;padding:20px;color:#6c757d}"
".refresh-btn{width:auto;margin:10px 0;padding:8px 16px}"
"</style></head>"
"<body><div class='container'>"
"<h1>ESP32远程开机助手</h1>"
"<h2>可用WiFi网络</h2>"
"<button id='refreshBtn' class='refresh-btn'>🔄 扫描WiFi网络</button>"
"<div id='networksList' class='wifi-list'><div class='loading'>点击上方按钮扫描WiFi网络</div></div>"
"<h2>WiFi配置</h2>"
"<form id='wifiForm'>"
"<input type='text' id='ssid' placeholder='WiFi名称' required>"
"<input type='password' id='password' placeholder='WiFi密码'>"
"<button type='submit'>连接</button>"
"</form>"
"<div id='status' class='status'></div>"
"</div>"
"<script>"
"const networksList=document.getElementById('networksList');"
"const ssidInput=document.getElementById('ssid');"
"const passwordInput=document.getElementById('password');"
"const status=document.getElementById('status');"
"const refreshBtn=document.getElementById('refreshBtn');"
"function showStatus(msg,isError){"
"status.textContent=msg;status.className=isError?'status error':'status success';"
"status.style.display='block';}"
"async function scanWiFi(){"
"refreshBtn.disabled=true;refreshBtn.textContent='扫描中...';"
"networksList.innerHTML='<div class=\"loading\">正在扫描WiFi网络...</div>';"
"try{"
"console.log('开始WiFi扫描...');"
"const response=await fetch('/api/wifi/scan');"
"console.log('扫描响应状态:',response.status);"
"if(!response.ok)throw new Error('HTTP '+response.status);"
"const data=await response.json();"
"console.log('扫描数据:',data);"
"if(data.success&&data.networks&&data.networks.length>0){"
"networksList.innerHTML='';"
"data.networks.forEach(network=>{"
"const item=document.createElement('div');"
"item.className='wifi-item';"
"const signalIcon=network.is_open?'🔓':'🔒';"
"const signalPercent=network.signal_percent||Math.max(0,Math.min(100,(network.rssi+100)*2));"
"item.innerHTML='<div><div class=\"wifi-name\">'+signalIcon+' '+network.ssid+'</div><div class=\"wifi-signal\">信号:'+network.rssi+'dBm</div></div><div class=\"wifi-signal\">'+signalPercent+'%</div>';"
"item.onclick=()=>{"
"document.querySelectorAll('.wifi-item').forEach(el=>el.classList.remove('selected'));"
"item.classList.add('selected');"
"ssidInput.value=network.ssid;"
"if(network.is_open){passwordInput.value='';passwordInput.style.display='none';}else{passwordInput.style.display='block';passwordInput.focus();}"
"};"
"networksList.appendChild(item);});"
"}else{"
"networksList.innerHTML='<div class=\"loading\">未找到WiFi网络，请重试</div>';}"
"}catch(error){"
"console.error('扫描错误:',error);"
"networksList.innerHTML='<div class=\"loading\">扫描失败: '+error.message+'</div>';}"
"refreshBtn.disabled=false;refreshBtn.textContent='🔄 扫描WiFi网络';}"
"refreshBtn.onclick=scanWiFi;"
"document.getElementById('wifiForm').addEventListener('submit',function(e){"
"e.preventDefault();"
"const ssid=ssidInput.value;const password=passwordInput.value;"
"if(!ssid){showStatus('请选择或输入WiFi名称',true);return;}"
"showStatus('正在连接...',false);"
"fetch('/api/wifi/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ssid,password:password})})"
".then(response=>response.json()).then(data=>{"
"if(data.success){showStatus('连接成功！设备将重启并连接到WiFi网络。',false);}else{showStatus('连接失败：'+(data.message||'未知错误'),true);}})"
".catch(error=>{showStatus('请求失败，请重试',true);});});"
"</script></body></html>";

// 从SPIFFS读取文件并发送
static esp_err_t send_file(httpd_req_t *req, const char *filepath)
{
    ESP_LOGI(TAG, "尝试发送文件: %s", filepath);

    FILE *file = fopen(filepath, "r");
    if (!file) {
        ESP_LOGE(TAG, "无法打开文件: %s, errno: %d", filepath, errno);

        // 如果是setup.html文件不存在，发送备用HTML
        if (strstr(filepath, "setup.html") != NULL) {
            ESP_LOGW(TAG, "setup.html不存在，使用备用页面");
            httpd_resp_set_type(req, "text/html");
            httpd_resp_sendstr(req, backup_setup_html);
            return ESP_OK;
        }

        // 如果是index.html文件不存在，也发送备用HTML
        if (strstr(filepath, "index.html") != NULL) {
            ESP_LOGW(TAG, "index.html不存在，使用备用页面");
            httpd_resp_set_type(req, "text/html");
            httpd_resp_sendstr(req, backup_setup_html);
            return ESP_OK;
        }

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
    ESP_LOGI(TAG, "收到根路径请求: %s", req->uri);

    // 获取当前WiFi模式
    wifi_working_mode_t mode = wifi_manager_get_mode();

    // 检查请求的主机名和客户端IP，确保准确识别AP访问
    size_t host_len = httpd_req_get_hdr_value_len(req, "Host");
    bool is_ap_access = false;

    // 方法1：检查Host头
    if (host_len > 0) {
        char *host_buf = malloc(host_len + 1);
        if (host_buf) {
            if (httpd_req_get_hdr_value_str(req, "Host", host_buf, host_len + 1) == ESP_OK) {
                ESP_LOGI(TAG, "请求的Host: %s", host_buf);
                // 检查是否是AP IP地址 (192.168.4.1)
                if (strncmp(host_buf, "192.168.4.1", 11) == 0) {
                    is_ap_access = true;
                    ESP_LOGI(TAG, "通过Host头检测到AP访问");
                }
            }
            free(host_buf);
        }
    }

    // 方法2：检查是否有AP接口活跃并分析Host头（更可靠的方法）
    if (!is_ap_access) {
        // 检查AP接口是否启用
        wifi_mode_t wifi_mode;
        if (esp_wifi_get_mode(&wifi_mode) == ESP_OK) {
            if (wifi_mode == WIFI_MODE_AP || wifi_mode == WIFI_MODE_APSTA) {
                ESP_LOGI(TAG, "AP接口已启用，WiFi模式: %d", wifi_mode);

                // 分析Host头来判断是否为AP访问
                if (host_len > 0) {
                    char *host_buf = malloc(host_len + 1);
                    if (host_buf) {
                        if (httpd_req_get_hdr_value_str(req, "Host", host_buf, host_len + 1) == ESP_OK) {
                            ESP_LOGI(TAG, "分析Host头: %s", host_buf);

                            // 明确检查是否为AP网段 (192.168.4.x)
                            if (strncmp(host_buf, "192.168.4.", 10) == 0) {
                                is_ap_access = true;
                                ESP_LOGI(TAG, "通过Host头确认AP访问: %s", host_buf);
                            }
                            // 如果Host头包含端口号，也要检查
                            else if (strncmp(host_buf, "192.168.4.1:", 12) == 0) {
                                is_ap_access = true;
                                ESP_LOGI(TAG, "通过Host头(含端口)确认AP访问: %s", host_buf);
                            }
                            // 在APSTA模式下，如果Host不是明确的STA IP，可能是AP访问
                            else if (wifi_mode == WIFI_MODE_APSTA) {
                                // 获取STA的IP地址进行比较
                                esp_netif_ip_info_t sta_ip_info;
                                esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                                if (sta_netif && esp_netif_get_ip_info(sta_netif, &sta_ip_info) == ESP_OK) {
                                    char sta_ip_str[16];
                                    sprintf(sta_ip_str, IPSTR, IP2STR(&sta_ip_info.ip));
                                    ESP_LOGI(TAG, "STA IP: %s", sta_ip_str);

                                    // 如果Host不是STA的IP，则可能是AP访问
                                    if (strncmp(host_buf, sta_ip_str, strlen(sta_ip_str)) != 0) {
                                        is_ap_access = true;
                                        ESP_LOGI(TAG, "APSTA模式下，Host不是STA IP，判断为AP访问: %s", host_buf);
                                    }
                                } else {
                                    // 无法获取STA IP，保守判断为AP访问
                                    is_ap_access = true;
                                    ESP_LOGI(TAG, "APSTA模式下，无法获取STA IP，保守判断为AP访问: %s", host_buf);
                                }
                            }
                        }
                        free(host_buf);
                    }
                }
            }
        }
    }

    // 方法3：如果是纯AP模式，肯定是AP访问
    if (!is_ap_access && mode == WIFI_MANAGER_MODE_AP) {
        is_ap_access = true;
        ESP_LOGI(TAG, "纯AP模式，确认为AP访问");
    }



    // 检查STA是否已连接到WiFi（统一APSTA模式下的判断）
    bool sta_connected = false;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        sta_connected = true;
        ESP_LOGI(TAG, "STA已连接到: %s", ap_info.ssid);
    }

    // 详细的访问状态日志
    wifi_mode_t current_wifi_mode;
    esp_wifi_get_mode(&current_wifi_mode);
    ESP_LOGI(TAG, "访问状态总结 - AP访问: %s, WiFi管理器模式: %d, 实际WiFi模式: %d, STA连接: %s",
             is_ap_access ? "是" : "否", mode, current_wifi_mode, sta_connected ? "是" : "否");

    if (is_ap_access) {
        // 通过AP访问，无论设备是否连接WiFi都强制要求登录认证
        ESP_LOGI(TAG, "检测到AP访问，强制要求登录认证 (STA连接状态: %s)",
                 sta_connected ? "已连接" : "未连接");

        // 检查是否已认证
        if (!check_authentication(req)) {
            ESP_LOGI(TAG, "AP访问未认证，重定向到登录页面");
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "/login");
            httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }

        // AP访问已认证，重定向到配网页面
        ESP_LOGI(TAG, "AP访问已认证，重定向到配网页面 (STA连接状态: %s)",
                 sta_connected ? "已连接" : "未连接");
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/setup");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // 如果处于纯AP模式或STA未连接，重定向到配网页面
    if (mode == WIFI_MANAGER_MODE_AP || !sta_connected) {
        ESP_LOGI(TAG, "需要配网，重定向到配网页面 (模式: %d, STA连接: %s)",
                 mode, sta_connected ? "是" : "否");
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/setup");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // 通过STA模式（家庭WiFi）访问，需要先检查认证
    ESP_LOGI(TAG, "STA模式访问，检查认证状态");

    // 检查是否已认证
    if (!check_authentication(req)) {
        ESP_LOGI(TAG, "STA访问未认证，重定向到登录页面");
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/login");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // STA访问已认证，显示控制页面
    ESP_LOGI(TAG, "STA访问已认证，显示控制页面");
    return send_file(req, "/web/index.html");
}

// 设置页面处理函数
static esp_err_t setup_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "收到配网页面请求");

    // 检查是否通过AP访问（使用与根路径相同的检测逻辑）
    size_t host_len = httpd_req_get_hdr_value_len(req, "Host");
    bool is_ap_access = false;

    // 方法1：检查Host头
    if (host_len > 0) {
        char *host_buf = malloc(host_len + 1);
        if (host_buf) {
            if (httpd_req_get_hdr_value_str(req, "Host", host_buf, host_len + 1) == ESP_OK) {
                ESP_LOGI(TAG, "配网页面请求的Host: %s", host_buf);
                // 检查是否是AP IP地址 (192.168.4.x)
                if (strncmp(host_buf, "192.168.4.", 10) == 0) {
                    is_ap_access = true;
                    ESP_LOGI(TAG, "配网页面通过Host头检测到AP访问");
                }
            }
            free(host_buf);
        }
    }

    // 方法2：在APSTA模式下进一步检查
    if (!is_ap_access) {
        wifi_mode_t wifi_mode;
        if (esp_wifi_get_mode(&wifi_mode) == ESP_OK && wifi_mode == WIFI_MODE_APSTA) {
            if (host_len > 0) {
                char *host_buf = malloc(host_len + 1);
                if (host_buf) {
                    if (httpd_req_get_hdr_value_str(req, "Host", host_buf, host_len + 1) == ESP_OK) {
                        // 获取STA的IP地址进行比较
                        esp_netif_ip_info_t sta_ip_info;
                        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                        if (sta_netif && esp_netif_get_ip_info(sta_netif, &sta_ip_info) == ESP_OK) {
                            char sta_ip_str[16];
                            sprintf(sta_ip_str, IPSTR, IP2STR(&sta_ip_info.ip));

                            // 如果Host不是STA的IP，则可能是AP访问
                            if (strncmp(host_buf, sta_ip_str, strlen(sta_ip_str)) != 0) {
                                is_ap_access = true;
                                ESP_LOGI(TAG, "配网页面APSTA模式下，Host不是STA IP，判断为AP访问: %s", host_buf);
                            }
                        }
                    }
                    free(host_buf);
                }
            }
        }
    }

    // 如果是AP访问，需要检查认证
    if (is_ap_access) {
        ESP_LOGI(TAG, "AP访问配网页面，检查认证状态");
        if (!check_authentication(req)) {
            ESP_LOGI(TAG, "AP访问配网页面未认证，重定向到登录页面");
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "/login");
            httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
        ESP_LOGI(TAG, "AP访问配网页面已认证，显示配网页面");
    } else {
        ESP_LOGI(TAG, "STA访问配网页面，直接显示");
    }

    // 设置响应头，防止缓存
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");

    return send_file(req, "/web/setup.html");
}

// 登录页面处理函数
static esp_err_t login_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "收到登录页面请求");

    // 设置响应头，防止缓存
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");

    return send_file(req, "/web/login.html");
}

// 获取PC状态API
static esp_err_t status_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    // 检查认证
    if (!check_authentication(req)) {
        ESP_LOGW(TAG, "未认证的状态查询请求");
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"未认证，请先登录\"}");
        return ESP_OK;
    }

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
    ESP_LOGI(TAG, "收到PC开机请求");

    // 检查认证
    if (!check_authentication(req)) {
        ESP_LOGW(TAG, "未认证的PC开机请求");
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"未认证，请先登录\"}");
        return ESP_OK;
    }

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

// 获取信号强度描述
static const char* get_signal_strength_desc(int8_t rssi) {
    if (rssi >= -50) return "优秀";
    else if (rssi >= -60) return "良好";
    else if (rssi >= -70) return "一般";
    else if (rssi >= -80) return "较弱";
    else return "很弱";
}

// 获取加密类型描述
static const char* get_auth_mode_desc(wifi_auth_mode_t auth_mode) {
    switch (auth_mode) {
        case WIFI_AUTH_OPEN: return "开放";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2企业版";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        default: return "未知";
    }
}

// WiFi扫描API - 优化版
static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    // 设置响应头
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close"); // 确保连接关闭

    ESP_LOGI(TAG, "收到WiFi扫描请求");

    // 记录内存使用情况
    size_t free_heap = esp_get_free_heap_size();
    size_t min_free_heap = esp_get_minimum_free_heap_size();
    ESP_LOGI(TAG, "内存状态 - 可用: %d 字节, 最小可用: %d 字节", free_heap, min_free_heap);

    // 记录当前WiFi模式
    wifi_mode_t current_mode;
    esp_wifi_get_mode(&current_mode);
    ESP_LOGI(TAG, "当前WiFi模式: %d", current_mode);

    // 创建响应JSON对象
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "创建JSON对象失败");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "开始执行WiFi扫描...");

    // 执行WiFi扫描
    uint16_t ap_count = 0;
    wifi_ap_record_t *ap_records = wifi_manager_scan_networks(&ap_count);
    bool need_free_records = false; // 标记是否需要释放ap_records

    ESP_LOGI(TAG, "WiFi扫描完成，结果: ap_count=%d, ap_records=%p", ap_count, ap_records);

    if (ap_records == NULL) {
        ESP_LOGE(TAG, "WiFi扫描失败，返回NULL");

        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddArrayToObject(root, "networks");
        cJSON_AddStringToObject(root, "message", "WiFi扫描失败，请稍后重试");
        cJSON_AddNumberToObject(root, "count", 0);
    } else if (ap_count == 0) {
        ESP_LOGW(TAG, "WiFi扫描成功但未找到网络");

        cJSON_AddBoolToObject(root, "success", true);
        cJSON_AddArrayToObject(root, "networks");
        cJSON_AddStringToObject(root, "message", "未发现WiFi网络，请检查周围是否有WiFi信号");
        cJSON_AddNumberToObject(root, "count", 0);

        need_free_records = true; // 标记需要释放
    } else {
        ESP_LOGI(TAG, "WiFi扫描成功，找到 %d 个网络", ap_count);

        cJSON_AddBoolToObject(root, "success", true);
        cJSON_AddNumberToObject(root, "count", ap_count);
        cJSON_AddStringToObject(root, "message", "扫描完成");

        cJSON *networks = cJSON_AddArrayToObject(root, "networks");
        if (networks == NULL) {
            ESP_LOGE(TAG, "创建networks数组失败");
            cJSON_Delete(root);
            if (ap_records != NULL) {
                free(ap_records);
            }
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        // 添加网络信息
        for (int i = 0; i < ap_count; i++) {
            cJSON *network = cJSON_CreateObject();
            if (network == NULL) {
                ESP_LOGE(TAG, "创建network对象失败");
                continue;
            }

            // 基本信息
            cJSON_AddStringToObject(network, "ssid", (char *)ap_records[i].ssid);
            cJSON_AddNumberToObject(network, "rssi", ap_records[i].rssi);
            cJSON_AddNumberToObject(network, "channel", ap_records[i].primary);

            // 附加信息
            cJSON_AddStringToObject(network, "signal_strength", get_signal_strength_desc(ap_records[i].rssi));
            cJSON_AddStringToObject(network, "auth_mode", get_auth_mode_desc(ap_records[i].authmode));
            cJSON_AddNumberToObject(network, "authmode", ap_records[i].authmode);
            cJSON_AddBoolToObject(network, "is_open", ap_records[i].authmode == WIFI_AUTH_OPEN);

            // 计算信号强度百分比 (0-100%)
            int signal_percent = 0;
            if (ap_records[i].rssi >= -50) signal_percent = 100;
            else if (ap_records[i].rssi >= -60) signal_percent = 80;
            else if (ap_records[i].rssi >= -70) signal_percent = 60;
            else if (ap_records[i].rssi >= -80) signal_percent = 40;
            else signal_percent = 20;
            cJSON_AddNumberToObject(network, "signal_percent", signal_percent);

            cJSON_AddItemToArray(networks, network);
        }

        need_free_records = true; // 标记需要释放
    }

    // 发送响应
    char *json_str = cJSON_Print(root);
    if (json_str == NULL) {
        ESP_LOGE(TAG, "创建JSON字符串失败");
        cJSON_Delete(root);
        if (need_free_records && ap_records != NULL) {
            free(ap_records);
        }
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    esp_err_t ret = httpd_resp_sendstr(req, json_str);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "发送HTTP响应失败: %s", esp_err_to_name(ret));
    }

    // 清理资源
    free(json_str);
    cJSON_Delete(root);

    // 统一释放ap_records内存
    if (need_free_records && ap_records != NULL) {
        free(ap_records);
        ESP_LOGI(TAG, "已释放WiFi扫描结果内存");
    }

    // 记录最终内存状态
    size_t final_free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG, "WiFi扫描处理完成 - 最终可用内存: %d 字节", final_free_heap);

    return ret;
}

// 网络信息API - 获取设备IP地址等网络信息
static esp_err_t network_info_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    ESP_LOGI(TAG, "收到网络信息请求");

    // 记录内存使用情况
    size_t free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG, "网络信息请求 - 可用内存: %d 字节", free_heap);

    // 创建响应JSON对象
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "创建JSON对象失败");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // 获取WiFi模式
    wifi_mode_t mode;
    esp_err_t mode_err = esp_wifi_get_mode(&mode);

    // 获取STA接口信息
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    char ip_str[16] = "0.0.0.0";
    char netmask_str[16] = "0.0.0.0";
    char gateway_str[16] = "0.0.0.0";
    bool sta_connected = false;

    if (sta_netif != NULL) {
        esp_err_t ip_err = esp_netif_get_ip_info(sta_netif, &ip_info);
        if (ip_err == ESP_OK && ip_info.ip.addr != 0) {
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
            esp_ip4addr_ntoa(&ip_info.netmask, netmask_str, sizeof(netmask_str));
            esp_ip4addr_ntoa(&ip_info.gw, gateway_str, sizeof(gateway_str));
            sta_connected = true;
            ESP_LOGI(TAG, "STA IP信息 - IP: %s, 网关: %s", ip_str, gateway_str);
        }
    }

    // 获取AP接口信息
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ap_ip_info;
    char ap_ip_str[16] = "192.168.4.1";

    if (ap_netif != NULL) {
        esp_err_t ap_ip_err = esp_netif_get_ip_info(ap_netif, &ap_ip_info);
        if (ap_ip_err == ESP_OK && ap_ip_info.ip.addr != 0) {
            esp_ip4addr_ntoa(&ap_ip_info.ip, ap_ip_str, sizeof(ap_ip_str));
        }
    }

    // 获取连接的WiFi信息
    wifi_ap_record_t ap_info;
    char connected_ssid[33] = "";
    int8_t rssi = 0;

    if (sta_connected && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        strncpy(connected_ssid, (char*)ap_info.ssid, sizeof(connected_ssid) - 1);
        connected_ssid[sizeof(connected_ssid) - 1] = '\0';
        rssi = ap_info.rssi;
        ESP_LOGI(TAG, "已连接WiFi: %s, 信号强度: %d dBm", connected_ssid, rssi);
    }

    // 构建响应数据
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "message", "网络信息获取成功");

    // STA信息
    cJSON *sta_info = cJSON_AddObjectToObject(root, "sta");
    cJSON_AddBoolToObject(sta_info, "connected", sta_connected);
    cJSON_AddStringToObject(sta_info, "ip", ip_str);
    cJSON_AddStringToObject(sta_info, "netmask", netmask_str);
    cJSON_AddStringToObject(sta_info, "gateway", gateway_str);
    cJSON_AddStringToObject(sta_info, "ssid", connected_ssid);
    cJSON_AddNumberToObject(sta_info, "rssi", rssi);

    // AP信息
    cJSON *ap_info_obj = cJSON_AddObjectToObject(root, "ap");
    cJSON_AddStringToObject(ap_info_obj, "ip", ap_ip_str);
    cJSON_AddStringToObject(ap_info_obj, "ssid", DEFAULT_AP_SSID);

    // 主要IP地址（优先STA，其次AP）
    cJSON_AddStringToObject(root, "ip", sta_connected ? ip_str : ap_ip_str);
    cJSON_AddStringToObject(root, "primary_interface", sta_connected ? "sta" : "ap");

    // WiFi模式信息
    const char* mode_str = "unknown";
    if (mode_err == ESP_OK) {
        switch (mode) {
            case WIFI_MODE_NULL: mode_str = "null"; break;
            case WIFI_MODE_STA: mode_str = "sta"; break;
            case WIFI_MODE_AP: mode_str = "ap"; break;
            case WIFI_MODE_APSTA: mode_str = "apsta"; break;
            default: mode_str = "unknown"; break;
        }
    }
    cJSON_AddStringToObject(root, "wifi_mode", mode_str);

    // 发送响应
    char *json_str = cJSON_Print(root);
    if (json_str == NULL) {
        ESP_LOGE(TAG, "创建JSON字符串失败");
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    esp_err_t ret = httpd_resp_sendstr(req, json_str);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "发送HTTP响应失败: %s", esp_err_to_name(ret));
    }

    // 清理资源
    free(json_str);
    cJSON_Delete(root);

    // 记录最终内存状态
    size_t final_free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG, "网络信息处理完成 - 最终可用内存: %d 字节", final_free_heap);

    return ret;
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

    // 获取User-Agent信息用于调试
    size_t ua_len = httpd_req_get_hdr_value_len(req, "User-Agent");
    if (ua_len > 0 && ua_len < 200) {
        char *ua_buf = malloc(ua_len + 1);
        if (ua_buf) {
            if (httpd_req_get_hdr_value_str(req, "User-Agent", ua_buf, ua_len + 1) == ESP_OK) {
                ESP_LOGI(TAG, "User-Agent: %s", ua_buf);
            }
            free(ua_buf);
        }
    }

    // 检查WiFi模式
    wifi_working_mode_t mode = wifi_manager_get_mode();

    // 检查Host头部信息
    size_t host_len = httpd_req_get_hdr_value_len(req, "Host");
    if (host_len > 0) {
        char *host_buf = malloc(host_len + 1);
        if (host_buf) {
            if (httpd_req_get_hdr_value_str(req, "Host", host_buf, host_len + 1) == ESP_OK) {
                ESP_LOGI(TAG, "Host头: %s", host_buf);
            }
            free(host_buf);
        }
    }
    
    // 根据不同的检测URL和模式返回适当的响应
    // 注意：现在统一使用APSTA模式，通过STA连接状态判断是否需要配网

    // 检查STA是否已连接到WiFi
    bool sta_connected = false;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        sta_connected = true;
        ESP_LOGI(TAG, "STA已连接到: %s", ap_info.ssid);
    }

    if (mode == WIFI_MANAGER_MODE_AP || !sta_connected) {
        // AP模式或STA未连接 - 需要触发Captive Portal

        if (strcmp(req->uri, "/generate_204") == 0) {
            // Android/Chrome OS检测 - 返回非204状态码触发Captive Portal
            ESP_LOGI(TAG, "Android/Chrome OS检测，重定向到配网页面");
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/setup");
            httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
        else if (strcmp(req->uri, "/hotspot-detect.html") == 0) {
            // iOS/macOS检测 - 返回非Success内容触发Captive Portal
            ESP_LOGI(TAG, "iOS/macOS检测，重定向到配网页面");
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/setup");
            httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
        else if (strcmp(req->uri, "/ncsi.txt") == 0 || strcmp(req->uri, "/connecttest.txt") == 0) {
            // Windows检测 - 返回非标准内容触发Captive Portal
            ESP_LOGI(TAG, "Windows检测，重定向到配网页面");
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/setup");
            httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
        else {
            // 其他请求，重定向到配网页面
            ESP_LOGI(TAG, "其他Captive Portal请求，重定向到配网页面");
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/setup");
            httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
    }
    else {
        // STA已连接模式下，返回正常的连接检测响应，表示网络连接正常
        ESP_LOGI(TAG, "STA已连接，返回网络连接正常响应");

        if (strcmp(req->uri, "/generate_204") == 0) {
            // Android/Chrome OS - 返回204表示网络正常
            httpd_resp_set_status(req, "204 No Content");
            httpd_resp_send(req, NULL, 0);
        }
        else if (strcmp(req->uri, "/hotspot-detect.html") == 0) {
            // iOS/macOS - 返回Success页面表示网络正常
            httpd_resp_set_status(req, "200 OK");
            httpd_resp_set_hdr(req, "Content-Type", "text/html");
            const char* success_response =
                "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
            httpd_resp_send(req, success_response, strlen(success_response));
        }
        else if (strcmp(req->uri, "/ncsi.txt") == 0 || strcmp(req->uri, "/connecttest.txt") == 0) {
            // Windows - 返回标准NCSI响应
            httpd_resp_set_status(req, "200 OK");
            httpd_resp_set_hdr(req, "Content-Type", "text/plain");
            const char* windows_response = "Microsoft NCSI";
            httpd_resp_send(req, windows_response, strlen(windows_response));
        }
        else {
            // 其他检测请求
            httpd_resp_set_status(req, "204 No Content");
            httpd_resp_send(req, NULL, 0);
        }

        return ESP_OK;
    }
}



// 从NVS中保存用户名和密码
static esp_err_t save_auth_credentials(const char *username, const char *password)
{
    if (username == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open(AUTH_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    err = nvs_set_str(nvs_handle, AUTH_NVS_USERNAME_KEY, username);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_str(nvs_handle, AUTH_NVS_PASSWORD_KEY, password);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    return err;
}

// 从NVS中加载用户名和密码
static esp_err_t load_auth_credentials(char *username, char *password)
{
    if (username == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open(AUTH_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // 如果命名空间不存在，使用默认凭据
        strcpy(username, DEFAULT_USERNAME);
        strcpy(password, DEFAULT_PASSWORD);
        
        // 保存默认凭据
        save_auth_credentials(DEFAULT_USERNAME, DEFAULT_PASSWORD);
        return ESP_OK;
    } else if (err != ESP_OK) {
        return err;
    }
    
    size_t username_len = 32; // 假设用户名最长32字符
    err = nvs_get_str(nvs_handle, AUTH_NVS_USERNAME_KEY, username, &username_len);
    if (err != ESP_OK) {
        // 如果找不到用户名，使用默认用户名
        strcpy(username, DEFAULT_USERNAME);
        
        // 尝试保存默认用户名
        nvs_set_str(nvs_handle, AUTH_NVS_USERNAME_KEY, DEFAULT_USERNAME);
    }
    
    size_t password_len = 64; // 假设密码最长64字符
    err = nvs_get_str(nvs_handle, AUTH_NVS_PASSWORD_KEY, password, &password_len);
    if (err != ESP_OK) {
        // 如果找不到密码，使用默认密码
        strcpy(password, DEFAULT_PASSWORD);
        
        // 尝试保存默认密码
        nvs_set_str(nvs_handle, AUTH_NVS_PASSWORD_KEY, DEFAULT_PASSWORD);
        nvs_commit(nvs_handle);
    }
    
    nvs_close(nvs_handle);
    return ESP_OK;
}

// 获取当前用户名的API处理函数
static esp_err_t get_auth_info_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    // 加载当前凭据
    char username[32];
    char password[64];
    esp_err_t load_result = load_auth_credentials(username, password);

    // 构建响应JSON
    cJSON *resp = cJSON_CreateObject();
    if (load_result == ESP_OK) {
        cJSON_AddBoolToObject(resp, "success", true);
        cJSON_AddStringToObject(resp, "username", username);
        // 不返回密码，只返回用户名
    } else {
        cJSON_AddBoolToObject(resp, "success", false);
        cJSON_AddStringToObject(resp, "username", DEFAULT_USERNAME);
    }

    char *json_resp = cJSON_Print(resp);
    httpd_resp_sendstr(req, json_resp);

    free(json_resp);
    cJSON_Delete(resp);

    return ESP_OK;
}

// 验证用户凭据的API处理函数
static esp_err_t auth_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret, remaining = req->content_len;
    
    if (remaining > sizeof(buf) - 1) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内容太长");
        return ESP_FAIL;
    }
    
    ret = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));
    
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    
    buf[ret] = '\0';
    
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "无效JSON");
        return ESP_FAIL;
    }
    
    cJSON *username_json = cJSON_GetObjectItem(root, "username");
    cJSON *password_json = cJSON_GetObjectItem(root, "password");
    
    if (!username_json || !password_json || !cJSON_IsString(username_json) || !cJSON_IsString(password_json)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少用户名或密码");
        return ESP_FAIL;
    }
    
    const char *username = username_json->valuestring;
    const char *password = password_json->valuestring;
    
    // 加载保存的凭据
    char saved_username[32];
    char saved_password[64];
    load_auth_credentials(saved_username, saved_password);
    
    // 验证凭据
    bool auth_success = (strcmp(username, saved_username) == 0 && strcmp(password, saved_password) == 0);

    // 构建响应JSON
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", auth_success);

    if (auth_success) {
        // 认证成功，创建新session
        create_new_session();
        cJSON_AddStringToObject(resp, "session_token", current_session_token);
        cJSON_AddStringToObject(resp, "message", "登录成功");

        // 设置Cookie
        char cookie_header[128];
        snprintf(cookie_header, sizeof(cookie_header),
                "session_token=%s; Path=/; HttpOnly; Max-Age=%d",
                current_session_token, SESSION_TIMEOUT_SECONDS);
        httpd_resp_set_hdr(req, "Set-Cookie", cookie_header);

        ESP_LOGI(TAG, "用户 %s 登录成功", username);
    } else {
        cJSON_AddStringToObject(resp, "message", "用户名或密码错误");
        ESP_LOGW(TAG, "用户 %s 登录失败", username);
    }

    char *json_resp = cJSON_Print(resp);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_resp);
    
    free(json_resp);
    cJSON_Delete(resp);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// 登出API处理函数
static esp_err_t logout_handler(httpd_req_t *req)
{
    // 清除当前session
    memset(current_session_token, 0, sizeof(current_session_token));
    session_created_time = 0;

    // 清除Cookie
    httpd_resp_set_hdr(req, "Set-Cookie", "session_token=; Path=/; HttpOnly; Max-Age=0");

    // 构建响应JSON
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddStringToObject(resp, "message", "已成功登出");

    char *json_resp = cJSON_Print(resp);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_resp);

    free(json_resp);
    cJSON_Delete(resp);

    ESP_LOGI(TAG, "用户已登出");
    return ESP_OK;
}

// 设置用户名密码的API处理函数（简化版，只需要新的用户名和密码）
static esp_err_t update_auth_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret, remaining = req->content_len;

    if (remaining > sizeof(buf) - 1) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内容太长");
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));

    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }

    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "无效JSON");
        return ESP_FAIL;
    }

    cJSON *username_json = cJSON_GetObjectItem(root, "username");
    cJSON *password_json = cJSON_GetObjectItem(root, "password");

    if (!username_json || !password_json ||
        !cJSON_IsString(username_json) || !cJSON_IsString(password_json)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少用户名或密码");
        return ESP_FAIL;
    }

    const char *username = username_json->valuestring;
    const char *password = password_json->valuestring;

    // 验证用户名和密码长度
    if (strlen(username) == 0 || strlen(password) == 0 ||
        strlen(username) > 31 || strlen(password) > 63) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "用户名或密码长度无效");
        return ESP_FAIL;
    }

    // 直接保存新凭据
    esp_err_t update_result = save_auth_credentials(username, password);

    // 构建响应JSON
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", update_result == ESP_OK);
    if (update_result == ESP_OK) {
        cJSON_AddStringToObject(resp, "message", "登录凭据设置成功");
        ESP_LOGI(TAG, "登录凭据已更新: 用户名=%s", username);
    } else {
        cJSON_AddStringToObject(resp, "message", "保存失败");
        ESP_LOGE(TAG, "保存登录凭据失败: %s", esp_err_to_name(update_result));
    }

    char *json_resp = cJSON_Print(resp);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_resp);

    free(json_resp);
    cJSON_Delete(resp);
    cJSON_Delete(root);

    return ESP_OK;
}

// 测试页面处理函数
static esp_err_t test_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");

    const char* test_html =
        "<!DOCTYPE html>"
        "<html><head><title>WiFi扫描测试</title></head>"
        "<body><h1>WiFi扫描测试页面</h1>"
        "<button onclick='testScan()'>测试WiFi扫描</button>"
        "<div id='result'></div>"
        "<script>"
        "async function testScan(){"
        "const result=document.getElementById('result');"
        "result.innerHTML='测试中...';"
        "try{"
        "const response=await fetch('/api/wifi/scan');"
        "const data=await response.json();"
        "result.innerHTML='<pre>'+JSON.stringify(data,null,2)+'</pre>';"
        "}catch(error){"
        "result.innerHTML='错误: '+error.message;"
        "}}"
        "</script></body></html>";

    httpd_resp_sendstr(req, test_html);
    return ESP_OK;
}

// 404错误处理函数
static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    ESP_LOGW(TAG, "404错误: %s", req->uri);

    // 对于某些特定的路径，重定向到配网页面
    if (strstr(req->uri, "/mmtls/") != NULL ||
        strstr(req->uri, "/wifi/") != NULL ||
        strstr(req->uri, "/connecttest") != NULL) {

        // 重定向到配网页面
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/setup");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // 其他404错误返回简单的错误页面
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/html");

    const char* error_page =
        "<!DOCTYPE html><html><head><title>404 - 页面未找到</title>"
        "<style>body{font-family:Arial;text-align:center;margin-top:50px;}"
        "h1{color:#dc3545;}a{color:#007bff;text-decoration:none;}</style></head>"
        "<body><h1>404 - 页面未找到</h1>"
        "<p>请求的页面不存在</p>"
        "<a href='/'>返回首页</a> | <a href='/setup'>配网页面</a></body></html>";

    httpd_resp_send(req, error_page, strlen(error_page));
    return ESP_OK;
}

// 注册URL处理程序
static void register_handlers(httpd_handle_t server)
{
    ESP_LOGI(TAG, "开始注册URI处理器...");

    httpd_uri_t root = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = root_get_handler,
        .user_ctx  = NULL
    };
    esp_err_t ret = httpd_register_uri_handler(server, &root);
    ESP_LOGI(TAG, "注册 / : %s", esp_err_to_name(ret));
    
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

    httpd_uri_t login = {
        .uri       = "/login",
        .method    = HTTP_GET,
        .handler   = login_get_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &login);
    
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
    
    // 添加认证API
    httpd_uri_t auth_post_uri = {
        .uri       = "/api/auth",
        .method    = HTTP_POST,
        .handler   = auth_post_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &auth_post_uri);

    // 添加登出API
    httpd_uri_t logout_post_uri = {
        .uri       = "/api/logout",
        .method    = HTTP_POST,
        .handler   = logout_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &logout_post_uri);

    // 添加设置认证凭据API
    httpd_uri_t update_auth_post_uri = {
        .uri       = "/api/set_auth",
        .method    = HTTP_POST,
        .handler   = update_auth_post_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &update_auth_post_uri);

    // 添加获取认证信息API
    httpd_uri_t get_auth_info_uri = {
        .uri       = "/api/auth_info",
        .method    = HTTP_GET,
        .handler   = get_auth_info_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &get_auth_info_uri);
    ESP_LOGI(TAG, "注册 /api/auth_info : %s", esp_err_to_name(ret));

    // 添加测试页面
    httpd_uri_t test_uri = {
        .uri       = "/test",
        .method    = HTTP_GET,
        .handler   = test_get_handler,
        .user_ctx  = NULL
    };
    ret = httpd_register_uri_handler(server, &test_uri);
    ESP_LOGI(TAG, "注册 /test : %s", esp_err_to_name(ret));

    // 添加一些常见的连接性检测路径
    httpd_uri_t cw_html = {
        .uri       = "/wifi/cw.html",
        .method    = HTTP_GET,
        .handler   = captive_portal_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &cw_html);

    // 添加通用的mmtls路径处理（用于某些设备的连接检测）
    httpd_uri_t mmtls_wildcard = {
        .uri       = "/mmtls/*",
        .method    = HTTP_GET,
        .handler   = captive_portal_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &mmtls_wildcard);

    ESP_LOGI(TAG, "已注册所有URI处理器");
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
    config.max_uri_handlers = 20;  // 增加URI处理器数量
    
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

    // 设置错误处理器
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, http_404_error_handler);

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