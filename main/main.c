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
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "cJSON.h"

#define PIN_NUM_MISO 37
#define PIN_NUM_MOSI 35
#define PIN_NUM_CLK  36
#define PIN_NUM_CS   38

#define BUTTON 3
#define LED_PIN  2

#define INPUT_BUFFER_SIZE  (8 * 1024)
#define MAINBUF_SIZE       1152
#define PCM_BUFFER_SIZE    (MAINBUF_SIZE * 2)
#define MOUNT_POINT        "/sdcard"

#define I2S_READ_LEN    (1024)
#define RECORD_TIME_SEC (3)
#define SAMPLE_RATE     (16000)
#define SAMPLE_BITS     (I2S_BITS_PER_SAMPLE_16BIT)
#define WAV_HEADER_SIZE (44)
#define WAV_FILE_PATH   MOUNT_POINT"/record.wav"

static const char *TAG = "MP3Streamer";
SemaphoreHandle_t xWifiConnected = NULL;
SemaphoreHandle_t xUploadDoneSemaphore = NULL;
SemaphoreHandle_t xJsonReady = NULL;

// I2S pins
#define I2S_NUM       I2S_NUM_1
#define I2S_NUM_MIC   I2S_NUM_0
#define I2S_BCLK_PIN  GPIO_NUM_4
#define I2S_WS_PIN    GPIO_NUM_5
#define I2S_DO_PIN    GPIO_NUM_6
#define VOLUME_MULTIPLIER 3

char server_url[512]; // Default server URL, modifiable via form

char recognized_text[512];
char uploaded_url[512];

static volatile bool stop_playback = false;
static volatile uint64_t last_button_press = 0;
#define DEBOUNCE_TIME_MS 200 // Thời gian chống rung 200ms

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

void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        switch(event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi STA started, connecting...");
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGI(TAG, "WiFi disconnected, retrying...");
                esp_wifi_connect();
                break;
            case WIFI_EVENT_AP_STACONNECTED:
                ESP_LOGI(TAG, "Station connected to AP");
                break;
            case WIFI_EVENT_AP_STADISCONNECTED:
                ESP_LOGI(TAG, "Station disconnected from AP");
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        if (xWifiConnected) xSemaphoreGive(xWifiConnected);
    }
}

void wifi_init(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);

    wifi_config_t ap_config = {
        .ap = {
            .ssid = "ESP32-Setup",
            .ssid_len = strlen("ESP32-Setup"),
            .password = "12345678",
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "Access Point started. Connect to SSID: ESP32-Setup");
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    const char* html =
        "<!DOCTYPE html>"
        "<html lang=\"en\">"
        "<head>"
        "<meta charset=\"UTF-8\" />"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />"
        "<title>ESP32 Wi-Fi Setup</title>"
        "<style>"
        "  body {"
        "    background-color: #f4f6f8;"
        "    font-family: Arial, sans-serif;"
        "    display: flex;"
        "    justify-content: center;"
        "    align-items: center;"
        "    height: 100vh;"
        "    margin: 0;"
        "  }"
        "  .container {"
        "    background: #fff;"
        "    padding: 24px 32px;"
        "    border-radius: 8px;"
        "    box-shadow: 0 4px 12px rgba(0,0,0,0.1);"
        "    width: 320px;"
        "  }"
        "  h2 {"
        "    margin-top: 0;"
        "    margin-bottom: 20px;"
        "    font-weight: 700;"
        "    font-size: 22px;"
        "    text-align: center;"
        "    color: #333;"
        "  }"
        "  label {"
        "    font-size: 14px;"
        "    color: #555;"
        "  }"
        "  input[type=text],"
        "  input[type=password] {"
        "    width: 100%;"
        "    padding: 10px 12px;"
        "    margin-top: 6px;"
        "    margin-bottom: 18px;"
        "    border: 1px solid #ccc;"
        "    border-radius: 5px;"
        "    font-size: 14px;"
        "    box-sizing: border-box;"
        "  }"
        "  button[type=submit] {"
        "    width: 100%;"
        "    background-color: #007BFF;"
        "    color: white;"
        "    padding: 12px 0;"
        "    border: none;"
        "    border-radius: 6px;"
        "    font-weight: 600;"
        "    font-size: 16px;"
        "    cursor: pointer;"
        "    transition: background-color 0.3s ease;"
        "  }"
        "  button[type=submit]:hover {"
        "    background-color: #0056b3;"
        "  }"
        "  .checkbox-wrapper {"
        "    display: flex;"
        "    align-items: center;"
        "    margin-bottom: 20px;"
        "    user-select: none;"
        "  }"
        "  .checkbox-wrapper input[type=checkbox] {"
        "    margin-right: 8px;"
        "  }"
        "</style>"
        "</head>"
        "<body>"
        "  <div class=\"container\">"
        "    <h2>Connect to Wi-Fi</h2>"
        "    <form id=\"wifiForm\" action=\"/connect\" method=\"post\">"
        "      <label for=\"ssid\">Wi-Fi SSID</label>"
        "      <input id=\"ssid\" name=\"ssid\" type=\"text\" placeholder=\"Enter Wi-Fi name\" required>"
        "      <label for=\"pass\">Password</label>"
        "      <input id=\"pass\" name=\"pass\" type=\"password\" placeholder=\"Enter password\" required>"
        "      <div class=\"checkbox-wrapper\">"
        "        <input type=\"checkbox\" id=\"showPass\" onclick=\"togglePassword()\" />"
        "        <label for=\"showPass\">Show password</label>"
        "      </div>"
        "      <label for=\"server_url\">Server URL</label>"
        "      <input id=\"server_url\" name=\"server_url\" type=\"text\" placeholder=\"e.g. 192.168.1.1:5000\" required>"
        "      <button type=\"submit\">Connect</button>"
        "    </form>"
        "  </div>"
        "  <script>"
        "    function togglePassword() {"
        "      var passInput = document.getElementById('pass');"
        "      var checkbox = document.getElementById('showPass');"
        "      if (checkbox.checked) {"
        "        passInput.type = 'text';"
        "      } else {"
        "        passInput.type = 'password';"
        "      }"
        "    }"
        "    document.getElementById('wifiForm').addEventListener('submit', function(event) {"
        "      var serverInput = document.getElementById('server_url');"
        "      var val = serverInput.value.trim();"
        "      if (!val.startsWith('http://') && !val.startsWith('https://')) {"
        "        val = 'http://' + val;"
        "      }"
        "      if (!val.endsWith('/upload-audio')) {"
        "        if (!val.endsWith('/')) {"
        "          val += '/';"
        "        }"
        "        val += 'upload-audio';"
        "      }"
        "      serverInput.value = val;"
        "    });"
        "  </script>"
        "</body>"
        "</html>";

    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}



