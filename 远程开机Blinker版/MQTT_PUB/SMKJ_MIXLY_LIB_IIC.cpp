#include "SMKJ_MIXLY_LIB_IIC.h"

// 定义静态成员变量
SemaphoreHandle_t IIC_Device::i2c_mutex = NULL;

// 定义全局对象
IIC_Device iic;

/**
 * @brief 初始化IIC总线
 */
void IIC_Device::begin(uint8_t sda, uint8_t scl, uint32_t freq)
{
    Wire.begin(sda, scl, freq);
}

/**
 * @brief 写入数据
 */
int IIC_Device::write(uint8_t addr, uint8_t* data, uint8_t len)
{
    int ret = -1;

    // 尝试获取互斥锁，增加超时时间到10ms
    if(xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        Wire.beginTransmission(addr);
        if(data != NULL && len > 0) {
            Wire.write(data, len);
        }
        ret = Wire.endTransmission();

        // 释放互斥锁
        xSemaphoreGive(i2c_mutex);
    } else {
        Serial.println("IIC: Failed to acquire mutex for write");
    }

    return ret;
}

/**
 * @brief 读取数据
 */
int IIC_Device::read(uint8_t addr, uint8_t* data, uint8_t len)
{
    int count = 0;

    // 尝试获取互斥锁，增加超时时间到10ms
    if(xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        Wire.requestFrom(addr, len);

        // 等待数据可用，最多等待10ms
        unsigned long startTime = millis();
        while(Wire.available() < len && (millis() - startTime) < 10) {
            delay(1);
        }

        while(Wire.available() && count < len) {
            data[count++] = Wire.read();
        }

        // 释放互斥锁
        xSemaphoreGive(i2c_mutex);
    } else {
        Serial.println("IIC: Failed to acquire mutex for read");
    }

    return count;
}

/**
 * @brief 写入寄存器
 */
int IIC_Device::writeReg(uint8_t addr, uint8_t reg, uint8_t data)
{
    int ret = -1;

    // 尝试获取互斥锁，增加超时时间到10ms
    if(xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        Wire.beginTransmission(addr);
        Wire.write(reg);
        Wire.write(data);
        ret = Wire.endTransmission();

        // 释放互斥锁
        xSemaphoreGive(i2c_mutex);
    } else {
        Serial.println("IIC: Failed to acquire mutex for writeReg");
    }

    return ret;
}

/**
 * @brief 读取寄存器
 */
int IIC_Device::readReg(uint8_t addr, uint8_t reg)
{
    int data = -1;

    // 尝试获取互斥锁，增加超时时间到10ms
    if(xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        Wire.beginTransmission(addr);
        Wire.write(reg);
        Wire.endTransmission();

        Wire.requestFrom(addr, (uint8_t)1);

        // 等待数据可用，最多等待10ms
        unsigned long startTime = millis();
        while(!Wire.available() && (millis() - startTime) < 10) {
            delay(1);
        }

        if(Wire.available()) {
            data = Wire.read();
        }

        // 释放互斥锁
        xSemaphoreGive(i2c_mutex);
    } else {
        Serial.println("IIC: Failed to acquire mutex for readReg");
    }

    return data;
}