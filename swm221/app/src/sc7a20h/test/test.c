#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sc7a20h.h"

#define TAG "SC7A20H"

void app_main(void) {
    // I2C初始化
    ESP_ERROR_CHECK(sc7a20h_i2c_init(I2C_NUM_0));
    
    // SC7A20H驱动初始化,最多重试重试
    for (int i = 0; i < 3; i++) {
        //【】 esp_err_t err = sc7a20h_init();
        esp_err_t err = SL_SC7A20H_Config();
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "Init attempt %d failed", i+1);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    //使用
    while (1) {
        Check_SC7A20H_State(acc_task(&SC7A20H,&Counter));
    }
}

