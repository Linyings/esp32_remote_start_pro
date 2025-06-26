#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"
#include "esp_http_server.h"

// 初始化Web服务器
esp_err_t web_server_init(void);

// 停止Web服务器
esp_err_t web_server_stop(void);

// 获取当前服务器实例
httpd_handle_t web_server_get_handle(void);

#endif /* WEB_SERVER_H */ 