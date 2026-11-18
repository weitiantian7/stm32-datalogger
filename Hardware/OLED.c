#include "OLED.h"
#include "i2c.h"
#include "OLED_Font.h"
#include "string.h"
#include <stdio.h>

#define OLED_I2C_TIMEOUT_MS 10U

/* 本轮显示是否允许继续发送 */
static uint8_t oledWriteAllowed=1U; //Allowed：允许

/* 每轮刷新开始时，允许重新尝试通信 */
void OLED_BeginUpdate(void)
{
	oledWriteAllowed=1U;
}


/* 本轮出现一次发送失败，就跳过剩余发送 */
static HAL_StatusTypeDef OLED_Transmit(uint8_t *data,uint16_t length)
{
	HAL_StatusTypeDef status;
	if(oledWriteAllowed==0U)
	{
		return HAL_ERROR;
	}
	
	status=HAL_I2C_Master_Transmit(&hi2c1,0x3CU<<1,data,length,OLED_I2C_TIMEOUT_MS);
	if(status!=HAL_OK)           //0x3c是从机的7位地址，在一个字节里面左移一位放读写位
	{
		oledWriteAllowed=0U;
	}
	
	return status;
	
}
	


/**
  * @brief  OLED写命令
  * @param  Command 要写入的命令
  * @retval 无
  */
void OLED_WriteCommand(uint8_t Command)
{
	uint8_t buf[]={0x00,Command};
	
	OLED_Transmit(buf,2U);
	
}

/**
  * @brief  OLED写数据
  * @param  Data 要写入的数据
  * @retval 无
  */
void OLED_WriteData(uint8_t Data)
{
	uint8_t buf[]={0x40,Data};
	OLED_Transmit(buf,sizeof(buf));
}

/**
  * @brief  OLED设置光标位置
  * @param  Y 以左上角为原点，向下方向的坐标，范围：0~7
  * @param  X 以左上角为原点，向右方向的坐标，范围：0~127
  * @retval 无
  */
void OLED_SetCursor(uint8_t Y,uint8_t X)
{
	OLED_WriteCommand(0xB0 | Y);//0xB0字节低三位表示页码，范围0~7
	OLED_WriteCommand(0x10 | (X>>4));//存放列的高四位
	OLED_WriteCommand(0x00 | (X & 0x0F));//列低四位
}

/**
  * @brief  OLED清屏
  * @param  无
  * @retval 无
  */
void OLED_Clear(void)
{
	uint8_t buf[129]={0x40};
	uint8_t page;
	for(page=0;page<8;page++)
	{
		OLED_SetCursor(page,0);
		OLED_Transmit(buf,sizeof(buf));
	}
}

/**
  * @brief  OLED显示一个字符
  * @param  Line 行位置，范围：1~4
  * @param  Column 列位置，范围：1~16
  * @param  Char 要显示的一个字符，范围：ASCII可见字符
  * @retval 无
  */
void OLED_ShowChar(uint8_t Line,uint8_t Column,char Char)
{
	uint8_t half;
	uint8_t buf[9]={0x40}; //控制字节+8字节字模
	
	for(half=0U;half<2U;half++)
	{
		memcpy(&buf[1],&OLED_F8x16[Char - ' '][half*8U],8U);
		
		OLED_SetCursor((Line-1U)*2U+half,(Column-1U)*8U);
		
		if(OLED_Transmit(buf,sizeof(buf))!=HAL_OK)
		{
			return;
		}
	}
}

/**
  * @brief  OLED显示字符串
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  String 要显示的字符串，范围：ASCII可见字符
  * @retval 无
  */
void OLED_ShowString(uint8_t Line,uint8_t Column,char *String)
{
	uint8_t i;
	for(i=0;String[i]!='\0';i++)
	{
		OLED_ShowChar(Line,Column+i,String[i]);
	}
	
}

/**
  * @brief  OLED次方函数
  * @retval 返回值等于X的Y次方
  */
uint32_t OLED_Pow(uint32_t X,uint32_t Y)
{

	uint32_t i;
	uint32_t Result=1;
	for(i=0;i<Y;i++)
	{
		Result*=X;
	}
	return Result;
}

/**
  * @brief  OLED显示数字（十进制，正数）
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  Number 要显示的数字，范围：0~4294967295
  * @param  Length 要显示数字的长度，范围：1~10
  * @retval 无
  */
void OLED_ShowNum(uint8_t Line,uint8_t Column,uint32_t Number,uint8_t Length)
{
	uint8_t i;
	for(i=0;i<Length;i++)
	{
		OLED_ShowChar(Line,Column+i,(Number/OLED_Pow(10,Length-1-i))%10+'0');
	}
}

