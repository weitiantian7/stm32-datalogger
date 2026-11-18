#include "ModbusTask.h"
#include "FreeRTOS.h"
#include "task.h"
#include "usart.h"
#include "tim.h"
#include "RingBuffer.h"
#include "ModbusCRC.h"
#include "ModbusSlave.h"
#include "ModbusRegisters.h"
#include "SensorData.h"
#include "DataLogger.h"
#include "StorageTask.h"
#include "AppResources.h"


static TaskHandle_t modbusTaskHandle=NULL;

static uint8_t rxByte=0U;//接收字节
static uint8_t frameBuffer[256];//接收帧
static uint8_t modbusTxBuff[256];//回复帧
static uint16_t frameLength=0U;//帧长度
static uint8_t frameDiscard=0U;//帧丢弃

static volatile uint8_t rxFrameResetRequest=0U;//接收帧重启
static volatile uint8_t rxDiscardUntilGap=0U;//接收丢弃直到间隔
static volatile uint8_t uartRxRestartRequest=0U;//串口重新接收请求

static volatile uint32_t rxCount=0U,popCount=0U;//接收计数、出队计数
static volatile uint32_t rxDropCount=0U,rxFrameEdnCount=0U;//接收丢弃计数、接收帧结束计数
static volatile uint32_t uartErrorCount=0U;//串口错误计数
static volatile uint32_t uartRxRestartFailCount=0U;//串口接收重启失败计数
static volatile uint32_t rxQueueOverFlowCount=0U;//队列接收溢出计数
static volatile uint32_t rxResyncCount=0U;//接收重新同步计数
static volatile uint32_t uartLastError=HAL_UART_ERROR_NONE;//串口最新状态

static volatile uint32_t frameLengthErrorCount=0U;//帧长度错误计数
static volatile uint32_t crcPassCount=0U,crcFailCount=0U;//crc校验通过计数，crc失败计数
static volatile uint32_t modbusTxCount=0U,modbusTxErrorCount=0U;//modbus发送计数、modbus发送错误计数
static volatile HAL_StatusTypeDef modbusTxStatus=HAL_OK;//modbus发送状态

static volatile UBaseType_t modbusStackFreeWords=0U;//栈剩余字节
static volatile size_t modbusHeapFreeBytes=0U;//堆剩余字节




static void Modbus_RestartGapTimer(void)//重新开始帧间隔计时。
{
	HAL_TIM_Base_Stop_IT(&htim2);
	
	__HAL_TIM_SET_COUNTER(&htim2,0U); //设置计数单元
	__HAL_TIM_CLEAR_FLAG(&htim2,TIM_FLAG_UPDATE);//清除溢出标志位
	
	if(HAL_TIM_Base_Start_IT(&htim2)!=HAL_OK)//打开定时器2以及中断
	{
		Error_Handler();
	}
}

static void Modbus_ResetRxFrame(uint8_t waitForGap)//重置Modbus接收帧，waitForGap=是否等待报文间隔
{
	RingBuffer_Init();//初始化接收队列
	
	rxFrameResetRequest=1U;        //帧重新设置请求
	rxDiscardUntilGap=waitForGap;  // 设置“丢弃数据，直到出现帧间隔”的标志
	
	rxResyncCount++;               // 重新同步的次数加1
}

static void Modbus_ArmRx(void)//开启单字节接收，失败时留下重试请求。Arm：武装、就位、开启
{
	if(HAL_UART_Receive_IT(&huart1,&rxByte,1U)==HAL_OK)//开启串口接收中断
	{
		uartRxRestartRequest=0U;//将重启请求标志位恢复
	}
	else
	{
		uartRxRestartFailCount++;//重启失败次数增加
		uartRxRestartRequest=1U;
		
		Modbus_ResetRxFrame(1U);
		Modbus_RestartGapTimer();
	}
}

