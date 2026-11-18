#include "DataLogger.h"
#include "W25Q32.h"
#include "FlashLayout.h"
#include "ModbusCRC.h"
#include <stddef.h>      //使用offsetof
#include <string.h>      //memset memcmp


#define LOG_START_ADDRESS 0x000000UL            //起始地址
#define LOG_SECTOR_SIZE 0x1000UL                // 每个扇区4096字节
#define LOG_END_ADDRESS FLASH_LOG_END_ADDRESS   // 
#define PAGE_SIZE   256U                        // 每页256字节
#define LOG_SLOT_SIZE  32U                      //每个Flash记录槽位32字节
#define LOG_RECORD_MAGIC   0x31434552UL         //日志记录标识
#define LOG_FORMAT_VERSION 1U                   //日志格式版本
#define LOG_COMMIT         0xA55AU              //写入完成标识
#define LOG_RECORD_CAPACITY  ((LOG_END_ADDRESS - LOG_START_ADDRESS)/LOG_SLOT_SIZE) //获取最大容量 

static uint32_t logAddress;
static uint32_t logSequence;
static volatile DataLogger_State_t logState=DATA_LOGGER_STATE_NOT_READY;



typedef struct
{
	uint32_t Magic;            //记录标识
	uint16_t Version;          //格式版本
	uint16_t Reserved;         //保留字段
	
	DataLogger_Record_t Data;  //原来的16字节采集数据
	
	uint32_t ReservedTail;     //保留字段，使整条记录占32字节
	uint16_t Crc;              //前28字节的CRC校验值
	uint16_t Commit;           //最后单独写入的完成标识
	
}DataLogger_FlashRecord_t;


/* 检查完整的 Flash 记录：1 有效，0 无效 */
static uint8_t DataLogger_RecordVail(const DataLogger_FlashRecord_t *Record)
{
	uint16_t  calculatedCrc;//计算CRC
	
	if(Record==NULL)
	{
		return 0U;
	}
	
	/* 检查记录标识、格式版本和写入完成标志 */
	if(Record->Magic!=LOG_RECORD_MAGIC || Record->Version!=LOG_FORMAT_VERSION || Record->Commit!=LOG_COMMIT)
	{
		return 0U;
	}
	/* 0 和 0xFFFFFFFF 不作为正常日志序号 */
	if(Record->Data.Sequence==0U || Record->Data.Sequence==0xFFFFFFFFUL)
	{
		return 0U;
	}
	/* 计算从结构体开头到 Crc 字段之前的 CRC */
	calculatedCrc=Modbus_CRC16((const uint8_t *)Record,(uint16_t)offsetof(DataLogger_FlashRecord_t,Crc));
	if(calculatedCrc!=Record->Crc)
	{
		return 0U;
	}
	
	return 1U;
}

/* 检查Flash槽位：1表示全部为FF，0表示非空 */
static uint8_t DataLogger_IsFlashRecordEmpty(const DataLogger_FlashRecord_t *Record)
{
	const uint8_t *bytes;
	uint32_t i;
	
	if(Record==NULL)
	{
		return 0U;
	}
	
	bytes=(const uint8_t *)Record;
	
	for(i=0U;i<sizeof(*Record);i++)
	{
		if(bytes[i]!=0xFFU)
		{
			return 0U;
		}
	}
	
	return 1U;
}

/* 仅传入已经检查合法的序号：1～0xFFFFFFFE */
static uint32_t DataLogger_SquenceAddress(uint32_t Squence)//返回对应地址
{
	return (uint32_t)(LOG_START_ADDRESS+((Squence-1U)%LOG_RECORD_CAPACITY*LOG_SLOT_SIZE));
}



