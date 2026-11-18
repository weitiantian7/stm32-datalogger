#include "AnalogInput.h"
#include "adc.h"
#include "dma.h"
#include "tim.h"

#define ADC_SAMPLES_PER_WINDOW 100U

static volatile uint16_t Data[4];// DMA缓冲区   static将数组限制在当前模块内部，volatile提示编译器数据可能被DMA更新。
static volatile uint16_t LatestData[2];// 最近一对完整结果
static volatile uint8_t DataReady=0; // 采样完成标志

/* 正在累计的窗口 */
static volatile uint32_t SampleSum1=0U;
static volatile uint32_t SampleSum2=0U;
static volatile uint32_t SampleCount=0U;

/* 已完成窗口的结果，留给主循环读取 */
static volatile uint32_t CompletedSum1=0U;
static volatile uint32_t CompletedSum2=0U;
static volatile uint8_t AverageReady=0U;


/* 已完成的窗口数量，方便观察采样速度 */
static volatile uint32_t CompletedWindows=0U;


/**
 * @brief  启动 ADC1 双通道定时采集及 DMA 数据搬运
 *
 * @return HAL_OK：所有启动步骤成功；
 *         其他值：返回发生失败的底层 HAL 函数状态。
 *
 * @note   调用前需完成 ADC1、DMA 和 TIM3 的初始化，
 *         并配置好双通道扫描、TIM3 TRGO 触发及 DMA 循环模式。
 * @note   执行顺序：ADC 自校准 → 启动 ADC/DMA → 启动 TIM3。
 *         若 TIM3 启动失败，则停止已经启动的 ADC/DMA。
 * @note   当前设计用于初始化后调用一次，不要在主循环中反复调用。
 *         HAL_OK 仅表示启动成功，不代表首次采样已经完成。
 */
HAL_StatusTypeDef AnalogInput_Start(void)
{
	HAL_StatusTypeDef status;
	DataReady=0;
	
	SampleSum1=0U;
	SampleSum2=0U;
	SampleCount=0U;
	AverageReady=0U;
	CompletedWindows=0;
	
	
	status=HAL_ADCEx_Calibration_Start(&hadc1);//执行ADC自动自校准
	if(status!=HAL_OK)
	{
		return status;
	}
	
	status=HAL_ADC_Start_DMA(&hadc1,(uint32_t *) Data,4);//准备ADC和DMA，等待定时器触发
	if(status!=HAL_OK)
	{
		return status;
	}
	
	status=HAL_TIM_Base_Start(&htim3);// 启动定时器，定时器开始计数，周期性输出TRGO
	if(status!=HAL_OK)
	{
		HAL_ADC_Stop_DMA(&hadc1);
		return status;
	}
	
	return status;
}



/**
 * @brief  读取两路 ADC 当前的原始转换值
 *
 * @param[out] Data1  保存 Rank 1 转换结果的变量地址。
 * @param[out] Data2  保存 Rank 2 转换结果的变量地址。
 *
 * @retval HAL_OK     已将缓冲区数据复制到输出变量。
 * @retval HAL_ERROR  任一输出指针为 NULL。
 *
 * @note   ADC 配置为 12 位、右对齐时，有效转换值范围为 0～4095。
 * @note   本函数不启动转换，也不等待新数据。
 *         返回 HAL_OK 不代表本轮采样刚刚完成或采集硬件没有故障。
 * @note   当前直接依次读取 DMA 缓冲区，未保证两路数据属于同一轮扫描。
 */
HAL_StatusTypeDef AnalogInput_GetRaw(uint16_t *Data1,uint16_t *Data2)
{
	uint32_t primask;
	uint16_t raw1;
	uint16_t raw2;
	
	if(Data1==NULL || Data2==NULL)
	{
		return HAL_ERROR;
	}
	
	primask=__get_PRIMASK();//保存原中断状态
	__disable_irq();//短暂禁止中断
	
	if(DataReady==0)//1表示采样完成
	{
		__set_PRIMASK(primask);//恢复中断状态
		return HAL_BUSY;
	}
	
	 /* 防止读到一半时，回调更新了这对结果 */
	raw1=LatestData[0];//将DMA获取到的最新数据写入到临时变量中
	raw2=LatestData[1];
	
	__set_PRIMASK(primask);
	
	*Data1=raw1;
	*Data2=raw2;
	
	return HAL_OK;
}


