/* *****************************************************************
 *
 * Download latest Blinker library here:
 * https://github.com/blinker-iot/blinker-library/archive/master.zip
 * 
 * 
 * Blinker is a cross-hardware, cross-platform solution for the IoT. 
 * It provides APP, device and server support, 
 * and uses public cloud services for data transmission and storage.
 * It can be used in smart home, data monitoring and other fields 
 * to help users build Internet of Things projects better and faster.
 * 
 * Make sure installed 2.7.4 or later ESP8266/Arduino package,
 * if use ESP8266 with Blinker.
 * https://github.com/esp8266/Arduino/releases
 * 
 * Make sure installed 1.0.5 or later ESP32/Arduino package,
 * if use ESP32 with Blinker.
 * https://github.com/espressif/arduino-esp32/releases
 * 
 * Docs: https://diandeng.tech/doc
 *       
 * 
 * *****************************************************************
 * 
 * Blinker 库下载地址:
 * https://github.com/blinker-iot/blinker-library/archive/master.zip
 * 
 * Blinker 是一套跨硬件、跨平台的物联网解决方案，提供APP端、设备端、
 * 服务器端支持，使用公有云服务进行数据传输存储。可用于智能家居、
 * 数据监测等领域，可以帮助用户更好更快地搭建物联网项目。
 * 
 * 如果使用 ESP8266 接入 Blinker,
 * 请确保安装了 2.7.4 或更新的 ESP8266/Arduino 支持包。
 * https://github.com/esp8266/Arduino/releases
 * 
 * 如果使用 ESP32 接入 Blinker,
 * 请确保安装了 1.0.5 或更新的 ESP32/Arduino 支持包。
 * https://github.com/espressif/arduino-esp32/releases
 * 
 * 文档: https://diandeng.tech/doc
 *       
 * 
 * *****************************************************************/

#define BLINKER_WIFI

#include <Blinker.h>
#include "SMKJ_MIXLY_LIB_SERVO.h"
#include "SMKJ_MIXLY_LIB_PCF8574.h"
#include "SMKJ_MIXLY_LIB_IIC.h"

char auth[] = "e7fcf1573cb5";
char ssid[] = "YIJIAJU";
char pswd[] = "716434lyq";

// 新建Blinker组件对象
BlinkerButton Button1("btn-abc");        // 原有按钮：控制LED
BlinkerButton ButtonServo("btn-servo");  // 新增按钮：控制舵机
BlinkerNumber Number1("num-abc");        // 计数器显示
BlinkerNumber NumberLight("num-light");  // 光线传感器状态显示
BlinkerText Text1("tex-abc");            // 开机状态文本显示

int counter = 0;

// 创建舵机和传感器对象
SMKJ_SERVO servo;
PCF8574 pcf8574;

// 舵机状态变量
int servoAngle = 0;  // 当前角度
const int SERVO_SWING_ANGLE = 90;  // 摆动角度

// 按下按键即会执行该函数
void button1_callback(const String & state)
{
    BLINKER_LOG("get button state: ", state);
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
}

// 舵机控制按钮回调函数
void buttonServo_callback(const String & state)
{
    BLINKER_LOG("Servo button pressed: ", state);
    
    // 摆动舵机：0度 -> 90度 -> 0度
    servo.control(1, SERVO_SWING_ANGLE);  // 摆动到90度
    delay(500);  // 等待500ms
    servo.control(1, 0);  // 回到0度
    
    BLINKER_LOG("Servo swing completed");
}

// 如果未绑定的组件被触发，则会执行其中内容
void dataRead(const String & data)
{
    BLINKER_LOG("Blinker readString: ", data);
    counter++;
    Number1.print(counter);

    if (BLINKER_PROTOCOL_MQTT != NULL) {
        String pub_topic = String("/device/") + Blinker.deviceName() + "/s";
        String pub_data = "{\"toDevice\":\"the device name you need pub to\",\"data\":{\"hello\":\"blinker\"}}";
        BLINKER_PROTOCOL_MQTT->publish(pub_topic.c_str(), pub_data.c_str());
    }
}

void setup()
{
    // 初始化串口
    Serial.begin(115200);
    BLINKER_DEBUG.stream(Serial);
    BLINKER_DEBUG.debugAll();
    
    // 初始化有LED的IO
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);
    
    // 初始化I2C总线 (SDA=22, SCL=21, 100kHz)
    iic.begin(22, 21, 100000);
    BLINKER_LOG("I2C initialized");
    
    // 初始化PCF8574
    if(pcf8574.pcf8574_begin(LIGHT_ADDRESS) == 0) {
        BLINKER_LOG("PCF8574 initialized successfully");
        // PCF8574作为输入使用前，需要将所有引脚设为高电平（输入模式）
        uint8_t initData = 0xFF;
        iic.write(LIGHT_ADDRESS, &initData, 1);
    } else {
        BLINKER_LOG("PCF8574 initialization failed!");
    }
    
    // 初始化舵机
    servo.init();
    servo.control(1, 0);  // 设置初始位置为0度
    BLINKER_LOG("Servo initialized at 0 degree");
    
    // 初始化blinker
    Blinker.begin(auth, ssid, pswd);
    Blinker.attachData(dataRead);

    // 绑定按钮回调
    Button1.attach(button1_callback);
    ButtonServo.attach(buttonServo_callback);
    
    BLINKER_LOG("Setup completed!");
}

void loop() {
    Blinker.run();
    
    // 每隔2秒读取一次光线传感器状态
    static unsigned long lastReadTime = 0;
    unsigned long currentTime = millis();
    
    if(currentTime - lastReadTime >= 2000) {
        lastReadTime = currentTime;
        
        // 读取光线传感器状态
        int lightState = pcf8574.pcf8574_read_light();
        
        // 更新数值到Blinker
        NumberLight.print(lightState);
        
        // 根据光线传感器状态更新文本显示
        if(lightState == 1) {
            Text1.print("已开机");
            BLINKER_LOG("Light sensor: ON - 已开机");
        } else {
            Text1.print("已关机");
            BLINKER_LOG("Light sensor: OFF - 已关机");
        }
    }
}
