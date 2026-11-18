#ifndef __App_RESOURCES_H
#define __App_RESOURCES_H

#include "FreeRTOS.h"
#include "semphr.h"
#include "queue.h"
#include "event_groups.h"

extern SemaphoreHandle_t g_i2cMutex;
extern QueueHandle_t g_storageQueue;
extern EventGroupHandle_t g_storageEvents;

#define STORAGE_INIT_DONE_BIT   (1U<<0)

#endif
