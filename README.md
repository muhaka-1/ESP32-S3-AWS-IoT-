# ESP32 · AWS IoT Core · Secure Telemetry System

> Production-grade IoT telemetry with mutual TLS, Device Shadow sync, OTA updates, and deep-sleep power optimisation.

![Platform](https://img.shields.io/badge/SoC-ESP32--S3-red)
![SDK](https://img.shields.io/badge/ESP--IDF-v5.x-blue)
![Cloud](https://img.shields.io/badge/Cloud-AWS_IoT_Core-orange)
![TLS](https://img.shields.io/badge/Security-TLS_1.3_mTLS-green)
![License](https://img.shields.io/badge/License-MIT-lightgrey)

---

## System Architecture

```
┌────────────────────────────────────────────────────────────────┐
│  ESP32-S3                                                      │
│                                                                │
│  app_main()                                                    │
│    ├── wifi_manager         802.11 WPA2 connect / reconnect    │
│    ├── aws_iot_client       TLS 1.3 mTLS → AWS IoT endpoint   │
│    │     └── coreMQTT       QoS-1 pub/sub                      │
│    │                                                           │
│    ├── telemetry_task  (Core 0, prio 5)                        │
│    │     ├── sensor_read()  BME280 I²C                         │
│    │     ├── JSON serialize                                     │
│    │     ├── aws_iot_publish()                                  │
│    │     └── esp_deep_sleep() ──────────────────────────────┐  │
│    │                                                        │  │
│    ├── shadow_task     (Core 0, prio 4)          wakeup ←──┘  │
│    │     └── desired ↔ reported state sync                     │
│    │         (publish interval, sleep mode, LED)               │
│    │                                                           │
│    └── ota_task        (Core 1, prio 3)                        │
│          └── AWS IoT Jobs poll → HTTPS download → OTA write   │
└────────────────────────────────────────────────────────────────┘
              │ TLS 1.3 (X.509 mTLS)
              ▼
    ┌──────────────────────┐
    │   AWS IoT Core       │
    │                      │
    │  Topics:             │
    │  telemetry/esp32-001 │──► IoT Rule ──► DynamoDB / S3
    │  shadow/get/accepted │              └► CloudWatch
    │  jobs/notify         │
    └──────────────────────┘
```

## Security Model

| Layer | Implementation |
|---|---|
| **Transport** | TLS 1.3 with ECDHE-ECDSA cipher suites |
| **Auth** | X.509 mutual TLS (device cert + private key) |
| **Cert storage** | NVS encrypted flash partition (AES-256 XTS) |
| **Provisioning** | `scripts/provision_certs.py` – one-time NVS write |
| **Key generation** | AWS IoT Console or `aws iot create-keys-and-certificate` |
| **Revocation** | AWS IoT policy detach / certificate deactivation |
| **OTA integrity** | SHA-256 + RSA-2048 signature verified before swap |

## Features

- **Deep-sleep power mode**: ~10 µA sleep, wake every N seconds (configurable via Shadow)
- **Device Shadow**: remote config changes take effect without reflashing
- **OTA Updates**: AWS IoT Jobs + esp_https_ota, dual-partition rollback
- **Automatic reconnect**: Wi-Fi + MQTT with exponential back-off
- **Structured logging**: ESP_LOG with configurable level per component

## Quick Start

### 1. AWS Setup
```bash
# Create a Thing
aws iot create-thing --thing-name esp32-001

# Create cert + key
aws iot create-keys-and-certificate --set-as-active \
  --certificate-pem-outfile certs/device-cert.pem.crt \
  --private-key-outfile     certs/device-private.pem.key \
  --public-key-outfile      certs/device-public.pem.key

# Download AWS Root CA
curl -o certs/AmazonRootCA1.pem \
  https://www.amazontrust.com/repository/AmazonRootCA1.pem

# Attach policy (see docs/iot_policy.json for minimal policy)
aws iot attach-policy --policy-name ESP32TelemetryPolicy \
  --target <certificate-arn>

aws iot attach-thing-principal --thing-name esp32-001 \
  --principal <certificate-arn>
```

### 2. Configure
Edit `main/app_config.h`:
```c
#define AWS_IOT_ENDPOINT    "xxxxxx-ats.iot.eu-west-1.amazonaws.com"
#define AWS_IOT_PORT        8883
#define DEVICE_ID           "esp32-001"
#define WIFI_SSID           "your-ssid"
#define WIFI_PASS           "your-password"
#define PUBLISH_INTERVAL_S  30
```

### 3. Build & Flash
```bash
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

### 4. Provision Certs
```bash
python scripts/provision_certs.py \
    --root-ca certs/AmazonRootCA1.pem \
    --cert    certs/device-cert.pem.crt \
    --key     certs/device-private.pem.key \
    --port    /dev/ttyUSB0
```

## Telemetry Payload

```json
{
  "device": "esp32-001",
  "boot": 42,
  "temp_c": 24.13,
  "hum_pct": 61.07,
  "pres_hpa": 1012.88,
  "rssi_dbm": -67,
  "free_heap": 182432
}
```

## Device Shadow (Remote Config)

```json
{
  "state": {
    "desired": {
      "publish_interval_s": 60,
      "deep_sleep": true,
      "led": "on"
    },
    "reported": {
      "publish_interval_s": 30,
      "deep_sleep": false,
      "fw_version": "1.2.0"
    }
  }
}
```

## Power Budget (Deep-Sleep Mode)

| State | Current | Duration |
|---|---|---|
| Deep sleep | ~10 µA | 29.5 s |
| Wake + Wi-Fi connect | ~180 mA | 300 ms |
| TLS handshake + publish | ~120 mA | 200 ms |
| **Average (30 s cycle)** | **~2.7 mA** | — |

On a 2000 mAh LiPo → ~740 days battery life.

## License
MIT © 2025
