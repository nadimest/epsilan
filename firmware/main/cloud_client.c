#include "cloud_client.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "cloud_config.h"
#include "sila_server.h"
#include "unitelabs_yr2_ca.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/error.h"
#include "nghttp2/nghttp2.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define NV(NAME, VALUE) { (uint8_t *)(NAME), (uint8_t *)(VALUE), sizeof(NAME) - 1, strlen(VALUE), NGHTTP2_NV_FLAG_NONE }
#define CLOUD_CONNECT_PATH "/sila2.org.silastandard.CloudClientEndpoint/ConnectSiLAServer"

static const char *TAG = "epsilan_cloud";
static bool task_started;
static bool sntp_started;

static void cloud_log_memory(const char *stage)
{
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ESP_LOGI(TAG,
             "Memory %s: internal free=%u largest=%u low=%u; psram free=%u largest=%u low=%u",
             stage,
             (unsigned)heap_caps_get_free_size(internal_caps),
             (unsigned)heap_caps_get_largest_free_block(internal_caps),
             (unsigned)heap_caps_get_minimum_free_size(internal_caps),
             (unsigned)heap_caps_get_free_size(psram_caps),
             (unsigned)heap_caps_get_largest_free_block(psram_caps),
             (unsigned)heap_caps_get_minimum_free_size(psram_caps));
}

// The gateway serves leaf -> YR2 -> Root YR (cross-signed by ISRG Root X1).
// ESP-IDF 5.4's bundle predates this 2026 hierarchy.  Supplying both public
// CA certificates lets mbedTLS build that chain while keeping verification on.
static const char unitelabs_ca_chain_pem[] __attribute__((unused)) =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n";

// Self-signed Root YR is the actual trust anchor for the YR2 hierarchy.
// Its SHA-256 fingerprint is E5:7B:7E:6F:15:0C:41:91:02:E8:D5:C0:55:72:9F:F9:
// 67:B9:D1:A8:29:BF:00:CE:C8:9C:A6:04:EB:F4:A8:6F (Let's Encrypt).
static const char unitelabs_root_yr_pem[] __attribute__((unused)) =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIFKTCCAxGgAwIBAgIRAOxGNJNgz0sP+KmC2Tqpyj0wDQYJKoZIhvcNAQELBQAw\n"
    "LjELMAkGA1UEBhMCVVMxDTALBgNVBAoTBElTUkcxEDAOBgNVBAMTB1Jvb3QgWVIw\n"
    "HhcNMjUwOTAzMDAwMDAwWhcNNDUwOTAyMjM1OTU5WjAuMQswCQYDVQQGEwJVUzEN\n"
    "MAsGA1UEChMESVNSRzEQMA4GA1UEAxMHUm9vdCBZUjCCAiIwDQYJKoZIhvcNAQEB\n"
    "BQADggIPADCCAgoCggIBANvGJnN78CTJdWL3+eGfsLN5TrNBJs+VH9hRXqRbwxu9\n"
    "sGNiB0BD1fcOxbSUQCJIM1xE13Db+5Cw1w0s0EBYsvuIP/6joF0w8cuImbgR1OGg\n"
    "YbSQ4OpzI+DG8SGuTlcE873OCS+kh3srlo6vl43M5OJg4Aeo1sfHp6kTJDoIiFBN\n"
    "JAY+OKfX/FUvYKuhjT+no49lmqmupSBI5PkBQiqrEGtWU5uxU/cQWHGu8jSjFBzn\n"
    "ZqvbNPLMXMLFxCb3WTfrJBXXjqvWG+v4bjzxjjeAtOlU7qarRDvNOyAuQYLln904\n"
    "M+faKx8hnLCpJ15ZqaEgcNlY+9MMWcC5yvL2A2j3l9+2buggZX+dOE91zYmIdawT\n"
    "vSZuVvlbRrAlLxIB6pwMBjneXCjYQ8+3BCCjssbSNpZU3hTcBDdhfAlEDlYr6pEa\n"
    "tnMdmDT5BqnKC92bd0EhM1fbLHioLccLCuievT8ZkPhZrq7Mii7gNXAcUEAR8+lz\n"
    "Yal+9zTg7C5DALyVOeG/CqfRAMn1KSHCR0NSA6P8tn/mGRlnCct5rtVCLnVySVpU\n"
    "6H1qGg3DgTOuskf8eahTMiYbI5ezPJmO5ertalskQ1utp74+eDy92PI4ftHKTbq9\n"
    "IWhH4YZKh3WnJEIt+oQvlYZbY8tpEroKrFB6PFGzrJIDRyts4HqvuH52RFj2zv/B\n"
    "AgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8EBTADAQH/MB0GA1Ud\n"
    "DgQWBBTe51tg0CJtQCh9Pw0B/qS1UrRRlDANBgkqhkiG9w0BAQsFAAOCAgEAWHnf\n"
    "713Bdkq7t5yN2dNIgQakUb94X9WuyhMEHHkgx4oDpSUlnG0w4g94MoqaEUE31ZjR\n"
    "LU7L5LD1g9ujFHTQu8AD215AHMVQFbm6j8hQxdXHAzDajFNQnOlDJrLjzIx176oy\n"
    "AjvUtejZx2NNmdb5fd0WGVGsCdoAJ3N8ozo7ajE8t6vfxStZb4BQ9WYJGHUDrv2N\n"
    "i5tJF6CNiPnlzs3BUfECRbE4JSk+jvy8+VoGiFE8qsH/j78x2fjgQhAQFV7P7Zxy\n"
    "dBTZ1wEkNpZNW2qnaK1SKBLa+xf6E06YRIq5uaI+HWH8SY1y5VbRgzq40EKg3yxP\n"
    "06fz+uYAUIFJoLNfhwRCc3Q6pQVuMX3yAjHAes4gk4moGcLQ5p7HAh39yeylZc1J\n"
    "41sx/jKwLIkPE6Rr1Nf4pxdsxf9SA4yOEiAkDgq04DVxn8hgYFdUtBCuiuVC2heA\n"
    "EiqVEa+8QZjuw8Gj0EbHXcRd1nInvGqRS1o9Is7YBdQN57X1AYveGBNNqjICSb7c\n"
    "awuw1EawTDrs13VUlJVEsbQ0/O/1aaV73mCdOQ8azqL2KTv1Ewu1xbquE2S+kdQU\n"
    "To9TUwat3wUA6cwXh1EfpS/3fJ0aGah5hdpRyoCLDlsSn8tkrjMfFFX0viC+GxHc\n"
    "sI1ANRYvqSFC2X1VRZfDg+wD6E21BccmifG4yWc=\n"
    "-----END CERTIFICATE-----\n";

