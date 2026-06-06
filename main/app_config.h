/* app_config.h – Centralised project configuration */
#pragma once

/* ─── AWS IoT Core ───────────────────────────────────────────────────────── */
#define AWS_IOT_ENDPOINT     "xxxxxx-ats.iot.eu-west-1.amazonaws.com"
#define AWS_IOT_PORT          8883
#define AWS_TOPIC_TELEMETRY  "telemetry/" DEVICE_ID
#define AWS_TOPIC_SHADOW_GET "$aws/things/" DEVICE_ID "/shadow/get"
#define AWS_TOPIC_SHADOW_UPD "$aws/things/" DEVICE_ID "/shadow/update"

/* ─── Device identity ────────────────────────────────────────────────────── */
#define DEVICE_ID            "esp32-001"
#define FW_VERSION           "1.0.0"

/* ─── Wi-Fi ──────────────────────────────────────────────────────────────── */
#define WIFI_SSID            "your-ssid"
#define WIFI_PASS            "your-password"
#define WIFI_MAX_RETRY        10

/* ─── Telemetry ──────────────────────────────────────────────────────────── */
#define PUBLISH_INTERVAL_S   30
#define AWS_MQTT_NET_BUF     4096

/* ─── OTA ────────────────────────────────────────────────────────────────── */
#define OTA_POLL_INTERVAL_S  60
