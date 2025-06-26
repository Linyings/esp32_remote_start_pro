#include "servo_control/servo_control.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "servo_control";

// 舵机PWM配置
#define SERVO_PWM_PIN              5  // GPIO5用于控制舵机
#define LEDC_TIMER                 LEDC_TIMER_0
#define LEDC_MODE                  LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL               LEDC_CHANNEL_0
#define LEDC_DUTY_RESOLUTION       LEDC_TIMER_13_BIT  // 13位分辨率, 0-8191
#define LEDC_FREQUENCY             50   // 50Hz PWM频率，适合大多数舵机

// 舵机角度配置（以PWM占空比表示）
#define SERVO_INIT_DUTY            409   // 初始角度占空比 (约0度)
#define SERVO_PRESS_DUTY           614   // 按下角度占空比 (约90度)

// 舵机按压时间配置
#define SERVO_PRESS_TIME_MS        200   // 按下时间(毫秒)

esp_err_t servo_control_init(void)
{
    // 配置LEDC定时器
    ledc_timer_config_t ledc_timer = {
        .duty_resolution = LEDC_DUTY_RESOLUTION,
        .freq_hz = LEDC_FREQUENCY,
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    
    esp_err_t ret = ledc_timer_config(&ledc_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC定时器配置失败: %d", ret);
        return ret;
    }
    
    // 配置LEDC通道
    ledc_channel_config_t ledc_channel = {
        .channel    = LEDC_CHANNEL,
        .duty       = SERVO_INIT_DUTY,
        .gpio_num   = SERVO_PWM_PIN,
        .speed_mode = LEDC_MODE,
        .timer_sel  = LEDC_TIMER,
        .hpoint     = 0,
    };
    
    ret = ledc_channel_config(&ledc_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC通道配置失败: %d", ret);
        return ret;
    }
    
    // 安装LEDC渐变功能
    ret = ledc_fade_func_install(0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC渐变功能安装失败: %d", ret);
        return ret;
    }
    
    // 初始化舵机位置
    ret = ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, SERVO_INIT_DUTY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "设置LEDC占空比失败: %d", ret);
        return ret;
    }
    
    ret = ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "更新LEDC占空比失败: %d", ret);
        return ret;
    }
    
    ESP_LOGI(TAG, "舵机控制初始化完成");
    return ESP_OK;
}

esp_err_t servo_press_power_button(void)
{
    ESP_LOGI(TAG, "执行按下电源按钮动作");
    
    // 设置舵机到按下位置
    esp_err_t ret = ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, SERVO_PRESS_DUTY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "设置LEDC占空比失败: %d", ret);
        return ret;
    }
    
    ret = ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "更新LEDC占空比失败: %d", ret);
        return ret;
    }
    
    // 保持按下状态
    vTaskDelay(SERVO_PRESS_TIME_MS / portTICK_PERIOD_MS);
    
    // 返回到初始位置
    ret = ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, SERVO_INIT_DUTY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "设置LEDC占空比失败: %d", ret);
        return ret;
    }
    
    ret = ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "更新LEDC占空比失败: %d", ret);
        return ret;
    }
    
    ESP_LOGI(TAG, "电源按钮按下动作完成");
    return ESP_OK;
} 