#include "ConfigStore.h"
#include "DeviceConfig.h"
#include "FlashLayout.h"
#include "ModbusCRC.h"
#include "W25Q32.h"


#include <stddef.h>
#include <string.h>

#define CONFIG_MAGIC       0x31474643UL    //配置记录标识	判断读到的数据是不是本项目的配置记录
#define CONFIG_VERSION     1U              //配置格式版本号	判断这份记录的格式是否与当前代码兼容
#define CONFIG_COMMIT      0xA55AC33CUL    //写入完成标记	判断这份配置是否已经写完
#define CONFIG_TIMEOUT_MS  1000U           //等待超时阈值	等待 Flash 空闲或擦除完成时，超过规定时间就报告失败

typedef struct
{
	uint32_t Magic;          // 配置记录标识
	uint32_t Generation;     // 每成功保存一次，序号加一
	
	DeviceConfig_t Config;   // 实际配置
	
	uint16_t Version;        // 存储格式版本
	uint16_t Crc;            // 前面各字段的 CRC
	
	uint32_t Commit;         // 最后写入的完成标记
}ConfigRecord_t;

/* 当前记录应为 24 字节，结构变化时让编译器报错 */
typedef char ConfigRecordSizeCheck[sizeof(ConfigRecord_t)==24U?1:-1];//校验结构体是不是24字节

static volatile configStore_State_t storeState=CONFIG_STORE_IDLE;  // 配置保存状态机的当前状态，初始为空闲

static uint8_t storeReady=0U;        // 配置存储模块就绪标志
static uint8_t eraseStarted=0U;      // 擦除操作已启动标志

static uint32_t activeAddress=0U;    // 当前有效记录所在扇区
static uint32_t savedGeneration=0U;  // 最近一次有效保存的序号
static uint32_t targetAddress=0U;    // 本次准备写入的扇区
static uint32_t phaseTick=0;         // 当前阶段的计时起点，用于超时判断
 
static ConfigRecord_t pendingRecord;  // 本次待保存的配置记录




/* 检查一条配置记录是否有效 */
static uint8_t Configstore_RecordValid(const ConfigRecord_t *record)
{
	if(record->Magic!=CONFIG_MAGIC || record->Version!=CONFIG_VERSION || record->Commit!=CONFIG_COMMIT)
	{
		return 0U;
	}
	
	if(record->Crc!=Modbus_CRC16((const uint8_t *)record,(uint16_t)offsetof(ConfigRecord_t,Crc)))
	{                                                            //offsetof(ConfigRecord_t, Crc)获取 Crc 成员距离结构体开头的字节数
		return 0U;
	}
	
	return DeviceConfig_Validate(&record->Config);//检查配置是否合法
}

/* 比较保存序号，同时处理 uint32_t 回绕 */
static uint8_t ConfigStore_IsNewer(uint32_t a, uint32_t b)
{
	uint32_t difference=a-b;
	
	return (difference!=0U && difference<0x80000000UL)?1U:0U;
}

/* 上电读取两个扇区，加载其中最新的有效配置 */
HAL_StatusTypeDef ConfigStore_Init(void)
{
	ConfigRecord_t recordA;
	ConfigRecord_t recordB;
	
	const ConfigRecord_t *selected=NULL;//存放待选配置
	
	uint8_t validA;
	uint8_t validB;
	
	HAL_StatusTypeDef status;
	
	storeReady=0U;
	storeState=CONFIG_STORE_IDLE;
	activeAddress=0U;
	savedGeneration=0U;
	
	status=W25Q32_ReadData(FLASH_CONFIG_A_ADDRESS,(uint8_t *)&recordA,sizeof(recordA));
	if(status!=HAL_OK)
	{
		storeState=CONFIG_STORE_ERROR;
		return status;
	}
	
	status=W25Q32_ReadData(FLASH_CONFIG_B_ADDRESS,(uint8_t *)&recordB,sizeof(recordB));
	if(status!=HAL_OK)
	{
		storeState=CONFIG_STORE_ERROR;
		return status;
	}
	
	validA=Configstore_RecordValid(&recordA);// 检查配置是否有效且合法
	validB=Configstore_RecordValid(&recordB);// 检查配置是否有效且合法
	
	if(validA!=0U)
	{
		selected=&recordA;
		activeAddress=FLASH_CONFIG_A_ADDRESS;// 本次准备写入的扇区
	}
	if(validB!=0U && (validA==0U || ConfigStore_IsNewer(recordB.Generation,recordA.Generation)!=0U))
	{
		selected=&recordB;
		activeAddress=FLASH_CONFIG_B_ADDRESS;
	}
	
	if(selected!=NULL)
	{
		 if(DeviceConfig_Apply(&selected->Config)==0U)//将最新的配置恢复
		 {
			 storeState=CONFIG_STORE_ERROR;
			 return HAL_ERROR;
		 }
		 
		 savedGeneration=selected->Generation;//更新最近一次保存配置的序号
	}
	 
	 /*
     * 两个扇区都没有有效记录时，
     * 保留 DeviceConfig_Init() 设置的默认配置。
     */
	 storeReady=1U;
	 return HAL_OK;
}


