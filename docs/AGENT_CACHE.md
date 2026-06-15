# ESPAgent Agent Cache

This document records the first agent-cache update for ESPAgent on ESP32-S3.

## Goal

ESPAgent runs the agent loop on an MCU, so cache design must reduce repeated local work without adding unbounded RAM use or flash wear.

This update adds an application-level KV cache for agent metadata and prompt fragments. It does not implement transformer tensor KV cache; model-side KV/prompt caching remains provider-managed.

## Added Modules

- `main/cache/cache_store.h`
- `main/cache/cache_store.c`

The cache is RAM-only in this first version. It uses fixed metadata slots, heap-owned string values, TTL expiry, and LRU-style eviction.

Values prefer PSRAM through `heap_caps_malloc(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` and fall back to regular heap allocation.

## Runtime Limits

Configured in `main/espagent_config.h`:

```c
#define ESPAGENT_CACHE_MAX_ENTRIES       32
#define ESPAGENT_CACHE_MAX_KEY_BYTES     64
#define ESPAGENT_CACHE_MAX_VALUE_BYTES   4096
#define ESPAGENT_CACHE_MAX_TOTAL_BYTES   (24 * 1024)
#define ESPAGENT_CACHE_DEFAULT_TTL_S     300
#define ESPAGENT_CACHE_SKILLS_TTL_S      (24 * 60 * 60)
```

These limits are deliberately conservative for ESP32-S3. The cache is useful for short strings, compact JSON, prompt fragments, tool metadata, and small tool result summaries.

## Public API

```c
esp_err_t cache_store_init(void);
esp_err_t cache_get(const char *key, char *out, size_t out_size);
esp_err_t cache_put(const char *key, const char *value, uint32_t ttl_s);
esp_err_t cache_delete(const char *key);
esp_err_t cache_delete_prefix(const char *prefix);
void cache_stats(cache_stats_t *stats);
size_t cache_dump(char *out, size_t out_size);
void cache_clear(void);
```

`cache_stats_t` exposes:

- `hits`
- `misses`
- `evictions`
- `expired`
- `truncated`
- `entries`
- `bytes`

## Current Agent Integration

`app_main()` initializes the cache before `skill_loader_init()`.

`skill_loader_build_summary()` now uses key `prompt:skills_summary`:

1. Try RAM cache.
2. If hit, return cached skills summary.
3. If miss or expired, scan SPIFFS and rebuild summary.
4. Store rebuilt summary with `ESPAGENT_CACHE_SKILLS_TTL_S`.

`write_file` and `edit_file` invalidate `prompt:skills_summary` when they change files under `/spiffs/skills/`.

This reduces repeated SPIFFS directory scans and markdown reads during normal agent turns.

The skills summary TTL is intentionally long. Normal skill changes should invalidate explicitly through `write_file` or `edit_file`; the TTL is a fallback for unusual cases.

## CLI Observability

Serial CLI commands:

```text
cache_stats
cache_dump
cache_clear
```

`cache_stats` prints:

```text
Cache entries:   1/32
Cache bytes:     812/24576
Cache hits:      4
Cache misses:    1
Cache hit rate:  80.00% (5 lookups)
Cache evictions: 0
Cache expired:   0
Cache truncated: 0
```

Hit rate is calculated as:

```text
hits / (hits + misses)
```

If there have been no lookups, hit rate is reported as `0.00% (0 lookups)`.

`cache_dump` prints per-entry observability:

```text
=== Cache Entries ===
- key=prompt:skills_summary bytes=812 ttl_s=86340 hits=4 last_access_age_s=2
```

`cache_clear` clears entries and resets cache counters.

## Key Naming

Use readable prefixes and hash only large or sensitive arguments:

- `prompt:skills_summary`
- `mcp:tools:<server_id>`
- `tool:web_search:<query_hash>`
- `tool:mcp:<tool_name>:<args_hash>`
- `sensor:sgp30:last`
- `sensor:presence:last`

## Safety Rules

- Do not cache secrets, API tokens, or full private conversations.
- Prefer caching read-only observations over actions.
- Do not use stale cache as proof of hardware safety state.
- Keep flash persistence out of high-churn paths until write amplification is designed.
- Use `cache_delete_prefix()` when config changes invalidate a namespace.

## Next Steps

- Cache MCP `tools/list` after MCP support lands.
- Add TTL web-search result summaries keyed by normalized query hash.
- Add short TTL sensor observation cache for non-safety telemetry.
- Consider SPIFFS JSONL warm cache after RAM behavior is stable.
- Add local prompt-state hash diagnostics when LLM usage metrics include provider prompt-cache counters.
