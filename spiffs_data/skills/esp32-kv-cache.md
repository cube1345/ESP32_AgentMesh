# ESP32 KV Cache

Design a bounded key-value cache for ESPAgent on ESP32-S3.

## When to use
Use this skill when implementing or reviewing local cache storage, tool result caching, MCP tool cache, sensor cache, prompt fragment cache, or SPIFFS/NVS-backed KV data.

## Design goals
- Bounded RAM usage.
- Low flash wear.
- Deterministic eviction.
- Explicit TTL and invalidation.
- Safe values only: no raw API keys, tokens, or full private conversations unless explicitly intended.
- Short tool outputs should be cached; large outputs should be summarized or truncated.

## Recommended cache tiers
1. RAM hot cache
   - Fast lookup.
   - Volatile.
   - Store recent tool results, MCP tools list, prompt fragments.
   - Eviction: LRU, CLOCK, or oldest-entry FIFO when simplicity matters.
2. SPIFFS warm cache
   - Survives reboot.
   - Store small JSON records and summaries.
   - Use append-log plus occasional compaction to reduce write amplification.
3. NVS config cache
   - Only for small configuration and stable metadata.
   - Do not use NVS for high-churn entries.

## Suggested namespaces
- `prompt:skills_summary`
- `prompt:static_hash`
- `mcp:tools:<server_id>`
- `tool:web_search:<query_hash>`
- `tool:mcp:<tool_name>:<args_hash>`
- `sensor:sgp30:last`
- `sensor:presence:last`
- `file:stat:<path_hash>`

## Suggested record fields
```json
{
  "key": "tool:web_search:abc123",
  "value": "short text or compact JSON",
  "created_ms": 123456,
  "expires_ms": 423456,
  "last_access_ms": 223456,
  "hits": 3,
  "flags": 0,
  "schema": 1
}
```

## RAM entry shape
For MCU code, prefer fixed-size metadata plus heap-owned value:
```c
typedef struct {
    char key[64];
    char *value;
    uint32_t value_len;
    int64_t created_ms;
    int64_t expires_ms;
    int64_t last_access_ms;
    uint16_t hits;
    uint8_t flags;
    bool refreshing;
    bool in_use;
} cache_entry_t;
```

Keep the entry array fixed-size. Bound total value bytes separately.

## Suggested C API
```c
esp_err_t cache_store_init(void);
esp_err_t cache_get(const char *key, char *out, size_t out_size);
esp_err_t cache_put(const char *key, const char *value, uint32_t ttl_s);
esp_err_t cache_delete(const char *key);
esp_err_t cache_delete_prefix(const char *prefix);
void cache_stats(cache_stats_t *stats);
size_t cache_dump(char *out, size_t out_size);
```

Optional stale-while-refresh API for slow remote calls:
```c
typedef esp_err_t (*cache_refresh_fn_t)(void *ctx, char *out, size_t out_size);

esp_err_t cache_get_or_refresh(const char *key,
                               uint32_t ttl_s,
                               uint32_t stale_ttl_s,
                               cache_refresh_fn_t refresh,
                               void *ctx,
                               char *out,
                               size_t out_size);
```

Behavior:
- Fresh hit: return immediately.
- Stale hit: return stale value, mark `refreshing=true`, and let caller schedule a background refresh when possible.
- Cold miss: block and compute once.
- Refresh failure: keep stale value until `stale_ttl_s`, then delete.

## Key hashing
- Use FNV-1a or SHA-256 when available.
- Keep human-readable prefixes, hash only large or sensitive argument payloads.
- Include tool name, normalized arguments, model/provider when response semantics depend on them.

## TTL defaults
- Sensor samples: 1-10 seconds.
- MCP tools list: 5-30 minutes.
- Web search results: 10-60 minutes depending on query freshness.
- Skills summary: until skill files change.
- Prompt static fragment hash: until firmware/config changes.

## Eviction rules
1. Drop expired entries first.
2. Then drop lowest-value entries:
   - low hit count
   - old last access
   - large value size
3. Keep hard limits:
   - max RAM entries
   - max RAM bytes
   - max SPIFFS cache bytes
   - max single value bytes

