#ifndef __SENSORDATA_H
#define __SENSORDATA_H

#include <stdint.h>

typedef struct
{
	float Temperature;         //温度
	float Humidity;            //湿度
	uint8_t Valid;             //1:有效  0:无效
	uint8_t AlarmActive;       //1:温度超限报警  2:无警报
	uint32_t UpdateTimeMS;     //温湿度最近成功更新时间
	
	float Voltage1;            //第一路最新电压 单位V
	float Voltage2;            //第二路最新电压 单位V
	uint8_t AdcValid;          //两路电压有效值
	uint32_t AdcUpdateTimeMS;  //电压最近成功更新时间
	
}SensorData_t;

uint8_t SensorData_Init(void);
void SensorData_Publish(float temperature,float humidity,uint8_t valid,uint8_t alarmActive);
void SensorData_PublishAdc(float Voltage1,float Voltage2,uint8_t Adcvalid);
uint8_t SensorData_GetSnapshot(SensorData_t *snapshot);


#endif
