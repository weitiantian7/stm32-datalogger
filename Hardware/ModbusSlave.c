#include "ModbusSlave.h"
#include "ModbusRegisters.h"
#include "DeviceConfig.h"
#include "ModbusCRC.h"
#include "stddef.h"
#include "StorageTask.h"

/* 把高字节在前的两个字节，组合成 uint16_t */
static uint16_t Modbus_ReadU16BE(const uint8_t *data)//从字节数组中，按大端顺序读取两个字节，组合成一个无符号 16 位整数。
{
	return (uint16_t)(((uint16_t)data[0]<<8) | (uint16_t)data[1]);
}



uint16_t ModbusSlave_ProcessFrame(
    const uint8_t *frame,      // 收到的请求帧
    uint16_t frameLength,      // 请求帧实际有多少字节
    uint8_t *response,         // 将生成的回复写到哪里
    uint16_t responseCapacity  // 回复缓冲区最多能容纳多少字节
)
{
	uint8_t slaveAddress;  //从机地址
	uint8_t function;      //功能码
	uint8_t exception=0U;  //错误标志位
	
	uint16_t startAddress; //寄存器起始地址
	uint16_t quantity;     //读取或写入寄存器数量
	uint16_t values[MODBUS_INPUT_COUNT]; //临时存放数据
	
	uint16_t i;
	uint16_t responseLength=0U; //回复数据长度
	uint16_t crc;
	
	ModbusRegResult_t result;//寄存器操作状态位
	
	DeviceConfig_t config;
	
	if(frame==NULL || response==NULL || frameLength<4U || frameLength>256U || responseCapacity<8U)
	{
		return 0U;
	}
	
	/* 保存本次请求中的地址和功能码 */
	slaveAddress=frame[0];
	function=frame[1];
	
	if(DeviceConfig_GetSnapshot(&config,NULL)==0U)
	{
		return 0U;
	}
	
	if(slaveAddress!=0 && slaveAddress!=config.SlaveAddress)
	{
		return 0U;
	}
	
	response[0]=slaveAddress;
	response[1]=function;
	
	switch(function)
	{
		case 0x03U://读保持寄存器
		case 0x04U://读输入寄存器
		{
			if(slaveAddress==0U)
			{
				return 0;
			}
			
			if(frameLength!=8U)
			{
				exception=3U;//记录异常码 ,在这里表示请求长度、格式不合法。
				break;
			}
			
			startAddress=Modbus_ReadU16BE(&frame[2]);//拼接起始寄存器地址
			quantity=Modbus_ReadU16BE(&frame[4]);//拼接读取数量
			
			
			if(function==0x3U)
			{
				result=ModbusRegisters_ReadHolding(startAddress,quantity,values);
			}
			else
			{
				result=ModbusRegisters_ReadInput(startAddress,quantity,values);
			}
			
			if(result!=MODBUS_REG_OK)
			{
				exception=(uint8_t)result;
				break;
			}
			
			
			/* 地址1 + 功能1 + 字节数1 + 数据 + CRC2 */
			if((uint32_t)quantity *2U+5U > responseCapacity)//大于回复缓冲区最多能容纳字节
			{
				return 0U;
			}
			
			response[2]=(uint8_t)(quantity*2U);//写要读取的数据所占大小，一个寄存器16位占2字节
			
			for(i=0U;i<quantity;i++)
			{
				response[3U+i*2U]=(uint8_t)(values[i]>>8);//先发送高位
				response[4U+i*2U]=(uint8_t)(values[i]);				
			}
			
			 /* 此时还没有添加 CRC */
			responseLength=(uint16_t)(3U+quantity*2U);//3U=地址1 + 功能1 + 字节数1
			break;		
		}
		
		case 0x06U://写单个保持寄存器
		{
			if(frameLength!=8U)//判断请求帧是不是8字节，地址1+功能1+寄存器地址2+要写入寄存器的值2+CRC校验2
			{
				exception=3U;
				break;
			}
			
			startAddress=Modbus_ReadU16BE(&frame[2]);
			values[0]=Modbus_ReadU16BE(&frame[4]);
			
			result=ModbusRegisters_WriteHolding(startAddress,1U,values);//读保持寄存器
			if(result!=MODBUS_REG_OK)
			{
				exception=(uint8_t)result;
				break;
			}
			
		    /* 成功后复制请求的前 6 字节作为应答正文 */
			for(i=0U;i<6U;i++)
			{
				response[i]=frame[i];
			}
			
			responseLength=6U;
			break;
		}
		
		case 0x10U://写多个保持寄存器
		{
			if(frameLength<9U)
			{
				exception=3U;
				break;
			}
			
			startAddress=Modbus_ReadU16BE(&frame[2]);
			quantity=Modbus_ReadU16BE(&frame[4]);
			
			 /*
             * frame[6]：数据字节数
             * 整帧长度：9 + 数据字节数
             */
			if(quantity==0U || quantity>123U || frame[6]!=quantity*2U || frameLength!=(uint16_t)(9U+frame[6]))
			{
				exception=3U;
				break;
			}
			
			/*
             * 必须先检查地址范围，再往 values 中装数据，
             * 因为 values 只有当前寄存器数量那么大。
             */
			if((uint32_t)startAddress+quantity>MODBUS_HOLDING_COUNT)//
			{
				exception=2U;
				break;
			}
			
			for(i=0U;i<quantity;i++)
			{
				values[i]=Modbus_ReadU16BE(&frame[7U+i*2U]);
			}
			
			result=ModbusRegisters_WriteHolding(startAddress,quantity,values);
			if(result!=MODBUS_REG_OK)
			{
				exception=(uint8_t)result;
				break;
			}
			
			 /*
             * 前 6 字节正好是：
             * 地址、功能码、起始地址、寄存器数量。
             */
			for(i=0U;i<6U;i++)
			{
				response[i]=frame[i];
			}
			responseLength=6U;
			break;
		}
		
		case 0x41U:/* 按序号读取一条历史记录 */
		{
			uint32_t sequence;
			DataLogger_Record_t record;
			HAL_StatusTypeDef status;
			uint16_t recordWords[8];
			
			/* 历史读取不响应广播 */
			if(slaveAddress==0U)
			{
				return 0U;
			}
			
			if(frameLength!=8U)
			{
				exception=3U;
				break;
			}
			
			sequence=((uint32_t)Modbus_ReadU16BE(&frame[2])<<16) | (uint32_t)Modbus_ReadU16BE(&frame[4]);
			if(sequence==0U || sequence==0xFFFFFFFFUL)
			{
				exception=3U;
				break;
			}
			
			if(responseCapacity<21U)
			{
				 exception = 4U;
                 break;
			}
			
			status= Storage_ReadHistory(sequence, &record, 1500U);
			if(status==HAL_BUSY || status==HAL_TIMEOUT)
			{
				exception=6U;
				break;
			}
			
			if(status!=HAL_OK)
			{
				exception=4U; /* 本阶段统一表示记录读取失败 */
				break;
			}
			
			/* 将记录整理为8个16位数，高16位在前 */
			recordWords[0]=(uint16_t)(record.Sequence>>16);
			recordWords[1]=(uint16_t)record.Sequence;
			recordWords[2]=(uint16_t)(record.TimeMs>>16);
			recordWords[3]=(uint16_t)record.TimeMs;
			recordWords[4]=(uint16_t)record.TemperatureX100;
			recordWords[5]=record.HumidityX100;
			recordWords[6]=record.Analog1;
			recordWords[7]=record.Analog2;
			
			response[2]=16U;
			
			for(i=0U;i<8U;i++)
			{
				response[3U+i*2U]=(uint8_t)(recordWords[i]>>8);
				response[4U+i*2U]=(uint8_t)recordWords[i];
			}
			
			responseLength=19U;/* 正文19字节，函数末尾统一追加CRC */
			break;
		}
		
		default:
		{
			exception=1U;
			break;
		}
	}
	
	/* 广播请求无论成功或失败，都不发送应答 */
	if(slaveAddress==0U)//从机地址=0时代表广播操作，所有从机都执行写入操作
	{
		return 0U;
	}
	
	if(exception!=0U)
	{
		response[0]=slaveAddress;
		response[1]=(uint8_t)(function | 0x80U);//Modbus 规定：异常回复要把原功能码的最高位，也就是 bit7，设置为 1
		response[2]=exception;//具体是什么异常
		
		responseLength=3U;
	}
	
	/* 正常应答和异常应答，都在这里添加 CRC */
	crc=Modbus_CRC16(response,responseLength);
	
	response[responseLength]=(uint8_t)crc;
	response[responseLength+1]=(uint8_t)(crc>>8);
	
	return (uint16_t)(responseLength+2U);//responseLength 在添加 CRC 前，只表示应答正文长度。
}

