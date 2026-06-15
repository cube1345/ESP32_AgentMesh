#pragma once

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t hits;
    uint32_t misses;
    uint32_t evictions;
    uint32_t expired;
    uint32_t truncated;
    uint32_t entries;
    uint32_t bytes;
} cache_stats_t;

esp_err_t cache_store_init(void);
esp_err_t cache_get(const char *key, char *out, size_t out_size);
esp_err_t cache_put(const char *key, const char *value, uint32_t ttl_s);
esp_err_t cache_delete(const char *key);
esp_err_t cache_delete_prefix(const char *prefix);
void cache_stats(cache_stats_t *stats);
size_t cache_dump(char *out, size_t out_size);
void cache_clear(void);
