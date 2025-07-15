#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "esp_http_server.h"
#include "driver/gpio.h"

static const char *TAG = "space_monitor";

// ----- GPIO & Wi-Fi config -----
#define LED_RED_PIN        GPIO_NUM_2   // Alert LED
#define LED_GREEN_PIN      GPIO_NUM_4   // Heartbeat & Mode LED
#define BUTTON_PIN         GPIO_NUM_15  // Physical button
#define DOSIMETER_CHANNEL  ADC1_CHANNEL_6 // GPIO34

#define WIFI_SSID          "Wokwi-GUEST"
#define WIFI_PASS          ""
#define MAX_RETRY          5

// ----- System state & Wi-Fi event group -----
static volatile bool     system_mode         = false;  // NORMAL=false, ALERT=true
static volatile bool     alert_state         = false;  // set true during blink sequence
static volatile bool     led_state           = false;  // unused here, could drive extra LED
static volatile int      latest_sensor_value = 0;      // updated by queue consumer

static EventGroupHandle_t wifi_event_group;
#define GOT_IP_BIT   BIT0
#define FAIL_BIT     BIT1
static int retry_count = 0;

// ----- Synchronization primitives -----
static SemaphoreHandle_t serial_mutex;
static SemaphoreHandle_t sensor_alert_sem;
static SemaphoreHandle_t button_event_sem;
static QueueHandle_t    sensor_queue;  // queue of ints (sensor readings)

// ----- HTTP UI handlers -----
static void send_console(httpd_req_t *req) {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "<html><body><h1>🚀 Space Monitor</h1>"
        "<p>Mode: <b>%s</b></p>"
        "<p>Alert: <b>%s</b></p>"
        "<p>Latest Radiation: <b>%d</b></p>"
        "<p><a href=\"/toggle\">Toggle Mode</a></p>"
        "</body></html>",
        system_mode ? "ALERT" : "NORMAL",
        alert_state  ? "ACTIVE" : "CLEAR",
        latest_sensor_value
    );
    httpd_resp_send(req, buf, strlen(buf));
}
static esp_err_t root_get_handler(httpd_req_t *req) {
    send_console(req);
    return ESP_OK;
}
static esp_err_t toggle_mode_handler(httpd_req_t *req) {
    xSemaphoreGive(button_event_sem);
    send_console(req);
    return ESP_OK;
}
static const httpd_uri_t uri_root   = { "/",      HTTP_GET, root_get_handler   };
static const httpd_uri_t uri_toggle = { "/toggle",HTTP_GET, toggle_mode_handler };

// ----- HTTP server task ----- (prio 5)
static void http_task(void* pv) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    xSemaphoreTake(serial_mutex, portMAX_DELAY);
    ESP_LOGI(TAG, "HTTP Server starting");
    xSemaphoreGive(serial_mutex);

    if (httpd_start(&server, &config)==ESP_OK) {
        httpd_register_uri_handler(server, &uri_root);
        httpd_register_uri_handler(server, &uri_toggle);
    }
    vTaskDelete(NULL);
}

// ----- Heartbeat task ----- (prio 1)
static void heartbeat_task(void* pv) {
    gpio_reset_pin(LED_GREEN_PIN);
    gpio_set_direction(LED_GREEN_PIN, GPIO_MODE_OUTPUT);
    const TickType_t half = pdMS_TO_TICKS(1000);
    while (1) {
        gpio_set_level(LED_GREEN_PIN, 1);
        vTaskDelay(half);
        gpio_set_level(LED_GREEN_PIN, 0);
        vTaskDelay(half);
    }
}