HAL_StatusTypeDef AnalogInput_GetAverageRaw(uint16_t *Average1,uint16_t *Average2)
{
	uint32_t primask;
	uint32_t sum1;
	uint32_t sum2;
	
	if(Average1==NULL || Average2==NULL)
	{
		return HAL_ERROR;
	}
	
	primask= __get_PRIMASK();
	__disable_irq();
	
	if(AverageReady==0)
	{
		__set_PRIMASK(primask);
		return HAL_BUSY;
	}
	
	/* 一次取出同一个窗口的两路累计值 */
	sum1=CompletedSum1;
	sum2=CompletedSum2;
	AverageReady=0U;
	
	__set_PRIMASK(primask);
	
	/* 恢复中断后再计算，整数四舍五入 */
	*Average1=(uint16_t)((sum1+ADC_SAMPLES_PER_WINDOW/2U)/ADC_SAMPLES_PER_WINDOW);
	*Average2=(uint16_t)((sum2+ADC_SAMPLES_PER_WINDOW/2U)/ADC_SAMPLES_PER_WINDOW);
	
	return HAL_OK;
	
}



/**
 * @brief  读取两路 ADC 数据并换算为浮点电压
 *
 * @param[out] Voltage1  保存 Rank 1 电压的变量地址，单位 V。
 * @param[out] Voltage2  保存 Rank 2 电压的变量地址，单位 V。
 *
 * @return HAL_OK：读取及换算完成；
 *         HAL_ERROR：任一输出指针为 NULL；
 *         原始值读取失败时，返回 AnalogInput_GetRaw() 的状态。
 *
 * @note   按 12 位 ADC、参考电压 3.30 V 计算：
 *         电压 = 原始值 × 3.30 / 4096。
 *         实际参考电压不同时，应调整换算系数。
 * @note   本函数保留浮点计算结果，不处理显示小数位。
 *         数据更新及同步限制与 AnalogInput_GetRaw() 相同。
 */
HAL_StatusTypeDef AnalogInput_GetVoltage(float *Voltage1,float *Voltage2)
{
	HAL_StatusTypeDef status;
	uint16_t RawData[2];
	
	if(Voltage1==NULL || Voltage2==NULL)
	{
		return HAL_ERROR;
	}
	
	status=AnalogInput_GetRaw(&RawData[0],&RawData[1]);//获取原始数据
	
	if(status!=HAL_OK)
	{
		return status;
	}
	
	*Voltage1=(RawData[0]*3.30f/4096.0f);
	*Voltage2=(RawData[1]*3.30f/4096.0f);
	
	return status;
}



/*计算积累值*/
static void AnalogInput_Accumulate(uint16_t raw1,uint16_t raw2)
{
	SampleSum1+=raw1;
	SampleSum2+=raw2;
	SampleCount++;
	
	if(SampleCount==ADC_SAMPLES_PER_WINDOW)
	{
		/* 保存刚完成的这一窗口 */
		CompletedSum1=SampleSum1;
		CompletedSum2=SampleSum2;
		AverageReady=1U;
		CompletedWindows++;
		
		/* 开始累计下一窗口 */
		SampleSum1=0U;
		SampleSum2=0U;
		SampleCount=0U;
	}
}



void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)//获取半字中断
{
	if(hadc->Instance==ADC1)
	{
		LatestData[0]=Data[0];
		LatestData[1]=Data[1];
		
		AnalogInput_Accumulate(LatestData[0],LatestData[1]);
		
		DataReady=1;
	}	
}



void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)//获取字中断
{
	if(hadc->Instance==ADC1)
	{
		LatestData[0]=Data[2];
		LatestData[1]=Data[3];
		
		AnalogInput_Accumulate(LatestData[0],LatestData[1]);
		
		DataReady=1;
	}
}