/** 
  * @brief  OLED显示数字（十进制，带符号数）
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  Number 要显示的数字，范围：-2147483648~2147483647
  * @param  Length 要显示数字的长度，范围：1~10
  * @retval 无
  */
void OLED_ShowSignedNum(uint8_t Line, uint8_t Column,int32_t Number,uint8_t Length)
{
	uint32_t Number1;
	if(Number>=0)
	{
		OLED_ShowChar(Line,Column,'+');
		Number1=Number;
	}
	else
	{
		OLED_ShowChar(Line,Column,'-');
		Number1=-Number;
	}
	
	OLED_ShowNum(Line,Column+1,Number1,Length);
}


/**
  * @brief  OLED显示数字（十六进制，正数）
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  Number 要显示的数字，范围：0x00000000 ~ 0xFFFFFFFF
  * @param  Length 要显示数字的长度，范围：1~16
  * @retval 无
  */
void OLED_ShowHexNum(uint8_t Line, uint8_t Column,int32_t Number,uint8_t Length)
{
	uint8_t i,SingleNumber;
	for(i=0;i<Length;i++)
	{
		SingleNumber=Number / OLED_Pow(16, Length - i - 1) % 16;
		if(SingleNumber<10)
		{
			OLED_ShowChar(Line, Column + i, SingleNumber + '0');
		}
		else
		{
			OLED_ShowChar(Line, Column + i, SingleNumber - 10 + 'A');
		}
	}
}


/**
  * @brief  OLED显示数字（二进制，正数）
  * @param  Line 起始行位置，范围：1~4
  * @param  Column 起始列位置，范围：1~16
  * @param  Number 要显示的数字，范围：0~1111 1111 1111 1111
  * @param  Length 要显示数字的长度，范围：1~16
  * @retval 无
  */
void OLED_ShowBinNum(uint8_t Line,uint8_t Column,uint32_t Number,uint8_t Length)
{
	uint8_t i;
	for(i=0;i<Length;i++)
	{
		OLED_ShowChar(Line,Column+i,Number/OLED_Pow(2,Length-i-1)%2+'0');
	}
}

/**
 * @brief 显示浮点数，自动处理负号和小数位舍入
 * @param Line      行号
 * @param Column    起始列
 * @param Number    要显示的数值
 * @param Width     显示总宽度，包含负号和小数点
 * @param Precision 小数点后的位数，支持 0～6
 */
void OLED_ShowFloat(uint8_t Line, uint8_t Column, float Number,uint8_t Width, uint8_t Precision)
{
	char text[32];
    int length;
    uint8_t i;
	
	if (Width == 0 || Width > 16 || Precision > 6)
    {
        return;
    }
	
	length = snprintf(text, sizeof(text), "%*.*f",(int)Width, (int)Precision, (double)Number);
	
	if (length < 0 || length > Width)
    {
        for (i = 0; i < Width; i++)
        {
            text[i] = '#';
        }
        text[Width] = '\0';
    }

    OLED_ShowString(Line, Column, text);
}


/*
 * Line：第1～4行
 * Column：第1～15列，每列宽8像素，一个汉字占两列
 * Text：一个UTF-8编码的常用汉字，例如"温"
 *
 * 字模格式：前16字节是上半部分，后16字节是下半部分
 */
void OLED_ShowChineseChar(uint8_t Line,uint8_t Column,const char *Text)
{
	
	uint16_t k;
	uint16_t count=sizeof(ChineseFont)/sizeof(ChineseFont[0]);
		
	uint8_t half;
	uint8_t buf[17]={0x40};//第一个元素是OLED写入数据命令，后面的值默认为0用来存放字模数据
	
	if(Line<1||Line>4||Column<1||Column>15)//检查显示位置
	{
		return;
	}
	
	if(Text==NULL||Text[0]=='\0'||Text[1]=='\0'||Text[2]=='\0')//按照一个汉族占用三个字节处理
	{
		return;
	}
	
	for(k=0;k<count;k++)//查找汉字
	{
		if(memcmp(Text,ChineseFont[k].text,3)==0)//比较Text跟ChineseFont[k].text中前三个元素的值是否相等，都相等返回0
		{
			break;//找到了，k就是对应的数组下标,如果循环完都没有找到k=count
		}
	}
	
	if(k==count) // 没找到这个汉字
	{
		return;
	}
	
	for(half=0;half<2;half++) // 发送这个汉字的字模，上、下各发送16字节
	{
		OLED_SetCursor((Line-1)*2+half,(Column-1)*8);
		
		
		memcpy(&buf[1],&ChineseFont[k].bitmap[half*16],16);//memcpy(目标地址, 源地址, 复制的字节数);
		
		
		if(OLED_Transmit(buf,sizeof(buf))!=HAL_OK)
		{
			return;
		}
		
		
	}
	
	
}


