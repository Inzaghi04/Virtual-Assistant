#include "wifi.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <ctype.h>

static const char *TAG = "WiFiSetup";
SemaphoreHandle_t xWifiConnected = NULL;
char server_url[512];

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
