#pragma once
typedef int SemaphoreHandle_t;
#define portMAX_DELAY 0xFFFFFFFF
#define pdTRUE 1
#define pdFALSE 0
#define xSemaphoreCreateRecursiveMutex() 1
#define xSemaphoreTakeRecursive(m, t) ((void)(m), (void)(t), 1)
#define xSemaphoreGiveRecursive(m) ((void)(m), 1)
