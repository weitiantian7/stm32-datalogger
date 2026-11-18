#include "SensorData.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "stm32f1xx_hal.h"

static SemaphoreHandle_t sensorDataMutex=NULL;
static SensorData_t latestData={0};

uint8_t SensorData_Init(void)
{
	if(sensorDataMutex==NULL)
	{
		sensorDataMutex=xSemaphoreCreateMutex();//创建传感器数据锁
	}
	
	return (sensorDataMutex!=NULL)? 1U : 0U;//成功返回1否则返回0
}

void SensorData_Publish(float temperature,float humidity,uint8_t valid,uint8_t alarmActive)//Publish：发布
{
	if(sensorDataMutex==NULL)
	{
		return;
	}
	
	if(xSemaphoreTake(sensorDataMutex,portMAX_DELAY)==pdTRUE)//拿传感器数据锁
	{
		if(valid!=0U)//判断采集的温湿度有效，1为有效
		{
			latestData.Temperature=temperature;//记录最新温度
		    latestData.Humidity=humidity;//记录最新湿度
			latestData.UpdateTimeMS=HAL_GetTick();//记录采集时间
		}
		
		latestData.Valid=(valid!=0U)? 1U : 0U;//标记数据有效性，不放在if为了防止遗留上一轮的有效性
		latestData.AlarmActive=(valid!=0U && alarmActive!=0U)? 1U : 0U;//温度警告有效位
		xSemaphoreGive(sensorDataMutex);//释放传感器数据锁
	}
}

void SensorData_PublishAdc(float Voltage1,float Voltage2,uint8_t Adcvalid)//发布ADC数据
{
	if(sensorDataMutex==NULL)
	{
		return;
	}
	
	if(xSemaphoreTake(sensorDataMutex,portMAX_DELAY)==pdTRUE)//拿传感器数据锁
	{
		if(Adcvalid!=0U)//判断数据有效
		{
			latestData.Voltage1=Voltage1;//记录最新电压1
		    latestData.Voltage2=Voltage2;//记录最新电压2
			latestData.AdcUpdateTimeMS=HAL_GetTick();//记录采集时间
		}
		
		latestData.AdcValid=(Adcvalid!=0U)? 1U : 0U;//标记数据有效性
		xSemaphoreGive(sensorDataMutex);//释放传感器数据锁
	}
}

uint8_t SensorData_GetSnapshot(SensorData_t *snapshot)//获取传感器数据快照
{
	if(snapshot==NULL || sensorDataMutex==NULL)
	{
		return 0U;
	}
	
	if(xSemaphoreTake(sensorDataMutex,portMAX_DELAY)==pdTRUE)//拿传感器数据锁
	{
		*snapshot=latestData;//返回传感器最新数据
		xSemaphoreGive(sensorDataMutex);//释放传感器数据锁
		
		return 1U;
	}
	
	return 0U;
}

