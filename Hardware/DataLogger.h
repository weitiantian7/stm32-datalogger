#ifndef __DATA_LOGGER_H
#define __DATA_LOGGER_H

#include "stm32f1xx_hal.h"
#include <stdint.h> 

typedef struct
{
	uint32_t Sequence; // 记录序号
	uint32_t TimeMs; // 本次开机后的运行时间，单位ms
	
	int16_t  TemperatureX100; //实际温度*100，℃
	uint16_t HumidityX100; //  实际湿度*100，%RH
	
	uint16_t Analog1; //ADC第一路原始值
	uint16_t Analog2;  //ADC第二路原始值
 }DataLogger_Record_t;



/* 日志模块当前状态 */
typedef enum
{
    DATA_LOGGER_STATE_NOT_READY = 0, // 尚未初始化成功
    DATA_LOGGER_STATE_READY,         // 可以继续追加
    DATA_LOGGER_STATE_FULL,          // 空间已满，仍可读取
    DATA_LOGGER_STATE_ERROR          // 初始化或存储发生错误
} DataLogger_State_t;



HAL_StatusTypeDef DataLogger_Init(void);
HAL_StatusTypeDef DataLogger_WriteRecord(uint32_t Address,DataLogger_Record_t *Record);
HAL_StatusTypeDef DataLogger_ReadRecord(uint32_t Address,DataLogger_Record_t *Record);
HAL_StatusTypeDef DataLogger_AppendRecord(DataLogger_Record_t *Record);
DataLogger_State_t DataLogger_GetState(void);
HAL_StatusTypeDef DataLogger_Format(void);
HAL_StatusTypeDef DataLogger_ReadBySequence(uint32_t Sequence,DataLogger_Record_t *Record);
HAL_StatusTypeDef DataLogger_ReadLatest(DataLogger_Record_t *Record);
uint32_t DataLogger_GetLatestSequence(void);

#endif

