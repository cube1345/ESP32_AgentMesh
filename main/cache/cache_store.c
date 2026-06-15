#include "cache/cache_store.h"

#include "espagent_config.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "cache";

// 一个简单的内存键值缓存，支持过期和LRU淘汰。所有数据存储在RAM中，适合频繁访问的小数据，如LLM上下文片段、工具调用结果等。设计目标是快速读写和简单的过期机制，不适合大规模或持久化存储。
typedef struct {
    bool used;
    char key[ESPAGENT_CACHE_MAX_KEY_BYTES];
    char *value;
    size_t value_len;
    int64_t expires_at_us;
    int64_t last_access_us;
    uint32_t hits;
} cache_entry_t;

// 全局静态数组
static cache_entry_t s_entries[ESPAGENT_CACHE_MAX_ENTRIES];
static SemaphoreHandle_t s_lock;
static uint32_t s_hits;
static uint32_t s_misses;
static uint32_t s_evictions;
static uint32_t s_expired;
static uint32_t s_truncated;
static size_t s_total_bytes;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static bool is_expired(const cache_entry_t *entry, int64_t now)
{
    return entry->used && entry->expires_at_us > 0 && now >= entry->expires_at_us;
}

static void free_entry(cache_entry_t *entry)
{
    if (!entry || !entry->used) {
        return;
    }

    s_total_bytes -= entry->value_len;
    free(entry->value);
    memset(entry, 0, sizeof(*entry));
}

static char *alloc_value(size_t len)
{
    char *ptr = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = malloc(len);
    }
    return ptr;
}