typedef struct cloud_outbound {
    struct cloud_outbound *next;
    uint8_t *data;
    size_t length;
    size_t offset;
} cloud_outbound_t;

typedef struct {
    esp_tls_t *tls;
    nghttp2_session *session;
    int32_t stream_id;
    bool stream_closed;
    uint8_t inbound[2048];
    size_t inbound_length;
    cloud_outbound_t *outbound_head;
    cloud_outbound_t *outbound_tail;
} cloud_connection_t;

static size_t cloud_varint_size(size_t value)
{
    size_t size = 1;
    while (value >= 0x80) { value >>= 7; ++size; }
    return size;
}

static uint8_t *cloud_write_varint(uint8_t *out, size_t value)
{
    while (value >= 0x80) {
        *out++ = (uint8_t)(value | 0x80);
        value >>= 7;
    }
    *out++ = (uint8_t)value;
    return out;
}

static bool cloud_read_varint(const uint8_t **cursor, const uint8_t *end, size_t *value)
{
    size_t result = 0;
    unsigned shift = 0;
    while (*cursor < end && shift < sizeof(size_t) * 8) {
        uint8_t byte = *(*cursor)++;
        result |= (size_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) { *value = result; return true; }
        shift += 7;
    }
    return false;
}

static bool cloud_read_bytes_field(const uint8_t *data, size_t length, size_t wanted,
                                   const uint8_t **value, size_t *value_length)
{
    const uint8_t *cursor = data;
    const uint8_t *end = data + length;
    while (cursor < end) {
        size_t tag;
        size_t size;
        if (!cloud_read_varint(&cursor, end, &tag)) return false;
        if ((tag & 7) != 2 || !cloud_read_varint(&cursor, end, &size) ||
            size > (size_t)(end - cursor)) return false;
        if (tag >> 3 == wanted) {
            *value = cursor;
            *value_length = size;
            return true;
        }
        cursor += size;
    }
    return false;
}

