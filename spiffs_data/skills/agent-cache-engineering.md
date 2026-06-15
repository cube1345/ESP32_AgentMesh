# Agent Cache Engineering

Design cache-friendly agent behavior for ESPAgent.

## When to use
Use this skill when the user asks about cache, prompt caching, context caching, KV cache, lower latency, lower token cost, or keeping repeated agent context stable.

## Key ideas
- Remote LLM prompt caches work best when the beginning of the request is stable.
- Put stable content first: base system prompt, tool definitions, project rules, skills summary.
- Put dynamic content last: current user message, current time, sensor readings, tool results.
- Do not rewrite the system prompt just to add changing facts. Put changing facts into the next user message, tool result, or short turn-context section.
- Keep tool order deterministic. Randomized tool ordering can break prefix cache hits.
- Keep model/provider stable during a session when cache reuse matters.
- Treat API prompt caching as provider-managed. The ESP32 cannot directly access the model's internal attention KV tensors.

## ESPAgent guidance
1. Keep `tool_registry_get_tools_json()` deterministic.
2. Keep `context_build_system_prompt()` stable where possible.
3. Move frequently changing data out of the static prompt:
   - current time
   - current channel/chat_id
   - transient sensor readings
   - network status
4. Use session history for conversational memory, but compact old turns into memory notes before context grows too large.
5. Cache expensive local computations separately:
   - skills summary
   - file stat/hash results
   - MCP tools list
   - web/search results with TTL
   - sensor samples with short TTL

Current project status:
- Built-in tool JSON is rendered once by `tool_registry_init()`.
- Skills summary is cached under `prompt:skills_summary` by the RAM KV cache.
- `cache_stats`, `cache_dump`, and `cache_clear` expose cache observability through Serial CLI.
- Skill writes/edits invalidate the cached skills summary.

## Patterns observed from Claude Code
- Session-stable date: capture the local date once at session start so midnight does not rewrite the cached prompt prefix. If the date changes, append a small tail update instead of changing the front of the prompt.
- Tool schema cache: render tool schemas once per session. Mid-session feature flag changes, MCP reconnects, or dynamic tool descriptions can otherwise invalidate a large tool block and everything after it.
- Cache break tracking: hash the stable prompt blocks, tool schemas, model, beta flags, cache-control fields, and extra body options. Compare cache read tokens across calls to identify whether a break came from client-side changes or provider TTL/routing.
- Per-tool hash diagnostics: when the aggregate tools hash changes but the tool set is the same, compute per-tool schema hashes to name the changed schema.
- Bounded tracking: cap prompt-state tracking by source or session to prevent long-running agent/subagent sessions from growing unbounded.
- Sanitize diagnostics: collapse user-controlled MCP tool names in logs or analytics so server names, paths, or private tool identifiers are not leaked.

## Cache layers to consider
- Provider prompt cache: remote API feature, optimized by stable prefix layout.
- Agent context cache: cached system prompt fragments and skill summaries on ESP32.
- Tool result cache: short-lived cache for web/MCP/sensor results.
- Durable KV cache: SPIFFS/NVS backed metadata and small values.

## Prompt cache break detection for ESPAgent
Track one small prompt-cache state record per active chat/source:
- `system_hash`: hash of stable system prompt text.
- `tools_hash`: hash of tools JSON.
- `tool_names`: ordered tool names.
- `model`: current model id.
- `provider`: current provider id.
- `context_flags`: compact bitfield for cache-relevant toggles.
- `last_cache_read_tokens`: from provider usage when available.
- `last_call_ms`: local time of previous call.

When provider usage includes cache metrics, report a cache break when:
1. cache read tokens drop by more than a threshold, and
2. the absolute drop is large enough to matter, and
3. no known compaction/cache deletion just happened.

If provider cache metrics are unavailable, still log local prompt-state changes:
- tools changed
- system prompt changed
- model/provider changed
- MCP tool list changed
- skills summary changed

This gives enough observability to tune stable-prefix behavior on ESP32.

## ESPAgent prompt layout recommendation
Use this ordering:
1. Static identity and behavior.
2. Built-in tool descriptions.
3. Cached remote MCP tool descriptions.
4. Stable skills summary.
5. Long-term memory summary.
6. Recent session summary.
7. Current turn context.
8. Current user message and tool results.

Do not put current time, IP address, heap status, WiFi RSSI, sensor readings, or chat id before stable blocks unless absolutely necessary.

## Do not confuse these
- Transformer KV cache stores attention key/value tensors inside the model runtime.
- ESPAgent's ESP32 KV cache should store application data: strings, JSON, tool results, hashes, and timestamps.
- If the model runs in the cloud, true tensor KV cache lives with the provider, not on the ESP32.

## Good response pattern
When asked to improve caching:
1. Identify which layer is being optimized.
2. Check whether data is stable or dynamic.
3. Preserve stable prefixes.
4. Add TTL and invalidation for dynamic values.
5. Bound memory and flash writes for ESP32.
6. Verify by logging cache hits, misses, evictions, and stale reads.
