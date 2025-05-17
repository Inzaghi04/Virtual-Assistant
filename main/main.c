#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "driver/i2s.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "mp3dec.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_netif.h"  // Thêm thư viện mạng
#include "esp_crt_bundle.h"
#include "esp_wifi.h"   // Thêm thư viện Wi-Fi
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "text_to_speech.h"
#include "speech_to_text.h"

#include "esp_timer.h"
#include "cJSON.h"

#define PIN_NUM_MISO 37
#define PIN_NUM_MOSI 35
#define PIN_NUM_CLK  36
#define PIN_NUM_CS   38

#define BUTTON 3

#define INPUT_BUFFER_SIZE  (8 * 1024)
#define MAINBUF_SIZE       1152
#define PCM_BUFFER_SIZE    (MAINBUF_SIZE * 2)
#define MOUNT_POINT        "/sdcard"
#define MP3_PATH           MOUNT_POINT"/audio.mp3"

#define I2S_READ_LEN    (1024)
#define RECORD_TIME_SEC (3)
#define SAMPLE_RATE     (16000)
#define SAMPLE_BITS     (I2S_BITS_PER_SAMPLE_16BIT)
#define WAV_HEADER_SIZE (44)
#define WAV_FILE_PATH  MOUNT_POINT"/record.wav"

static const char *TAG = "MP3Player";
static FILE *audio_file = NULL;
SemaphoreHandle_t xWifiConnected = NULL;


// I2S pins
#define I2S_NUM       I2S_NUM_1
#define I2S_NUM_MIC   I2S_NUM_0
#define I2S_BCLK_PIN  GPIO_NUM_4
#define I2S_WS_PIN    GPIO_NUM_5
#define I2S_DO_PIN    GPIO_NUM_6
#define VOLUME_MULTIPLIER 3

#define WIFI_SSID "YOUR_SSID"
#define WIFI_PASS "YOUR_PASSWORD"
#define SEVER_URL   "YOUR_URL/upload-audio"

char recognized_text[512];

void button_init() {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}


esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI("HTTP", "Receiving data, len=%d", evt->data_len);
            if (audio_file && evt->data_len > 0) {
                fwrite(evt->data, 1, evt->data_len, audio_file);
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI("HTTP", "Download finished");
            if (audio_file) {
                fclose(audio_file);
                audio_file = NULL;
                ESP_LOGI("HTTP", "File closed");
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

void download_audio_file(const char *url) {
    audio_file = fopen(MP3_PATH, "wb");
    if (!audio_file) {
        ESP_LOGE("Audio", "Failed to open file for writing");
        return;
    }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = _http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI("Audio", "Successfully downloaded file");
    } else {
        ESP_LOGE("Audio", "HTTP GET request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);

    // Đảm bảo đóng file nếu chưa đóng
    if (audio_file) {
        fclose(audio_file);
        audio_file = NULL;
    }
}

void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (xWifiConnected) xSemaphoreGive(xWifiConnected);
    }
}

// Wi-Fi setup
void wifi_init() {
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}

void i2s_init(void) {
    i2s_config_t cfg = {
        .mode                = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate         = 44100,  // overridden by first MP3 frame
        .bits_per_sample     = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format      = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format= I2S_COMM_FORMAT_I2S_MSB,
        .dma_buf_count       = 8,
        .dma_buf_len         = 1024,
        .use_apll            = false,
    };
    i2s_pin_config_t pins = {
        .bck_io_num   = I2S_BCLK_PIN,
        .ws_io_num    = I2S_WS_PIN,
        .data_out_num = I2S_DO_PIN,
        .data_in_num  = I2S_PIN_NO_CHANGE
    };
    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM, &cfg, 0, NULL));
    
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM, &pins));
}
void init_i2s_mic(void) {
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_MSB,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 1024,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = 17,
        .ws_io_num = 18,
        .data_out_num = -1,
        .data_in_num = 16
    };

    i2s_driver_install(I2S_NUM_MIC, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_MIC, &pin_config);
    i2s_zero_dma_buffer(I2S_NUM_MIC);
}
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

