#ifndef SMKJ_MIXLY_LIB_IIC_H
#define SMKJ_MIXLY_LIB_IIC_H

#include <Arduino.h>
#include <Wire.h>

/**
 * @brief IIC通信封装类
 * @details 封装Wire库的基本操作，提供互斥访问机制
 */
class IIC_Device 
{
    private:
        static SemaphoreHandle_t i2c_mutex;  // I2C总线互斥锁
        
    public:
        /**
         * @brief 构造函数
         */
        IIC_Device(void) {
            if(i2c_mutex == NULL) {
                i2c_mutex = xSemaphoreCreateMutex();
            }
        }
        
        /**
         * @brief 初始化IIC总线
         * @param sda SDA引脚号
         * @param scl SCL引脚号
         * @param freq 通信频率(Hz)
         */
        void begin(uint8_t sda, uint8_t scl, uint32_t freq);
        
        /**
         * @brief 写入数据
         * @param addr 设备地址
         * @param data 要写入的数据
         * @param len 数据长度
         * @return 0:成功 其他:失败
         */
        int write(uint8_t addr, uint8_t* data, uint8_t len);
        
        /**
         * @brief 读取数据
         * @param addr 设备地址
         * @param data 数据缓冲区
         * @param len 要读取的数据长度
         * @return 实际读取的数据长度
         */
        int read(uint8_t addr, uint8_t* data, uint8_t len);
        
        /**
         * @brief 写入寄存器
         * @param addr 设备地址
         * @param reg 寄存器地址
         * @param data 要写入的数据
         * @return 0:成功 其他:失败
         */
        int writeReg(uint8_t addr, uint8_t reg, uint8_t data);
        
        /**
         * @brief 读取寄存器
         * @param addr 设备地址
         * @param reg 寄存器地址
         * @return 寄存器值，-1表示读取失败
         */
        int readReg(uint8_t addr, uint8_t reg);
};

// 声明全局对象
extern IIC_Device iic;

#endif // SMKJ_MIXLY_LIB_IIC_H
