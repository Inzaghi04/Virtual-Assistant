#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "driver/i2s.h"
#include "speech_to_text.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"




#define RAPIDAPI_URL "https://speech-to-text-ai.p.rapidapi.com/transcribe"
#define RAPIDAPI_KEY "YOUR_API_KEY"
#define RAPIDAPI_HOST "speech-to-text-ai.p.rapidapi.com"

#define API_URL "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent"
#define API_KEY "YOUR_API_KEY"
#define MAX_INPUT_LEN 512

#define SAMPLE_RATE     (16000)

SemaphoreHandle_t xUploadDoneSemaphore;
SemaphoreHandle_t xJsonReady = NULL;

static const char *TAG = "UPLOAD_WAV";

static char *g_response_json = NULL;
static char *g_transcript = NULL;
char *g_bot_response = NULL;
char uploaded_url[256] = {0};


/*-------------------- HTTP Event Handler --------------------*/
esp_err_t rapid_event(esp_http_client_event_t *evt) {
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
                //ESP_LOGI("RapidAPI", "Phản hồi JSON:\n%s", output_buffer);
                cJSON *root = cJSON_Parse(output_buffer);
                if (root) {
                    cJSON *text = cJSON_GetObjectItem(root, "text");
                    if (text && cJSON_IsString(text)) {
                        g_transcript = strdup(text->valuestring);
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

void convert_to_direct_url(const char *input_url, char *output_url, size_t len) {
    const char *base = "https://tmpfiles.org/";
    const char *dl_base = "https://tmpfiles.org/dl/";

    if (strncmp(input_url, base, strlen(base)) == 0) {
        snprintf(output_url, len, "%s%s", dl_base, input_url + strlen(base));
    } else {
        strncpy(output_url, input_url, len);
    }
}


void get_text_from_rapidapi() {
    char direct_url[512];
    convert_to_direct_url(uploaded_url, direct_url, sizeof(direct_url));

    char full_url[1024];
    snprintf(full_url, sizeof(full_url),
             "https://speech-to-text-ai.p.rapidapi.com/transcribe?url=%s&lang=en&task=transcribe",
             direct_url);

    esp_http_client_config_t config = {
        .url = full_url,
        .method = HTTP_METHOD_POST,
        .event_handler = rapid_event,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_http_client_set_header(client, "x-rapidapi-key", RAPIDAPI_KEY);
    esp_http_client_set_header(client, "x-rapidapi-host", RAPIDAPI_HOST);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");

    ESP_LOGI(TAG, "Gửi yêu cầu nhận dạng...");
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Yêu cầu thành công, mã trạng thái: %d", esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(TAG, "Yêu cầu thất bại: %s", esp_err_to_name(err));
    }
    esp_http_client_perform(client);
    esp_http_client_cleanup(client);
}


/*-------------------- gemini --------------------*/
void print_pretty_response(const char *json_str) {
    cJSON *json = cJSON_Parse(json_str);
    if (!json) {
        ESP_LOGE(TAG, "Lỗi phân tích JSON!");
        return;
    }
    cJSON *candidates = cJSON_GetObjectItem(json, "candidates");
    if (candidates && cJSON_IsArray(candidates)) {
        cJSON *candidate = cJSON_GetArrayItem(candidates, 0);
        if (candidate) {
            cJSON *content = cJSON_GetObjectItem(candidate, "content");
            if (content) {
                cJSON *parts = cJSON_GetObjectItem(content, "parts");
                if (parts && cJSON_IsArray(parts)) {
                    cJSON *part = cJSON_GetArrayItem(parts, 0);
                    if (part) {
                        cJSON *text = cJSON_GetObjectItem(part, "text");
                        if (text && cJSON_IsString(text)) {
                            g_bot_response = strdup(text->valuestring);
                            printf("\nBot:%s\n", text->valuestring);
                        }
                    }
                }
            }
        }
    }
    cJSON_Delete(json);
}

esp_err_t gemini_handler(esp_http_client_event_t *evt) {
    static char *output_buffer = NULL;
    static int output_len = 0;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                if (output_buffer == NULL) {
                    output_buffer = malloc(evt->data_len + 1);
                    output_len = 0;
                } else {
                    output_buffer = realloc(output_buffer, output_len + evt->data_len + 1);
                }
                memcpy(output_buffer + output_len, evt->data, evt->data_len);
                output_len += evt->data_len;
                output_buffer[output_len] = 0;
            } else {
                // Nếu phản hồi dạng chunked thì cộng dồn vào buffer toàn cục luôn
                if (g_response_json == NULL) {
                    g_response_json = malloc(evt->data_len + 1);
                    memcpy(g_response_json, evt->data, evt->data_len);
                    g_response_json[evt->data_len] = '\0';
                } else {
                    int old_len = strlen(g_response_json);
                    g_response_json = realloc(g_response_json, old_len + evt->data_len + 1);
                    memcpy(g_response_json + old_len, evt->data, evt->data_len);
                    g_response_json[old_len + evt->data_len] = '\0';
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            if (output_buffer != NULL) {
                if (g_response_json != NULL) {
                    free(g_response_json);
                }
                g_response_json = strdup(output_buffer);
                free(output_buffer);
                output_buffer = NULL;
                output_len = 0;
            }
            if (xJsonReady != NULL) xSemaphoreGive(xJsonReady);
            break;
        default:
            break;
    }
    return ESP_OK;
}

void check_gemini_api_connection(const char *prompt) {
    char post_data[1024];
    snprintf(post_data, sizeof(post_data),
             "{\"contents\":[{\"parts\":[{\"text\":\"%s\"}]}]}",
             prompt);

    esp_http_client_config_t config = {
        .url = API_URL "?key=" API_KEY,
        .event_handler = gemini_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Yêu cầu HTTP thất bại: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

void chatbot_task(void) {
    if (xSemaphoreTake(xUploadDoneSemaphore, portMAX_DELAY) == pdTRUE) {
        get_text_from_rapidapi();
    }

        if (g_transcript) {
            printf("You: %s\n", g_transcript);
            free(g_transcript);
            if (strlen(g_transcript) > 0) {
                check_gemini_api_connection(g_transcript);
            }
            if (g_response_json != NULL) {
                print_pretty_response(g_response_json);
                free(g_response_json);
                g_response_json = NULL;
            }
        } else {
            printf("Mời bạn nói lại\n");
    }
}

const char *text(void)
{
    return g_bot_response;
}