void amplify_audio(int16_t *pcm_buf, size_t frame_size) {
    for (size_t i = 0; i < frame_size; i++) {
        pcm_buf[i] = (int16_t)(pcm_buf[i] * VOLUME_MULTIPLIER);
        // Giới hạn giá trị để tránh tràn
        if (pcm_buf[i] > 32767) pcm_buf[i] = 32767;
        if (pcm_buf[i] < -32768) pcm_buf[i] = -32768;
    }
}

void play_mp3(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Open %s failed", path);
        return;
    }

    char id3_header[10];
    if (fread(id3_header, 1, 10, f) == 10) {
        if (memcmp(id3_header, "ID3", 3) == 0) {
            // ID3v2 tag found, calculate size
            int tag_size = ((id3_header[6] & 0x7F) << 21) |
                           ((id3_header[7] & 0x7F) << 14) |
                           ((id3_header[8] & 0x7F) << 7)  |
                           (id3_header[9] & 0x7F);
            fseek(f, tag_size, SEEK_CUR);  // Skip over the tag
            ESP_LOGI(TAG, "Skipped ID3v2 tag of %d bytes", tag_size + 10);
        } else {
            rewind(f);  // Not an ID3 tag, rewind to beginning
        }
    }

    HMP3Decoder dec = MP3InitDecoder();
    if (!dec) {
        ESP_LOGE(TAG, "MP3InitDecoder failed");
        fclose(f);
        return;
    }

    uint8_t *in_buf  = heap_caps_malloc(INPUT_BUFFER_SIZE, MALLOC_CAP_DEFAULT);
    int16_t *pcm_buf = heap_caps_malloc(PCM_BUFFER_SIZE * sizeof(int16_t), MALLOC_CAP_DMA);
    if (!in_buf || !pcm_buf) {
        ESP_LOGE(TAG, "Buffer alloc failed: in_buf=%p, pcm_buf=%p", in_buf, pcm_buf);
        goto CLEAN;
    }

    MP3FrameInfo frameInfo;
    int bytes_left = 0;
    unsigned char *read_ptr = in_buf;
    bool first_frame = true;
    bool eof = false;

    while (!eof || bytes_left > 0) {
        // Refill buffer if needed
        if (bytes_left < MAINBUF_SIZE && !eof) {
            if (bytes_left > 0) memmove(in_buf, read_ptr, bytes_left);
            read_ptr = in_buf;
            int r = fread(in_buf + bytes_left, 1, INPUT_BUFFER_SIZE - bytes_left, f);
            if (r <= 0) {
                eof = true;
            } else {
                bytes_left += r;
            }
        }

        // Decode one MP3 frame
        int err = MP3Decode(dec, &read_ptr, &bytes_left, pcm_buf, 0);
        if (err == ERR_MP3_NONE) {
            MP3GetLastFrameInfo(dec, &frameInfo);
            if (first_frame) {
                ESP_LOGI(TAG, "MP3 sample rate: %d, channels: %d", frameInfo.samprate, frameInfo.nChans);
                i2s_set_clk(I2S_NUM,
                            frameInfo.samprate / 2,  // hoặc giữ nguyên nếu bạn đã xử lý tốc độ
                            I2S_BITS_PER_SAMPLE_16BIT,
                            (frameInfo.nChans == 1)
                                ? I2S_CHANNEL_FMT_ONLY_LEFT
                                : I2S_CHANNEL_FMT_RIGHT_LEFT);
                first_frame = false;
                
                continue;
            }
            // Bắt đầu phát từ khung thứ 2 trở đi
            size_t out_bytes = frameInfo.outputSamps * sizeof(int16_t);
            size_t written = 0;
            amplify_audio(pcm_buf, frameInfo.outputSamps);
            ESP_ERROR_CHECK(i2s_write(I2S_NUM, pcm_buf, out_bytes, &written, portMAX_DELAY));
        }
        else if (err == ERR_MP3_INDATA_UNDERFLOW) {
            if (eof && bytes_left == 0) {
                break;  // Đã hết sạch dữ liệu và decoder không còn gì để giải mã
            }
            if (bytes_left > 0) {
                read_ptr++;
                bytes_left--;
            }
            continue;
        } else if (err == ERR_MP3_INVALID_FRAMEHEADER) {
            // Skip bad header
            if (bytes_left > 0) {
                read_ptr++;
                bytes_left--;
            }
            continue;
        } else {
            break; 
        }
    }

    // Đợi một thời gian để dữ liệu phát xong
    vTaskDelay(500 / portTICK_PERIOD_MS);  // wait 500ms or adjust based on your data rate

    // Clear DMA buffer to avoid replay
    i2s_zero_dma_buffer(I2S_NUM);

