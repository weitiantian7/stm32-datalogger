#include "AcquisitionTask.h"
#include "FreeRTOS.h"
#include "AppResources.h"
#include "semphr.h"
#include "task.h"
#include "SHT30.h"
#include "SensorData.h"
#include "AnalogInput.h"
#include "TempAlarm.h"
#include "DataLogger.h"
#include "queue.h"
#include "ConfigStore.h"
#include "DeviceConfig.h"
#include "StorageTask.h"




void AcquisitionTask(void *pvParameters)
{
	HAL_StatusTypeDef status;
	uint8_t alarmActive=0U;
	
	StorageRequest_t logRequest={STORAGE_REQUEST_LOG};
	
	float temperature=0.0f;
	float humidity=0.0f;
	float Voltage1=0.0f;
	float Voltage2=0.0f;
	
	static uint16_t average1;
	static uint16_t average2;
	static volatile uint32_t averageReadyCount;
	static DataLogger_Record_t writeRecord={0};
	
	SensorData_t logsnapshot={0};
	uint8_t logSnapshotOK;
	static volatile uint32_t logSendCount=0U;
	static volatile uint32_t logQueueFullCount;
	static volatile uint32_t logInvalidCount=0U;
	
	static volatile uint32_t sampleOKCount=0U;
	static volatile uint32_t sampleErrorCount=0U;
	
	TickType_t lastWakeTime=xTaskGetTickCount();//最近唤醒时间
	TickType_t lastShtReadTime=lastWakeTime;
	TickType_t now;
	
	DeviceConfig_t configSnapshot={0};
	uint8_t configSnapshotOK=0U;
	
	uint16_t activeLogIntervalSeconds=0U;//Interval:间隔
	
	uint32_t lastLogTick=HAL_GetTick();//最近记录时间
	uint32_t logIntervalMs=0U;
	uint32_t logNowMs;
	uint32_t logElapseMs;
	
	
	(void)pvParameters;
	
	xEventGroupWaitBits(g_storageEvents,STORAGE_INIT_DONE_BIT,pdFALSE,pdTRUE,portMAX_DELAY);//参数二：设置等待比特位 参数三：pdFALSE不清除对应比特位   参数四：pdTRUE等待所有位就绪 由存储任务设置比特位
	
	lastWakeTime=xTaskGetTickCount();
	lastShtReadTime=lastWakeTime;
	
	for(;;)
	{
		now=xTaskGetTickCount();
		
		configSnapshotOK=DeviceConfig_GetSnapshot(&configSnapshot,NULL);//获取配置快照，以及配置序号  成功返回1
		if(configSnapshotOK!=0U && configSnapshot.LogIntervalSeconds!=activeLogIntervalSeconds)//获取的成功，并且当前配置的记录间隔不等于活动记录间隔，更新活动记录间隔
		{
			activeLogIntervalSeconds=configSnapshot.LogIntervalSeconds;//更新活动记录间隔
			logIntervalMs=(uint32_t)activeLogIntervalSeconds*1000U;//记录间隔由s转换成ms
			lastLogTick=HAL_GetTick();
		}
		
		if((TickType_t)(now - lastShtReadTime)>=pdMS_TO_TICKS(1000))//判断采集间隔是否达到1s
		{
			lastShtReadTime=now;//更新采集时间
			
		    if(xSemaphoreTake(g_i2cMutex,portMAX_DELAY)==pdTRUE)//申请I2C互斥锁
		    {
		    	status=SHT30_Convert(&temperature,&humidity);//获取温度、湿度
				xSemaphoreGive(g_i2cMutex);//释放I2C互斥锁
				
		        if(status==HAL_OK)//判断温湿度获取成功
		        {
		        	sampleOKCount++;//采集成功+1
					TempAlarm_Update(temperature);//根据最新采集温度更新温度警告
					alarmActive=(TempAlarm_GetState()==TEMP_ALARM_STATE_HIGH)? 1U : 0U;//更新温度警告状态，报警状态返回1否则返回0。用于显示警告和操作蜂鸣器
		        }
		        else if(status!=HAL_BUSY)
		        {
		        	sampleErrorCount++;//采集失败+1
		        }
		    	
		    	SensorData_Publish(temperature,humidity,(status==HAL_OK)? 1U : 0U,alarmActive);//发布温湿度传感器数据，根据驱动状态标记数据有效性，传入告警状态
		    }
		}
		
		status=AnalogInput_GetVoltage(&Voltage1,&Voltage2);//获取最新电压值	
	    SensorData_PublishAdc(Voltage1,Voltage2,(status==HAL_OK)? 1U : 0U);//根据驱动状态标记数据有效性
		
		status=AnalogInput_GetAverageRaw(&average1,&average2);//采集平均原始数值，1秒100次
		if(status==HAL_OK)
		{
			averageReadyCount++;
			
			logNowMs=HAL_GetTick();//获取当前时间
			logElapseMs=(uint32_t)(logNowMs-lastLogTick);//获取上次记录到现在的时间间隔
			
			if(configSnapshotOK!=0U && logIntervalMs!=0 && logElapseMs>=logIntervalMs)//判断配置快照获取成功，并且当前时间间隔>配置的时间间隔
			{
				lastLogTick+=(logElapseMs/logIntervalMs)*logIntervalMs;//更新记录时间
				
				logSnapshotOK=SensorData_GetSnapshot(&logsnapshot);//获取传感器数据快照
			    if(logSnapshotOK!=0 && logsnapshot.Valid!=0U && (uint32_t)(HAL_GetTick() - logsnapshot.UpdateTimeMS)<2500U)//判断传感器数据快照获取成功，并且温湿度数据有效，采集时间合法
			    {
			    	writeRecord.Sequence=0U;//记录序号暂时填0，后续由记录函数写入
			    	writeRecord.TimeMs=logNowMs;//写入记录时间
			    	
			    	writeRecord.TemperatureX100=(int16_t)(logsnapshot.Temperature*100.0f+((logsnapshot.Temperature>=0.0f)? 0.5f : -0.5f));//写入温度*100并四舍五入
			    	writeRecord.HumidityX100=(uint16_t)(logsnapshot.Humidity*100.0f+0.5f);//写入湿度*100并四舍五入
			    	
			    	writeRecord.Analog1=average1;//写入第一路ADC平均原始值
			    	writeRecord.Analog2=average2;//写入第二路ADC平均原始值
					
					logRequest.Type=STORAGE_REQUEST_LOG;//将记录请求设置为日志记录
					logRequest.LOG=writeRecord;//写入日志结构体
			    	
			    	if(xQueueSend(g_storageQueue,&logRequest,0U)==pdPASS)//将记录通过队列发送
			    	{
			    		logSendCount++;//记录发送数+1
			    	}
			    	else
			    	{
			    		logQueueFullCount++;//发送失败数+1
			    	}
			    }
			    else
			    {
			    	logInvalidCount++;//数据无效+1；
			    }
			}	
		}
			
        xTaskDelayUntil(&lastWakeTime,pdMS_TO_TICKS(200));//按固定周期延时，保证任务以200ms周期运行，更新唤醒时间戳
	}
}

