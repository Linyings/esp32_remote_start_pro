#ifndef PC_MONITOR_H
#define PC_MONITOR_H

#include "esp_err.h"
#include <stdbool.h>

// PC状态枚举
typedef enum {
    PC_STATE_OFF = 0,   // PC关机状态
    PC_STATE_ON = 1     // PC开机状态
} pc_state_t;

// PC状态检测模式
typedef enum {
    PC_STATUS_READ_GPIO = 0,  // 通过GPIO读取
    PC_STATUS_READ_I2C = 1    // 通过I2C读取PCF8574
} pc_status_read_mode_t;

// PC状态改变回调类型
typedef void (*pc_state_change_callback_t)(pc_state_t new_state);

// 初始化PC监控模块
esp_err_t pc_monitor_init(void);

// PC监控任务（需要在独立任务中运行）
void pc_monitor_task(void *pvParameter);

// 获取当前PC状态
pc_state_t pc_monitor_get_state(void);

// 注册PC状态变化回调
void pc_monitor_register_callback(pc_state_change_callback_t callback);

// 设置PC状态检测模式 (GPIO或I2C)
esp_err_t pc_monitor_set_mode(pc_status_read_mode_t mode);

// 获取当前PC状态检测模式
pc_status_read_mode_t pc_monitor_get_mode(void);

#endif /* PC_MONITOR_H */ 