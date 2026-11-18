#ifndef __CONFIG_STORE_H
#define __CONFIG_STORE_H

#include "stm32f1xx_hal.h"
#include "DeviceConfig.h"

typedef enum
{
	CONFIG_STORE_IDLE=0,  // 空闲，尚未执行本次开机后的保存
	CONFIG_STORE_BUSY=1,  // 正在保存
	CONFIG_STORE_SAVED=2, // 最近一次保存成功
	CONFIG_STORE_ERROR=3  // 初始化或保存失败
}configStore_State_t;

HAL_StatusTypeDef ConfigStore_Init(void);           //启动加载
HAL_StatusTypeDef ConfigStore_RequestSave(const DeviceConfig_t *config); //提交保存请求
void ConfigStore_Process(void);                  //主循环推进保存
uint8_t ConfigStore_IsBusy(void);                //查询忙状态
configStore_State_t ConfigStore_GetState(void);  //查询保存结果
void ConfigStore_StopOnFault(void);

#endif