/* 复制当前配置，提交保存请求；这里不执行 Flash 擦写 */
HAL_StatusTypeDef ConfigStore_RequestSave(const DeviceConfig_t *config)
{
	DeviceConfig_t snapshot;
	
	if(storeReady==0U || storeState==CONFIG_STORE_ERROR)
	{
		return HAL_ERROR;
	}
	
	if(storeState==CONFIG_STORE_BUSY)
	{
		return HAL_BUSY;
	}
	
	if(config==NULL)
	{
		return HAL_ERROR;
	}
	
	snapshot=*config;
	
	if(DeviceConfig_Validate(&snapshot)==0)
	{
		return HAL_ERROR;
	}
	
	memset(&pendingRecord,0xFF,sizeof(pendingRecord));  //从这个结构体的起始地址开始填充,每个字节都填成 FF,	一共填充多少字节，即整个结构体的大小
	
	pendingRecord.Magic=CONFIG_MAGIC;
	pendingRecord.Generation=savedGeneration+1U;
	pendingRecord.Config=snapshot;
	pendingRecord.Version=CONFIG_VERSION;
	
	pendingRecord.Crc=Modbus_CRC16((const uint8_t *)&pendingRecord,(uint16_t)offsetof(ConfigRecord_t,Crc));
	
	/* 当前有效记录在 A，就写 B；否则写 A */
	targetAddress=(activeAddress==FLASH_CONFIG_A_ADDRESS)?FLASH_CONFIG_B_ADDRESS:FLASH_CONFIG_A_ADDRESS;
	
	eraseStarted=0U;
	phaseTick=HAL_GetTick();//获取起始时间
	
	storeState=CONFIG_STORE_BUSY;//更新状态为正在保存
	
	return HAL_OK;
	
}


/* 在主循环中反复调用，推进保存过程 */
void ConfigStore_Process(void)
{
	ConfigRecord_t checkRecord;
	HAL_StatusTypeDef status;
	uint8_t statusRegister;
	uint16_t i;
	
	uint16_t bodyLength=(uint16_t)offsetof(ConfigRecord_t,Commit);//获取完成标志前面的字节数
	
	if(storeState!=CONFIG_STORE_BUSY)
	{
		return;
	}
	
	/* 第一步：启动目标扇区擦除 */
	if(eraseStarted==0U)
	{
		status=W25Q32_StartSectorErase(targetAddress);//擦除待写入扇区的数据
		if(status==HAL_BUSY)
		{
			if((uint32_t)(HAL_GetTick() - phaseTick)>=CONFIG_TIMEOUT_MS)//当前时间 - 申请保存起始时间 >设定的超时时间
			{
				storeState=CONFIG_STORE_ERROR;
			}
			
			return;
		}
		
		if(status!=HAL_OK)
		{
			storeState=CONFIG_STORE_ERROR;
			return;
		}
		
		eraseStarted=1U;
		phaseTick=HAL_GetTick();
		
		return;
	}
	
	  /* 第二步：查询擦除是否完成 */
	status=W25Q32_ReadStatus(&statusRegister);
	if(status!=HAL_OK)
	{
		storeState=CONFIG_STORE_ERROR;
		return;
	}
	
	if((statusRegister & 0x01U)!=0)
	{
		if((uint32_t)(HAL_GetTick() - phaseTick)>=CONFIG_TIMEOUT_MS)//当前时间 — 擦除命令开始时间
		{
			storeState=CONFIG_STORE_ERROR;
		}
		
		return;
	}
	
	/* 第三步：确认记录所在区域已经擦成 0xFF */
	status=W25Q32_ReadData(targetAddress,(uint8_t *)&checkRecord,sizeof(checkRecord));
	if(status!=HAL_OK)
	{
		storeState=CONFIG_STORE_ERROR;
		return;
	}
	
	for(i=0U;i<sizeof(checkRecord);i++)
	{
		if(((const uint8_t *)&checkRecord)[i]!=0xFFU)
		{
			storeState=CONFIG_STORE_ERROR;
			return;
		}
	}
	
	/* 第四步：写入正文，暂时不写完成标记 */
	status=W25Q32_PageProgram(targetAddress,(uint8_t *)&pendingRecord,bodyLength);//将待存储数据写入
	if(status==HAL_OK)
	{
		status=W25Q32_ReadData(targetAddress,(uint8_t *)&checkRecord,bodyLength);//如果数据写入成功，就将写入的数据重新读取出来
	}
	
	if(status!=HAL_OK || memcmp(&checkRecord,&pendingRecord,bodyLength)!=0)//按字节比较两块内存
	{
		storeState=CONFIG_STORE_ERROR;
		return;
	}
	
	/* 第五步：正文核对正确后，再写完成标记 */
	pendingRecord.Commit=CONFIG_COMMIT;
	
	status=W25Q32_PageProgram(targetAddress+bodyLength,(uint8_t *)&pendingRecord.Commit,sizeof(pendingRecord.Commit));//写入完成标记
	if(status==HAL_OK)
	{
		status=W25Q32_ReadData(targetAddress,(uint8_t *)&checkRecord,sizeof(checkRecord));
	}
	
	if(status!=HAL_OK || memcmp(&checkRecord,&pendingRecord,sizeof(checkRecord))!=0 || Configstore_RecordValid(&checkRecord)==0U)
	{
		storeState=CONFIG_STORE_ERROR;
		return;
	}
	
	/* 全部成功后，才切换当前有效记录 */
	activeAddress=targetAddress;//将运行配置切换到新的
	savedGeneration=pendingRecord.Generation;
	
	storeState=CONFIG_STORE_SAVED;
}



uint8_t ConfigStore_IsBusy(void)
{
	return (storeState==CONFIG_STORE_BUSY)?1U:0U;
}



configStore_State_t ConfigStore_GetState(void)
{
	return storeState;
}

void ConfigStore_StopOnFault(void)//故障时停止存储，置错误状态
{
	storeReady=0U;//清除存储就绪标志
	storeState=CONFIG_STORE_ERROR;//设置存储为错误状态
}





























































