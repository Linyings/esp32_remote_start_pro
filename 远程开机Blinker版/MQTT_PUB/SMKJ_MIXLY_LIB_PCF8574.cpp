#include "SMKJ_MIXLY_LIB_PCF8574.h"

/**
 * @brief 初始化PCF8574设备
 * @param addres PCF8574的I2C地址
 * @return true:初始化成功 false:初始化失败
 */
int PCF8574::pcf8574_begin(uint16_t addres)
{
    uint8_t data = 0;
    return iic.write(addres, &data, 0);  // 发送空数据测试设备是否存在
}

/**
 * @brief   读取光敏传感器状态
 * @details 通过I2C读取光敏传感器的状态
 * @return  返回光敏传感器状态
 *          - 0: 未检测到光线
 *          - 1: 检测到光线
 */
int PCF8574::pcf8574_read_light(void)
{
    uint8_t data = 0;
    
    // 从PCF8574读取一个字节的数据
    if(iic.read(LIGHT_ADDRESS, &data, 1) == 1) {
        return !(data & (1 << LIGHT_PIN));  // 指定引脚
    }
    return 0;
}


/**
 * @brief   读取火焰传感器状态
 * @details 通过I2C读取火焰传感器的状态
 * @return  返回火焰传感器状态
 *          - 0: 未检测到火焰
 *          - 1: 检测到火焰
 */
int PCF8574::pcf8574_read_flame(void)
{
    uint8_t data = 0;
    
    // 从PCF8574读取一个字节的数据
    if(iic.read(FLAME_ADDRESS, &data, 1) == 1) {
        return !(data & (1 << FLAME_PIN));  // 指定引脚
    }
    return 0;
}

/**
 * @brief   读取雨滴传感器状态
 * @details 通过I2C读取雨滴传感器的状态
 * @return  返回雨滴传感器状态
 *          - 0: 未检测到水滴
 *          - 1: 检测到水滴
 */
int PCF8574::pcf8574_read_rain(void)
{
    uint8_t data = 0;
    
    // 从PCF8574读取一个字节的数据
    if(iic.read(RAIN_ADDRESS, &data, 1) == 1) {
        return !(data & (1 << RAIN_PIN));  // 指定引脚
    }
    return 0;
}

/**
 * @brief   读取烟雾传感器状态
 * @details 通过I2C读取烟雾传感器的状态
 * @return  返回烟雾传感器状态
 *          - 0: 未检测到烟雾
 *          - 1: 检测到烟雾
 */
int PCF8574::pcf8574_read_smoke(void)
{
    uint8_t data = 0;
    
    // 从PCF8574读取一个字节的数据
    if(iic.read(SMOKE_ADDRESS, &data, 1) == 1) {
        return !(data & (1 << SMOKE_PIN));  // 指定引脚
    }
    return 0;
}

/**
 * @brief   读取水银传感器状态
 * @details 通过I2C读取水银传感器的状态
 * @return  返回水银传感器状态
 *          - 0: 未倾斜
 *          - 1: 倾斜
 */
int PCF8574::pcf8574_read_mercury(void)
{
    uint8_t data = 0;
    
    // 从PCF8574读取一个字节的数据
    if(iic.read(MERCURY_ADDRESS, &data, 1) == 1) {
        return !(data & (1 << MERCURY_PIN));  // 指定引脚
    }
    return 0;
}

/**
 * @brief   读取红外巡线传感器状态
 * @details 通过I2C读取红外传感器的两个引脚状态
 * @return  返回红外传感器状态
 *          - 0: 无障碍物
 *          - 1: 左侧有障碍物
 *          - 2: 右侧有障碍物
 *          - 3: 两侧都有障碍物
 */
int PCF8574::pcf8574_read_infrared(void)
{
    uint8_t data = 0;
    uint8_t infrared_state = 0;
    
    // 从PCF8574读取一个字节的数据
    if(iic.read(INFRARED_ADDRESS, &data, 1) == 1) {
        // 获取两个引脚的状态并取反（低电平表示检测到障碍物）
        bool pin1_state = !(data & (1 << INFRARED_PIN1));
        bool pin2_state = !(data & (1 << INFRARED_PIN2));
        
        // 组合两个引脚的状态
        infrared_state = (pin1_state << 1) | pin2_state;
        return infrared_state;
    }
    return 0;
}

/**
 * @brief   读取按键传感器状态
 * @details 通过I2C读取8个按键的状态
 * @return  返回8位按键状态
 *          - bit0~bit7对应KEY1~KEY8
 *          - 0: 按键未按下
 *          - 1: 按键按下
 */
int PCF8574::pcf8574_read_keys(void)
{
    uint8_t data = 0;
    uint8_t key_state = 0;
    
    // 从PCF8574读取一个字节的数据
    if(iic.read(KEY_ADDRESS, &data, 1) == 1) {
        // 获取每个按键的状态并取反（低电平表示按下）
        key_state |= (!(data & (1 << KEY1_PIN))) << 0;  // KEY1
        key_state |= (!(data & (1 << KEY2_PIN))) << 1;  // KEY2
        key_state |= (!(data & (1 << KEY3_PIN))) << 2;  // KEY3
        key_state |= (!(data & (1 << KEY4_PIN))) << 3;  // KEY4
        key_state |= (!(data & (1 << KEY5_PIN))) << 4;  // KEY5
        key_state |= (!(data & (1 << KEY6_PIN))) << 5;  // KEY6
        key_state |= (!(data & (1 << KEY7_PIN))) << 6;  // KEY7
        key_state |= (!(data & (1 << KEY8_PIN))) << 7;  // KEY8
        
        return key_state;
    }
    return 0;
}

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
bool PCF8574::pcf8574_write_relay(uint8_t state)
{
    uint8_t data = 0;
    
    //（继电器使用PCF8574引脚输出，所以需要单独一个iic地址，避免干扰到其他输入的io口）

    // 先读取当前状态
    if(iic.read(RELAY_ADDRESS, &data, 1) != 1) {
        return false;
    }
    
    // 修改P0位的状态
    if(state) {
        data |= (1 << RELAY_PIN);  // 设置P0位为1
    } else {
        data &= ~(1 << RELAY_PIN); // 设置P0位为0
    }
    
    // 写回数据
    return (iic.write(RELAY_ADDRESS, &data, 1) == 1);
}




