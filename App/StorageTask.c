#include "StorageTask.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "AppResources.h"
#include "DataLogger.h"
#include "W25Q32.h"


static volatile configStore_State_t configSaveState=CONFIG_STORE_BUSY;//配置保存状态

typedef struct
{
	uint32_t RequestId;//请求编号
	HAL_StatusTypeDef Status;//请求操作返回状态
	DataLogger_Record_t Record;//历史记录
}StorageHistoryReply_t;//存储历史记录读取回复结构体

static QueueHandle_t historyReplyQueue=NULL;//回复队列
static uint32_t historyRequestId=0U;//请求编号

uint8_t Storage_HistoryInit(void)
{
	if(historyReplyQueue==NULL)
	{
		historyReplyQueue=xQueueCreate(1U,sizeof(StorageHistoryReply_t));//创建回复队列
	}
	
	return (historyReplyQueue!=NULL)? 1U : 0U;
}

static void Storage_SetSaveState(configStore_State_t state)//设置配置存储状态
{
	taskENTER_CRITICAL(); // 进入临界区
	configSaveState=state;
	taskEXIT_CRITICAL();  // 退出临界区
}

/**
 * @brief 恢复存储保存状态
 * @param previous 要恢复的前序状态
 * @note 临界区内操作：仅当当前为忙状态时，回退为传入的历史状态
 */
static void Storage_RestoreSaveState(configStore_State_t previous)//previous：先前的
{
	taskENTER_CRITICAL();
	
	if(configSaveState==CONFIG_STORE_BUSY)//判断配置存储状态为忙
	{
		configSaveState=previous;//配置保存状态设置为之前的
	}
	
	taskEXIT_CRITICAL();
}

configStore_State_t Storage_GetConfigSaveState(void)
{
	return configSaveState;//返回配置存储状态
}


/**
 * @brief 发起配置保存请求
 * @retval HAL_OK：请求成功；其他：请求失败
 */
HAL_StatusTypeDef Storage_RequestConfigSave(void)//请求保存配置
{
	StorageRequest_t request={STORAGE_REQUEST_CONFIG_SAVE};//存储请求类型设置为配置保存
	configStore_State_t previous;//配置存储状态
	
	if(g_storageQueue==NULL)
	{
		return HAL_ERROR;
	}
	
	taskENTER_CRITICAL();//进入临界区
	
	if(configSaveState==CONFIG_STORE_BUSY)//判断配置保存状态为忙
	{
		taskEXIT_CRITICAL();//退出临界区
		return HAL_BUSY;//返回忙状态
	}
	
	if(configSaveState==CONFIG_STORE_ERROR)//判断配置保存状态为错误
	{
		taskEXIT_CRITICAL();//退出临界区
		return HAL_ERROR;//返回错误
	}
	
	previous=configSaveState;//保存当前配置保存状态
	configSaveState=CONFIG_STORE_BUSY;//设置配置保存状态为忙
	
	taskEXIT_CRITICAL();//退出临界区
	
	if(DeviceConfig_GetSnapshot(&request.Config,NULL)==0U)//获取设备配置快照，以及配置序号。返回值=0，获取失败
	{
		Storage_RestoreSaveState(previous);//恢复原来的配置保存状态
		return HAL_ERROR;
	}
	
	if(xQueueSend(g_storageQueue,&request,0U)!=pdPASS)//发送存储请求到队列
	{
		Storage_RestoreSaveState(previous);//恢复原来的配置保存状态
		return HAL_BUSY;
	}
	
	return HAL_OK;
}



static void Storage_ReportFault(void)//报告错误 Report：报告
{
	ConfigStore_StopOnFault();//设置存储为错误状态
	Storage_SetSaveState(CONFIG_STORE_ERROR);//设置配置保存为错误状态
}