static uint8_t *cloud_bytes_field(size_t field, const uint8_t *data, size_t length,
                                  size_t *out_length)
{
    size_t total = cloud_varint_size((field << 3) | 2) + cloud_varint_size(length) + length;
    uint8_t *out = malloc(total ? total : 1);
    if (!out) return NULL;
    uint8_t *cursor = cloud_write_varint(out, (field << 3) | 2);
    cursor = cloud_write_varint(cursor, length);
    if (length) memcpy(cursor, data, length);
    *out_length = total;
    return out;
}

static uint8_t *cloud_server_message(const uint8_t *uuid, size_t uuid_length,
                                     size_t body_field, const uint8_t *body, size_t body_length,
                                     size_t *out_length)
{
    size_t uuid_field_length;
    size_t body_field_length;
    uint8_t *uuid_field = cloud_bytes_field(1, uuid, uuid_length, &uuid_field_length);
    uint8_t *wrapped_body = cloud_bytes_field(body_field, body, body_length, &body_field_length);
    if (!uuid_field || !wrapped_body) { free(uuid_field); free(wrapped_body); return NULL; }
    uint8_t *out = malloc(uuid_field_length + body_field_length);
    if (!out) { free(uuid_field); free(wrapped_body); return NULL; }
    memcpy(out, uuid_field, uuid_field_length);
    memcpy(out + uuid_field_length, wrapped_body, body_field_length);
    free(uuid_field);
    free(wrapped_body);
    *out_length = uuid_field_length + body_field_length;
    return out;
}

static uint8_t *cloud_error_message(const uint8_t *uuid, size_t uuid_length, bool property,
                                    const char *text, size_t *out_length)
{
    size_t string_length, undefined_length;
    uint8_t *string = cloud_bytes_field(1, (const uint8_t *)text, strlen(text), &string_length);
    uint8_t *undefined = string ? cloud_bytes_field(3, string, string_length, &undefined_length) : NULL;
    free(string);
    if (!undefined) return NULL;
    uint8_t *out = cloud_server_message(uuid, uuid_length, property ? 17 : 16,
                                        undefined, undefined_length, out_length);
    free(undefined);
    return out;
}

static void cloud_queue_message(cloud_connection_t *connection, const uint8_t *protobuf,
                                size_t protobuf_length)
{
    if (!protobuf || protobuf_length > 0xffff) return;
    cloud_outbound_t *item = calloc(1, sizeof(*item));
    if (!item) return;
    item->length = protobuf_length + 5;
    item->data = malloc(item->length);
    if (!item->data) { free(item); return; }
    item->data[0] = 0;
    item->data[1] = (uint8_t)(protobuf_length >> 24);
    item->data[2] = (uint8_t)(protobuf_length >> 16);
    item->data[3] = (uint8_t)(protobuf_length >> 8);
    item->data[4] = (uint8_t)protobuf_length;
    memcpy(item->data + 5, protobuf, protobuf_length);
    if (connection->outbound_tail) connection->outbound_tail->next = item;
    else connection->outbound_head = item;
    connection->outbound_tail = item;
    nghttp2_session_resume_data(connection->session, connection->stream_id);
}

static void cloud_free_outbound(cloud_connection_t *connection)
{
    while (connection->outbound_head) {
        cloud_outbound_t *item = connection->outbound_head;
        connection->outbound_head = item->next;
        free(item->data);
        free(item);
    }
    connection->outbound_tail = NULL;
}

static nghttp2_ssize cloud_send_callback(nghttp2_session *session, const uint8_t *data,
                                         size_t length, int flags, void *user_data)
{
    (void)session;
    (void)flags;
    cloud_connection_t *connection = user_data;
    ssize_t written = esp_tls_conn_write(connection->tls, data, length);
    return written > 0 ? written : NGHTTP2_ERR_CALLBACK_FAILURE;
}

