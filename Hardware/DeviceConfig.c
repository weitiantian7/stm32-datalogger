#include "DeviceConfig.h"
#include <stddef.h>
#include "FreeRTOS.h"
#include "semphr.h"


static DeviceConfig_t currentConfig;//设备配置，保存当前使用的参数。static 将它限制在本文件内，其他模块通过接口访问。
static uint32_t configRevision=0U;//配置修订号。每次成功修改就加一，主循环据此判断是否需要更新报警和记录周期。
static SemaphoreHandle_t configMutex=NULL;

uint8_t DeviceConfig_Init(void)
{
	if(configMutex!=NULL)
	{
		return 1U;
	}
	
	configMutex=xSemaphoreCreateMutex();
	if(configMutex==NULL)
	{
		return 0U;
	}
	
	currentConfig.TemperatureLimitX100=3100;
	currentConfig.HysteresisX100=100U;
	currentConfig.LogIntervalSeconds=5U;
	currentConfig.SlaveAddress=1U;
	
	configRevision=1U;
	
	return 1U;
}


uint8_t DeviceConfig_GetSnapshot(DeviceConfig_t *snapshot,uint32_t *revision)
{
	if(snapshot==NULL || configMutex==NULL)
	{
		return 0;
	}
	
	if(xSemaphoreTake(configMutex,portMAX_DELAY)!=pdTRUE)
	{
		return 0U;
	}
	
	*snapshot=currentConfig;
	
	if(revision!=NULL)
	{
		*revision=configRevision;
	}
	
	xSemaphoreGive(configMutex);
	
	return 1U;
}




uint8_t DeviceConfig_Validate(const DeviceConfig_t *config)//设备配置校验
{
	if(config==NULL)
	{
		return 0U;
	}
	
	if(config->TemperatureLimitX100<-4000 ||config->TemperatureLimitX100>12500)
	{
		return 0U;
	}
	if(config->HysteresisX100<1 ||config->HysteresisX100>5000)
	{
		return 0U;
	}
	if((int32_t)config->TemperatureLimitX100 - (int32_t)config->HysteresisX100<-4000)//限制温度 - 回差温度不能小于温度计的下限值
	{
		return 0U;
	}
	if(config->LogIntervalSeconds<1 ||config->LogIntervalSeconds>60)//Log:日志、记录   Interval：间隔  Seconds:秒
	{
		return 0U;
	}
	if(config->SlaveAddress<1 || config->SlaveAddress>247)
	{
		return 0U;
	}
	
	return 1U;
}



uint8_t DeviceConfig_Apply(const DeviceConfig_t *config)
{
	DeviceConfig_t candidate;
	
	if(config==NULL || configMutex==NULL)
	{
		return 0U;
	}
	candidate=*config;
	
	if(DeviceConfig_Validate(&candidate)==0U)
	{
		return 0U;
	}
	
	if(xSemaphoreTake(configMutex,portMAX_DELAY)!=pdTRUE)
	{
		return 0U;
	}
	
	currentConfig=candidate;
	configRevision++;
	
	xSemaphoreGive(configMutex);
	
	return 1U;
}



