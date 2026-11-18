#include "displayTask.h"
#include "FreeRTOS.h"
#include "AppResources.h"
#include "semphr.h"
#include "task.h"
#include "OLED.h"
#include "SensorData.h"


void DisplayTask(void *pvParameters)
{
	uint32_t displayStartMs;//显示起始时间
    static volatile uint32_t displayFrameMs=0U;//显示一帧时间
    static volatile uint32_t displayMaxFrameMs=0U;//显示最长一帧时间
	
	
	TickType_t lastWakeTime=xTaskGetTickCount();//获取最新唤醒时间
	SensorData_t snapshot={0};//传感器数据
	uint8_t snapshotOK;
	uint8_t shtValid;
	uint8_t adcValid;
	uint8_t alarmActive;
	
	(void)pvParameters;//消除未使用参数警告，本任务无传入参数

    for(;;)
    {
		snapshotOK=SensorData_GetSnapshot(&snapshot);//获取传感器数据快照
		
		shtValid=(snapshotOK!=0U && snapshot.Valid!=0U && (uint32_t)(HAL_GetTick() - snapshot.UpdateTimeMS)<2500U)? 1U : 0U;//判断数据快照获取成功，温湿度数据有效，并且采集时间有效。都有效标记数据有效
		
		if(xSemaphoreTake(g_i2cMutex,portMAX_DELAY)==pdTRUE)//拿I2C互斥锁
		{
			displayStartMs = HAL_GetTick();//获取显示起始时间
			
			OLED_BeginUpdate();//开始OLED画面更新，允许发送显示数据
	
		    OLED_ShowString(1,1,"Temp:");
		    OLED_ShowString(2,1,"RH:");
			OLED_ShowString(3,1,"V1:");
			OLED_ShowString(4,1,"V2:");
			
			if(shtValid!=0U)//温湿度数据有效
			{
				OLED_ShowFloat(1,6,snapshot.Temperature,5,2);
				OLED_ShowFloat(2,4,snapshot.Humidity,5,2);
				OLED_ShowChineseChar(1,11,"℃");
			    OLED_ShowChineseChar(2,9,"％");
			}
			else
			{
				OLED_ShowString(1,6,"-----");
				OLED_ShowString(2,4,"-----");	
				OLED_ShowString(1,11,"  ");
				OLED_ShowString(2,9,"  ");
			}
			
		    adcValid=(snapshotOK!=0U && snapshot.AdcValid!=0U && (uint32_t)(HAL_GetTick() - snapshot.AdcUpdateTimeMS)<2500U)? 1U : 0U;//判断数据快照获取成功，两路ADC数据有效，并且采集时间有效
		    if(adcValid!=0U)//ADC数据有效
			{
				OLED_ShowFloat(3,4,snapshot.Voltage1,4,2);
				OLED_ShowFloat(4,4,snapshot.Voltage2,4,2);
				OLED_ShowString(3,8,"V");
			    OLED_ShowString(4,8,"V");
			}
			else
			{
				OLED_ShowString(3,4,"----");
				OLED_ShowString(4,4,"----");	
				OLED_ShowString(3,8," ");
				OLED_ShowString(4,8," ");
			}
			
			alarmActive=(shtValid!=0U && snapshot.AlarmActive!=0U? 1U : 0U);//判断温度数据有效，处于警告状态
			if(alarmActive!=0)//警告状态有效
			{
				HAL_GPIO_WritePin(GPIOB,GPIO_PIN_11,GPIO_PIN_RESET);//蜂鸣器触发
				OLED_ShowString(1,14,"ALM");//显示警告
			}
			else
			{
				HAL_GPIO_WritePin(GPIOB,GPIO_PIN_11,GPIO_PIN_SET);
				OLED_ShowString(1,14,"   ");
			}
			
			displayFrameMs = HAL_GetTick() - displayStartMs;//获取当前一帧时间
            if (displayFrameMs > displayMaxFrameMs)
            {
                displayMaxFrameMs = displayFrameMs;//获取最长一帧的时间
            }
			
			xSemaphoreGive(g_i2cMutex);//释放I2C互斥锁
		}
		
		xTaskDelayUntil(&lastWakeTime,pdMS_TO_TICKS(200));//固定周期延时，保证任务周期200ms，更新唤醒时间戳
	}	
}
