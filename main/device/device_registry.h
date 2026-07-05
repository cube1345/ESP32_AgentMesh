#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define ESPAGENT_DEVICE_ID_MAX            40
#define ESPAGENT_DEVICE_PROTOCOL_MAX      24
#define ESPAGENT_DEVICE_ROLE_MAX          32
#define ESPAGENT_DEVICE_ADDR_MAX          40
#define ESPAGENT_DEVICE_NAME_MAX          48
#define ESPAGENT_DEVICE_CAPABILITIES_MAX  96
#define ESPAGENT_DEVICE_STATE_MAX         32

typedef struct {
    char id[ESPAGENT_DEVICE_ID_MAX];
    char name[ESPAGENT_DEVICE_NAME_MAX];
    char protocol[ESPAGENT_DEVICE_PROTOCOL_MAX];
    char role[ESPAGENT_DEVICE_ROLE_MAX];
    char address[ESPAGENT_DEVICE_ADDR_MAX];
    char capabilities[ESPAGENT_DEVICE_CAPABILITIES_MAX];
    char state[ESPAGENT_DEVICE_STATE_MAX];
    int64_t last_seen_ms;
    bool external;
} espagent_device_record_t;

esp_err_t espagent_device_registry_init(void);

esp_err_t espagent_device_registry_upsert(const espagent_device_record_t *record);

esp_err_t espagent_device_registry_note_mqtt_payload(const char *kind,
                                                     const char *payload,
                                                     size_t payload_len);

esp_err_t espagent_device_registry_to_json(char *buf, size_t buf_size);
