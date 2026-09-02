// Serial logging shared by the UI (core 1) and network (core 0) tasks.
// A mutex keeps log lines from interleaving with each other or with the
// binary screen dump, which must own the port for its whole duration.
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

extern SemaphoreHandle_t g_serialMutex;

#define LOGF(...)                                                     \
  do {                                                                \
    if (xSemaphoreTake(g_serialMutex, pdMS_TO_TICKS(500)) == pdTRUE) { \
      Serial.printf(__VA_ARGS__);                                     \
      xSemaphoreGive(g_serialMutex);                                  \
    }                                                                 \
  } while (0)
