#ifndef __W25Q32_H
#define __W25Q32_H

#include "stm32f1xx_hal.h"

void W25Q32_Init(void);
HAL_StatusTypeDef W25Q32_ReadID(uint8_t *MID,uint16_t *DID);
HAL_StatusTypeDef W25Q32_WriteEnable(void);
HAL_StatusTypeDef W25Q32_WaitReady(uint32_t Timeout);
HAL_StatusTypeDef W25Q32_PageProgram(uint32_t Address,uint8_t *DataArray,uint16_t Count);
HAL_StatusTypeDef W25Q32_SectorErase(uint32_t Address);
HAL_StatusTypeDef W25Q32_ReadData(uint32_t Address,uint8_t *DataArray,uint32_t Count);
HAL_StatusTypeDef W25Q32_ReadStatus(uint8_t *statusRegister);
HAL_StatusTypeDef W25Q32_StartSectorErase(uint32_t Address);

#endif

