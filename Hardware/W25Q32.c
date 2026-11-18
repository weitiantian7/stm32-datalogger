#include "W25Q32.h"
#include "spi.h"
#include "FreeRTOS.h"
#include "task.h"


/**
 * @brief 初始化W25Q32片选状态，将CS置高
 */
void W25Q32_Init(void)
{
	HAL_GPIO_WritePin(GPIOA,GPIO_PIN_4,GPIO_PIN_SET);
}


/**
 * @brief  读取JEDEC ID
 * @param  MID：用于接收制造商ID的变量地址
 * @param  DID：用于接收存储器类型和容量编码的变量地址
 * @retval HAL状态
 */
 HAL_StatusTypeDef W25Q32_ReadID(uint8_t *MID,uint16_t *DID)
{
	uint8_t cmd=0x9F;
	uint8_t id[3]={0};
	HAL_StatusTypeDef status;
	
	if(MID==NULL || DID==NULL)
	{
		return HAL_ERROR;
	}
	
	HAL_GPIO_WritePin(GPIOA,GPIO_PIN_4,GPIO_PIN_RESET);//拉低cs选择设备
	
	status=HAL_SPI_Transmit(&hspi1,&cmd,1,100);
	
	if(status==HAL_OK)
	{
		status=HAL_SPI_Receive(&hspi1,id,3,100);
	}
	
	HAL_GPIO_WritePin(GPIOA,GPIO_PIN_4,GPIO_PIN_SET);//拉高cs释放设备
	
	if(status==HAL_OK)
	{
		*MID=id[0];
		*DID=(uint16_t)id[1]<<8 | id[2];
	}
	
	return status;
}


/**
 * @brief  发送写使能命令
 * @retval HAL状态
 */
HAL_StatusTypeDef W25Q32_WriteEnable(void)
{
	uint8_t cmd=0x06;//写使能命令
	HAL_StatusTypeDef status;
	
	HAL_GPIO_WritePin(GPIOA,GPIO_PIN_4,GPIO_PIN_RESET);//拉低cs选择设备
	
	status=HAL_SPI_Transmit(&hspi1,&cmd,1,100);
	
	HAL_GPIO_WritePin(GPIOA,GPIO_PIN_4,GPIO_PIN_SET);//拉高cs释放设备
	
	return status;
}


/**
  * @brief  等待W25Q32就绪
  * @param  Timeout：整次等待的时间上限，单位ms，需大于0
  * @retval HAL_OK：已就绪
  *         HAL_TIMEOUT：等待超时
  *         其他值：SPI通信失败
  */
HAL_StatusTypeDef W25Q32_WaitReady(uint32_t Timeout)
{
	static volatile uint32_t waitTimeoutCount = 0U;
    static volatile uint32_t waitTimeoutLimitMs = 0U;
    static volatile uint32_t waitTimeoutElapsedMs = 0U;
    static volatile uint8_t waitTimeoutStatusReg = 0U;
	
	
	
	uint8_t cmd=0x05;
	uint8_t sr=0;//状态寄存器
	uint32_t start;
	uint32_t elapsed;
	HAL_StatusTypeDef status;
	
	if(Timeout==0)
	{
		return HAL_TIMEOUT;
	}
	
	start=HAL_GetTick();//获取当前时间
	
	HAL_GPIO_WritePin(GPIOA,GPIO_PIN_4,GPIO_PIN_RESET);//拉低cs选择设备
	
	status=HAL_SPI_Transmit(&hspi1,&cmd,1,Timeout);
	
	while(status==HAL_OK)
	{
		elapsed=HAL_GetTick()-start;
		
		if(elapsed>=Timeout)
		{
			waitTimeoutCount++;
            waitTimeoutLimitMs = Timeout;
            waitTimeoutElapsedMs = elapsed;
            waitTimeoutStatusReg = sr;
			
			status=HAL_TIMEOUT;
			break;
		}
		
		status=HAL_SPI_Receive(&hspi1,&sr,1,Timeout-elapsed);//接收最大等待时间用总剩余等待时间
		
		if (status!=HAL_OK)
		{
			break;
		}
		
		if((sr & 0x01)==0)//就绪时结束循环
		{
			break;
		}
		vTaskDelay(1);    // Flash 仍忙，等待一个 tick 后再检查
	}
	HAL_GPIO_WritePin(GPIOA,GPIO_PIN_4,GPIO_PIN_SET);//拉高cs释放设备
	
	return status;
}


