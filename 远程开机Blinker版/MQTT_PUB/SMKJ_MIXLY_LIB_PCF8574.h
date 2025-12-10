#ifndef SMKJ_MIXLY_LIB_PCF8574_H
#define SMKJ_MIXLY_LIB_PCF8574_H

#include <Arduino.h>
#include "SMKJ_MIXLY_LIB_IIC.h"


/**
 * @brief PCF8574 IO扩展芯片控制类
 */
class PCF8574
{
    public:
        /**
         * @brief 读取PCF8574的IO口状态
         * @details 通过I2C读取PCF8574的8位IO状态
         * @return 返回8位IO状态(0:低电平 1:高电平)
         *         - bit0~bit7对应P0~P7引脚
         *         - 返回-1表示读取失败
         */
        int pcf8574_read_flame(void);
        
        /**
         * @brief 初始化PCF8574设备
         * @param addres PCF8574的I2C地址
         * @return true:初始化成功 false:初始化失败
         */
        int pcf8574_begin(uint16_t addres);
        
        /**
         * @brief 读取雨滴传感器状态
         * @details 通过I2C读取雨滴传感器的状态
         * @return 返回雨滴传感器状态
         *         - 0: 未检测到水滴
         *         - 1: 检测到水滴
         */
        int pcf8574_read_rain(void);
        
        /**
         * @brief   读取烟雾传感器状态
         * @details 通过I2C读取烟雾传器的状态
         * @return  返回烟雾传感器状态
         *          - 0: 未检测到烟雾
         *          - 1: 检测到烟雾
         */
        int pcf8574_read_smoke(void);
        
        /**
         * @brief   读取水银传感器状态
         * @details 通过I2C读取水银传感器的状态
         * @return  返回水银传感器状态
         *          - 0: 未倾斜
         *          - 1: 倾斜
         */
        int pcf8574_read_mercury(void);
        
        /**
         * @brief   读取红外巡线传感器状态
         * @details 通过I2C读取红外传感器的两个引脚状态
         * @return  返回红外传感器状态
         *          - 0: 无障碍物
         *          - 1: 左侧有障碍物
         *          - 2: 右侧有障碍物
         *          - 3: 两侧都有障碍物
         */
        int pcf8574_read_infrared(void);
        
        /**
         * @brief   读取按键传感器状态
         * @details 通过I2C读取8个按键的状态
         * @return  返回8位按键状态
         *          - bit0~bit7对应KEY1~KEY8
         *          - 0: 按键未按下
         *          - 1: 按键按下
         */
        int pcf8574_read_keys(void);
        
        /**
         * @brief   控制PCF8574的P0引脚输出
         * @details 通过I2C控制PCF8574的P0引脚输出高低电平
         * @param   state 输出状态
         *          - 0: 输出低电平
         *          - 1: 输出高电平
         * @return  返回操作状态
         *          - true: 操作成功
         *          - false: 操作失败
         */
        bool pcf8574_write_relay(uint8_t state);

        /**
         * @brief   读取光敏传感器状态
         * @details 通过I2C读取光敏传感器的状态
         * @return  返回光敏传感器状态
         *          - 0: 未检测到光线
         *          - 1: 检测到光线
         */
        int pcf8574_read_light(void);
};

// PCF8574相关地址定义
#define LIGHT_ADDRESS       0x42>>1  // 光敏传感器地址
#define LIGHT_PIN           0        // 光敏传感器引脚

#define FLAME_ADDRESS       0x42>>1  // 火焰传感器地址
#define FLAME_PIN           1        // 火焰传感器引脚

#define RAIN_ADDRESS        0x42>>1  // 雨滴传感器地址
#define RAIN_PIN            2        // 雨滴传感器引脚

#define SMOKE_ADDRESS       0x42>>1  // 烟雾传感器地址
#define SMOKE_PIN           3        // 烟雾传感器引脚

#define MERCURY_ADDRESS     0x42>>1  // 水银传感器地址
#define MERCURY_PIN         4        // 水银传感器引脚

#define INFRARED_ADDRESS    0x4A>>1  // 红外传感器地址
#define INFRARED_PIN1       0        // 红外传感器引脚1
#define INFRARED_PIN2       1        // 红外传感器引脚2

#define KEY_ADDRESS         0x4E>>1  // 按键传感器地址
#define KEY1_PIN    0    // 按键1引脚
#define KEY2_PIN    1    // 按键2引脚
#define KEY3_PIN    2    // 按键3引脚
#define KEY4_PIN    3    // 按键4引脚
#define KEY5_PIN    4    // 按键5引脚
#define KEY6_PIN    5    // 按键6引脚
#define KEY7_PIN    6    // 按键7引脚
#define KEY8_PIN    7    // 按键8引脚

#define RELAY_ADDRESS       0x44>>1  // 继电器地址（继电器使用PCF8574引脚输出，所以需要单独一个iic地址，避免干扰到其他输入的io口）
#define RELAY_PIN           4        // 继电器引脚  



#endif