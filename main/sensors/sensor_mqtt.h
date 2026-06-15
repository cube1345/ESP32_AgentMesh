#pragma once

#include "esp_err.h"

esp_err_t sensor_mqtt_start(void);
esp_err_t sensor_mqtt_publish_text(const char *topic, const char *payload);
esp_err_t sensor_mqtt_publish_node_event(const char *event_type, const char *detail);
