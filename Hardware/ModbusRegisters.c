#include "ModbusRegisters.h"
#include "DeviceConfig.h"
#include "ConfigStore.h"
#include "stm32f1xx_hal.h"
#include "StorageTask.h"
#include <stddef.h>

#define MODBUS_INPUT_TIMEOUT_MS  2500U//输入超时


static ModbusInputData_t currentInputData;//存当前的的输入数据
static uint32_t inputUpdateTick=0U;//最近一次更新输入数据缓存的时间

/**
 * @brief  读取连续的保持寄存器。
 *
 * @param[in]  startAddress  起始寄存器地址，从 0 开始。
 * @param[in]  quantity      读取的寄存器数量，协议允许 1～125 个，
 *                          实际读取范围还必须落在本设备的寄存器范围内。
 * @param[out] values        保存读取结果的数组首地址。
 *                          调用者必须保证数组至少容纳 quantity 个元素。
 *
 * @retval MODBUS_REG_OK
 *         读取成功，结果已依次写入 values[0]～values[quantity-1]。
 * @retval MODBUS_REG_ILLEGAL_ADDRESS
 *         起始地址或读取范围超出本设备支持的寄存器范围。
 * @retval MODBUS_REG_ILLEGAL_VALUE
 *         values 为空指针，或读取数量不合法。
 *
 * @note   当前保持寄存器映射：
 *         地址 0：温度报警阈值，单位 0.01℃，按有符号 16 位数解释。
 *         地址 1：温度回差，单位 0.01℃。
 *         地址 2：记录间隔，单位秒。
 *         地址 3：Modbus 从机地址。
 * @note   本函数只读取配置并填充输出数组，不负责串口发送。
 */
ModbusRegResult_t ModbusRegisters_ReadHolding(uint16_t startAddress,uint16_t quantity,uint16_t *values)
{
    DeviceConfig_t config;
	uint16_t i;
	uint16_t address;
	
	if(values==NULL)
	{
		return MODBUS_REG_ILLEGAL_VALUE;//参数不合法
	}
	
	if(quantity==0U || quantity>125U)
	{
		return MODBUS_REG_ILLEGAL_VALUE;//参数不合法
	}
	
	if((uint32_t)startAddress+quantity>MODBUS_HOLDING_COUNT)
	{
		 return MODBUS_REG_ILLEGAL_ADDRESS;//地址或地址范围不存在
	}
	
	if(DeviceConfig_GetSnapshot(&config,NULL)==0U)//获取设备配置
	{
		return MODBUS_REG_DEVICE_FAILURE;
	}
	
	for(i=0U;i<quantity;i++)
	{
		address=(uint16_t)(startAddress+i);
		
		switch(address)
		{
			case 0U:
				values[i]=(uint16_t)config.TemperatureLimitX100;
			    break;
			
			case 1U:
				values[i]=config.HysteresisX100;
			    break;
			
			case 2U:
				values[i]=config.LogIntervalSeconds;
			    break;
			
			case 3U:
				values[i]=config.SlaveAddress;
			    break;
			
			case 4U:
				values[i]=(uint16_t)Storage_GetConfigSaveState();//获取配置存储的状态
			    break;
			
			default:
				return MODBUS_REG_ILLEGAL_ADDRESS;//寄存器地址非法
		}
	}
	return MODBUS_REG_OK;//成功
}



/**
 * @brief  写入连续的保持寄存器，校验通过后统一更新配置。
 *
 * @param[in] startAddress  起始寄存器地址，从 0 开始。
 * @param[in] quantity      写入的寄存器数量，协议允许 1～123 个，
 *                         实际写入范围还必须落在本设备的寄存器范围内。
 * @param[in] values        待写入数据的数组首地址。
 *                         调用者必须保证数组至少包含 quantity 个元素。
 *
 * @retval MODBUS_REG_OK
 *         写入成功，配置已通过校验并提交。
 * @retval MODBUS_REG_ILLEGAL_ADDRESS
 *         起始地址或写入范围超出本设备支持的寄存器范围。
 * @retval MODBUS_REG_ILLEGAL_VALUE
 *         values 为空指针、写入数量不合法，
 *         或修改后的配置未通过参数及组合条件校验。
 *
 * @note   寄存器映射与 ModbusRegisters_ReadHolding() 相同。
 * @note   先复制当前配置到 candidate，只修改本次请求涉及的成员，
 *         其余成员保留原值。循环结束后统一校验、提交一次。
 * @note   校验失败时，当前正式配置保持原值。
 * @note   本函数不修改 values 数组，也不负责串口收发。
 */