/**
  * @brief  在同一页内写入数据，不自动擦除
  * @param  Address：Flash起始地址，范围0x000000～0x3FFFFF
  * @param  DataArray：待写入的数据
  * @param  Count：写入字节数，1～256，且不能跨页
  * @retval HAL状态
  * @note   调用前，目标区域应已擦除
  */
HAL_StatusTypeDef W25Q32_PageProgram(uint32_t Address,uint8_t *DataArray,uint16_t Count)//24位字节地址，包含页号和页内偏移
{
	uint8_t header[4];//存放命令和三个字节地址
	HAL_StatusTypeDef status;
	
	if(DataArray==NULL || Count==0 || Address>=0x400000UL)//0x400000UL 就表示“数值为十六进制 400000 的无符号长整数常量”。
	{
		return HAL_ERROR;
	}
	
	if(Count>(256U-(Address & 0xFFU)))//判断写入字节数有没有超过当前页剩余字节数
	{
		return HAL_ERROR;
	}
	
	status=W25Q32_WaitReady(500);//确认芯片当前就绪,如果没就绪最大等待时间500ms
	if(status!=HAL_OK)
	{
		return status;
	}
	
	status=W25Q32_WriteEnable();//写使能
	if(status!=HAL_OK)
	{
		return status;
	}
	
	header[0]=0x02;
	header[1]=(uint8_t)(Address>>16);
	header[2]=(uint8_t)(Address>>8);
	header[3]=(uint8_t)Address;
	
	HAL_GPIO_WritePin(GPIOA,GPIO_PIN_4,GPIO_PIN_RESET);//拉低cs选择设备
	
	status=HAL_SPI_Transmit(&hspi1,header,4,100);//发送写命令和页地址
	
	if(status==HAL_OK)
	{
		status=HAL_SPI_Transmit(&hspi1,DataArray,Count,100);
	}
	
	HAL_GPIO_WritePin(GPIOA,GPIO_PIN_4,GPIO_PIN_SET);//拉高cs释放设备
	
	if(status!=HAL_OK)
	{
		return status;
	}
	
	return W25Q32_WaitReady(100); // 等待本次页编程结束
	
}


/**
 * @brief  擦除一个4KB扇区
 * @param  Address：待擦除扇区内的任意字节地址，范围0x000000～0x3FFFFF
 * @retval HAL状态
 */
HAL_StatusTypeDef W25Q32_SectorErase(uint32_t Address)
{
	uint8_t header[4];
	HAL_StatusTypeDef status;
	
	if(Address>=0x400000UL)
	{
		return HAL_ERROR;
	}
	
	status=W25Q32_WaitReady(500);//确认芯片当前就绪,如果没就绪最大等待时间500ms
	if(status!=HAL_OK)
	{
		return status;
	}
	
	status=W25Q32_WriteEnable();//写使能
	if(status!=HAL_OK)
	{
		return status;
	}
	
	header[0]=0x20;
	header[1]=(uint8_t)(Address>>16);
	header[2]=(uint8_t)(Address>>8);
	header[3]=(uint8_t)Address;
	
	HAL_GPIO_WritePin(GPIOA,GPIO_PIN_4,GPIO_PIN_RESET);//拉低cs选择设备
	
	status=HAL_SPI_Transmit(&hspi1,header,4,100);// 发送扇区擦除命令和24位地址
	
	HAL_GPIO_WritePin(GPIOA,GPIO_PIN_4,GPIO_PIN_SET);//拉高cs释放设备
	
	if(status!=HAL_OK)
	{
		return status;
	}
	
	return W25Q32_WaitReady(500); // 等待本次扇区擦除完成
}


/**
 * @brief  从W25Q32连续读取数据
 * @param  Address：起始字节地址，范围0x000000～0x3FFFFF
 * @param  DataArray：接收缓冲区，容量至少为Count字节
 * @param  Count：读取字节数，范围1～65535，且不能超出芯片末尾
 * @retval HAL状态
 */