HAL_StatusTypeDef DataLogger_Init(void)
{
	HAL_StatusTypeDef status;                  //状态位
	DataLogger_FlashRecord_t checkRecord;
	
	uint32_t sectorAddress;
	uint32_t address;
	uint32_t maxSequence=0;
	uint32_t latestSectorAddress=LOG_START_ADDRESS;
	
	/* 恢复成功前，禁止追加 */
	logState=DATA_LOGGER_STATE_NOT_READY;
	logAddress=LOG_END_ADDRESS;
	logSequence=0U;
	
	/* 1. 查找每个扇区的第一条有效记录，确定最新扇区 */
	for(sectorAddress=LOG_START_ADDRESS;sectorAddress<LOG_END_ADDRESS;sectorAddress+=LOG_SECTOR_SIZE)
	{
		for(address=sectorAddress;address<sectorAddress+LOG_SECTOR_SIZE;address+=LOG_SLOT_SIZE)
		{
			status=W25Q32_ReadData(address,(uint8_t *)&checkRecord,sizeof(checkRecord));
			if(status!=HAL_OK)
			{
				logState=DATA_LOGGER_STATE_ERROR;
				return status;
			}
			
			/* 顺序追加的扇区，遇到空白就结束本扇区查找 */
			if(DataLogger_IsFlashRecordEmpty(&checkRecord)!=0U)//1表示空
			{
				break;
			}
			
            /* 记录有效，并且序号与物理地址对应 */		
            if(DataLogger_RecordVail(&checkRecord)!=0 && 
				DataLogger_SquenceAddress(checkRecord.Data.Sequence)==address)
            {
				if(checkRecord.Data.Sequence>maxSequence)
				{
					maxSequence=checkRecord.Data.Sequence;
					latestSectorAddress=sectorAddress;
				}
				
				/* 已找到本扇区第一条有效记录 */
				break;
			}			
		}
	}
	
	/* 2. 完整扫描最新扇区，查找其中最大的有效序号 */
	if(maxSequence!=0U)
	{
		for(address=latestSectorAddress;address<latestSectorAddress+LOG_SECTOR_SIZE;
		    address+=LOG_SLOT_SIZE)
		{
			status=W25Q32_ReadData(address,(uint8_t *)&checkRecord,sizeof(checkRecord));
			if(status!=HAL_OK)
			{
				logState=DATA_LOGGER_STATE_ERROR;
				return status;
			}
			if(DataLogger_RecordVail(&checkRecord)!=0U && DataLogger_SquenceAddress(checkRecord.Data.Sequence)==address &&
				checkRecord.Data.Sequence>maxSequence)
			{
				maxSequence=checkRecord.Data.Sequence;
			}
		}
	}
	
	/* 3. 恢复下一条记录的序号和对应地址 */
	logSequence=maxSequence+1U;
	if(logSequence==0xFFFFFFFFUL)
	{
		logState=DATA_LOGGER_STATE_ERROR;
		return HAL_ERROR;
	}
	
	logAddress=DataLogger_SquenceAddress(logSequence);
	
	 /*
     * 4. 同一扇区内，跳过已经占用的槽位。
     * 到达新扇区开头后停止，由追加函数负责擦除。
     */
	while((logAddress - LOG_START_ADDRESS)%LOG_SECTOR_SIZE!=0U)
	{
		status=W25Q32_ReadData(logAddress,(uint8_t *)&checkRecord,sizeof(checkRecord));
		if(status!=HAL_OK)
		{
			logState=DATA_LOGGER_STATE_ERROR;
			return status;
		}
		
		/* 找到真正空白的槽位，可以作为下一次写入位置 */
		if(DataLogger_IsFlashRecordEmpty(&checkRecord)!=0U)
		{
			break;
		}
		
		/* 非空槽位不能直接重写，序号也必须一起跳过 */
		logSequence++;
		if(logSequence==0xFFFFFFFFUL)
		{
			logState=DATA_LOGGER_STATE_ERROR;
			return HAL_ERROR;
		}
		
		logAddress=DataLogger_SquenceAddress(logSequence);
	}
	
	logState=DATA_LOGGER_STATE_READY;
	return HAL_OK;
	
}



