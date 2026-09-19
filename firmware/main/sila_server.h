#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EPSILAN_SILA_PORT 50052

esp_err_t sila_server_start(const char *server_uuid);
void sila_server_set_acceleration(float x, float y, float z);

/** Returns the current Acceleration property protobuf payload; caller frees it. */
uint8_t *sila_server_acceleration_value(size_t *out_length);

/**
 * Dispatches one unobservable SiLA Cloud call through the same implementation
 * used by the LAN gRPC server.  The returned payload is the unframed protobuf
 * response and must be freed by the caller.
 */
bool sila_server_cloud_unary_call(const char *fqi, bool is_property,
                                  const uint8_t *request, size_t request_length,
                                  uint8_t **response, size_t *response_length,
                                  int *grpc_status);