static void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst++ = '\0';
}

static esp_err_t connect_post_handler(httpd_req_t *req) {
    int total_len = req->content_len;
    int cur_len = 0;
    char buf[256];

    if (total_len >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    while (cur_len < total_len) {
        int received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) {
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[cur_len] = '\0';

    char ssid_enc[32] = {0};
    char pass_enc[64] = {0};
    char server_url_enc[128] = {0};
    sscanf(buf, "ssid=%31[^&]&pass=%63[^&]&server_url=%127s", ssid_enc, pass_enc, server_url_enc);

    char ssid[32] = {0};
    char pass[64] = {0};
    url_decode(ssid, ssid_enc);
    url_decode(pass, pass_enc);
    url_decode(server_url, server_url_enc);

    ESP_LOGI(TAG, "Received SSID: %s, PASS: %s, Server URL: %s", ssid, pass, server_url);

    esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();

    const char* resp = "Connecting to WiFi, please wait...";
    httpd_resp_sendstr(req, resp);

    return ESP_OK;
}

void start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return;
    }

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler
    };
    httpd_register_uri_handler(server, &root);

    httpd_uri_t connect = {
        .uri = "/connect",
        .method = HTTP_POST,
        .handler = connect_post_handler
    };
    httpd_register_uri_handler(server, &connect);

    ESP_LOGI(TAG, "HTTP Server started");
}

void i2s_init(void) {
    i2s_config_t cfg = {
        .mode                = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate         = 44100,
        .bits_per_sample     = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format      = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S_MSB,
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

static void upload_wav_task(void *pv) {
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
    xJsonReady = xSemaphoreCreateBinary();

    button_init();
    i2s_init();
    init_i2s_mic();
    sd_card_init();

    wifi_init();
    start_webserver();
    if (xSemaphoreTake(xWifiConnected, portMAX_DELAY) == pdTRUE) {
        while (1) {
            if (gpio_get_level(BUTTON) == 0) {
                ESP_LOGI(TAG, "Start recording...");
                FILE *wav_file = fopen("/sdcard/record.wav", "wb");
                if (!wav_file) {
                    ESP_LOGE(TAG, "Failed to open file for writing");
                    return;
                }

                write_wav_header(wav_file, SAMPLE_RATE, 16, 1, 0);

                int32_t *i2s_read_buff = (int32_t*) malloc(I2S_READ_LEN);
                int16_t *wav_write_buff = (int16_t*) malloc(I2S_READ_LEN / 2);

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

                fseek(wav_file, 4, SEEK_SET);
                int chunk_size = 36 + total_bytes_written;
                fwrite(&chunk_size, sizeof(int), 1, wav_file);

                fseek(wav_file, 40, SEEK_SET);
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