CLEAN:
    if (in_buf)  heap_caps_free(in_buf);
    if (pcm_buf) heap_caps_free(pcm_buf);
    MP3FreeDecoder(dec);
    fclose(f);
    ESP_LOGI(TAG, "Playback finished"); 
}

void write_wav_header(FILE *f, int sample_rate, int bits_per_sample, int channels, int data_len) {
    uint8_t header[WAV_HEADER_SIZE];
    int byte_rate = sample_rate * channels * bits_per_sample / 8;
    int block_align = channels * bits_per_sample / 8;
    int subchunk2_size = data_len;

    memcpy(header, "RIFF", 4);
    *(int *)(header + 4)  = 36 + subchunk2_size;
    memcpy(header + 8, "WAVEfmt ", 8);
    *(int *)(header + 16) = 16;
    *(short *)(header + 20) = 1;
    *(short *)(header + 22) = channels;
    *(int *)(header + 24) = sample_rate;
    *(int *)(header + 28) = byte_rate;
    *(short *)(header + 32) = block_align;
    *(short *)(header + 34) = bits_per_sample;
    memcpy(header + 36, "data", 4);
    *(int *)(header + 40) = subchunk2_size;

    fwrite(header, 1, WAV_HEADER_SIZE, f);
}
/*-------------------- HTTP Event --------------------*/
static esp_err_t upload_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            //printf("%.*s", evt->data_len, (char*)evt->data);
            break;
        default:
            break;
    }
    return ESP_OK;
}

/*-------------------- Upload File --------------------*/
static void upload_wav_task(void *pv)
{
    // Mở file WAV
    FILE *f = fopen(WAV_FILE_PATH, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Không mở được file: %s", WAV_FILE_PATH);
        vTaskDelete(NULL);
        return;
    }
    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Chuẩn bị multipart header/footer
    const char *boundary = "ESP32Boundary";
    char header[256];
    int header_len = snprintf(header, sizeof(header),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: audio/wav\r\n\r\n",
        boundary, "record.wav");
    char footer[64];
    int footer_len = snprintf(footer, sizeof(footer),
        "\r\n--%s--\r\n", boundary);

    size_t total_len = header_len + file_size + footer_len;

    // Cấu hình HTTP client
    esp_http_client_config_t config = {
        .url = SEVER_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = upload_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    // Headers cần thiết
    char content_type_header[64];
    snprintf(content_type_header, sizeof(content_type_header),
             "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type_header);
    esp_http_client_set_header(client, "Content-Length",
                               (char[]){0}); // sẽ dùng esp_http_client_open

    // Mở kết nối với server (chunked theo total_len)
    esp_http_client_open(client, total_len);

    // Gửi header
    esp_http_client_write(client, header, header_len);

    // Gửi dữ liệu file theo chunk
    const size_t CHUNK_SIZE = 1024;
    uint8_t buf[CHUNK_SIZE];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, CHUNK_SIZE, f)) > 0) {
        esp_http_client_write(client, (const char*)buf, read_bytes);
    }
    fclose(f);

    // Gửi footer
    esp_http_client_write(client, footer, footer_len);

    // Nhận response
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    //ESP_LOGI(TAG, "Upload HTTP status: %d", status);

    // Đọc và in ra nội dung response
    char *resp = malloc(1024);
    int len = esp_http_client_read_response(client, resp, 1024);
    if (len > 0) {
        resp[len] = '\0';
        //ESP_LOGI(TAG, "Response: %s", resp);
    
        cJSON *root = cJSON_Parse(resp);
                if (root) {
            cJSON *error = cJSON_GetObjectItem(root, "error");
            if (error && cJSON_IsString(error)) {
                ESP_LOGW(TAG, "Lỗi từ server: %s", error->valuestring);
            } else {
                // Nếu không có lỗi, lấy text và mp3_url từ response
                cJSON *text = cJSON_GetObjectItem(root, "text");
                if (text && cJSON_IsString(text)) {
                    strncpy(recognized_text, text->valuestring, sizeof(recognized_text) - 1);
                    ESP_LOGI(TAG, "Văn bản nhận dạng: %s", recognized_text);
                }

                cJSON *mp3_url = cJSON_GetObjectItem(root, "mp3_url");
                if (mp3_url && cJSON_IsString(mp3_url)) {
                    strncpy(uploaded_url, mp3_url->valuestring, sizeof(uploaded_url) - 1);
                    ESP_LOGI(TAG, "✅ File MP3 URL: %s", uploaded_url);
                    download_audio_file(uploaded_url);
                    play_mp3(MP3_PATH);
                    xSemaphoreGive(xUploadDoneSemaphore);
                } else {
                    ESP_LOGW(TAG, "Không tìm thấy mp3_url trong JSON.");
                }
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGE(TAG, "Không parse được JSON.");
        }
    }
    free(resp);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);


    vTaskDelete(NULL);
}


