#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

// ----- Pin Definitions -----
const int BEACON_LED      = 5;   // green heartbeat beacon
const int ALERT_LED       = 4;   // red warning beacon
const int BUTTON_PIN      = 18;  // physical mode toggle button
const int DOSIMETER_PIN   = 34;  // analog input for “radiation sensor”

// ----- Parameters -----
const int MAX_EVENTS         = 10;
const int EXPOSURE_THRESHOLD = 3000;  // 12‑bit scale ⇒ 0–4095
const int DEBOUNCE_MS        = 50;

// ----- Wi‑Fi Credentials -----
const char* WIFI_SSID = "Wokwi-GUEST";//"WiFiMyWay-f200";
const char* WIFI_PWD  = "";//"17479f58ea";

// ----- HTTP Server -----
WebServer console(80);

// ----- FreeRTOS primitives -----
SemaphoreHandle_t sensor_alert_sem;
SemaphoreHandle_t button_event_sem;
SemaphoreHandle_t serial_mutex;
QueueHandle_t    sensor_queue;

volatile bool system_mode        = false;  // NORMAL = false, ALERT = true
volatile bool alert_state        = false;  // true while blinking red
volatile int  latest_sensor_value = 0;

// ----- Forward declarations -----
void sendConsolePage();
void commsTask(void* pv);
void heartbeatTask(void* pv);
void modeLedTask(void* pv);
void sensorTask(void* pv);
void sensorConsumerTask(void* pv);
void buttonTask(void* pv);
void eventResponseTask(void* pv);

// ----- Web UI handlers -----
void sendConsolePage() {
  String html = "<html><body><h1>🚀 Space Radiation Control Panel</h1>";
  html += "<p>Mode: <b>" + String(system_mode ? "ALERT" : "NORMAL") + "</b></p>";
  html += "<p>Alert: <b>" + String(alert_state ? "ACTIVE" : "CLEAR") + "</b></p>";
  html += "<p>Latest Radiation: <b>" + String(latest_sensor_value) + "</b></p>";
  html += "<p><a href=\"/toggle\"><button>Toggle Mode</button></a></p>";
  html += "</body></html>";
  console.send(200, "text/html", html);
}

void handleRoot() {
  sendConsolePage();
}
void handleToggle() {
  xSemaphoreGive(button_event_sem);
  sendConsolePage();
}

// ----- Setup & Loop -----
void setup() {
  Serial.begin(115200);

  // create synchronization primitives
  sensor_alert_sem = xSemaphoreCreateCounting(MAX_EVENTS, 0);
  button_event_sem = xSemaphoreCreateBinary();
  serial_mutex     = xSemaphoreCreateMutex();
  sensor_queue     = xQueueCreate(20, sizeof(int));

  // HTTP routes
  console.on("/",      handleRoot);
  console.on("/toggle", handleToggle);

  // spawn FreeRTOS tasks
  xTaskCreate(commsTask,           "Comms",     4096, NULL, 3, NULL);
  xTaskCreate(sensorTask,          "Sensor",    2048, NULL, 4, NULL);
  xTaskCreate(sensorConsumerTask,  "Consume",   2048, NULL, 3, NULL);
  xTaskCreate(buttonTask,          "Button",    2048, NULL, 6, NULL);
  xTaskCreate(eventResponseTask,   "Respond",   2048, NULL, 2, NULL);
  xTaskCreate(heartbeatTask,       "Heartbeat", 1024, NULL, 1, NULL);
  xTaskCreate(modeLedTask,         "ModeLED",   1024, NULL, 2, NULL);
}

void loop() {
  // nothing here; all work is in tasks
  vTaskDelay(pdMS_TO_TICKS(1000));
}

// ----- Task Definitions -----
void commsTask(void* pv) {
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  Serial.print("[COMMS] Connecting to Wi‑Fi");
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
  }
  Serial.println(" Connected, IP=" + WiFi.localIP().toString());
  console.begin();
  Serial.println("[COMMS] Web server started");

  for (;;) {
    console.handleClient();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void heartbeatTask(void* pv) {
  pinMode(BEACON_LED, OUTPUT);
  const TickType_t halfSec = pdMS_TO_TICKS(500);
  while (1) {
    digitalWrite(BEACON_LED, HIGH);
    vTaskDelay(halfSec);
    digitalWrite(BEACON_LED, LOW);
    vTaskDelay(halfSec);
  }
}

void modeLedTask(void* pv) {
  pinMode(BEACON_LED, OUTPUT);
  while (1) {
    digitalWrite(BEACON_LED, system_mode);
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void sensorTask(void* pv) {
  bool lastAlert = false;
  while (1) {
    int reading = analogRead(DOSIMETER_PIN);
    xQueueSend(sensor_queue, &reading, 0);

    if (reading > EXPOSURE_THRESHOLD && !lastAlert) {
      lastAlert = true;
      xSemaphoreGive(sensor_alert_sem);
    } else if (reading <= EXPOSURE_THRESHOLD) {
      lastAlert = false;
    }

    vTaskDelay(pdMS_TO_TICKS(17));
  }
}

void sensorConsumerTask(void* pv) {
  int val;
  while (1) {
    if (xQueueReceive(sensor_queue, &val, portMAX_DELAY) == pdTRUE) {
      latest_sensor_value = val;
    }
  }
}

void buttonTask(void* pv) {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  TickType_t last = 0;
  while (1) {
    if (digitalRead(BUTTON_PIN) == LOW) {
      TickType_t now = xTaskGetTickCount();
      if ((now - last) * portTICK_PERIOD_MS > DEBOUNCE_MS) {
        last = now;
        xSemaphoreGive(button_event_sem);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void eventResponseTask(void* pv) {
  pinMode(ALERT_LED, OUTPUT);
  while (1) {
    if (xSemaphoreTake(sensor_alert_sem, pdMS_TO_TICKS(100)) == pdTRUE) {
      xSemaphoreTake(serial_mutex, portMAX_DELAY);
      Serial.println("🚨 Sensor ALERT!");
      xSemaphoreGive(serial_mutex);

      alert_state = true;
      for (int i = 0; i < 3; ++i) {
        digitalWrite(ALERT_LED, HIGH);
        vTaskDelay(pdMS_TO_TICKS(200));
        digitalWrite(ALERT_LED, LOW);
        vTaskDelay(pdMS_TO_TICKS(200));
      }
      alert_state = false;
    }

    if (xSemaphoreTake(button_event_sem, 0) == pdTRUE) {
      system_mode = !system_mode;
      xSemaphoreTake(serial_mutex, portMAX_DELAY);
      Serial.print("🔄 Mode toggled to ");
      Serial.println(system_mode ? "ALERT" : "NORMAL");
      xSemaphoreGive(serial_mutex);
    }
  }
}