static void Modbus_ProcessRxRestart(void)//主循环处理接收重启请求。Process：处理
{
	uint32_t irqState;
	
	if(uartRxRestartRequest==0U)
	{
		return;
	}
	
	irqState=__get_PRIMASK();//获取中断状态
	__disable_irq();         //暂停中断
	
	if(uartRxRestartRequest!=0)
	{
		if(huart1.RxState==HAL_UART_STATE_READY)
		{
			__HAL_UART_CLEAR_OREFLAG(&huart1);//清除 USART1 的接收溢出错误标志
			
			Modbus_ResetRxFrame(1U);
			Modbus_RestartGapTimer();
			Modbus_ArmRx();
		}
		else if(huart1.RxState==HAL_UART_STATE_BUSY_RX)
		{
			/* 已经在等待接收，不需要重复开启 */
			uartRxRestartRequest=0U;
		}
	}
	
	__set_PRIMASK(irqState);
}



void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)//串口接收中断回调函数
{
	if(huart!=&huart1)
	{
		return;
	}
	
	rxCount++;//接收数+1
	Modbus_RestartGapTimer();//重新开始计时
	
	/*
    * 必须在重新开启接收之前记录错误。
    * HAL_UART_Receive_IT() 会清除 ErrorCode。
    */
	if(huart->ErrorCode!=HAL_UART_ERROR_NONE)//判断串口状态错误
	{
		uartLastError=huart->ErrorCode;//记录串口最新错误
		uartErrorCount++;//串口错误计数+1
		
		rxDropCount++;
		Modbus_ResetRxFrame(1U);
	}
	else if(rxDiscardUntilGap!=0U)//判断还处于丢弃状态
	{
		/* 受损帧尚未结束，继续丢弃 */
		rxDropCount++;//记录软件主动丢弃、没有入队的字节。
	}
	else if(RingBuffer_Push(rxByte)==0)//将接收到的字节压入接收队列中
	{
		rxDropCount++;//压入失败时丢弃数+1
		rxQueueOverFlowCount++;//接收队列溢出数+1
		
		Modbus_ResetRxFrame(1);//重置接收帧
	}
	
	Modbus_ArmRx();
}



void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)//串口错误回调函数
{
	if(huart!=&huart1 || huart->ErrorCode==HAL_UART_ERROR_NONE)
	{
		return;
	}
	
	uartLastError=huart->ErrorCode;//记录串口错误
	uartErrorCount++;//串口错误+1
	
	Modbus_ResetRxFrame(1U);
	Modbus_RestartGapTimer();
	__HAL_UART_CLEAR_OREFLAG(huart);//清除串口错误
	
	 /*
     * 溢出错误可能使 HAL 停止接收。
     * 只有已经停止时，才重新开启。
     */
	if(huart->RxState==HAL_UART_STATE_READY)//判断串口接收状态为就绪状态
	{
		Modbus_ArmRx();//开启单字节接收
	}
}



