#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t dynamic_extension_init(void);

esp_err_t dynamic_extension_build_catalog(char *buf, size_t size);