HAL_StatusTypeDef DataLogger_WriteRecord(uint32_t Address,DataLogger_Record_t *Record)
{
	HAL_StatusTypeDef status;
	DataLogger_FlashRecord_t pendingRecord; //等待写入FLASH
	DataLogger_FlashRecord_t checkRecord;   //校验
	uint16_t bodyLength;
	uint32_t i;
	
	if(Record==NULL || Address>=LOG_END_ADDRESS )
	{
		return HAL_ERROR;
	}
	
	if((Address - LOG_START_ADDRESS)%LOG_SLOT_SIZE!=0U || sizeof(pendingRecord)>(LOG_END_ADDRESS - Address) || Record->Sequence==0 || Record->Sequence==0xFFFFFFFFUL)
	{
		return HAL_ERROR;
	}
	
	/* 2. 原始读取目标槽位，检查32字节是否全部为FF */
	status=W25Q32_ReadData(Address,(uint8_t *)&checkRecord,sizeof(checkRecord));
    if(status !=HAL_OK)
	{
		return status;
	}
		
	for(i=0U;i<sizeof(checkRecord);i++)
	{
		if(((const uint8_t *)&checkRecord)[i]!=0xFFU)
		{
			return HAL_ERROR;
		}
	}
	
	/* 3. 整理完整记录，Commit暂时保持FFFF */
	memset(&pendingRecord,0xFF,sizeof(pendingRecord));//将0xFF填入
	
	pendingRecord.Magic=LOG_RECORD_MAGIC;
	pendingRecord.Version=LOG_FORMAT_VERSION;
	pendingRecord.Data=*Record;
	
	pendingRecord.Crc=Modbus_CRC16((const uint8_t *)&pendingRecord,(uint16_t)offsetof(DataLogger_FlashRecord_t,Crc));
	
	/* 正文长度为30字节，不包含最后的Commit */
	bodyLength=(uint16_t)offsetof(DataLogger_FlashRecord_t,Commit);
	
	/* 4. 写正文，并回读核对 */
	status=W25Q32_PageProgram(Address,(uint8_t *)&pendingRecord,bodyLength);
	if(status!=HAL_OK)
	{
		return status;
	}
	
	status=W25Q32_ReadData(Address,(uint8_t *)&checkRecord,bodyLength);
	if(status!=HAL_OK)
	{
		return status;
	}
	
	if(memcmp(&checkRecord,&pendingRecord,bodyLength)!=0)//比较两个结构体内容
	{
		return HAL_ERROR;
	}
	
	pendingRecord.Commit=LOG_COMMIT;
	
	status=W25Q32_PageProgram(Address+bodyLength,(uint8_t *)&pendingRecord.Commit,sizeof(pendingRecord.Commit));
	if(status!=HAL_OK)
	{
		return status;
	}
	
	/* 6. 回读完整32字节，检查实际写入结果 */
	status=W25Q32_ReadData(Address,(uint8_t *)&checkRecord,sizeof(checkRecord));
	if(status!=HAL_OK)
	{
		return status;
	}
	
	if(memcmp(&checkRecord,&pendingRecord,sizeof(checkRecord))!=0 || DataLogger_RecordVail(&checkRecord)==0U)
	{
		return HAL_ERROR;
	}
	
	return HAL_OK;		
}


HAL_StatusTypeDef DataLogger_ReadRecord(uint32_t Address,DataLogger_Record_t *Record)
{
	HAL_StatusTypeDef status;
	DataLogger_FlashRecord_t flashRecord;
	
	if(Record==NULL  || Address>=LOG_END_ADDRESS)
	{
		return HAL_ERROR;
	}
	
	/* 地址必须对齐到槽位起点，剩余空间应能容纳完整记录 */
	if((Address - LOG_START_ADDRESS)%LOG_SLOT_SIZE!=0U || sizeof(flashRecord)>(LOG_END_ADDRESS - Address))
	{
		return HAL_ERROR;
	}
	
	status=W25Q32_ReadData(Address,(uint8_t *)&flashRecord,sizeof(flashRecord));
	if(status!=HAL_OK)
	{
		return status;
	}
	
	/* 检查标识、版本、完成标志、序号和CRC */
	if(DataLogger_RecordVail(&flashRecord)==0U)
	{
		return HAL_ERROR;
	}
	
	/* 校验通过，才把内部的采样数据复制给调用者 */
	*Record=flashRecord.Data;
	
	return HAL_OK;
}


