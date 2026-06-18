#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

esp_err_t sensor_mqtt_start(void);
esp_err_t sensor_mqtt_publish_text(const char *topic, const char *payload);
esp_err_t sensor_mqtt_wait_output_message(const char *command_id,
                                          char *output_json,
                                          size_t output_json_size,
                                          uint32_t timeout_ms);
esp_err_t sensor_mqtt_wait_policy_decision(const char *command_id,
                                           char *decision_json,
                                           size_t decision_json_size,
                                           uint32_t timeout_ms);
esp_err_t sensor_mqtt_publish_node_event(const char *event_type, const char *detail);
esp_err_t sensor_mqtt_stateboard_json(char *buf, size_t buf_size);
esp_err_t sensor_mqtt_publish_output_message(const char *event,
                                             const char *command_id,
                                             const char *trace_id,
                                             const char *action,
                                             const char *recipient,
                                             esp_err_t result_err,
                                             const char *summary,
                                             const char *result_text);
esp_err_t sensor_mqtt_publish_timeline_event(const char *phase,
                                             const char *event_type,
                                             const char *status,
                                             const char *summary,
                                             const char *command_id,
                                             const char *target_role,
                                             const char *target_node,
                                             const char *action);
