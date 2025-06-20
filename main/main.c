#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_vfs_fat.h"
#include "mp3dec.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "i2s_audio.h"
#include "sd_card.h"
#include "wifi.h"
#include "audio.h"


void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(2000));

    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    xWifiConnected = xSemaphoreCreateBinary();
    xUploadDoneSemaphore = xSemaphoreCreateBinary();
    button_init();
    i2s_init();
    init_i2s_mic();
    sd_card_init();

    wifi_init();
    start_webserver();
    if (xSemaphoreTake(xWifiConnected, portMAX_DELAY) == pdTRUE) {
        while (1) {
            if (gpio_get_level(BUTTON) == 0) {
                recording();
                
                xTaskCreate(upload_wav_task, "upload_wav", 8192, NULL, 5, NULL);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    } else {
        ESP_LOGE("Wi-Fi", "Không kết nối Wi-Fi.");
    }
}