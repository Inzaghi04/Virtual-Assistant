#ifndef SD_CARD_H
#define SD_CARD_H

#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#define PIN_NUM_MISO 37
#define PIN_NUM_MOSI 35
#define PIN_NUM_CLK  36
#define PIN_NUM_CS   38
#define MOUNT_POINT  "/sdcard"

void sd_card_init(void);

#endif // SD_CARD_H