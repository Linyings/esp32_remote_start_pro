#ifndef SMKJ_MIXLY_LIB_SERVO_H
#define SMKJ_MIXLY_LIB_SERVO_H

#include <ESP32Servo.h>

class SMKJ_SERVO {
public:
    SMKJ_SERVO();
    void init();
    void control(int servoNumber, int angle);

private:
    Servo servos[2];
    const int servoPins[2] = {5, 23}; // 假设舵机连接到这些引脚
};

#endif // SMKJ_MIXLY_LIB_SERVO_H

