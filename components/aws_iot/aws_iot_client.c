/**
 * @file    aws_iot_client.c
 * @brief   AWS IoT Core coreMQTT client with mutual TLS (mbedTLS)
 *
 * Loads device certificate, private key, and AWS root CA from NVS flash.
 * Establishes TLS 1.3 session to AWS IoT endpoint and wraps coreMQTT.
 *
 * Certificate provisioning:
 *   Use scripts/provision_certs.py to write certs to NVS before first boot.
 */

#include "aws_iot_client.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

/* coreMQTT */
#include "core_mqtt.h"
#include "core_mqtt_state.h"

static const char *TAG = "AWS_IOT";

/* ─── Module state ───────────────────────────────────────────────────────── */
static MQTTContext_t       mqtt_ctx;
static MQTTFixedBuffer_t   mqtt_buf;
static uint8_t             network_buf[AWS_MQTT_NET_BUF];
static esp_tls_t          *tls_conn = NULL;

/* Cert buffers (loaded from NVS) */
static char root_ca[4096];
static char client_cert[2048];
static char client_key[2048];

/* ─── NVS cert loader ────────────────────────────────────────────────────── */
static esp_err_t load_cert_from_nvs(const char *key, char *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t rc = nvs_open("certs", NVS_READONLY, &h);
    if (rc != ESP_OK) return rc;
    rc = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    return rc;
}

/* ─── coreMQTT transport callbacks (over mbedTLS/esp-tls) ─────────────── */
static int32_t tls_recv(NetworkContext_t *ctx, void *buf, size_t len)
{
    (void)ctx;
    return esp_tls_conn_read(tls_conn, buf, len);
}

static int32_t tls_send(NetworkContext_t *ctx, const void *buf, size_t len)
{
    (void)ctx;
    return esp_tls_conn_write(tls_conn, buf, len);
}

/* ─── Public API ─────────────────────────────────────────────────────────── */

esp_err_t aws_iot_client_init(void)
{
    /* Load certs from NVS */
    ESP_ERROR_CHECK(load_cert_from_nvs("root_ca",     root_ca,     sizeof(root_ca)));
    ESP_ERROR_CHECK(load_cert_from_nvs("client_cert", client_cert, sizeof(client_cert)));
    ESP_ERROR_CHECK(load_cert_from_nvs("client_key",  client_key,  sizeof(client_key)));

    /* TLS config */
    esp_tls_cfg_t tls_cfg = {
        .cacert_buf        = (const uint8_t *)root_ca,
        .cacert_bytes      = strlen(root_ca) + 1,
        .clientcert_buf    = (const uint8_t *)client_cert,
        .clientcert_bytes  = strlen(client_cert) + 1,
        .clientkey_buf     = (const uint8_t *)client_key,
        .clientkey_bytes   = strlen(client_key) + 1,
        .non_block         = false,
        .timeout_ms        = 10000,
    };

    tls_conn = esp_tls_conn_new_sync(AWS_IOT_ENDPOINT, strlen(AWS_IOT_ENDPOINT),
                                     AWS_IOT_PORT, &tls_cfg);
    if (!tls_conn) {
        ESP_LOGE(TAG, "TLS connection failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "TLS connected to %s:%d", AWS_IOT_ENDPOINT, AWS_IOT_PORT);

    /* coreMQTT setup */
    TransportInterface_t transport = {
        .recv            = tls_recv,
        .send            = tls_send,
        .pNetworkContext = NULL,
    };

    mqtt_buf.pBuffer = network_buf;
    mqtt_buf.size    = sizeof(network_buf);

    MQTTStatus_t ms = MQTT_Init(&mqtt_ctx, &transport, NULL, NULL, &mqtt_buf);
    if (ms != MQTTSuccess) {
        ESP_LOGE(TAG, "MQTT_Init failed: %d", ms);
        return ESP_FAIL;
    }

    /* CONNECT */
    MQTTConnectInfo_t conn_info = {
        .cleanSession        = true,
        .pClientIdentifier   = DEVICE_ID,
        .clientIdentifierLength = strlen(DEVICE_ID),
        .keepAliveSeconds    = 60,
    };

    bool session_present;
    ms = MQTT_Connect(&mqtt_ctx, &conn_info, NULL, 5000, &session_present);
    if (ms != MQTTSuccess) {
        ESP_LOGE(TAG, "MQTT_Connect failed: %d", ms);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "MQTT connected (session_present=%d)", session_present);
    return ESP_OK;
}

esp_err_t aws_iot_publish(const char *topic, const char *payload, int len, uint8_t qos)
{
    MQTTPublishInfo_t pub = {
        .qos             = (MQTTQoS_t)qos,
        .retain          = false,
        .dup             = false,
        .pTopicName      = topic,
        .topicNameLength = strlen(topic),
        .pPayload        = payload,
        .payloadLength   = len,
    };
    uint16_t packet_id = MQTT_GetPacketId(&mqtt_ctx);
    MQTTStatus_t ms = MQTT_Publish(&mqtt_ctx, &pub, packet_id);
    return (ms == MQTTSuccess) ? ESP_OK : ESP_FAIL;
}

esp_err_t aws_iot_subscribe(const char *topic, uint8_t qos,
                             MQTTEventCallback_t callback)
{
    MQTTSubscribeInfo_t sub = {
        .qos             = (MQTTQoS_t)qos,
        .pTopicFilter    = topic,
        .topicFilterLength = strlen(topic),
    };
    uint16_t packet_id = MQTT_GetPacketId(&mqtt_ctx);
    MQTTStatus_t ms = MQTT_Subscribe(&mqtt_ctx, &sub, 1, packet_id);
    (void)callback; /* Register separately via MQTT_Init eventCallback */
    return (ms == MQTTSuccess) ? ESP_OK : ESP_FAIL;
}

esp_err_t aws_iot_client_reconnect(void)
{
    ESP_LOGW(TAG, "Reconnecting...");
    esp_tls_conn_destroy(tls_conn);
    tls_conn = NULL;
    vTaskDelay(pdMS_TO_TICKS(3000));
    return aws_iot_client_init();
}
