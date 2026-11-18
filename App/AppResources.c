#include "AppResources.h"

SemaphoreHandle_t g_i2cMutex=NULL;//I2C互斥锁
QueueHandle_t g_storageQueue=NULL;//存储消息队列
EventGroupHandle_t g_storageEvents=NULL;//存储事件组
