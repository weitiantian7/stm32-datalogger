#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <stdint.h>

typedef struct
{
	int16_t TemperatureLimitX100; //温度上限，单位0.01℃
	uint16_t HysteresisX100;      //回差，单位0.01℃
	uint16_t LogIntervalSeconds;  //记录间隔，单位秒
	uint16_t SlaveAddress;        // 从站地址
}DeviceConfig_t;

uint8_t DeviceConfig_Init(void);
uint8_t DeviceConfig_GetSnapshot(DeviceConfig_t *snapshot,uint32_t *revision);
uint8_t DeviceConfig_Validate(const DeviceConfig_t *config);
uint8_t DeviceConfig_Apply(const DeviceConfig_t *config);

#endif