/* 追加记录：管理扇区擦除、记录序号和下一次写入地址 */
HAL_StatusTypeDef DataLogger_AppendRecord(DataLogger_Record_t *Record)
{
	HAL_StatusTypeDef status;//状态位
	
	/* 1. 检查参数和模块状态 */
	if(Record==NULL || logState!=DATA_LOGGER_STATE_READY)
	{
		return HAL_ERROR;
	}
	
	
	if(logAddress>=LOG_END_ADDRESS || (logAddress - LOG_START_ADDRESS)%LOG_SLOT_SIZE!=0U || 
	    LOG_SLOT_SIZE>(LOG_END_ADDRESS - logAddress) || logSequence==0U || logSequence==0xFFFFFFFFUL)
	{
		logState=DATA_LOGGER_STATE_ERROR;
		return HAL_ERROR;
	}
	
	/* 2. 进入一个扇区的第一个槽位时，先擦除该扇区 */
	if((logAddress - LOG_START_ADDRESS)%LOG_SECTOR_SIZE==0U)
	{
		status=W25Q32_SectorErase(logAddress);
		if(status!=HAL_OK)
		{
			logState=DATA_LOGGER_STATE_ERROR;
			return status;
		}	
	}
	
	/* 3. 分配序号，交给写入函数保存并核对 */
	Record->Sequence=logSequence;
	
	status=DataLogger_WriteRecord(logAddress,Record);
	if(status!=HAL_OK)
	{
		logState=DATA_LOGGER_STATE_ERROR;
		return status;
	}
	
	/* 4. 保存成功后，地址推进一个32字节槽位 */
	logAddress+=LOG_SLOT_SIZE;
	if(logAddress>=LOG_END_ADDRESS)
	{
		logAddress=LOG_START_ADDRESS;
	}
	
	/* 5. 更新下一条记录的序号 */
	logSequence++;
	
	/* 最大序号保留不用，到达后停止追加 */
	if(logSequence==0xFFFFFFFFUL)
	{
		logState=DATA_LOGGER_STATE_ERROR;
	}
	
	return HAL_OK;
}


/**
 * @brief  获取日志模块当前状态
 * @retval 日志状态：未就绪、可追加、已满或错误
 */
DataLogger_State_t DataLogger_GetState(void)
{
	return logState;
}


HAL_StatusTypeDef DataLogger_Format(void)//格式化日志区
{
	HAL_StatusTypeDef status;
	uint32_t address;
	
	logState=DATA_LOGGER_STATE_NOT_READY;//先将存储模块设置为未就绪状态，防止有数据写入
	
	for(address=LOG_START_ADDRESS;address<LOG_END_ADDRESS;address+=LOG_SECTOR_SIZE)//从起始地址开始，一个扇区一个扇区的增加，将地址增加到模块最大地址
	{
		status=W25Q32_SectorErase(address);//按扇区擦除存储模块
		if(status!=HAL_OK)
		{
			logState=DATA_LOGGER_STATE_ERROR;//擦除失败将存储模块状态设置为错误
			return status;
		}
	}
	
	return DataLogger_Init();
}



/**
 * @brief  按序号读取日志记录
 * @param  Sequence 要读取的记录序号，从 1 开始
 * @param  Record   接收记录的结构体指针
 * @retval HAL_OK：读取成功且序号匹配
 *         HAL_ERROR：参数错误、记录不存在或已被覆盖
 *         其他状态：底层读取返回的状态
 */
HAL_StatusTypeDef DataLogger_ReadBySequence(uint32_t Sequence,DataLogger_Record_t *Record)
{
	HAL_StatusTypeDef status;
	uint32_t address;
	DataLogger_Record_t checkRecord;
	
	if(Record==NULL || Sequence==0U || Sequence==0xFFFFFFFF)
	{
		return HAL_ERROR;
	}
	
	/* 根据序号，计算对应的32字节槽位地址 */
	address=DataLogger_SquenceAddress(Sequence);
	
	/* 读取并校验，取出其中的采样数据 */
	status=DataLogger_ReadRecord(address,&checkRecord);
	if(status!=HAL_OK)
	{
		return status;
	}
	
	/* 槽位会循环使用，确认里面仍是目标序号 */
	if(checkRecord.Sequence!=Sequence)
	{
		return HAL_ERROR;
	}
	
	*Record=checkRecord;
	
	return HAL_OK;
}


