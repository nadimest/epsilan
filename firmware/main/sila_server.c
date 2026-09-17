#include "sila_server.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "nghttp2/nghttp2.h"

#define MAX_REQUEST_BYTES 512
#define ACCELERATION_INTERVAL_US 500000
#define SERVER_NAME_MAX 96
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define NV(NAME, VALUE) {                                                    \
    (uint8_t *)(NAME), (uint8_t *)(VALUE), sizeof(NAME) - 1, sizeof(VALUE) - 1, \
    NGHTTP2_NV_FLAG_NONE                                                    \
}

static const char *TAG = "epsilan_sila";
static const char *CORE_FQI = "org.silastandard/core/SiLAService/v1";
static const char *ACCEL_FQI = "io.epsilan/sensors/Accelerometer/v1";
static const char *CORE_PATH = "/sila2.org.silastandard.core.silaservice.v1.SiLAService/";

extern const uint8_t sila_service_xml_start[] asm("_binary_SiLAService_sila_xml_start");
extern const uint8_t sila_service_xml_end[] asm("_binary_SiLAService_sila_xml_end");
extern const uint8_t accelerometer_xml_start[] asm("_binary_Accelerometer_sila_xml_start");
extern const uint8_t accelerometer_xml_end[] asm("_binary_Accelerometer_sila_xml_end");

typedef struct stream_state {
    struct stream_state *next;
    int32_t id;
    char path[192];
    uint8_t request[MAX_REQUEST_BYTES];
    size_t request_len;
    uint8_t *response;
    size_t response_len;
    size_t response_offset;
    bool observable;
    bool trailer_submitted;
    int grpc_status;
    int64_t next_publish_us;
} stream_state_t;

typedef struct {
    int socket_fd;
    nghttp2_session *session;
    stream_state_t *streams;
} connection_t;

static bool task_started;
static char device_uuid[37];
static char server_name[SERVER_NAME_MAX] = "Epsilan CoreS3";
static portMUX_TYPE acceleration_lock = portMUX_INITIALIZER_UNLOCKED;
static float acceleration_x;
static float acceleration_y;
static float acceleration_z;

static size_t varint_size(size_t value)
{
    size_t size = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++size;
    }
    return size;
}

static uint8_t *write_varint(uint8_t *out, size_t value)
{
    while (value >= 0x80) {
        *out++ = (uint8_t)(value | 0x80);
        value >>= 7;
    }
    *out++ = (uint8_t)value;
    return out;
}

