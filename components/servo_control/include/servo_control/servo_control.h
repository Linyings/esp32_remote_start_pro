#ifndef SERVO_CONTROL_H
#define SERVO_CONTROL_H

#include "esp_err.h"

// 初始化舵机控制
esp_err_t servo_control_init(void);

// 按下电源按钮（执行舵机按下动作）
esp_err_t servo_press_power_button(void);

#endif /* SERVO_CONTROL_H */ 