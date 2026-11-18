#ifndef __MODBUS_TASK_H
#define __MODBUS_TASK_H

void ModbusTask(void *Parameters);
void Modbus_GapElapsedFromISR(void);//Modbus帧间隙超时中断服务函数，判定一帧接收完成

#endif