static bool read_varint(const uint8_t **cursor, const uint8_t *end, size_t *value)
{
    size_t result = 0;
    unsigned shift = 0;
    while (*cursor < end && shift < sizeof(size_t) * 8) {
        uint8_t byte = *(*cursor)++;
        result |= (size_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) {
            *value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

static bool read_field_one_bytes(const uint8_t *data, size_t length,
                                 const uint8_t **value, size_t *value_length)
{
    const uint8_t *cursor = data;
    const uint8_t *end = data + length;
    size_t tag;
    size_t size;
    if (!read_varint(&cursor, end, &tag) || tag != 0x0a ||
        !read_varint(&cursor, end, &size) || size > (size_t)(end - cursor)) {
        return false;
    }
    *value = cursor;
    *value_length = size;
    return true;
}

static uint8_t *wrap_string(const uint8_t *text, size_t text_length, size_t *out_length)
{
    size_t inner_length = 1 + varint_size(text_length) + text_length;
    size_t total = 1 + varint_size(inner_length) + inner_length;
    uint8_t *message = malloc(total);
    if (!message) return NULL;
    uint8_t *out = message;
    *out++ = 0x0a;
    out = write_varint(out, inner_length);
    *out++ = 0x0a;
    out = write_varint(out, text_length);
    memcpy(out, text, text_length);
    *out_length = total;
    return message;
}

static uint8_t *wrap_cstring(const char *text, size_t *out_length)
{
    return wrap_string((const uint8_t *)text, strlen(text), out_length);
}

static uint8_t *implemented_features(size_t *out_length)
{
    const char *features[] = { CORE_FQI, ACCEL_FQI };
    size_t total = 0;
    for (size_t i = 0; i < ARRAY_SIZE(features); ++i) {
        size_t text_length = strlen(features[i]);
        size_t wrapped_length = 1 + varint_size(text_length) + text_length;
        total += 1 + varint_size(wrapped_length) + wrapped_length;
    }
    uint8_t *message = malloc(total);
    if (!message) return NULL;
    uint8_t *out = message;
    for (size_t i = 0; i < ARRAY_SIZE(features); ++i) {
        size_t text_length = strlen(features[i]);
        size_t wrapped_length = 1 + varint_size(text_length) + text_length;
        *out++ = 0x0a;
        out = write_varint(out, wrapped_length);
        *out++ = 0x0a;
        out = write_varint(out, text_length);
        memcpy(out, features[i], text_length);
        out += text_length;
    }
    *out_length = total;
    return message;
}

static uint8_t *acceleration_response(size_t *out_length)
{
    float x;
    float y;
    float z;
    taskENTER_CRITICAL(&acceleration_lock);
    x = acceleration_x;
    y = acceleration_y;
    z = acceleration_z;
    taskEXIT_CRITICAL(&acceleration_lock);

    const double values[] = { x, y, z };
    const size_t structure_length = 3 * 11;
    const size_t message_length = 2 + structure_length;
    uint8_t *message = malloc(message_length);
    if (!message) return NULL;
    uint8_t *out = message;
    *out++ = 0x0a;
    *out++ = (uint8_t)structure_length;
    for (size_t i = 0; i < ARRAY_SIZE(values); ++i) {
        uint64_t bits;
        memcpy(&bits, &values[i], sizeof(bits));
        *out++ = (uint8_t)(((i + 1) << 3) | 2);
        *out++ = 9;
        *out++ = 0x09;
        for (unsigned byte = 0; byte < 8; ++byte) {
            *out++ = (uint8_t)(bits >> (byte * 8));
        }
    }
    *out_length = message_length;
    return message;
}

static bool grpc_request_message(const stream_state_t *stream,
                                 const uint8_t **message, size_t *message_length)
{
    if (stream->request_len < 5 || stream->request[0] != 0) return false;
    size_t length = ((size_t)stream->request[1] << 24) |
                    ((size_t)stream->request[2] << 16) |
                    ((size_t)stream->request[3] << 8) |
                    stream->request[4];
    if (length != stream->request_len - 5) return false;
    *message = stream->request + 5;
    *message_length = length;
    return true;
}

static bool set_grpc_response(stream_state_t *stream, uint8_t *protobuf, size_t protobuf_length)
{
    uint8_t *framed = malloc(protobuf_length + 5);
    if (!framed) {
        free(protobuf);
        return false;
    }
    framed[0] = 0;
    framed[1] = (uint8_t)(protobuf_length >> 24);
    framed[2] = (uint8_t)(protobuf_length >> 16);
    framed[3] = (uint8_t)(protobuf_length >> 8);
    framed[4] = (uint8_t)protobuf_length;
    if (protobuf_length) memcpy(framed + 5, protobuf, protobuf_length);
    free(protobuf);
    free(stream->response);
    stream->response = framed;
    stream->response_len = protobuf_length + 5;
    stream->response_offset = 0;
    return true;
}

static uint8_t *copy_xml(const uint8_t *start, const uint8_t *end, size_t *length)
{
    *length = (size_t)(end - start);
    if (*length && start[*length - 1] == 0) --*length;
    uint8_t *copy = malloc(*length);
    if (copy) memcpy(copy, start, *length);
    return copy;
}

static bool prepare_unary_response(stream_state_t *stream)
{
    const char *method = stream->path;
    uint8_t *protobuf = NULL;
    size_t protobuf_length = 0;

    if (!strncmp(method, CORE_PATH, strlen(CORE_PATH))) {
        method += strlen(CORE_PATH);
        if (!strcmp(method, "Get_ServerUUID")) {
            protobuf = wrap_cstring(device_uuid, &protobuf_length);
        } else if (!strcmp(method, "Get_ServerName")) {
            protobuf = wrap_cstring(server_name, &protobuf_length);
        } else if (!strcmp(method, "Get_ServerType")) {
            protobuf = wrap_cstring("EpsilanCoreS3", &protobuf_length);
        } else if (!strcmp(method, "Get_ServerDescription")) {
            protobuf = wrap_cstring("M5Stack CoreS3 SiLA sensor and serial-device connector", &protobuf_length);
        } else if (!strcmp(method, "Get_ServerVersion")) {
            protobuf = wrap_cstring("0.2.0", &protobuf_length);
        } else if (!strcmp(method, "Get_ServerVendorURL")) {
            protobuf = wrap_cstring("https://github.com/nadim", &protobuf_length);
        } else if (!strcmp(method, "Get_ImplementedFeatures")) {
            protobuf = implemented_features(&protobuf_length);
        } else if (!strcmp(method, "SetServerName")) {
            const uint8_t *request;
            const uint8_t *wrapped;
            size_t request_length;
            size_t wrapped_length;
            const uint8_t *name;
            size_t name_length;
            if (grpc_request_message(stream, &request, &request_length) &&
                read_field_one_bytes(request, request_length, &wrapped, &wrapped_length) &&
                read_field_one_bytes(wrapped, wrapped_length, &name, &name_length) &&
                name_length > 0 && name_length < sizeof(server_name)) {
                memcpy(server_name, name, name_length);
                server_name[name_length] = 0;
                mdns_service_txt_item_set("_sila", "_tcp", "server_name", server_name);
                protobuf = calloc(1, 1);
                protobuf_length = 0;
            }
        } else if (!strcmp(method, "GetFeatureDefinition")) {
            const uint8_t *request;
            const uint8_t *wrapped;
            size_t request_length;
            size_t wrapped_length;
            const uint8_t *identifier;
            size_t identifier_length;
            const uint8_t *xml_start = NULL;
            const uint8_t *xml_end = NULL;
            if (grpc_request_message(stream, &request, &request_length) &&
                read_field_one_bytes(request, request_length, &wrapped, &wrapped_length) &&
                read_field_one_bytes(wrapped, wrapped_length, &identifier, &identifier_length)) {
                if (identifier_length == strlen(CORE_FQI) && !memcmp(identifier, CORE_FQI, identifier_length)) {
                    xml_start = sila_service_xml_start;
                    xml_end = sila_service_xml_end;
                } else if (identifier_length == strlen(ACCEL_FQI) && !memcmp(identifier, ACCEL_FQI, identifier_length)) {
                    xml_start = accelerometer_xml_start;
                    xml_end = accelerometer_xml_end;
                }
            }
            if (xml_start) {
                size_t xml_length;
                uint8_t *xml = copy_xml(xml_start, xml_end, &xml_length);
                if (xml) {
                    protobuf = wrap_string(xml, xml_length, &protobuf_length);
                    free(xml);
                }
            }
        }
    } else if (!strcmp(method, "/sila2.io.epsilan.sensors.accelerometer.v1.Accelerometer/Subscribe_Acceleration")) {
        stream->observable = true;
        stream->next_publish_us = 0;
        protobuf = acceleration_response(&protobuf_length);
    }

    if (!protobuf && protobuf_length == 0) {
        stream->grpc_status = 12;
        protobuf = calloc(1, 1);
    }
    return protobuf && set_grpc_response(stream, protobuf, protobuf_length);
}

static stream_state_t *find_stream(connection_t *connection, int32_t id)
{
    for (stream_state_t *stream = connection->streams; stream; stream = stream->next) {
        if (stream->id == id) return stream;
    }
    return NULL;
}

static nghttp2_ssize send_callback(nghttp2_session *session, const uint8_t *data,
                                   size_t length, int flags, void *user_data)
{
    (void)session;
    (void)flags;
    connection_t *connection = user_data;
    ssize_t sent = send(connection->socket_fd, data, length, 0);
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return NGHTTP2_ERR_WOULDBLOCK;
    }
    return sent <= 0 ? NGHTTP2_ERR_CALLBACK_FAILURE : sent;
}

static nghttp2_ssize response_read_callback(nghttp2_session *session, int32_t stream_id,
                                            uint8_t *buffer, size_t length,
                                            uint32_t *data_flags,
                                            nghttp2_data_source *source, void *user_data)
{
    (void)stream_id;
    (void)user_data;
    stream_state_t *stream = source->ptr;

    if (stream->observable && stream->response_offset == stream->response_len) {
        if (esp_timer_get_time() < stream->next_publish_us) return NGHTTP2_ERR_DEFERRED;
        size_t protobuf_length;
        uint8_t *protobuf = acceleration_response(&protobuf_length);
        if (!protobuf || !set_grpc_response(stream, protobuf, protobuf_length)) {
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        }
    }

    size_t remaining = stream->response_len - stream->response_offset;
    size_t count = remaining < length ? remaining : length;
    if (count) {
        memcpy(buffer, stream->response + stream->response_offset, count);
        stream->response_offset += count;
    }

    if (stream->response_offset == stream->response_len) {
        if (stream->observable) {
            stream->next_publish_us = esp_timer_get_time() + ACCELERATION_INTERVAL_US;
        } else {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF | NGHTTP2_DATA_FLAG_NO_END_STREAM;
            if (!stream->trailer_submitted) {
                char status_value[4];
                snprintf(status_value, sizeof(status_value), "%d", stream->grpc_status);
                nghttp2_nv trailers[] = {
                    { (uint8_t *)"grpc-status", (uint8_t *)status_value, 11, strlen(status_value), NGHTTP2_NV_FLAG_NONE },
                };
                if (nghttp2_submit_trailer(session, stream->id, trailers, ARRAY_SIZE(trailers)) != 0) {
                    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
                }
                stream->trailer_submitted = true;
            }
        }
    }
    return (nghttp2_ssize)count;
}

static int submit_response(nghttp2_session *session, stream_state_t *stream)
{
    nghttp2_nv headers[] = {
        NV(":status", "200"),
        NV("content-type", "application/grpc"),
        NV("grpc-encoding", "identity"),
        NV("grpc-accept-encoding", "identity"),
    };
    nghttp2_data_provider2 provider = {
        .source.ptr = stream,
        .read_callback = response_read_callback,
    };
    return nghttp2_submit_response2(session, stream->id, headers, ARRAY_SIZE(headers), &provider);
}

static int on_begin_headers(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    connection_t *connection = user_data;
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) return 0;
    stream_state_t *stream = calloc(1, sizeof(*stream));
    if (!stream) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    stream->id = frame->hd.stream_id;
    stream->next = connection->streams;
    connection->streams = stream;
    nghttp2_session_set_stream_user_data(session, stream->id, stream);
    return 0;
}

static int on_header(nghttp2_session *session, const nghttp2_frame *frame,
                     const uint8_t *name, size_t name_length,
                     const uint8_t *value, size_t value_length,
                     uint8_t flags, void *user_data)
{
    (void)flags;
    (void)user_data;
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) return 0;
    stream_state_t *stream = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    if (stream && name_length == 5 && !memcmp(name, ":path", 5)) {
        size_t copy = value_length < sizeof(stream->path) - 1 ? value_length : sizeof(stream->path) - 1;
        memcpy(stream->path, value, copy);
        stream->path[copy] = 0;
    }
    return 0;
}