static void Modbus_UpdataInputSnapshot(void)//更新输入数据快照
{
	SensorData_t snapshot;//传感器数据快照
	ModbusInputData_t input={0};//输入寄存器结构体
	DataLogger_State_t Logstate;//日志模块状态
	uint32_t now;
	configStore_State_t ConfigState;//配置状态
	
	if(SensorData_GetSnapshot(&snapshot)!=0U)//判断获取传感器数据快照成功
	{
		now=HAL_GetTick();//记录获取时间
		
		if(snapshot.Valid!=0U && (uint32_t)(now - snapshot.UpdateTimeMS)<2500U)//判断温湿度数据有效并且采集时间合法
		{
			input.TemperatureX100=(int16_t)(snapshot.Temperature*100.0f+((snapshot.Temperature)>0.0f? 0.5f : -0.5f));//将温度转换写入输入寄存器
			input.HumidityX100=(uint16_t)(snapshot.Humidity*100.0f+0.5f);//将湿度转换写入输入寄存器
			
			input.ValidMask |=MODBUS_VALID_TEMPERATURE | MODBUS_VALID_HUMIDIY;//写入温湿度数据有效标记
			
			if(snapshot.AlarmActive!=0)//判断温度警告状态
			{
				input.Status |=MODBUS_STATUS_ALARM;//写入设备状态，温度警告
			}
		}
		else
		{
			input.Status |=MODBUS_STATUS_SENSOR_FAULT;//温湿度数据无效或采集超时，写入设备状态 传感器故障
		}
		
		if(snapshot.AdcValid!=0U && (uint32_t)(now - snapshot.AdcUpdateTimeMS)<2500U)//判断两路ADC数据有效且采集时间合法
		{
			input.Analog1mV=(uint16_t)(snapshot.Voltage1*1000.0f+0.5f);//将第一路电压转换并写入
			input.Analog2mV=(uint16_t)(snapshot.Voltage2*1000.0f+0.5f);//第二路电压转换并写入
			
			input.ValidMask |=MODBUS_VALID_ANALOG1 | MODBUS_VALID_ANALOG2;//写入两路ADC数据有效标记
		}
		else
		{
			input.Status |=MODBUS_STATUS_SENSOR_FAULT;//两路ADC数据无效或采集超时，写入设备状态 传感器故障
		}
	}
	else
	{
		input.Status |=MODBUS_STATUS_SENSOR_FAULT;//获取快照失败，写入设备状态 传感器故障
	}
	
	Logstate=DataLogger_GetState();//获取日志记录器状态
	ConfigState=Storage_GetConfigSaveState();//配置状态
	
	if(Logstate==DATA_LOGGER_STATE_READY && ConfigState!=CONFIG_STORE_ERROR)//判断日志状态为就绪，配置保存状态不为失败
	{
		input.Status |=MODBUS_STATUS_LOG_READY;//写入设备状态为就绪
	}
	else if(Logstate==DATA_LOGGER_STATE_ERROR || ConfigState==CONFIG_STORE_ERROR)
	{
		input.Status |=MODBUS_STATUS_SENSOR_FAULT;//写入设备状态为错误
	}
	
	ModbusRegisters_UpdateInput(&input);//将数据更新到输入寄存器
}



static void Modbus_ProcessRxQueue(void)//处理接收队列
{
	for(;;)
	{
		uint16_t Data;
		uint8_t popStatus;
		uint32_t irqState;
		
		irqState= __get_PRIMASK();//获取中断状态
		__disable_irq();//关闭中断
		
		if(rxFrameResetRequest!=0U)//检查是否需要放弃旧帧，重置拼帧状态,1时为需要
		{
			rxFrameResetRequest=0U;
			
			frameLength=0U;
			frameDiscard=0U;
		}
		
        popStatus=RingBuffer_Pop(&Data);//读取队列，获取一个元素
		__set_PRIMASK(irqState);//恢复原来的状态
		if(popStatus==0U)
		{
			break;//接收队列数据获取失败退出循环
		}
		
		if(popStatus==1)//判断有没有读取成功
		{
			if(Data==RING_BUFFER_FRAME_END)//判断读到的是不是帧末尾
			{
				if(frameDiscard==0U)//判断是不是丢弃帧，1为丢弃帧
				{
					if(frameLength<4U)//Modbus RTU 帧包括，从站地址 功能码  数据区 CRC校验(占两个字节)，最少4字节
					{
						frameLengthErrorCount++;// 长度异常次数
					}
					else if(Modbus_CheckCRC(frameBuffer,frameLength)==1U)//计算读取帧的CRC校验值
					{
						uint16_t responseLength;//应答帧长度
						crcPassCount++;	//校验通过
						
						Modbus_UpdataInputSnapshot();//更新输入寄存器数据
						
						responseLength=ModbusSlave_ProcessFrame(frameBuffer,frameLength,modbusTxBuff,sizeof(modbusTxBuff));//处理帧
						if(responseLength>0U)
						{
							HAL_GPIO_WritePin(GPIOA,GPIO_PIN_11,GPIO_PIN_SET);
							modbusTxStatus=HAL_UART_Transmit(&huart1,modbusTxBuff,responseLength,100U);//从机回复
							HAL_GPIO_WritePin(GPIOA,GPIO_PIN_11,GPIO_PIN_RESET);
							
							if(modbusTxStatus==HAL_OK)//判断发送成功
							{
								modbusTxCount++;
							}
							else
							{
								modbusTxErrorCount++;
							}
						}
					}
					else
					{
						crcFailCount++;//校验不通过
					}
				}
				
				/* 准备下一帧 */
                frameLength = 0U;
                frameDiscard = 0U;						
			 }
		     else
		     {
		     	popCount++;//读取数+1
		     	
		     	if(frameDiscard==0U)
				{
					if(frameLength<sizeof(frameBuffer))
					{
						frameBuffer[frameLength]=(uint8_t)Data;//将读到的字节放到到数组里拼成一帧
						frameLength++;
					}
					else
					{
						frameDiscard=1U;
						frameLengthErrorCount++;//错误长度+1
					}
				} 	
		     }
		 } 
	}
}

