#include "sd_card.h"
#include "esp_log.h"

static const char *TAG = "SDCard";

void sd_card_init(void) {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus = {
        .mosi_io_num = GPIO_NUM_35,
        .miso_io_num = GPIO_NUM_37,
        .sclk_io_num = GPIO_NUM_36,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA));
    sdspi_device_config_t dev_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev_cfg.gpio_cs = GPIO_NUM_38;
    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
    };
    sdmmc_card_t* card;
    esp_err_t ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &dev_cfg, &mcfg, &card);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SD card mounted");
    } else {
        ESP_LOGE(TAG, "Mount SD failed: %s", esp_err_to_name(ret));
    }
}
