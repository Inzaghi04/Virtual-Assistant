#include "audio.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"

static const char *TAG = "AudioProcs";
SemaphoreHandle_t xUploadDoneSemaphore = NULL;
char recognized_text[512];
char uploaded_url[512];
volatile bool stop_playback = false;

// Hàm xử lý ngắt cho nút bấm
static void IRAM_ATTR button_isr_handler(void* arg) {
    uint64_t current_time = esp_timer_get_time() / 1000; // Thời gian tính bằng ms
    if (current_time - last_button_press > DEBOUNCE_TIME_MS) {
        stop_playback = true;
        last_button_press = current_time;
    }
}

void button_init() {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON, button_isr_handler, NULL));
}

void control_led(const char* recognized_text) {
    char text_copy[64];
    strncpy(text_copy, recognized_text, sizeof(text_copy) - 1);
    text_copy[sizeof(text_copy) - 1] = '\0';

    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

    if (strcmp(text_copy, "bật đèn") == 0) {
        printf(text_copy);
        gpio_set_level(LED_PIN, 1);
    }
    if (strcmp(text_copy, "tắt đèn") == 0) {
        printf(text_copy);
        gpio_set_level(LED_PIN, 0);
    }
}
void amplify_audio(int16_t *pcm_buf, size_t frame_size) {
    for (size_t i = 0; i < frame_size; i++) {
        pcm_buf[i] = (int16_t)(pcm_buf[i] * VOLUME_MULTIPLIER);
        if (pcm_buf[i] > 32767) pcm_buf[i] = 32767;
        if (pcm_buf[i] < -32768) pcm_buf[i] = -32768;
    }
}

