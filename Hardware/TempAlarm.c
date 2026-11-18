#include "TempAlarm.h"
#include "DeviceConfig.h"
#include <stddef.h>


static TempAlarm_State_t alarmState=TEMP_ALARM_STATE_NORMAL;


/**
 * @brief 初始化温度报警状态
 */
void TempAlarm_Init(void)
{
	alarmState=TEMP_ALARM_STATE_NORMAL;
}



/**
 * @brief 根据当前状态和最新温度更新报警
 */
void TempAlarm_Update(float temperature)
{
	DeviceConfig_t config;
	
	float triggerTemperature;
	float hysteresis;
	
	if(DeviceConfig_GetSnapshot(&config,NULL)==0U)//获取当前配置
	{
		return;
	}
	
	triggerTemperature=(float)config.TemperatureLimitX100/100.0f;
	hysteresis=(float)config.HysteresisX100/100.0f;
	
	if(alarmState==TEMP_ALARM_STATE_NORMAL)
	{
		if(temperature>=triggerTemperature)
		{
			alarmState=TEMP_ALARM_STATE_HIGH;
		}
	}
	else if(alarmState==TEMP_ALARM_STATE_HIGH)
	{
		if(temperature<=(triggerTemperature-hysteresis))
		{
			alarmState=TEMP_ALARM_STATE_NORMAL;
		}
	}
}



/**
 * @brief 获取当前报警状态
 */
TempAlarm_State_t TempAlarm_GetState(void)
{
	return alarmState;
}