void mp3_task(void *arg) {
    play_mp3(MP3_PATH);
    vTaskDelete(NULL);
}

void app_main(void)
{
    // 1) Delay để ổn định hệ thống
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

    // 2) Khởi tạo NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    xWifiConnected = xSemaphoreCreateBinary();
    xUploadDoneSemaphore = xSemaphoreCreateBinary();
    xJsonReady = xSemaphoreCreateBinary();
    // 4) Khởi tạo I2S và SD‑card
    button_init();
    i2s_init();
    init_i2s_mic(); 
    sd_card_init();
    // 3) Kết nối Wi‑Fi
    wifi_init();
    if (xSemaphoreTake(xWifiConnected, pdMS_TO_TICKS(10000)) == pdTRUE) {
        while (1) {
            if (gpio_get_level(BUTTON) == 0) {
                ESP_LOGI(TAG, "Start recording...");
                FILE *wav_file = fopen("/sdcard/record.wav", "wb");
                if (!wav_file) {
                    ESP_LOGE(TAG, "Failed to open file for writing");
                    return;
                }
            
                // Ghi header tạm thời với kích thước dữ liệu là 0
                 write_wav_header(wav_file, SAMPLE_RATE, 16, 1, 0);
            
                int32_t *i2s_read_buff = (int32_t*) malloc(I2S_READ_LEN);
                int16_t *wav_write_buff = (int16_t*) malloc(I2S_READ_LEN / 2); // Mỗi mẫu 2 byte
            
                if (!i2s_read_buff || !wav_write_buff) {
                    ESP_LOGE(TAG, "Malloc buffer failed");
                    fclose(wav_file);
                    free(i2s_read_buff);
                    free(wav_write_buff);
                    return;
                }
            
                int total_bytes_written = 0;
                size_t bytes_read;
            
                while (gpio_get_level(BUTTON) == 0) {
                    i2s_read(I2S_NUM_MIC, (void*)i2s_read_buff, I2S_READ_LEN, &bytes_read, portMAX_DELAY);
                    int n_samples = bytes_read / 4;
            
                    for (int j = 0; j < n_samples; j++) {
                        wav_write_buff[j] = i2s_read_buff[j] >> 14;
                    }
            
                    fwrite(wav_write_buff, sizeof(int16_t), n_samples, wav_file);
                    total_bytes_written += n_samples * sizeof(int16_t);
                }
            
                // Cập nhật header với kích thước thực tế
                fseek(wav_file, 4, SEEK_SET); // Di chuyển đến byte 4 (ChunkSize)
                int chunk_size = 36 + total_bytes_written;
                fwrite(&chunk_size, sizeof(int), 1, wav_file);
            
                fseek(wav_file, 40, SEEK_SET); // Di chuyển đến byte 40 (Subchunk2Size)
                fwrite(&total_bytes_written, sizeof(int), 1, wav_file);
            
                fclose(wav_file);
                ESP_LOGI(TAG, "Recording done. Bytes = %d", total_bytes_written);
                free(i2s_read_buff);
                free(wav_write_buff);
                xTaskCreate(upload_wav_task, "upload_wav", 8192, NULL, 5, NULL);
                
                
                
            }
            vTaskDelay(pdMS_TO_TICKS(100)); 
        }
            
        
    } else {
        ESP_LOGE("Wi-Fi", "Không kết nối Wi-Fi.");
    }
}