/*
 * Line：1～4
 * Column：1～15，每个汉字占两列
 * Text：纯中文字符串，每个汉字为3字节UTF-8编码
 * 超过屏幕右边界时停止显示，不自动换行
 */
void OLED_ShowChineseString(uint8_t Line,uint8_t Column,const char *Text)
{
	if(Text==NULL||Line<1||Line>4||Column<1||Column>15)
	{
		return;
	}
	
	while(Column<=15 && Text[0]!='\0')
	{
		if(Text[1]=='\0'||Text[2]=='\0')
		{
			break;
		}
		OLED_ShowChineseChar(Line,Column,Text);
		
		Text+=3;
		Column+=2;		
	}
}


/**
  * @brief  OLED显示中英文、数字及符号混合字符串
  * @param  Line：起始行，范围1～4，每行高16像素
  * @param  Column：起始列，范围1～16，每列宽8像素
  * @param  Text：以'\0'结尾的UTF-8字符串
  * @retval 无
  * @note   英文、数字及半角符号占1列；16×16汉字占2列
  * @note   支持可显示的ASCII字符及3字节UTF-8字符
  * @note   非ASCII字符需要在字库中有对应字模
  * @note   到达右边界或遇到不完整、不支持的字符格式时停止，不自动换行
  */
void OLED_ShowText(uint8_t Line,uint8_t Column,const char *Text)
{
	uint8_t ch;
	
	if(Text==NULL||Line<1||Line>4||Column<1||Column>16)
	{
		return;
	}
	
	while(Column<=16 && Text[0]!='\0') //// 遍历字符串，逐个判断并显示ASCII或三字节UTF-8字符
	{
		ch=(uint8_t)Text[0];
		
		if(ch>=0x20 && ch<=0x7E)
		{
			OLED_ShowChar(Line,Column,Text[0]);
			Text+=1;
			Column+=1;	
		}
		else if((ch & 0xF0)==0xE0)//UTF-8 三字节字符的格式是：1110xxxx  10xxxxxx  10xxxxxx
		{
			if(Column>15 || Text[1]=='\0' || Text[2]=='\0')
			{
				return;
			}			

			if(((uint8_t)Text[1] & 0xc0)!=0x80 || ((uint8_t)Text[2] & 0xc0)!=0x80)
			{
				return;
			}
			
			OLED_ShowChineseChar(Line,Column,Text);
			Text+=3;
			Column+=2;
		}
		else
		{
			return;
		}
		
	}
	
}



/**
  * @brief  OLED初始化
  * @param  无
  * @retval 无
  */
void OLED_Init(void)
{
	HAL_Delay(100);
	
	OLED_WriteCommand(0xAE);	//关闭显示
	
	OLED_WriteCommand(0xD5);	//设置显示时钟分频比/振荡器频率
	OLED_WriteCommand(0x80);
	
	OLED_WriteCommand(0xA8);	//设置多路复用率
	OLED_WriteCommand(0x3F);
	
	OLED_WriteCommand(0xD3);	//设置显示偏移
	OLED_WriteCommand(0x00);
	
	OLED_WriteCommand(0x40);	//设置显示开始行
	
	OLED_WriteCommand(0xA1);	//设置左右方向，0xA1正常 0xA0左右反置
	
	OLED_WriteCommand(0xC8);	//设置上下方向，0xC8正常 0xC0上下反置

	OLED_WriteCommand(0xDA);	//设置COM引脚硬件配置
	OLED_WriteCommand(0x12);
	
	OLED_WriteCommand(0x81);	//设置对比度控制
	OLED_WriteCommand(0xCF);

	OLED_WriteCommand(0xD9);	//设置预充电周期
	OLED_WriteCommand(0xF1);

	OLED_WriteCommand(0xDB);	//设置VCOMH取消选择级别
	OLED_WriteCommand(0x30);

	OLED_WriteCommand(0xA4);	//设置整个显示打开/关闭

	OLED_WriteCommand(0xA6);	//设置正常/倒转显示

	OLED_WriteCommand(0x8D);	//设置充电泵
	OLED_WriteCommand(0x14);

	OLED_WriteCommand(0xAF);	//开启显示
		
	OLED_Clear();				//OLED清屏
}





