/**
 * @file    main.c
 * @brief   ESP32 AWS IoT Core Secure Telemetry System
 *
 * Uses the AWS IoT Device SDK for Embedded C (coreMQTT + mbedTLS) to
 * establish a mutual-TLS connection to AWS IoT Core and publish sensor
 * telemetry. Implements:
 *   • TLS 1.3 with X.509 client certificate (stored in NVS flash)
 *   • AWS IoT Device Shadow for desired/reported state sync
 *   • coreMQTT QoS-1 publish with persistent session
 *   • OTA update via AWS IoT Jobs
 *   • Deep-sleep between publishes to minimise power draw
 *
 * SDK:     ESP-IDF v5.x  +  AWS IoT Device SDK for Embedded C v202211
 * Target:  ESP32-S3 (works on vanilla ESP32 with minor pin changes)
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nvs_flash.h"

#include "wifi_manager.h"
#include "aws_iot_client.h"
#include "shadow_handler.h"
#include "ota_handler.h"
#include "sensor_driver.h"
#include "app_config.h"

static const char *TAG = "MAIN";

/* Deep-sleep wakeup stub counter stored in RTC slow memory */
RTC_DATA_ATTR static uint32_t boot_count = 0;

/* ─── Application task prototypes ───────────────────────────────────────── */
static void telemetry_task(void *pvParameters);
static void shadow_task(void *pvParameters);
static void ota_task(void *pvParameters);

/* ═══════════════════════════════════════════════════════════════════════════
 *  app_main
 * ═════════════════════════════════════════════════════════════════════════ */
void app_main(void)
{
    boot_count++;
    ESP_LOGI(TAG, "Boot #%lu | wakeup cause: %d",
             (unsigned long)boot_count,
             (int)esp_sleep_get_wakeup_cause());

    /* ── NVS (stores certs + config) ────────────────────────────────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ── Wi-Fi: connect with retry, post IP to event group ──────────────── */
    wifi_manager_init();
    wifi_manager_connect(WIFI_SSID, WIFI_PASS);
    ESP_LOGI(TAG, "Wi-Fi connected");

    /* ── AWS IoT TLS client init ─────────────────────────────────────────── */
    aws_iot_client_init();   /* loads certs from NVS, establishes TLS session */

    /* ── Spawn application tasks ─────────────────────────────────────────── */
    xTaskCreatePinnedToCore(telemetry_task, "telemetry", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(shadow_task,    "shadow",    6144, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(ota_task,       "ota",       8192, NULL, 3, NULL, 1);
}

/* ─── Telemetry task ──────────────────────────────────────────────────────
 *  Reads sensors, serialises JSON, publishes to AWS IoT topic.
 *  Goes to deep-sleep for PUBLISH_INTERVAL_S seconds between publishes
 *  when deep-sleep mode is enabled in Device Shadow.
 */
static void telemetry_task(void *pvParameters)
{
    (void)pvParameters;
    sensor_data_t   data;
    char            payload[512];

    for (;;) {
        /* Read sensors */
        if (sensor_read(&data) != ESP_OK) {
            ESP_LOGW(TAG, "Sensor read failed – retrying");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* Serialise */
        int len = snprintf(payload, sizeof(payload),
            "{"
            "\"device\":\"%s\","
            "\"boot\":%lu,"
            "\"temp_c\":%.2f,"
            "\"hum_pct\":%.2f,"
            "\"pres_hpa\":%.2f,"
            "\"rssi_dbm\":%d,"
            "\"free_heap\":%lu"
            "}",
            DEVICE_ID,
            (unsigned long)boot_count,
            data.temperature,
            data.humidity,
            data.pressure,
            data.rssi,
            (unsigned long)esp_get_free_heap_size());

        /* Publish QoS-1 */
        esp_err_t pub_rc = aws_iot_publish(AWS_TOPIC_TELEMETRY, payload, len, 1);
        if (pub_rc != ESP_OK) {
            ESP_LOGE(TAG, "Publish failed: %d – reconnecting", pub_rc);
            aws_iot_client_reconnect();
        } else {
            ESP_LOGI(TAG, "Published %d bytes", len);
        }

        /* Deep-sleep if enabled (saves ~95% power vs polling) */
        if (shadow_get_deep_sleep_enabled()) {
            ESP_LOGI(TAG, "Entering deep-sleep for %d s", PUBLISH_INTERVAL_S);
            esp_sleep_enable_timer_wakeup((uint64_t)PUBLISH_INTERVAL_S * 1000000ULL);
            esp_deep_sleep_start();   /* does not return */
        }

        vTaskDelay(pdMS_TO_TICKS(PUBLISH_INTERVAL_S * 1000));
    }
}

/* ─── Shadow task ─────────────────────────────────────────────────────────
 *  Keeps the AWS Device Shadow in sync (desired ↔ reported).
 *  Handles remote config changes: publish interval, sleep mode, LED.
 */
static void shadow_task(void *pvParameters)
{
    (void)pvParameters;
    shadow_handler_init();

    for (;;) {
        shadow_handler_process();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ─── OTA task ────────────────────────────────────────────────────────────
 *  Polls AWS IoT Jobs for pending firmware updates every 60 s.
 *  Downloads via HTTPS, writes to OTA partition, validates, reboots.
 */
static void ota_task(void *pvParameters)
{
    (void)pvParameters;
    ota_handler_init();

    for (;;) {
        ota_handler_poll();
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