static nghttp2_ssize cloud_request_read_callback(nghttp2_session *session, int32_t stream_id,
                                                  uint8_t *buffer, size_t length,
                                                  uint32_t *data_flags,
                                                  nghttp2_data_source *source, void *user_data)
{
    (void)session;
    (void)stream_id;
    (void)data_flags;
    (void)user_data;
    cloud_connection_t *connection = source->ptr;
    cloud_outbound_t *item = connection->outbound_head;
    if (!item) return NGHTTP2_ERR_DEFERRED;
    size_t remaining = item->length - item->offset;
    size_t count = remaining < length ? remaining : length;
    memcpy(buffer, item->data + item->offset, count);
    item->offset += count;
    if (item->offset == item->length) {
        connection->outbound_head = item->next;
        if (!connection->outbound_head) connection->outbound_tail = NULL;
        free(item->data);
        free(item);
    }
    return (nghttp2_ssize)count;
}

static void cloud_handle_message(cloud_connection_t *connection, const uint8_t *message,
                                 size_t message_length)
{
    const uint8_t *uuid;
    const uint8_t *body;
    size_t uuid_length;
    size_t body_length;
    if (!cloud_read_bytes_field(message, message_length, 1, &uuid, &uuid_length) || !uuid_length) {
        ESP_LOGW(TAG, "Dropped cloud message without request UUID");
        return;
    }

    bool is_property = false;
    size_t body_field = 0;
    if (cloud_read_bytes_field(message, message_length, 2, &body, &body_length)) {
        body_field = 2;
    } else if (cloud_read_bytes_field(message, message_length, 8, &body, &body_length)) {
        body_field = 8;
        is_property = true;
    } else if (cloud_read_bytes_field(message, message_length, 7, &body, &body_length)) {
        size_t response_length;
        uint8_t *response = cloud_server_message(uuid, uuid_length, 7, NULL, 0, &response_length);
        if (response) { cloud_queue_message(connection, response, response_length); free(response); }
        return;
    } else {
        size_t response_length;
        uint8_t *response = cloud_error_message(uuid, uuid_length, false,
                                                "unsupported cloud request", &response_length);
        if (response) { cloud_queue_message(connection, response, response_length); free(response); }
        return;
    }

    const uint8_t *fqi_bytes;
    size_t fqi_length;
    const uint8_t *parameters = NULL;
    size_t parameters_length = 0;
    if (!cloud_read_bytes_field(body, body_length, 1, &fqi_bytes, &fqi_length) ||
        !fqi_length || fqi_length >= 192) {
        ESP_LOGW(TAG, "Dropped malformed cloud request");
        return;
    }
    char fqi[192];
    memcpy(fqi, fqi_bytes, fqi_length);
    fqi[fqi_length] = '\0';

    if (body_field == 2) {
        const uint8_t *parameter_wrapper;
        size_t parameter_wrapper_length;
        if (!cloud_read_bytes_field(body, body_length, 2, &parameter_wrapper,
                                    &parameter_wrapper_length) ||
            !cloud_read_bytes_field(parameter_wrapper, parameter_wrapper_length, 2,
                                    &parameters, &parameters_length)) {
            size_t response_length;
            uint8_t *response = cloud_error_message(uuid, uuid_length, false,
                                                    "missing command parameters", &response_length);
            if (response) { cloud_queue_message(connection, response, response_length); free(response); }
            return;
        }
    }

    uint8_t *payload = NULL;
    size_t payload_length = 0;
    int grpc_status = 0;
    bool handled = sila_server_cloud_unary_call(fqi, is_property, parameters, parameters_length,
                                                &payload, &payload_length, &grpc_status);
    if (!handled || grpc_status != 0) {
        size_t response_length;
        uint8_t *response = cloud_error_message(uuid, uuid_length, is_property,
                                                grpc_status == 3 ? "invalid request" :
                                                "unknown or failed SiLA call", &response_length);
        if (response) { cloud_queue_message(connection, response, response_length); free(response); }
        free(payload);
        return;
    }

    size_t value_length;
    uint8_t *value = cloud_bytes_field(1, payload, payload_length, &value_length);
    free(payload);
    if (!value) return;
    size_t response_length;
    uint8_t *response = cloud_server_message(uuid, uuid_length, is_property ? 8 : 2,
                                             value, value_length, &response_length);
    free(value);
    if (response) {
        ESP_LOGI(TAG, "Cloud %s %s", is_property ? "property" : "command", fqi);
        cloud_queue_message(connection, response, response_length);
        free(response);
    }
}

