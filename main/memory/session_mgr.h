#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Initialize session manager.
 */
esp_err_t session_mgr_init(void);

/**
 * Append a message to a session file (JSONL format).
 * @param chat_id   Session identifier (e.g., "12345")
 * @param role      "user" or "assistant"
 * @param content   Message text
 */
esp_err_t session_append(const char *chat_id, const char *role, const char *content);

/**
 * Append a structured trace event to a per-session trace file.
 * Trace events are not loaded into LLM chat history.
 */
esp_err_t session_append_trace(const char *chat_id,
                               const char *event_type,
                               const char *summary,
                               const char *raw_json);

/**
 * Load session history as a JSON array string suitable for LLM messages.
 * Returns the last max_msgs messages as:
 * [{"role":"user","content":"..."},{"role":"assistant","content":"..."},...]
 *
 * @param chat_id   Session identifier
 * @param buf       Output buffer (caller allocates)
 * @param size      Buffer size
 * @param max_msgs  Maximum number of messages to return
 */
esp_err_t session_get_history_json(const char *chat_id, char *buf, size_t size, int max_msgs);

/**
 * Load recent structured trace events as a JSON array string.
 * Trace events include tool_use, tool_result, async_result, and final_reply.
 */
esp_err_t session_get_trace_json(const char *chat_id, char *buf, size_t size, int max_events);

/**
 * Build a lightweight task tree by grouping recent trace events by trace_id or command_id.
 */
esp_err_t session_get_task_tree_json(const char *chat_id, char *buf, size_t size, int max_events);

/**
 * List persisted trace files under the session store.
 */
esp_err_t session_get_trace_index_json(char *buf, size_t size);

/**
 * Build a compact session brief for prompt injection. This is lighter than
 * raw history and focuses on recent user goals, recent assistant conclusions,
 * and recent task/trace state.
 */
esp_err_t session_build_context_brief(const char *chat_id, char *buf, size_t size);
esp_err_t session_refresh_context_brief(const char *chat_id);
esp_err_t session_build_relevant_task_brief(const char *chat_id,
                                            const char *query,
                                            char *buf,
                                            size_t size);

/**
 * Clear a session (delete the file).
 */
esp_err_t session_clear(const char *chat_id);

/**
 * List all session files (prints to log).
 */
void session_list(void);