void stream_and_play_mp3(const char *url) {
    stop_playback = false;
    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 60000,
        .buffer_size = 8192,
        .buffer_size_tx = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Accept", "audio/mpeg");
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGE(TAG, "Failed to fetch headers");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    HMP3Decoder dec = MP3InitDecoder();
    if (!dec) {
        ESP_LOGE(TAG, "MP3InitDecoder failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    uint8_t *in_buf = heap_caps_malloc(INPUT_BUFFER_SIZE, MALLOC_CAP_DEFAULT);
    int16_t *pcm_buf = heap_caps_malloc(PCM_BUFFER_SIZE * sizeof(int16_t), MALLOC_CAP_DMA);
    if (!in_buf || !pcm_buf) {
        ESP_LOGE(TAG, "Buffer alloc failed");
        goto CLEAN;
    }

    MP3FrameInfo frameInfo;
    int bytes_left = 0;
    unsigned char *read_ptr = in_buf;
    bool first_frame = true;
    bool eof = false;

    char id3_header[10];
    int bytes_read = esp_http_client_read(client, id3_header, 10);
    if (bytes_read == 10 && memcmp(id3_header, "ID3", 3) == 0) {
        int tag_size = ((id3_header[6] & 0x7F) << 21) |
                       ((id3_header[7] & 0x7F) << 14) |
                       ((id3_header[8] & 0x7F) << 7)  |
                       (id3_header[9] & 0x7F);
        ESP_LOGI(TAG, "Skipping ID3v2 tag of %d bytes", tag_size + 10);
        char *temp_buf = heap_caps_malloc(tag_size, MALLOC_CAP_DEFAULT);
        if (temp_buf) {
            esp_http_client_read(client, temp_buf, tag_size);
            heap_caps_free(temp_buf);
        }
    } else {
        memcpy(in_buf, id3_header, bytes_read);
        bytes_left = bytes_read;
        read_ptr = in_buf;
    }

    while (!eof && !stop_playback) {
        if (bytes_left < MAINBUF_SIZE) {
            if (bytes_left > 0) {
                memmove(in_buf, read_ptr, bytes_left);
                read_ptr = in_buf;
            }
            int r = esp_http_client_read(client, (char *)in_buf + bytes_left, INPUT_BUFFER_SIZE - bytes_left);
            if (r > 0) {
                bytes_left += r;
            } else if (r == 0) {
                eof = true;
            } else {
                ESP_LOGE(TAG, "HTTP read error");
                break;
            }
        }

        if (bytes_left > 0) {
            int err = MP3Decode(dec, &read_ptr, &bytes_left, pcm_buf, 0);
            if (err == ERR_MP3_NONE) {
                MP3GetLastFrameInfo(dec, &frameInfo);
                if (first_frame) {
                    ESP_LOGI(TAG, "MP3 sample rate: %d, channels: %d", frameInfo.samprate, frameInfo.nChans);
                    i2s_set_clk(I2S_NUM, frameInfo.samprate / 2, I2S_BITS_PER_SAMPLE_16BIT, frameInfo.nChans == 1 ? I2S_CHANNEL_FMT_ONLY_LEFT : I2S_CHANNEL_FMT_RIGHT_LEFT);
                    first_frame = false;
                }
                size_t out_bytes = frameInfo.outputSamps * sizeof(int16_t);
                amplify_audio(pcm_buf, frameInfo.outputSamps);
                size_t written = 0;
                ESP_ERROR_CHECK(i2s_write(I2S_NUM, pcm_buf, out_bytes, &written, portMAX_DELAY));
            } else if (err == ERR_MP3_INDATA_UNDERFLOW) {
                continue;
            } else if (err == ERR_MP3_INVALID_FRAMEHEADER) {
                ESP_LOGW(TAG, "Invalid frame header, skipping...");
                if (bytes_left > 0) {
                    read_ptr++;
                    bytes_left--;
                }
                continue;
            } else {
                ESP_LOGE(TAG, "MP3Decode error: %d", err);
                break;
            }
        }
    }
    if (stop_playback) {
        ESP_LOGI(TAG, "Playback stopped due to button interrupt");
    }
    vTaskDelay(pdMS_TO_TICKS(500));
CLEAN:
    if (in_buf) heap_caps_free(in_buf);
    if (pcm_buf) heap_caps_free(pcm_buf);
    MP3FreeDecoder(dec);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    i2s_zero_dma_buffer(I2S_NUM);
    ESP_LOGI(TAG, "Streaming finished");
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

static esp_err_t upload_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            break;
        default:
            break;
    }
    return ESP_OK;
}

void upload_wav_task(void *pv) {
    FILE *f = fopen(WAV_FILE_PATH, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Không mở được file: %s", WAV_FILE_PATH);
        vTaskDelete(NULL);
        return;
    }
    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

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

    esp_http_client_config_t config = {
        .url = server_url,
        .method = HTTP_METHOD_POST,
        .event_handler = upload_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    char content_type_header[64];
    snprintf(content_type_header, sizeof(content_type_header),
             "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type_header);
    esp_http_client_set_header(client, "Content-Length", (char[]){0});

    esp_http_client_open(client, total_len);

    esp_http_client_write(client, header, header_len);

    const size_t CHUNK_SIZE = 1024;
    uint8_t buf[CHUNK_SIZE];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, CHUNK_SIZE, f)) > 0) {
        esp_http_client_write(client, (const char*)buf, read_bytes);
    }
    fclose(f);

    esp_http_client_write(client, footer, footer_len);

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    char *resp = malloc(1024);
    int len = esp_http_client_read_response(client, resp, 1024);
    if (len > 0) {
        resp[len] = '\0';
        cJSON *root = cJSON_Parse(resp);
        if (root) {
            cJSON *error = cJSON_GetObjectItem(root, "error");
            if (error && cJSON_IsString(error)) {
                ESP_LOGW(TAG, "Lỗi từ server: %s", error->valuestring);
            } else {
                cJSON *text = cJSON_GetObjectItem(root, "text");
                if (text && cJSON_IsString(text)) {
                    strncpy(recognized_text, text->valuestring, sizeof(recognized_text) - 1);
                    ESP_LOGI(TAG, "Văn bản nhận dạng: %s", recognized_text);
                    control_led(recognized_text);
                }

                cJSON *mp3_url = cJSON_GetObjectItem(root, "mp3_url");
                if (mp3_url && cJSON_IsString(mp3_url)) {
                    strncpy(uploaded_url, mp3_url->valuestring, sizeof(uploaded_url) - 1);
                    ESP_LOGI(TAG, "✅ File MP3 URL: %s", uploaded_url);
                    stream_and_play_mp3(uploaded_url);
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

void recording(void) {
     ESP_LOGI(TAG, "Start recording...");
     FILE *wav_file = fopen("/sdcard/record.wav", "wb");
     if (!wav_file)
     {
         ESP_LOGE(TAG, "Failed to open file for writing");
         return;
     }

     write_wav_header(wav_file, SAMPLE_RATE, 16, 1, 0);

     int32_t *i2s_read_buff = (int32_t *)malloc(I2S_READ_LEN);
     int16_t *wav_write_buff = (int16_t *)malloc(I2S_READ_LEN / 2);

     if (!i2s_read_buff || !wav_write_buff)
     {
         ESP_LOGE(TAG, "Malloc buffer failed");
         fclose(wav_file);
         free(i2s_read_buff);
         free(wav_write_buff);
         return;
     }

     int total_bytes_written = 0;
     size_t bytes_read;

     while (gpio_get_level(BUTTON) == 0)
     {
         i2s_read(I2S_NUM_MIC, (void *)i2s_read_buff, I2S_READ_LEN, &bytes_read, portMAX_DELAY);
         int n_samples = bytes_read / 4;

         for (int j = 0; j < n_samples; j++)
         {
             wav_write_buff[j] = i2s_read_buff[j] >> 14;
         }

         fwrite(wav_write_buff, sizeof(int16_t), n_samples, wav_file);
         total_bytes_written += n_samples * sizeof(int16_t);
     }

     fseek(wav_file, 4, SEEK_SET);
     int chunk_size = 36 + total_bytes_written;
     fwrite(&chunk_size, sizeof(int), 1, wav_file);

     fseek(wav_file, 40, SEEK_SET);
     fwrite(&total_bytes_written, sizeof(int), 1, wav_file);

     fclose(wav_file);
     ESP_LOGI(TAG, "Recording done. Bytes = %d", total_bytes_written);
     free(i2s_read_buff);
     free(wav_write_buff);
}