static int cloud_on_data_chunk(nghttp2_session *session, uint8_t flags, int32_t stream_id,
                               const uint8_t *data, size_t length, void *user_data)
{
    (void)session;
    (void)flags;
    (void)stream_id;
    cloud_connection_t *connection = user_data;
    if (length > sizeof(connection->inbound) - connection->inbound_length) {
        ESP_LOGW(TAG, "Cloud message exceeds %u byte limit", (unsigned)sizeof(connection->inbound));
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    memcpy(connection->inbound + connection->inbound_length, data, length);
    connection->inbound_length += length;
    while (connection->inbound_length >= 5) {
        size_t message_length = ((size_t)connection->inbound[1] << 24) |
                                ((size_t)connection->inbound[2] << 16) |
                                ((size_t)connection->inbound[3] << 8) |
                                connection->inbound[4];
        if (connection->inbound[0] != 0 || message_length > sizeof(connection->inbound) - 5) {
            ESP_LOGW(TAG, "Unsupported or oversized cloud gRPC message");
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        }
        if (connection->inbound_length < message_length + 5) break;
        cloud_handle_message(connection, connection->inbound + 5, message_length);
        size_t remaining = connection->inbound_length - message_length - 5;
        memmove(connection->inbound, connection->inbound + message_length + 5, remaining);
        connection->inbound_length = remaining;
    }
    return 0;
}

static int cloud_on_stream_close(nghttp2_session *session, int32_t stream_id,
                                 uint32_t error_code, void *user_data)
{
    (void)session;
    (void)stream_id;
    cloud_connection_t *connection = user_data;
    connection->stream_closed = true;
    ESP_LOGW(TAG, "Cloud stream closed: 0x%08" PRIx32, error_code);
    return 0;
}

static void cloud_sync_time(void)
{
    if (!sntp_started) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_err_t err = esp_netif_sntp_init(&config);
        if (err == ESP_OK) {
            sntp_started = true;
            ESP_LOGI(TAG, "SNTP started");
        } else {
            ESP_LOGW(TAG, "SNTP initialization failed: %s", esp_err_to_name(err));
        }
    }
    for (unsigned attempt = 0; sntp_started && attempt < 10; ++attempt) {
        esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(1000));
        if (err == ESP_OK) {
            time_t now;
            time(&now);
            ESP_LOGI(TAG, "SNTP synchronized: %lld", (long long)now);
            return;
        }
        if (err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "SNTP synchronization failed: %s", esp_err_to_name(err));
            break;
        }
    }
    ESP_LOGW(TAG, "Time was not synchronized; TLS certificate validation may fail");
}

static esp_tls_t *cloud_connect_tls(const epsilan_cloud_config_t *config)
{
    const char *alpn[] = { "h2", NULL };
    esp_tls_cfg_t tls_config = {
        .alpn_protos = alpn,
        .timeout_ms = 15000,
        // ESP-IDF 5.4's TLS 1.3 CertificateVerify path fails with the
        // gateway's RSA chain; TLS 1.2 remains fully authenticated TLS.
        .tls_version = ESP_TLS_VER_TLS_1_2,
    };
    if (config->tls) {
        tls_config.cacert_buf = (const unsigned char *)unitelabs_yr2_pem;
        tls_config.cacert_bytes = sizeof(unitelabs_yr2_pem);
    } else {
        tls_config.is_plain_tcp = true;
    }

    cloud_log_memory("before TLS init");
    esp_tls_t *tls = esp_tls_init();
    if (!tls) return NULL;
    cloud_log_memory("before TLS handshake");
    if (esp_tls_conn_new_sync(config->endpoint, strlen(config->endpoint), config->port,
                              &tls_config, tls) != 1) {
        esp_tls_error_handle_t error_handle;
        int tls_code = 0;
        int cert_flags = 0;
        if (esp_tls_get_error_handle(tls, &error_handle) == ESP_OK) {
            esp_tls_get_and_clear_last_error(error_handle, &tls_code, &cert_flags);
        }
        char error_text[128] = { 0 };
        if (tls_code) mbedtls_strerror(-tls_code, error_text, sizeof(error_text));
        cloud_log_memory("after failed TLS handshake");
        ESP_LOGW(TAG, "TLS failure: code=%d (%s) certificate_flags=0x%x", tls_code,
                 error_text, cert_flags);
        esp_tls_conn_destroy(tls);
        return NULL;
    }
    cloud_log_memory("after TLS handshake");

    int socket_fd;
    if (esp_tls_get_conn_sockfd(tls, &socket_fd) == ESP_OK) {
        struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }
    return tls;
}