// ----- Mode LED task ----- (prio 3)
static void mode_led_task(void* pv) {
    gpio_reset_pin(LED_GREEN_PIN);
    gpio_set_direction(LED_GREEN_PIN, GPIO_MODE_OUTPUT);
    while (1) {
        gpio_set_level(LED_GREEN_PIN, system_mode);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ----- Sensor Monitor Task ----- (prio 4)
static void sensor_task(void* pv) {
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(DOSIMETER_CHANNEL, ADC_ATTEN_DB_11);
    const int THRESH = 3000;
    bool last = false;
    while (1) {
        int v = adc1_get_raw(DOSIMETER_CHANNEL);
        // send every sample to queue
        xQueueSend(sensor_queue, &v, 0);
        // give semaphore on threshold cross
        if (v > THRESH && !last) {
            last = true;
            xSemaphoreGive(sensor_alert_sem);
        } else if (v <= THRESH) {
            last = false;
        }
        vTaskDelay(pdMS_TO_TICKS(17));
    }
}

// ----- Sensor Queue Consumer Task ----- (prio 3)
static void sensor_consumer_task(void* pv) {
    int v;
    while (1) {
        if (xQueueReceive(sensor_queue, &v, portMAX_DELAY)==pdTRUE) {
            latest_sensor_value = v;
        }
    }
}

// ----- Button Watch Task ----- (prio 6)
static void button_task(void* pv) {
    gpio_reset_pin(BUTTON_PIN);
    gpio_set_direction(BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_PIN, GPIO_PULLUP_ONLY);
    TickType_t last=0;
    while (1) {
        if (gpio_get_level(BUTTON_PIN)==0) {
            TickType_t now = xTaskGetTickCount();
            if ((now-last)*portTICK_PERIOD_MS > 50) {
                last=now;
                xSemaphoreGive(button_event_sem);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ----- Unified Event Response Task ----- (prio 2)
static void event_response_task(void* pv) {
    gpio_reset_pin(LED_RED_PIN);
    gpio_set_direction(LED_RED_PIN, GPIO_MODE_OUTPUT);
    while (1) {
        if (xSemaphoreTake(sensor_alert_sem, pdMS_TO_TICKS(100))==pdTRUE) {
            xSemaphoreTake(serial_mutex, portMAX_DELAY);
            ESP_LOGW(TAG, "Sensor ALERT!");
            xSemaphoreGive(serial_mutex);
            alert_state = true;
            for (int i=0;i<3;i++){
                gpio_set_level(LED_RED_PIN,1);
                vTaskDelay(pdMS_TO_TICKS(200));
                gpio_set_level(LED_RED_PIN,0);
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            alert_state=false;
        }
        if (xSemaphoreTake(button_event_sem, 0)==pdTRUE) {
            system_mode = !system_mode;
            xSemaphoreTake(serial_mutex, portMAX_DELAY);
            ESP_LOGI(TAG, "Mode -> %s", system_mode?"ALERT":"NORMAL");
            xSemaphoreGive(serial_mutex);
        }
    }
}

// ----- Wi-Fi station task ----- (prio 5)
static void wifi_task(void* pv) {
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg=WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,&wifi_event_handler,NULL,NULL));
    wifi_config_t wcfg={ .sta={.ssid=WIFI_SSID,.password=WIFI_PASS}};
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    xSemaphoreTake(serial_mutex, portMAX_DELAY);
    ESP_LOGI(TAG, "wifi_init_sta done");
    xSemaphoreGive(serial_mutex);

    EventBits_t b = xEventGroupWaitBits(
        wifi_event_group, GOT_IP_BIT|FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY
    );
    if (b & GOT_IP_BIT) {
        xSemaphoreTake(serial_mutex, portMAX_DELAY);
        ESP_LOGI(TAG, "Wi-Fi OK, starting HTTP and tasks");
        xSemaphoreGive(serial_mutex);
        xTaskCreate(http_task,       "http",   4096, NULL, 5, NULL);
    } else {
        xSemaphoreTake(serial_mutex, portMAX_DELAY);
        ESP_LOGE(TAG, "Wi-Fi failed");
        xSemaphoreGive(serial_mutex);
    }
    vEventGroupDelete(wifi_event_group);
    vTaskDelete(NULL);
}

// ----- Wi-Fi event handler -----
static void wifi_event_handler(void* arg,
    esp_event_base_t eb, int32_t id, void* ed)
{
    if (eb==WIFI_EVENT && id==WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (eb==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_count<MAX_RETRY) {
            esp_wifi_connect(); retry_count++;
        } else {
            xEventGroupSetBits(wifi_event_group, FAIL_BIT);
        }
    } else if (eb==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, GOT_IP_BIT);
    }
}

// ----- Main entry -----
void app_main(void) {
    esp_err_t r = nvs_flash_init();
    if (r==ESP_ERR_NVS_NO_FREE_PAGES||r==ESP_ERR_NVS_NEW_VERSION_FOUND){
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(r);

    // Create sync primitives
    sensor_alert_sem = xSemaphoreCreateCounting(10, 0);
    button_event_sem = xSemaphoreCreateBinary();
    serial_mutex     = xSemaphoreCreateMutex();
    sensor_queue     = xQueueCreate(20, sizeof(int));

    // Spawn all tasks
    xTaskCreate(wifi_task,            "wifi",       4096, NULL, 5, NULL);
    xTaskCreate(sensor_task,          "sensor",     2048, NULL, 4, NULL);
    xTaskCreate(sensor_consumer_task, "consume",    2048, NULL, 3, NULL);
    xTaskCreate(event_response_task,  "respond",    2048, NULL, 2, NULL);
    xTaskCreate(button_task,          "button",     2048, NULL, 6, NULL);
    xTaskCreate(heartbeat_task,       "heartbeat",  1024, NULL, 1, NULL);
    xTaskCreate(mode_led_task,        "mode_led",   1024, NULL, 3, NULL);
}
