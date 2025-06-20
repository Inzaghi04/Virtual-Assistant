#ifndef AUDIO_H
#define AUDIO_H

#include <stdio.h>
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mp3dec.h"
#include "i2s_audio.h"
#include "sd_card.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "wifi.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#define INPUT_BUFFER_SIZE  (8 * 1024)
#define MAINBUF_SIZE       1152
#define PCM_BUFFER_SIZE    (MAINBUF_SIZE * 2)
#define I2S_READ_LEN       (1024)
#define RECORD_TIME_SEC    (3)
#define SAMPLE_BITS        (I2S_BITS_PER_SAMPLE_16BIT)
#define WAV_HEADER_SIZE    (44)
#define WAV_FILE_PATH      MOUNT_POINT"/record.wav"
#define VOLUME_MULTIPLIER  3
#define BUTTON 3
#define LED_PIN  2

static volatile uint64_t last_button_press = 0;
#define DEBOUNCE_TIME_MS 200 // Thời gian chống rung 200ms
extern char recognized_text[512];
extern char uploaded_url[512];
extern SemaphoreHandle_t xUploadDoneSemaphore;
extern volatile bool stop_playback;

static void IRAM_ATTR button_isr_handler(void* arg);
void button_init(void);
void control_led(const char* recognized_text);
void amplify_audio(int16_t *pcm_buf, size_t frame_size);
static esp_err_t upload_event_handler(esp_http_client_event_t *evt);
void stream_and_play_mp3(const char *url);
void write_wav_header(FILE *f, int sample_rate, int bits_per_sample, int channels, int data_len);
void recording(void);
void upload_wav_task(void *pv);

#endif // AUDIO_H