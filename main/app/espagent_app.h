#pragma once

#include "esp_err.h"

esp_err_t espagent_app_init_subsystems(void);
esp_err_t espagent_app_start_local_services(void);
esp_err_t espagent_app_connect_wifi_or_onboard(void);
esp_err_t espagent_app_start_network_services(void);
