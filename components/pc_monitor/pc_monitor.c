#include "pc_monitor/pc_monitor.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pc_monitor";

// GPIO引脚配置
#define PC_STATUS_PIN 14  // GPIO14用于检测PC状态

// I2C配置
#define I2C_MASTER_SCL_IO           21      // I2C时钟引脚
#define I2C_MASTER_SDA_IO           22      // I2C数据引脚
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          100000  // 100kHz
#define PCF8574_ADDR                0x21    // PCF8574 I2C地址
#define PCF8574_STATUS_BIT          0       // PCF8574中PC状态所在的位

// 检测间隔(毫秒) - 修改为10秒，减少频繁检测
#define PC_STATUS_CHECK_INTERVAL_MS 10000

// 当前PC状态
static pc_state_t s_current_pc_state = PC_STATE_OFF;

// 状态变化回调
static pc_state_change_callback_t s_state_change_callback = NULL;

// 当前使用的读取方式
static pc_status_read_mode_t s_read_mode = PC_STATUS_READ_I2C;

// I2C初始化标志
static bool s_i2c_initialized = false;

// 初始化I2C
static esp_err_t init_i2c(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ
    };
    
    esp_err_t ret = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C参数配置失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C驱动安装失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "I2C初始化成功");
    s_i2c_initialized = true;
    return ESP_OK;
}

// 通过I2C从PCF8574读取状态
static esp_err_t read_pcf8574_status(bool *status)
{
    if (!s_i2c_initialized) {
        ESP_LOGE(TAG, "I2C未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    uint8_t data = 0;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCF8574_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &data, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "从PCF8574读取数据失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 检查bit0的状态并取反
    // PCF8574返回0表示开机，返回1表示关机
    *status = (data & (1 << PCF8574_STATUS_BIT)) == 0;
    
    // 打印一下读取到的原始数据和解析后的状态
    ESP_LOGD(TAG, "PCF8574原始数据: 0x%02X, bit%d=%d, 解析状态: %s", 
             data, PCF8574_STATUS_BIT, (data >> PCF8574_STATUS_BIT) & 0x01,
             *status ? "开机" : "关机");
    
    return ESP_OK;
}

esp_err_t pc_monitor_init(void)
{
    esp_err_t ret = ESP_OK;
    
    // 配置GPIO引脚 
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PC_STATUS_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    
    ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO配置失败: %d", ret);
        return ret;
    }
    
    // 尝试初始化I2C
    ret = init_i2c();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C初始化失败，将使用GPIO检测模式: %s", esp_err_to_name(ret));
        s_read_mode = PC_STATUS_READ_GPIO;
    } else {
        // 尝试读取PCF8574状态，确认设备存在
        bool status;
        ret = read_pcf8574_status(&status);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "无法从PCF8574读取状态，切换到GPIO检测模式: %s", esp_err_to_name(ret));
            s_read_mode = PC_STATUS_READ_GPIO;
        } else {
            ESP_LOGI(TAG, "PCF8574检测成功，使用I2C检测模式");
            s_read_mode = PC_STATUS_READ_I2C;
        }
    }
    
    // 初始读取PC状态
    if (s_read_mode == PC_STATUS_READ_GPIO) {
        s_current_pc_state = gpio_get_level(PC_STATUS_PIN) ? PC_STATE_ON : PC_STATE_OFF;
        ESP_LOGI(TAG, "通过GPIO检测PC初始状态: %s", s_current_pc_state == PC_STATE_ON ? "开机" : "关机");
    } else {
        bool status;
        if (read_pcf8574_status(&status) == ESP_OK) {
            s_current_pc_state = status ? PC_STATE_ON : PC_STATE_OFF;
            ESP_LOGI(TAG, "通过I2C检测PC初始状态: %s", s_current_pc_state == PC_STATE_ON ? "开机" : "关机");
        }
    }
    
    return ESP_OK;
}

void pc_monitor_task(void *pvParameter)
{
    ESP_LOGI(TAG, "PC监控任务启动，使用模式: %s", s_read_mode == PC_STATUS_READ_GPIO ? "GPIO" : "I2C");
    
    while (1) {
        // 读取PC状态
        pc_state_t new_state;
        
        if (s_read_mode == PC_STATUS_READ_GPIO) {
            // 通过GPIO读取
            new_state = gpio_get_level(PC_STATUS_PIN) ? PC_STATE_ON : PC_STATE_OFF;
        } else {
            // 通过I2C读取
            bool status;
            if (read_pcf8574_status(&status) == ESP_OK) {
                new_state = status ? PC_STATE_ON : PC_STATE_OFF;
            } else {
                // I2C读取失败，尝试通过GPIO读取
                ESP_LOGW(TAG, "I2C读取失败，临时切换到GPIO读取");
                new_state = gpio_get_level(PC_STATUS_PIN) ? PC_STATE_ON : PC_STATE_OFF;
            }
        }
        
        // 检测状态变化
        if (new_state != s_current_pc_state) {
            ESP_LOGI(TAG, "检测到PC状态变化: %s -> %s",
                s_current_pc_state == PC_STATE_ON ? "开机" : "关机",
                new_state == PC_STATE_ON ? "开机" : "关机");

            // 更新状态
            s_current_pc_state = new_state;

            // 只在状态变化时调用回调函数（发送WebSocket消息）
            if (s_state_change_callback != NULL) {
                ESP_LOGI(TAG, "主动发送状态变化通知");
                s_state_change_callback(new_state);
            }
        } else {
            // 状态无变化，静默检测，不发送任何通知
            ESP_LOGD(TAG, "PC状态无变化，保持: %s",
                s_current_pc_state == PC_STATE_ON ? "开机" : "关机");
        }
        
        // 延时检测
        vTaskDelay(PC_STATUS_CHECK_INTERVAL_MS / portTICK_PERIOD_MS);
    }
}

pc_state_t pc_monitor_get_state(void)
{
    return s_current_pc_state;
}

void pc_monitor_register_callback(pc_state_change_callback_t callback)
{
    s_state_change_callback = callback;
}

// 切换PC状态检测模式
esp_err_t pc_monitor_set_mode(pc_status_read_mode_t mode)
{
    // 检查模式是否有效
    if (mode != PC_STATUS_READ_GPIO && mode != PC_STATUS_READ_I2C) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 如果切换到I2C模式，确保I2C已初始化
    if (mode == PC_STATUS_READ_I2C && !s_i2c_initialized) {
        esp_err_t ret = init_i2c();
        if (ret != ESP_OK) {
            return ret;
        }
        
        // 测试PCF8574是否可用
        bool status;
        ret = read_pcf8574_status(&status);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    
    // 切换模式
    s_read_mode = mode;
    ESP_LOGI(TAG, "PC状态检测模式切换为: %s", mode == PC_STATUS_READ_GPIO ? "GPIO" : "I2C");
    
    return ESP_OK;
}

// 获取当前检测模式
pc_status_read_mode_t pc_monitor_get_mode(void)
{
    return s_read_mode;
} 