static int on_data_chunk(nghttp2_session *session, uint8_t flags, int32_t stream_id,
                         const uint8_t *data, size_t length, void *user_data)
{
    (void)session;
    (void)flags;
    connection_t *connection = user_data;
    stream_state_t *stream = find_stream(connection, stream_id);
    if (!stream || length > sizeof(stream->request) - stream->request_len) {
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    memcpy(stream->request + stream->request_len, data, length);
    stream->request_len += length;
    return 0;
}

static int on_frame_recv(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    (void)user_data;
    if ((frame->hd.type == NGHTTP2_DATA || frame->hd.type == NGHTTP2_HEADERS) &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        stream_state_t *stream = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
        if (!stream) return 0;
        ESP_LOGI(TAG, "RPC %s", stream->path);
        if (!prepare_unary_response(stream) || submit_response(session, stream) != 0) {
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        }
    }
    return 0;
}

static int on_stream_close(nghttp2_session *session, int32_t stream_id,
                           uint32_t error_code, void *user_data)
{
    (void)session;
    (void)error_code;
    connection_t *connection = user_data;
    stream_state_t **slot = &connection->streams;
    while (*slot) {
        if ((*slot)->id == stream_id) {
            stream_state_t *stream = *slot;
            *slot = stream->next;
            free(stream->response);
            free(stream);
            break;
        }
        slot = &(*slot)->next;
    }
    return 0;
}

static void resume_observables(connection_t *connection)
{
    int64_t now = esp_timer_get_time();
    for (stream_state_t *stream = connection->streams; stream; stream = stream->next) {
        if (stream->observable && stream->response_offset == stream->response_len &&
            now >= stream->next_publish_us) {
            nghttp2_session_resume_data(connection->session, stream->id);
        }
    }
}

static void free_streams(connection_t *connection)
{
    while (connection->streams) {
        stream_state_t *stream = connection->streams;
        connection->streams = stream->next;
        free(stream->response);
        free(stream);
    }
}

static void serve_connection(int socket_fd)
{
    connection_t connection = { .socket_fd = socket_fd };
    nghttp2_session_callbacks *callbacks = NULL;
    if (nghttp2_session_callbacks_new(&callbacks) != 0) return;
    nghttp2_session_callbacks_set_send_callback2(callbacks, send_callback);
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close);
    int result = nghttp2_session_server_new(&connection.session, callbacks, &connection);
    nghttp2_session_callbacks_del(callbacks);
    if (result != 0) return;

    nghttp2_settings_entry settings[] = {
        { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 8 },
        { NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, 4096 },
    };
    nghttp2_submit_settings(connection.session, NGHTTP2_FLAG_NONE, settings, ARRAY_SIZE(settings));
    if (nghttp2_session_send(connection.session) != 0) goto done;

    struct timeval timeout = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    uint8_t input[2048];
    for (;;) {
        ssize_t received = recv(socket_fd, input, sizeof(input), 0);
        if (received > 0) {
            size_t offset = 0;
            while (offset < (size_t)received) {
                nghttp2_ssize used = nghttp2_session_mem_recv2(connection.session, input + offset,
                                                               (size_t)received - offset);
                if (used < 0) {
                    ESP_LOGW(TAG, "HTTP/2 receive failed: %s", nghttp2_strerror((int)used));
                    goto done;
                }
                offset += (size_t)used;
            }
        } else if (received == 0) {
            ESP_LOGI(TAG, "Client closed socket");
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            ESP_LOGW(TAG, "Socket receive failed: %d", errno);
            break;
        }
        resume_observables(&connection);
        int send_result = nghttp2_session_send(connection.session);
        if (send_result != 0) {
            ESP_LOGW(TAG, "HTTP/2 send failed: %s", nghttp2_strerror(send_result));
            break;
        }
    }

done:
    free_streams(&connection);
    nghttp2_session_del(connection.session);
}