HAL_StatusTypeDef W25Q32_ReadData(uint32_t Address,uint8_t *DataArray,uint32_t Count)
{
	uint8_t header[4];
	HAL_StatusTypeDef status;
	
	if(DataArray==NULL || Count==0 || Count>65535UL)//因为 HAL_SPI_Receive() 的长度参数是 uint16_t，最大值为 65535。
	{
		return HAL_ERROR;
	}
	
	if(Address>=0x400000UL || Count>(0x400000UL-Address))//判断读取是否超出地址
	{
		return HAL_ERROR;
	}
	
	status=W25Q32_WaitReady(500); // 确认芯片当前就绪,防止芯片正在擦除或编程时，忽略普通读取指令。
	if(status!=HAL_OK)
	{
		return status;
	}
	
	header[0]=0x03;
	header[1]=(uint8_t)(Address>>16);
	header[2]=(uint8_t)(Address>>8);
	header[3]=(uint8_t)Address;
	
	HAL_GPIO_WritePin(GPIOA,GPIO_PIN_4,GPIO_PIN_RESET);//拉低cs选择设备
	
	status=HAL_SPI_Transmit(&hspi1,header,4,100);//发送读命令和起始地址
	
	if(status==HAL_OK)
	{
		status=HAL_SPI_Receive(&hspi1,DataArray,(uint16_t)Count,100);
	}
	
	HAL_GPIO_WritePin(GPIOA,GPIO_PIN_4,GPIO_PIN_SET);//拉高cs释放设备
	
	
	return status; //返回本次读取的状态
	
}


HAL_StatusTypeDef W25Q32_ReadStatus(uint8_t *statusRegister)
{
	uint8_t command=0x05U;//读状态寄存器 1
	HAL_StatusTypeDef status;
	
	if(statusRegister==NULL)
	{
		return HAL_ERROR;
	}
	
	HAL_GPIO_WritePin(GPIOA,GPIO_PIN_4,GPIO_PIN_RESET);//拉低cs选择设备
	
	status=HAL_SPI_Transmit(&hspi1,&command,1,100U);
	if(status==HAL_OK)
	{
		status=HAL_SPI_Receive(&hspi1,statusRegister,1U,100U);
	}
	
	HAL_GPIO_WritePin(GPIOA,GPIO_PIN_4,GPIO_PIN_SET);//拉高cs释放设备
	
	return status;
}


HAL_StatusTypeDef W25Q32_StartSectorErase(uint32_t Address)
{
	uint8_t header[4];
	uint8_t statusRegister;
	HAL_StatusTypeDef status;
	
	/* 地址必须在芯片范围内，并对齐到4KB扇区 */
	if(Address>=0x400000UL || (Address & 0xFFFU)!=0U)//地址没有对齐到扇区起始位置，这个函数就返回错误。
	{
		return HAL_ERROR;
	}
	
	status=W25Q32_ReadStatus(&statusRegister);
	if(status!=HAL_OK)
	{
		return status;
	}
	
	/* BUSY位为1，表示芯片还在执行操作 */
	if((statusRegister & 0x01U)!=0U)
	{
		return HAL_BUSY;
	}
	
	status=W25Q32_WriteEnable();
	if(status!=HAL_OK)
	{
		return status;
	}
	
    /* 确认写使能已经生效 */
	status=W25Q32_ReadStatus(&statusRegister);
	if(status!=HAL_OK)
	{
		return status;
	}
	
	if((statusRegister & 0x02U)==0U)
	{
		return HAL_ERROR;
	}
	
	header[0]=0x20U;//扇区擦除命令
	header[1]=(uint8_t)(Address>>16);
	header[2]=(uint8_t)(Address>>8);
	header[3]=(uint8_t)Address;
	
	HAL_GPIO_WritePin(GPIOA,GPIO_PIN_4,GPIO_PIN_RESET);
	status=HAL_SPI_Transmit(&hspi1,header,sizeof(header),100U);
	HAL_GPIO_WritePin(GPIOA,GPIO_PIN_4,GPIO_PIN_SET);
	
	return status;
}