## Cold-miss and refresh rules
- Deduplicate concurrent cold misses for the same key. On FreeRTOS, keep a per-entry `refreshing` flag or a small pending table.
- Use identity guards: if a key is cleared or overwritten while a refresh is in flight, do not let the old refresh result overwrite the newer entry.
- Do not block the agent loop on background refresh unless the value is required for correctness.
- For hardware safety reads, prefer fresh blocking reads over stale cache.
- For web/MCP metadata, stale-while-refresh is acceptable.

## Flash-wear rules
- Do not write every hit to SPIFFS.
- Keep hit counters in RAM and flush lazily.
- Batch writes when possible.
- Compact only when wasted space crosses a threshold.
- Avoid NVS for rapidly changing cache data.
- Write persisted records atomically: write a temp file or append a complete log record, then commit/rename when the filesystem supports it.
- Include a cache schema version. If version is unknown, ignore the old cache instead of trying to parse it.

## File-backed cache invalidation
- Include file path plus mtime/size/hash in the cache metadata when caching file contents or skill summaries.
- If mtime or size changes, drop the cache entry.
- Normalize paths before using them as keys so `/a/../b` and `/b` do not create duplicate entries.
- For partial file views or truncated summaries, mark the record as partial and require an explicit fresh read before edits.

## Invalidation
- Invalidate `prompt:skills_summary` when `/spiffs/skills/*.md` changes.
- Invalidate `mcp:tools:<server_id>` when MCP server URL/token changes or `tools/list` fails schema validation.
- Invalidate tool results when related config changes.
- Never serve stale hardware safety state for actuator commands.

## Safety
- Cache read-only observations more aggressively than actions.
- Never cache the result of a command that changed hardware state as if it were still true forever.
- For GPIO/servo/light commands, cache only the last known state with a clear timestamp.
- Always allow manual cache clear through CLI.

## First implementation path
1. Add `main/cache/cache_store.c` and `main/cache/cache_store.h`.
2. Start with RAM-only fixed-size entries.
3. Add TTL, hit/miss stats, and prefix delete.
4. Use it for MCP `tools/list` and SGP30/presence last-read samples.
5. Add SPIFFS persistence only after RAM behavior is verified.

## Current ESPAgent implementation
- RAM cache lives in `main/cache/cache_store.c` with API in `main/cache/cache_store.h`.
- Startup initializes it from `app_main()` before skills are loaded.
- Config limits are in `main/espagent_config.h`:
  - `ESPAGENT_CACHE_MAX_ENTRIES=32`
  - `ESPAGENT_CACHE_MAX_KEY_BYTES=64`
  - `ESPAGENT_CACHE_MAX_VALUE_BYTES=4096`
  - `ESPAGENT_CACHE_MAX_TOTAL_BYTES=24KB`
  - `ESPAGENT_CACHE_DEFAULT_TTL_S=300`
- Values prefer PSRAM through `heap_caps_malloc(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` and fall back to heap.
- `prompt:skills_summary` is cached for `ESPAGENT_CACHE_SKILLS_TTL_S`.
- `write_file` and `edit_file` invalidate the skills summary cache when modifying files under `/spiffs/skills/`.
- Serial CLI commands:
  - `cache_stats`
  - `cache_dump`
  - `cache_clear`
- `cache_stats` includes hit rate and truncated-read counters.
- `cache_clear` clears entries and resets counters.
- Skills summary uses a long TTL and relies primarily on explicit invalidation.

Next good cache targets:
- MCP `tools/list` once MCP client/server support lands.
- Web-search result summaries using a query hash and freshness TTL.
- Non-safety sensor observations with short TTL.

## Suggested compile-time limits
Start conservative:
```c
#define ESPAGENT_CACHE_MAX_ENTRIES          32
#define ESPAGENT_CACHE_MAX_VALUE_BYTES      2048
#define ESPAGENT_CACHE_MAX_TOTAL_BYTES      (24 * 1024)
#define ESPAGENT_CACHE_DEFAULT_TTL_S        300
#define ESPAGENT_CACHE_STALE_TTL_S          1800
#define ESPAGENT_CACHE_SPIFFS_FILE          ESPAGENT_SPIFFS_BASE "/cache.jsonl"
```

For ESP32-S3 with PSRAM, values may live in PSRAM, but metadata should stay small enough for internal RAM when possible.