static void sila_server_task(void *argument)
{
    (void)argument;
    // GroundControl currently correlates the A record by its first label and
    // only accepts A names ending in the SiLA service suffix. Keep the UUID as
    // that first label while retaining a resolvable mDNS target.
    char hostname[64];
    snprintf(hostname, sizeof(hostname), "%s._sila._tcp", device_uuid);
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(hostname));
    ESP_ERROR_CHECK(mdns_instance_name_set(server_name));
    mdns_txt_item_t txt[] = {
        { "version", "1.0" },
        { "server_name", server_name },
        { "description", "M5Stack CoreS3 SiLA connector" },
    };
    ESP_ERROR_CHECK(mdns_service_add(device_uuid, "_sila", "_tcp", EPSILAN_SILA_PORT,
                                     txt, ARRAY_SIZE(txt)));

    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) {
        ESP_LOGE(TAG, "socket failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(EPSILAN_SILA_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(listener, 2) != 0) {
        ESP_LOGE(TAG, "listen failed: %d", errno);
        close(listener);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "SiLA 2 plaintext gRPC listening on %s.local:%d", hostname, EPSILAN_SILA_PORT);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_length = sizeof(peer);
        int client = accept(listener, (struct sockaddr *)&peer, &peer_length);
        if (client < 0) continue;
        ESP_LOGI(TAG, "Client connected from %s", inet_ntoa(peer.sin_addr));
        serve_connection(client);
        close(client);
        ESP_LOGI(TAG, "Client disconnected");
    }
}

esp_err_t sila_server_start(const char *server_uuid)
{
    if (task_started) return ESP_OK;
    if (!server_uuid || strlen(server_uuid) != 36) return ESP_ERR_INVALID_ARG;
    strlcpy(device_uuid, server_uuid, sizeof(device_uuid));
    if (xTaskCreate(sila_server_task, "sila_server", 12288, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    task_started = true;
    return ESP_OK;
}

void sila_server_set_acceleration(float x, float y, float z)
{
    taskENTER_CRITICAL(&acceleration_lock);
    acceleration_x = x;
    acceleration_y = y;
    acceleration_z = z;
    taskEXIT_CRITICAL(&acceleration_lock);
}
