#ifndef I2S_AUDIO_H
#define I2S_AUDIO_H

#include "driver/i2s.h"
#include "driver/gpio.h"
#include "esp_err.h"

#define I2S_NUM       I2S_NUM_1
#define I2S_NUM_MIC   I2S_NUM_0
#define I2S_BCLK_PIN  GPIO_NUM_4
#define I2S_WS_PIN    GPIO_NUM_5
#define I2S_DO_PIN    GPIO_NUM_6
#define SAMPLE_RATE     (16000)

void i2s_init(void);
void init_i2s_mic(void);

#endif // I2S_AUDIO_H