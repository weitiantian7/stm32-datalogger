#ifndef __STORAGE_TASK_H
#define __STORAGE_TASK_H

#include "DataLogger.h"
#include "DeviceConfig.h"
#include "ConfigStore.h"
#include <stdint.h>

typedef enum
{
	STORAGE_REQUEST_LOG=0,
	STORAGE_REQUEST_CONFIG_SAVE,
	STORAGE_REQUEST_HISTORY_READ
}StorageRequestType_t;//存储请求类型

typedef struct
{
	StorageRequestType_t Type;//存储类型
	DataLogger_Record_t LOG;  //存储日志
	DeviceConfig_t Config;    //存储配置
	
	uint32_t ReadSequence;  //要查询的日志序号
	uint32_t ReadRequestId; //本次请求的编号，用于识别超时后迟到的旧结果。
}StorageRequest_t;//存储请求结构体


void StorageTask(void *pvParameters);

HAL_StatusTypeDef Storage_RequestConfigSave(void);
configStore_State_t Storage_GetConfigSaveState(void);
uint8_t Storage_HistoryInit(void);
HAL_StatusTypeDef Storage_ReadHistory(uint32_t sequence,DataLogger_Record_t *record,uint32_t timeoutMs);

#endif
