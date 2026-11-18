#ifndef __SHT30_H
#define __SHT30_H

#include "stm32f1xx_hal.h"

HAL_StatusTypeDef SHT30_Init(void);
HAL_StatusTypeDef SHT30_Read(uint8_t *Data);
HAL_StatusTypeDef SHT30_CheckCRC(const uint8_t *Data);
HAL_StatusTypeDef SHT30_Convert(float *temperature,float *humidity);

#endif 
