#include "net/net_guard.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_net_guard;
static portMUX_TYPE s_net_guard_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_background_defer_until_ms;

static esp_err_t net_guard_ensure(void)
{
    if (!s_net_guard) {
        s_net_guard = xSemaphoreCreateMutex();
        if (!s_net_guard) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t espagent_net_guard_take(uint32_t timeout_ms)
{
    esp_err_t err = net_guard_ensure();
    if (err != ESP_OK) {
        return err;
    }

    TickType_t ticks = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(s_net_guard, ticks) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void espagent_net_guard_give(void)
{
    if (s_net_guard) {
        xSemaphoreGive(s_net_guard);
    }
}

void espagent_net_guard_defer_background(uint32_t defer_ms)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t until_ms = now_ms + defer_ms;
    portENTER_CRITICAL(&s_net_guard_mux);
    if (until_ms > s_background_defer_until_ms) {
        s_background_defer_until_ms = until_ms;
    }
    portEXIT_CRITICAL(&s_net_guard_mux);
}

bool espagent_net_guard_background_allowed(size_t min_internal_free, size_t min_largest_internal)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t defer_until_ms = 0;
    portENTER_CRITICAL(&s_net_guard_mux);
    defer_until_ms = s_background_defer_until_ms;
    portEXIT_CRITICAL(&s_net_guard_mux);

    if (now_ms < defer_until_ms) {
        return false;
    }

    if (min_internal_free > 0 &&
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < min_internal_free) {
        return false;
    }

    if (min_largest_internal > 0 &&
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < min_largest_internal) {
        return false;
    }

    return true;
}

esp_err_t espagent_net_guard_take_background(uint32_t timeout_ms,
                                             size_t min_internal_free,
                                             size_t min_largest_internal)
{
    if (!espagent_net_guard_background_allowed(min_internal_free, min_largest_internal)) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = espagent_net_guard_take(timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    if (!espagent_net_guard_background_allowed(min_internal_free, min_largest_internal)) {
        espagent_net_guard_give();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
