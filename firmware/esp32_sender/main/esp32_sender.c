#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    while (1) {
        printf("Simulated SpO2: 98%%, heart rate: 72 bpm\n");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}