static bool cloud_serve(esp_tls_t *tls, const epsilan_cloud_config_t *config)
{
    cloud_connection_t connection = { .tls = tls };
    nghttp2_session_callbacks *callbacks = NULL;
    if (nghttp2_session_callbacks_new(&callbacks) != 0) return false;
    nghttp2_session_callbacks_set_send_callback2(callbacks, cloud_send_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, cloud_on_data_chunk);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, cloud_on_stream_close);
    int result = nghttp2_session_client_new(&connection.session, callbacks, &connection);
    nghttp2_session_callbacks_del(callbacks);
    if (result != 0) return false;

    bool connected = false;
    char authority[EPSILAN_CLOUD_ENDPOINT_MAX + 8];
    snprintf(authority, sizeof(authority), "%s:%u", config->endpoint, config->port);
    nghttp2_nv headers[] = {
        NV(":method", "POST"),
        NV(":scheme", config->tls ? "https" : "http"),
        NV(":path", CLOUD_CONNECT_PATH),
        NV(":authority", authority),
        NV("content-type", "application/grpc"),
        NV("te", "trailers"),
        NV("grpc-encoding", "identity"),
        NV("grpc-accept-encoding", "identity"),
    };
    nghttp2_data_provider2 provider = {
        .read_callback = cloud_request_read_callback,
        .source.ptr = &connection,
    };
    connection.stream_id = nghttp2_submit_request2(connection.session, NULL, headers, ARRAY_SIZE(headers),
                                                   &provider, NULL);
    if (nghttp2_submit_settings(connection.session, NGHTTP2_FLAG_NONE, NULL, 0) != 0 ||
        connection.stream_id < 0 || nghttp2_session_send(connection.session) != 0) {
        goto done;
    }
    connected = true;
    ESP_LOGI(TAG, "Connected to cloud gateway %s:%u (%s)", config->endpoint, config->port,
             config->tls ? "TLS" : "plaintext");

    uint8_t input[2048];
    while (!connection.stream_closed) {
        ssize_t received = esp_tls_conn_read(tls, input, sizeof(input));
        if (received > 0) {
            size_t offset = 0;
            while (offset < (size_t)received) {
                nghttp2_ssize used = nghttp2_session_mem_recv2(connection.session, input + offset,
                                                               (size_t)received - offset);
                if (used < 0) goto done;
                offset += (size_t)used;
            }
        } else if (received == 0) {
            goto done;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            goto done;
        }
        if (nghttp2_session_send(connection.session) != 0) goto done;
    }

done:
    cloud_free_outbound(&connection);
    nghttp2_session_del(connection.session);
    return connected;
}

static void cloud_client_task(void *argument)
{
    (void)argument;
    cloud_sync_time();
    unsigned retry_seconds = 1;
    for (;;) {
        epsilan_cloud_config_t config;
        if (epsilan_cloud_config_load(&config) != ESP_OK || !config.enabled) break;
        ESP_LOGI(TAG, "Connecting to cloud gateway %s:%u", config.endpoint, config.port);
        esp_tls_t *tls = cloud_connect_tls(&config);
        if (tls) {
            cloud_serve(tls, &config);
            esp_tls_conn_destroy(tls);
        } else {
            ESP_LOGW(TAG, "Cloud TLS connection failed");
        }
        ESP_LOGI(TAG, "Cloud disconnected; retrying in %u seconds", retry_seconds);
        vTaskDelay(pdMS_TO_TICKS(retry_seconds * 1000));
        retry_seconds = retry_seconds < 60 ? retry_seconds * 2 : 60;
    }
    task_started = false;
    vTaskDelete(NULL);
}

esp_err_t epsilan_cloud_client_start(void)
{
    if (task_started) return ESP_OK;
    epsilan_cloud_config_t config;
    ESP_RETURN_ON_ERROR(epsilan_cloud_config_load(&config), TAG, "load configuration");
    if (!config.enabled) return ESP_OK;
    if (xTaskCreate(cloud_client_task, "sila_cloud", 16384, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    task_started = true;
    return ESP_OK;
}