static int find_entry(const char *key)
{
    for (int i = 0; i < ESPAGENT_CACHE_MAX_ENTRIES; ++i) {
        if (s_entries[i].used && strcmp(s_entries[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_free_entry(void)
{
    for (int i = 0; i < ESPAGENT_CACHE_MAX_ENTRIES; ++i) {
        if (!s_entries[i].used) {
            return i;
        }
    }
    return -1;
}

static int find_victim(int64_t now)
{
    int victim = -1;
    int64_t oldest_access = INT64_MAX;

    for (int i = 0; i < ESPAGENT_CACHE_MAX_ENTRIES; ++i) {
        if (!s_entries[i].used) {
            return i;
        }
        if (is_expired(&s_entries[i], now)) {
            s_expired++;
            return i;
        }
        if (s_entries[i].last_access_us < oldest_access) {
            oldest_access = s_entries[i].last_access_us;
            victim = i;
        }
    }

    return victim;
}

static int find_used_victim(int64_t now)
{
    int victim = -1;
    int64_t oldest_access = INT64_MAX;

    for (int i = 0; i < ESPAGENT_CACHE_MAX_ENTRIES; ++i) {
        if (!s_entries[i].used) {
            continue;
        }
        if (is_expired(&s_entries[i], now)) {
            s_expired++;
            return i;
        }
        if (s_entries[i].last_access_us < oldest_access) {
            oldest_access = s_entries[i].last_access_us;
            victim = i;
        }
    }

    return victim;
}

static void evict_until_fits(size_t incoming_len, int protected_idx, int64_t now)
{
    while (s_total_bytes + incoming_len > ESPAGENT_CACHE_MAX_TOTAL_BYTES) {
        int victim = find_used_victim(now);
        if (victim < 0 || victim == protected_idx) {
            break;
        }
        free_entry(&s_entries[victim]);
        s_evictions++;
    }
}

esp_err_t cache_store_init(void)
{
    if (!s_lock) {
        // 使用FreeRTOS的互斥锁保护访问，确保线程安全
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "RAM KV cache ready: entries=%d total=%d value=%d",
             ESPAGENT_CACHE_MAX_ENTRIES,
             ESPAGENT_CACHE_MAX_TOTAL_BYTES,
             ESPAGENT_CACHE_MAX_VALUE_BYTES);
    return ESP_OK;
}

esp_err_t cache_get(const char *key, char *out, size_t out_size)
{
    if (!key || !key[0] || !out || out_size == 0 || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }

    out[0] = '\0';

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_entry(key);
    int64_t now = now_us();
    if (idx < 0) {
        s_misses++;
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    cache_entry_t *entry = &s_entries[idx];
    if (is_expired(entry, now)) {
        free_entry(entry);
        s_misses++;
        s_expired++;
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    size_t copy = entry->value_len < out_size - 1 ? entry->value_len : out_size - 1;
    memcpy(out, entry->value, copy);
    out[copy] = '\0';
    entry->last_access_us = now;
    entry->hits++;
    s_hits++;
    if (copy != entry->value_len) {
        s_truncated++;
    }

    xSemaphoreGive(s_lock);
    return (copy == entry->value_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t cache_put(const char *key, const char *value, uint32_t ttl_s)
{
    if (!key || !key[0] || !value || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t key_len = strlen(key);
    size_t value_len = strlen(value);
    if (key_len >= ESPAGENT_CACHE_MAX_KEY_BYTES || value_len > ESPAGENT_CACHE_MAX_VALUE_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *copy = alloc_value(value_len + 1);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, value, value_len + 1);

    int64_t now = now_us();
    int64_t expires_at = now + ((int64_t)(ttl_s ? ttl_s : ESPAGENT_CACHE_DEFAULT_TTL_S) * 1000000LL);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_entry(key);
    if (idx >= 0) {
        free_entry(&s_entries[idx]);
    }

    evict_until_fits(value_len, idx, now);
    if (idx < 0) {
        idx = find_free_entry();
        if (idx < 0) {
            idx = find_victim(now);
            if (idx >= 0) {
                free_entry(&s_entries[idx]);
                s_evictions++;
            }
        }
    }

    if (idx < 0 || s_total_bytes + value_len > ESPAGENT_CACHE_MAX_TOTAL_BYTES) {
        xSemaphoreGive(s_lock);
        free(copy);
        return ESP_ERR_NO_MEM;
    }

    cache_entry_t *entry = &s_entries[idx];
    memset(entry, 0, sizeof(*entry));
    entry->used = true;
    memcpy(entry->key, key, key_len + 1);
    entry->value = copy;
    entry->value_len = value_len;
    entry->expires_at_us = expires_at;
    entry->last_access_us = now;
    s_total_bytes += value_len;

    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t cache_delete(const char *key)
{
    if (!key || !key[0] || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_entry(key);
    if (idx < 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    free_entry(&s_entries[idx]);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t cache_delete_prefix(const char *prefix)
{
    if (!prefix || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t prefix_len = strlen(prefix);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < ESPAGENT_CACHE_MAX_ENTRIES; ++i) {
        if (s_entries[i].used && strncmp(s_entries[i].key, prefix, prefix_len) == 0) {
            free_entry(&s_entries[i]);
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void cache_stats(cache_stats_t *stats)
{
    if (!stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));
    if (!s_lock) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    stats->hits = s_hits;
    stats->misses = s_misses;
    stats->evictions = s_evictions;
    stats->expired = s_expired;
    stats->truncated = s_truncated;
    stats->bytes = (uint32_t)s_total_bytes;
    for (int i = 0; i < ESPAGENT_CACHE_MAX_ENTRIES; ++i) {
        if (s_entries[i].used) {
            stats->entries++;
        }
    }
    xSemaphoreGive(s_lock);
}

size_t cache_dump(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return 0;
    }

    out[0] = '\0';
    if (!s_lock) {
        return 0;
    }

    size_t off = 0;
    int64_t now = now_us();
    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (int i = 0; i < ESPAGENT_CACHE_MAX_ENTRIES && off < out_size - 1; ++i) {
        cache_entry_t *entry = &s_entries[i];
        if (!entry->used) {
            continue;
        }

        int64_t ttl_left_s = -1;
        if (entry->expires_at_us > 0) {
            int64_t ttl_left_us = entry->expires_at_us - now;
            ttl_left_s = ttl_left_us > 0 ? ttl_left_us / 1000000LL : 0;
        }

        int64_t age_s = 0;
        if (entry->last_access_us > 0 && now >= entry->last_access_us) {
            age_s = (now - entry->last_access_us) / 1000000LL;
        }

        int n = snprintf(out + off, out_size - off,
                         "- key=%s bytes=%u ttl_s=%lld hits=%u last_access_age_s=%lld\n",
                         entry->key,
                         (unsigned)entry->value_len,
                         (long long)ttl_left_s,
                         (unsigned)entry->hits,
                         (long long)age_s);
        if (n < 0) {
            break;
        }
        if ((size_t)n >= out_size - off) {
            off = out_size - 1;
            out[off] = '\0';
            break;
        }
        off += (size_t)n;
    }

    xSemaphoreGive(s_lock);

    if (off == 0) {
        snprintf(out, out_size, "(cache empty)\n");
        off = strlen(out);
    }

    return off;
}

void cache_clear(void)
{
    if (!s_lock) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < ESPAGENT_CACHE_MAX_ENTRIES; ++i) {
        free_entry(&s_entries[i]);
    }
    s_total_bytes = 0;
    s_hits = 0;
    s_misses = 0;
    s_evictions = 0;
    s_expired = 0;
    s_truncated = 0;
    xSemaphoreGive(s_lock);
}
