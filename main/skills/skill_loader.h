#pragma once

#include "esp_err.h"
#include <stddef.h>

typedef struct {
    char skill_name[96];
    char trigger[256];
    char target_role[32];
    char action[48];
    char args_json[256];
} skill_rule_match_t;

/**
 * Initialize skills system.
 * Scans SPIFFS for available skill markdown files.
 */
esp_err_t skill_loader_init(void);

/**
 * Build a summary of all available skills for the system prompt.
 * Lists each skill with its title and description.
 *
 * @param buf   Output buffer
 * @param size  Buffer size
 * @return Number of bytes written (0 if no skills found)
 */
size_t skill_loader_build_summary(char *buf, size_t size);

esp_err_t skill_loader_build_index_text(char *buf, size_t size);

esp_err_t skill_loader_read_skill_by_name(const char *name,
                                          char *buf,
                                          size_t size,
                                          char *resolved_title,
                                          size_t resolved_title_size);

esp_err_t skill_loader_build_relevant_details(const char *query,
                                              char *buf,
                                              size_t size,
                                              int max_skills);

/**
 * Find the first single-tool rule declared in any Runtime Skill.
 *
 * Rule syntax inside a skill markdown file:
 *   @rule trigger="phrase|alias" target_role=control_agent action=set_status_light args={"color":"blue"}
 *
 * The first trigger phrase that appears in user_message wins. Rules are
 * intentionally one-shot: callers should execute at most one tool.
 */
esp_err_t skill_loader_find_matching_rule(const char *user_message,
                                          skill_rule_match_t *match);

/**
 * Drop cached skill metadata so the next prompt rebuild sees SPIFFS changes.
 */
void skill_loader_invalidate_cache(void);
