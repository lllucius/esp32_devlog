// Copyright 2017-2018 Leland Lucius
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <string.h>

#include "esp_err.h"
#include "esp_event_loop.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "devlog.h"

// set these either here or via "make menuconfig"
#define WIFI_SSID       CONFIG_DEVLOG_WIFI_SSID
#define WIFI_PASS       CONFIG_DEVLOG_WIFI_PASS
#define SYSLOG_IP       CONFIG_DEVLOG_SYSLOG_IP
#define SYSLOG_PORT     CONFIG_DEVLOG_SYSLOG_PORT

static EventGroupHandle_t wifi_event_group;
static const int WIFI_CONNECTED = BIT0;

static esp_err_t wifi_events(void *ctx, system_event_t *event)
{
    switch (event->event_id)
    {
        case SYSTEM_EVENT_STA_START:
            ESP_ERROR_CHECK(esp_wifi_connect());
        break;

        case SYSTEM_EVENT_STA_GOT_IP:
            xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED);
        break;

        case SYSTEM_EVENT_STA_DISCONNECTED:
            xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED);
            ESP_ERROR_CHECK(esp_wifi_connect());
        break;
    
        default:
        break;
    }

    return ESP_OK;
}

static void initialize_wifi(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    tcpip_adapter_init();

    wifi_event_group = xEventGroupCreate();
    assert(wifi_event_group != NULL);

    ESP_ERROR_CHECK(esp_event_loop_init(wifi_events, NULL));

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t cfg = {};
    strncpy((char *) &cfg.sta.ssid, WIFI_SSID, sizeof(cfg.sta.ssid));
    strncpy((char *) &cfg.sta.password, WIFI_PASS, sizeof(cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &cfg));

    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(wifi_event_group,
                        WIFI_CONNECTED,
                        false,
                        true,
                        portMAX_DELAY); 
}

extern "C" void app_main()
{
    // Emit some output (will be captured by devlog)
    printf("Testing devlog...\n");
    fprintf(stderr, "Stderr goes to devlog too.\n");
    ets_printf("Even the ROM output gets routed through devlog.\n");

    // Get WIFI going
    initialize_wifi();

    // Set the logging server
    devlog_set_udp_destination(SYSLOG_IP, SYSLOG_PORT);

    // Set the retention buffer size
    devlog_set_retention_destination(256);

    printf("Now output gets captured to the retention buffer as well.\n");

    char buf[1024];
    int cnt = devlog_get_retention_content(buf, 1024, false);
    printf("%d bytes retrieved from retention buffer\n", cnt);

    cnt = devlog_get_retention_content(buf, 1024, true);
    printf("the buffer has %d bytes now\n", cnt);

    cnt = devlog_get_retention_content(buf, 1024, false);
    printf("after clearing, buffer has only %d bytes\n", cnt);

    vTaskDelay(portMAX_DELAY);
    // Never reached
}