HAL_StatusTypeDef Storage_ReadHistory(uint32_t sequence,DataLogger_Record_t *record,uint32_t timeoutMs)
{
	StorageRequest_t request={STORAGE_REQUEST_HISTORY_READ};//存储请求类型设置为历史阅读
	StorageHistoryReply_t reply;//历史阅读回复结构体
	TimeOut_t timeoutState;//超时状态
	TickType_t remaining=pdMS_TO_TICKS(timeoutMs);//剩余等待时间
	configStore_State_t state;//配置存储状态
	
	if(record==NULL || sequence==0U || sequence==0xFFFFFFFFUL || g_storageQueue==NULL || historyReplyQueue==NULL)
	{
		return HAL_ERROR;//返回错误
	}
	
	if(remaining==0U)
	{
		return HAL_TIMEOUT;//返回超时
	}
	
	state=Storage_GetConfigSaveState();//获取配置存储状态
	if(state==CONFIG_STORE_BUSY)
	{
		return HAL_BUSY;//正在忙返回忙
	}
	
	if(state==CONFIG_STORE_ERROR)//判断状态错误
	{
		return HAL_ERROR;//返回错误状态
	}
	
	while(xQueueReceive(historyReplyQueue,&reply,0U)==pdPASS)//清空应答队列残留旧包，避免读到上次遗留应答
	{
		
	}
	
	historyRequestId++;//历史读取请求ID+1
	if(historyRequestId==0U)
	{
		historyRequestId++;
	}
	
	request.ReadSequence=sequence;//请求序号写入回复结构体
	request.ReadRequestId=historyRequestId;//本次请求序号写入回复结构体
	
	vTaskSetTimeOutState(&timeoutState);// 1. 记录起点：把此刻tick存入timeoutState
	
	if(xQueueSend(g_storageQueue,&request,0U)!=pdPASS)//发送存储请求到队列
	{
		return HAL_BUSY;//发送不成功返回忙
	}
	
	for(;;)
	{
		if(xTaskCheckForTimeOut(&timeoutState,&remaining)!=pdFALSE)//remaining：计算剩余时间  pdFALSE：还没超时
		{
			return HAL_TIMEOUT;
		}
		
		if(xQueueReceive(historyReplyQueue,&reply,remaining)!=pdPASS)//接收回复队列
		{
			return HAL_TIMEOUT;
		}
		
		if(reply.RequestId==request.ReadRequestId)//将本次请求序号写入回复结构体
		{
			if(reply.Status==HAL_OK)
			{
				*record=reply.Record;//将历史记录写入指定位置
			}
			
			return reply.Status;//返回回复状态
		}
	}
}



