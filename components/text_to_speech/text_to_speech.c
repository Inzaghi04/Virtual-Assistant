#include "text_to_speech.h"
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"


#define RAPIDAPI_URL "https://express-voic-text-to-speech.p.rapidapi.com/getAudioLink"
#define RAPIDAPI_KEY "3e88522fc4mshe4f3a32b6d1f360p1a5325jsn99ad543eae62"
#define RAPIDAPI_HOST "express-voic-text-to-speech.p.rapidapi.com"

static char *g_audio_url = NULL;
static const char *TAG = "TTS_RapidAPI";
esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    static char *output_buffer = NULL;
    static int output_len = 0;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            output_buffer = realloc(output_buffer, output_len + evt->data_len + 1);
            memcpy(output_buffer + output_len, evt->data, evt->data_len);
            output_len += evt->data_len;
            output_buffer[output_len] = '\0';
            break;

        case HTTP_EVENT_ON_FINISH:
            if (output_buffer) {
                ESP_LOGI(TAG, "Phản hồi JSON:\n%s", output_buffer);
                cJSON *root = cJSON_Parse(output_buffer);
                if (root) {
                    cJSON *url = cJSON_GetObjectItem(root, "audio_url");
                    if (url && cJSON_IsString(url)) {
                        g_audio_url = strdup(url->valuestring);
                    }
                    cJSON_Delete(root);
                }
                free(output_buffer);
                output_buffer = NULL;
                output_len = 0;
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

// Hàm mã hóa URL
char *url_encode(const char *src) {
    const char *hex = "0123456789ABCDEF";
    char *out = malloc(strlen(src) * 3 + 1);
    char *ret = out;
    if (!out) return NULL;

    while (*src) {
        if (('a' <= *src && *src <= 'z') ||
            ('A' <= *src && *src <= 'Z') ||
            ('0' <= *src && *src <= '9') ||
            (*src == '-' || *src == '_' || *src == '.' || *src == '~')) {
            *out++ = *src;
        } else {
            *out++ = '%';
            *out++ = hex[(*src >> 4) & 0xF];
            *out++ = hex[*src & 0xF];
        }
        src++;
    }
    *out = '\0';
    return ret;
}

// Gửi yêu cầu Text-to-Speech (đã sửa)
void request_tts_from_rapidapi(const char *text) {
    char *encoded = url_encode(text);
    if (!encoded) {
        ESP_LOGE(TAG, "Không thể mã hóa URL");
        return;
    }

    char full_url[1024];
    snprintf(full_url, sizeof(full_url),
             "%s?service=StreamElements&voice=Brian&text=%s",
             RAPIDAPI_URL, encoded);
    free(encoded);

    esp_http_client_config_t config = {
        .url = full_url,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 60000,
        .buffer_size     = 4096,   
        .buffer_size_tx  = 4096, 
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "x-rapidapi-key", RAPIDAPI_KEY);
    esp_http_client_set_header(client, "x-rapidapi-host", RAPIDAPI_HOST);

    ESP_LOGI(TAG, "Gửi yêu cầu Text-to-Speech...");
    esp_http_client_perform(client);
    esp_http_client_cleanup(client);
}
const char* tts_get_audio_url(void) {
    return g_audio_url;
}