ModbusRegResult_t ModbusRegisters_WriteHolding(uint16_t startAddress,uint16_t quantity,const uint16_t *values)
{
	DeviceConfig_t candidate;//设备配置参数
	uint16_t i;
	uint16_t address;
	
	if(values==NULL || quantity==0U || quantity >123)
	{
		return MODBUS_REG_ILLEGAL_VALUE;//
	}
	
	 /* 保存命令必须单独写，不能和配置参数混在一次写入中 */
	if(startAddress==4U && quantity==1U)
	{
		HAL_StatusTypeDef saveStatus;
		
		if(values[0] != 0xA55AU)
		{
			return MODBUS_REG_ILLEGAL_VALUE;
		}
		
		saveStatus=Storage_RequestConfigSave();
		if(saveStatus==HAL_OK)
		{
			return MODBUS_REG_OK; 
		}
		
		if(saveStatus==HAL_BUSY)
		{
			return MODBUS_REG_DEVICE_BUSY;
		}
	
		return MODBUS_REG_DEVICE_FAILURE;
	}
	
	/* 普通配置写入只允许访问地址 0～3 */
	if ((uint32_t)startAddress + quantity > 4U)
    {
        return MODBUS_REG_ILLEGAL_ADDRESS;
    }
	
    if(Storage_GetConfigSaveState()==CONFIG_STORE_BUSY)
	{
		return MODBUS_REG_DEVICE_BUSY;
	}
	
	if(DeviceConfig_GetSnapshot(&candidate,NULL)==0)//获取当前设备配置
	{
		return MODBUS_REG_DEVICE_FAILURE;
	}
	
	for(i=0U;i<quantity;i++)
	{
		address=(uint16_t)(startAddress+i);
		
		switch(address)
		{
			case 0U:
				candidate.TemperatureLimitX100=(int16_t)values[i];
			    break;
			
			case 1U:
				candidate.HysteresisX100=values[i];
			    break;
			
			case 2U:
				candidate.LogIntervalSeconds=values[i];
			    break;
			
			case 3U:
				candidate.SlaveAddress=values[i];
			    break;
			
			default:
				return MODBUS_REG_ILLEGAL_ADDRESS;
		}
	}
	
	if(DeviceConfig_Apply(&candidate)==0U)
	{
		return MODBUS_REG_ILLEGAL_VALUE;
	}
	
	return MODBUS_REG_OK;
}



void ModbusRegisters_Init(void)
{
	ModbusInputData_t emptyData={0};
	
	currentInputData=emptyData;
	inputUpdateTick=HAL_GetTick();
}


void ModbusRegisters_UpdateInput(const ModbusInputData_t *data)//更新输入数据
{
	if(data==NULL)
	{
		return;
	}	
	
	currentInputData=*data;
	inputUpdateTick=HAL_GetTick();	
}


ModbusRegResult_t ModbusRegisters_ReadInput(uint16_t startAddress,uint16_t quantity,uint16_t *values)
{
	uint16_t registers[MODBUS_INPUT_COUNT];
	uint16_t i;
	uint32_t now;
	
	if(values==NULL || quantity==0U || quantity>125)
	{
		return MODBUS_REG_ILLEGAL_VALUE;
	}
	
	if((uint32_t)startAddress+quantity>MODBUS_INPUT_COUNT)
	{
		return MODBUS_REG_ILLEGAL_ADDRESS;
	}
	
	now=HAL_GetTick();
	
	registers[0]=(uint16_t)currentInputData.TemperatureX100;//有符号转成无符号
	registers[1]=currentInputData.HumidityX100;
	registers[2]=currentInputData.Analog1mV;
	registers[3]=currentInputData.Analog2mV;
	registers[4]=currentInputData.Status;
	registers[5]=currentInputData.ValidMask;
	
	/* 缓存过期时，清除数据有效标志 */
	if((uint32_t)(now-inputUpdateTick)>=MODBUS_INPUT_TIMEOUT_MS)//当前时间 - 最近的数据更新时间
	{
		registers[5]=0U;
	}
	
	registers[6]=(uint16_t)(now>>16);//存高16位时间
	registers[7]=(uint16_t)now;//存低16位时间
	
	for(i=0U;i<quantity;i++)
	{
		values[i]=registers[startAddress+i];
	}
	
	return MODBUS_REG_OK;
}