void StorageTask(void *pvParameters)
{
	static DataLogger_Record_t receivedRecord={0};//记录结构体
	static volatile uint32_t logReceivedCount=0U;//记录接收计数
	
	static uint8_t flashManufacturerID=0U;//flash厂家id
	static uint16_t flashDeviceID=0U;//flash设备id
	
	static volatile HAL_StatusTypeDef flashIdStatus=HAL_ERROR;//id读取状态
	static volatile HAL_StatusTypeDef logInitStatus=HAL_ERROR;//记录日志初始化状态
	static volatile HAL_StatusTypeDef logWriteStatus=HAL_ERROR;//记录写入状态
	
	static volatile uint8_t storageReady=0U;//存储状态位
	
	static volatile uint32_t logWriteOKCount=0U;//写入成功计数
	static volatile uint32_t logWriteErrorCount=0U;//写入错误计数
	static volatile uint32_t logDropCount=0U;//记录丢弃计数
	static volatile uint32_t lastWrittenSequence=0U;//最新写入序号
	
	static volatile UBaseType_t storageStackFreeWords=0U; //存储任务剩余空闲栈字数
	
	static StorageRequest_t request={STORAGE_REQUEST_LOG};//存储请求结构体变量
	
	static volatile HAL_StatusTypeDef configInitStatus=HAL_ERROR;//配置恢复状态
	static volatile HAL_StatusTypeDef configSaveStartStatus=HAL_ERROR;//配置保存开始状态
	
	static volatile uint32_t configSaveRequestCount=0U;//配置保存请求计数
	static volatile uint32_t configSaveOKCount=0U;//配置保存成功计数
	static volatile uint32_t configSaveErrorCount=0U;//配置保存错误计数
	
	static volatile uint32_t historyReadRequestCount=0U;//历史阅读请求计数
	static volatile uint32_t historyReadOKCount=0U;//历史阅读成功计数
	static volatile uint32_t historyReadErrorCount=0U;//历史阅读错误计数
	
	(void)pvParameters;
	
	W25Q32_Init();//flash初始化
	
	flashIdStatus=W25Q32_ReadID(&flashManufacturerID,&flashDeviceID);//读取flash ID
	if(flashIdStatus==HAL_OK && flashManufacturerID==0xEFU &&flashDeviceID==0x4016U)//判断id读取成功并且符合设备信息
	{
		configInitStatus=ConfigStore_Init();//恢复配置信息
		if(configInitStatus==HAL_OK)//配置恢复成功
		{
			logInitStatus=DataLogger_Init();//恢复日志区
		    if(logInitStatus==HAL_OK)//日志恢复成功
		    {
		    	storageReady=1U;//标记flash就绪
		    }
		} 
	}
	
	if(storageReady!=0U)
	{
		Storage_SetSaveState(CONFIG_STORE_IDLE);//flash就绪设置配置保存状态为空闲
	}
	else
	{
		Storage_ReportFault();//报告错误
	}
	
	xEventGroupSetBits(g_storageEvents,STORAGE_INIT_DONE_BIT);//设置事件组标记，采集任务和Modbus任务在等待该事件位，就绪后往下运行
	
	storageStackFreeWords=uxTaskGetStackHighWaterMark(NULL);//获取本任务栈高水位（历史最小剩余栈空间，单位：word）
	
	for(;;)
	{
		if(xQueueReceive(g_storageQueue,&request,portMAX_DELAY)==pdPASS)//接收存储请求
		{
			if(request.Type==STORAGE_REQUEST_LOG)//判断存储请求类型为日志记录
			{
				receivedRecord=request.LOG;//获取请求记录日志
				logReceivedCount++;//日志接收计数+1
				
				if(storageReady!=0U)//判断flash就绪
				{
					logWriteStatus=DataLogger_AppendRecord(&receivedRecord);//日志写进flash
					if(logWriteStatus==HAL_OK)//日志记录成功
					{
						logWriteOKCount++;//写入成功数+1
						lastWrittenSequence=receivedRecord.Sequence;//保存最新写入序号
					}
					else
					{
						logWriteErrorCount++;//写入错误数+1
						logDropCount++;//写入丢弃数+1
						storageReady=0;//更改flash状态为未就绪
						Storage_ReportFault();//报告错误状态
					}
				}
				else
				{
					logDropCount++;//flash未就绪丢弃数+1
				}
			}
			else if(request.Type==STORAGE_REQUEST_CONFIG_SAVE)//判断存储请求为配置保存
			{
				configSaveRequestCount++;//配置保存请求数+1
				
				if(storageReady!=0)//判断flash就绪
				{
					configSaveStartStatus=ConfigStore_RequestSave(&request.Config);//请求保存配置
					if(configSaveStartStatus==HAL_OK)//保存请求成功
					{
						while(ConfigStore_IsBusy()!=0U)
						{
							ConfigStore_Process();//推进保存
							
							if(ConfigStore_IsBusy()!=0U)//判断还在保存
							{
								vTaskDelay(1);//延时1个节拍
							}
						}
					}
					
					if(configSaveStartStatus==HAL_OK && ConfigStore_GetState()==CONFIG_STORE_SAVED)//判断保存请求成功，并且配置保存状态为成功
					{
						configSaveOKCount++;//配置保存成功计数+1
						Storage_SetSaveState(CONFIG_STORE_SAVED);//设置配置保存状态为成功
					}
					else
					{
						configSaveErrorCount++;//配置保存错误+1
						storageReady=0U;//更改flash为未就绪
						Storage_ReportFault();//报告错误
					}
				}
				else
				{
					configSaveErrorCount++;//保存错误计数+1
					Storage_ReportFault();//报告错误
				}
			}
			else if(request.Type==STORAGE_REQUEST_HISTORY_READ)//判断存储请求为历史记录回读
			{
				StorageHistoryReply_t reply={0};//回复结构体
				
				historyReadRequestCount++;//回读请求数+1
				
				reply.RequestId=request.ReadRequestId;//写入本次请求编号
				reply.Status=HAL_ERROR;//回复状态先设置为错误
				
				if(storageReady!=0)//判断flash就绪
				{
					reply.Status=DataLogger_ReadBySequence(request.ReadSequence,&reply.Record);//按序号读取历史记录，写进回复结构体
				}
				
				if(reply.Status==HAL_OK)//判断历史记录读取成功
				{
					historyReadOKCount++;//历史记录读取成功计数+1
				}
				else
				{
					historyReadErrorCount++;//历史记录读取错误计数+1
				}
					
				xQueueOverwrite(historyReplyQueue,&reply);//覆盖写入应答队列，保存最新应答（队列深度为1）
			}
			
			storageStackFreeWords=uxTaskGetStackHighWaterMark(NULL);// 获取本任务栈高水位，历史最小剩余栈空间（单位：word）
		}
	}
	
}