/**
 * @brief  Modbus帧间隙超时中断服务函数（TIM2溢出，3.5字符空闲检测）
 * @note   Modbus RTU依靠3.5字符空闲间隔判定一帧接收结束；
 *         串口每收到一个字节就重装载间隙定时器，长时间无新字节触发本中断
 * @retval 无
 * @attention 中断上下文，只能调用FromISR系列FreeRTOS API
 */
void Modbus_GapElapsedFromISR(void)
{
	BaseType_t higherPriorityTaskWoken=pdFALSE; //标记是否唤醒更高优先级任务，初始为未唤醒
	
	HAL_TIM_Base_Stop(&htim2);//停止计时
	rxFrameEdnCount++;//帧结束计数+1
	
	if(rxDiscardUntilGap!=0U)// 判断是否处于丢弃接收数据的状态
	{
		rxDiscardUntilGap=0U;// 间隙到，退出丢弃模式
	}
	else if(RingBuffer_Push(RING_BUFFER_FRAME_END)==0U)// 正常接收：往环形缓冲区写入【帧结束标记】
	{
		rxQueueOverFlowCount++;//接收队列溢出计数+1
		Modbus_ResetRxFrame(0U);//重置接收帧
	}
	
	if(modbusTaskHandle!=NULL)
	{
		vTaskNotifyGiveFromISR(modbusTaskHandle,&higherPriorityTaskWoken);//给任务发通知，并标记是否是更高级任务
		
		portYIELD_FROM_ISR(higherPriorityTaskWoken);// 如果唤醒了更高优先级任务，触发一次任务切换
	}	
}


void ModbusTask(void *pvParameters)
{
	(void)pvParameters;
	
	xEventGroupWaitBits(g_storageEvents,STORAGE_INIT_DONE_BIT,pdFALSE,pdTRUE,portMAX_DELAY);//参数二：设置等待比特位 参数三：pdFALSE不清除对应比特位 参数四：pdTRUE等待所有位就绪  等待存储任务完成事件位
	
	modbusTaskHandle=xTaskGetCurrentTaskHandle();//获取当前正在运行的任务的任务句柄，保存到 modbusTaskHandle
	
	RingBuffer_Init();//接收队列初始化
	ModbusRegisters_Init();//寄存器初始化
	
	HAL_GPIO_WritePin(GPIOA,GPIO_PIN_11,GPIO_PIN_RESET);//设置RS485为接收状态
	__HAL_UART_CLEAR_OREFLAG(&huart1);//清除串口接收溢出标志位
	Modbus_ArmRx();//重新开启串口接收中断
	
	for(;;)
	{
		ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(100));//阻塞等待任务通知（帧结束通知），最多等100ms
		
		Modbus_ProcessRxRestart();//接收重启
		Modbus_ProcessRxQueue();//
		
		modbusStackFreeWords=uxTaskGetStackHighWaterMark(NULL);
		modbusHeapFreeBytes=xPortGetFreeHeapSize();
	}
}


