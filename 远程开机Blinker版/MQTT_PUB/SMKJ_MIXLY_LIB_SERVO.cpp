#include "SMKJ_MIXLY_LIB_SERVO.h"

SMKJ_SERVO::SMKJ_SERVO() {}

void SMKJ_SERVO::init() {
    for (int i = 0; i < 2; i++) {
        servos[i].attach(servoPins[i]);
    }
}

void SMKJ_SERVO::control(int servoNumber, int angle) {
    if (servoNumber < 1 || servoNumber > 2) {
        return; // 无效的舵机编号
    }
    
    if (angle < 0) {
        angle = 0;
    } else if (angle > 360) {
        angle = 360;
    }
    
    int mappedAngle = angle;
    
    servos[servoNumber - 1].write(mappedAngle);
}