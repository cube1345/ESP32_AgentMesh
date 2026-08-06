#include "skills/skill_loader.h"
#include "cache/cache_store.h"
#include "espagent_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include "esp_log.h"

static const char *TAG = "skills";
static const char *SKILLS_SUMMARY_CACHE_KEY = "prompt:skills_summary";

#define SKILL_LOADER_MAX_FILE_TEXT 2048

typedef struct {
    char rel_name[96];
    char path[320];
    char title[96];
    char desc[256];
} skill_file_info_t;

/*
 * Skills are stored as markdown files in spiffs_data/skills/
 * and flashed into the SPIFFS partition at build time.
 */

esp_err_t skill_loader_init(void)
{
    ESP_LOGI(TAG, "Initializing skills system");

    DIR *dir = opendir(ESPAGENT_SPIFFS_BASE);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open SPIFFS — skills may not be available");
        return ESP_OK;
    }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (strncmp(name, "skills/", 7) == 0 && len > 10 &&
            strcmp(name + len - 3, ".md") == 0) {
            count++;
        }
    }
    closedir(dir);

    ESP_LOGI(TAG, "Skills system ready (%d skills on SPIFFS)", count);
    return ESP_OK;
}

/* ── Build skills summary for system prompt ──────────────────── */

/**
 * Parse first line as title: expects "# Title".
 * Writes the title (without "# " prefix) into out.
 */
static void extract_title(const char *line, size_t len, char *out, size_t out_size)
{
    const char *start = line;
    if (len >= 2 && line[0] == '#' && line[1] == ' ') {
        start = line + 2;
        len -= 2;
    }

    /* Trim trailing whitespace/newline */
    while (len > 0 && (start[len - 1] == '\n' || start[len - 1] == '\r' || start[len - 1] == ' ')) {
        len--;
    }

    size_t copy = len < out_size - 1 ? len : out_size - 1;
    memcpy(out, start, copy);
    out[copy] = '\0';
}

/**
 * Extract description: text between the first line and the first blank line.
 */
static void extract_description(FILE *f, char *out, size_t out_size)
{
    size_t off = 0;
    char line[256];

    while (fgets(line, sizeof(line), f) && off < out_size - 1) {
        size_t len = strlen(line);

        /* Stop at blank line or section header */
        if (len == 0 || (len == 1 && line[0] == '\n') ||
            (len >= 2 && line[0] == '#' && line[1] == '#')) {
            break;
        }

        /* Skip leading blank lines */
        if (off == 0 && line[0] == '\n') continue;

        /* Trim trailing newline for concatenation */
        if (line[len - 1] == '\n') {
            line[len - 1] = ' ';
        }

        size_t copy = len < out_size - off - 1 ? len : out_size - off - 1;
        memcpy(out + off, line, copy);
        off += copy;
    }

    /* Trim trailing space */
    while (off > 0 && out[off - 1] == ' ') off--;
    out[off] = '\0';
}

static bool contains_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !needle[0]) {
        return false;
    }

    size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < needle_len && p[i]) {
            unsigned char hc = (unsigned char)p[i];
            unsigned char nc = (unsigned char)needle[i];
            if (hc >= 'A' && hc <= 'Z') hc = (unsigned char)(hc - 'A' + 'a');
            if (nc >= 'A' && nc <= 'Z') nc = (unsigned char)(nc - 'A' + 'a');
            if (hc != nc) {
                break;
            }
            i++;
        }
        if (i == needle_len) {
            return true;
        }
    }
    return false;
}

static void trim_in_place(char *s);

static int skill_match_directive_score(const char *query, const char *content)
{
    if (!query || !query[0] || !content || !content[0]) {
        return 0;
    }

    int score = 0;
    const char *line = content;
    while (*line) {
        const char *line_end = strpbrk(line, "\r\n");
        size_t line_len = line_end ? (size_t)(line_end - line) : strlen(line);
        if (line_len > 0 && line_len < 384) {
            char directive[384] = {0};
            memcpy(directive, line, line_len);
            directive[line_len] = '\0';

            char *value = strstr(directive, "@match");
            if (value) {
                value += strlen("@match");
                while (*value && isspace((unsigned char)*value)) value++;
                if (strncmp(value, "trigger=", 8) == 0) {
                    value += 8;
                } else if (*value == ':' || *value == '=') {
                    value++;
                }
                while (*value && isspace((unsigned char)*value)) value++;

                char quote = '\0';
                if (*value == '"' || *value == '\'') {
                    quote = *value++;
                }
                char *value_end = quote ? strchr(value, quote) : NULL;
                if (value_end) {
                    *value_end = '\0';
                }

                char *cursor = value;
                while (cursor && *cursor) {
                    char *next = strpbrk(cursor, "|,");
                    if (next) {
                        *next++ = '\0';
                    }
                    trim_in_place(cursor);
                    size_t alias_len = strlen(cursor);
                    if (alias_len >= 2 && contains_ci(query, cursor)) {
                        score += 40 + (int)(alias_len > 24 ? 24 : alias_len);
                    }
                    cursor = next;
                }
            }
        }

        if (!line_end) {
            break;
        }
        line = line_end + 1;
        if (line_end[0] == '\r' && line[0] == '\n') {
            line++;
        }
    }
    return score;
}

static int skill_query_score(const char *query,
                             const char *name,
                             const char *title,
                             const char *desc,
                             const char *content)
{
    if (!query || !query[0]) {
        return 0;
    }

    int score = skill_match_directive_score(query, content);

    /* Score ASCII words independently so mixed queries such as
       "cube最喜欢吃什么" still match a skill named "cube-favorite". */
    char ascii_token[48] = {0};
    size_t ascii_len = 0;
    for (const unsigned char *p = (const unsigned char *)query; ; p++) {
        unsigned char c = *p;
        bool ascii_word = (c >= '0' && c <= '9') ||
                          (c >= 'A' && c <= 'Z') ||
                          (c >= 'a' && c <= 'z') ||
                          c == '_' || c == '-';
        if (ascii_word && ascii_len + 1 < sizeof(ascii_token)) {
            ascii_token[ascii_len++] = (char)c;
        } else {
            if (ascii_len >= 2) {
                ascii_token[ascii_len] = '\0';
                if (contains_ci(name, ascii_token)) score += 4;
                if (contains_ci(title, ascii_token)) score += 6;
                if (contains_ci(desc, ascii_token)) score += 3;
                if (contains_ci(content, ascii_token)) score += 2;
            }
            ascii_len = 0;
        }
        if (c == '\0') {
            break;
        }
    }

    char token[48];
    size_t ti = 0;
    for (const unsigned char *p = (const unsigned char *)query; ; p++) {
        unsigned char c = *p;
        bool token_char = (c >= '0' && c <= '9') ||
                          (c >= 'A' && c <= 'Z') ||
                          (c >= 'a' && c <= 'z') ||
                          (c & 0x80);
        if (token_char && ti + 1 < sizeof(token)) {
            token[ti++] = (char)c;
            continue;
        }
        if (ti > 0) {
            token[ti] = '\0';
            if (ti >= 2) {
                if (contains_ci(name, token)) score += 4;
                if (contains_ci(title, token)) score += 6;
                if (contains_ci(desc, token)) score += 3;
                if (contains_ci(content, token)) score += 2;
            }
            ti = 0;
        }
        if (c == '\0') {
            break;
        }
    }
    return score;
}

static void trim_in_place(char *s)
{
    if (!s) {
        return;
    }
    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static bool parse_rule_value(const char *line,
                             const char *key,
                             char *out,
                             size_t out_size)
{
    if (!line || !key || !out || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    char pattern[32] = {0};
    snprintf(pattern, sizeof(pattern), "%s=", key);
    const char *p = strstr(line, pattern);
    if (!p) {
        return false;
    }
    p += strlen(pattern);

    char quote = '\0';
    if (*p == '"' || *p == '\'') {
        quote = *p++;
    }

    size_t off = 0;
    bool truncated = false;
    while (*p) {
        if (quote) {
            if (*p == quote) {
                break;
            }
        } else if (isspace((unsigned char)*p)) {
            break;
        }
        if (off + 1 < out_size) {
            out[off++] = *p;
        } else {
            truncated = true;
        }
        p++;
    }
    out[off] = '\0';
    if (truncated) {
        ESP_LOGW(TAG, "Ignoring oversized skill rule value key=%s", key);
        out[0] = '\0';
        return false;
    }
    trim_in_place(out);
    return out[0] != '\0';
}

static bool role_is_allowed_for_skill_rule(const char *role)
{
    return role &&
           (strcmp(role, "sensor_agent") == 0 ||
            strcmp(role, "control_agent") == 0 ||
            strcmp(role, "guardian_agent") == 0);
}

static bool action_is_allowed_for_skill_rule(const char *action)
{
    return action &&
           (strcmp(action, "read_temperature_humidity") == 0 ||
            strcmp(action, "read_environment") == 0 ||
            strcmp(action, "read_light_level") == 0 ||
            strcmp(action, "set_status_light") == 0 ||
            strcmp(action, "ws2812_set") == 0 ||
            strcmp(action, "set_curtain") == 0 ||
            strcmp(action, "servo_write") == 0 ||
            strcmp(action, "control_state") == 0 ||
            strcmp(action, "control_emergency_stop") == 0 ||
            strcmp(action, "control_clear_emergency_stop") == 0);
}

static bool args_json_looks_safe_object(const char *args_json)
{
    if (!args_json || args_json[0] == '\0') {
        return true;
    }
    size_t len = strlen(args_json);
    return len >= 2 && len < 240 && args_json[0] == '{' &&
           args_json[len - 1] == '}' && strstr(args_json, "..") == NULL;
}

static const char *find_trigger_delim(const char *s, size_t *delim_len)
{
    if (!s || !delim_len) {
        return NULL;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '|' || *p == ',') {
            *delim_len = 1;
            return (const char *)p;
        }
        if (p[0] == 0xEF && p[1] != '\0' && p[2] != '\0' &&
            p[1] == 0xBC && p[2] == 0x8C) {
            *delim_len = 3;
            return (const char *)p;
        }
    }
    return NULL;
}

static bool trigger_token_matches_message(const char *trigger,
                                          const char *message,
                                          char *matched,
                                          size_t matched_size)
{
    if (!trigger || !message) {
        return false;
    }

    const char *cursor = trigger;
    while (cursor && *cursor) {
        size_t delim_len = 0;
        const char *next = find_trigger_delim(cursor, &delim_len);
        size_t token_len = next ? (size_t)(next - cursor) : strlen(cursor);
        while (token_len > 0 && isspace((unsigned char)cursor[0])) {
            cursor++;
            token_len--;
        }
        while (token_len > 0 && isspace((unsigned char)cursor[token_len - 1])) {
            token_len--;
        }
        if (token_len >= 2 && token_len < matched_size) {
            char token[256] = {0};
            if (token_len >= sizeof(token)) {
                ESP_LOGW(TAG, "Ignoring oversized skill rule trigger token");
            } else {
                memcpy(token, cursor, token_len);
                token[token_len] = '\0';
                if (contains_ci(message, token)) {
                    if (matched && matched_size > 0) {
                        memcpy(matched, token, token_len + 1);
                    }
                    return true;
                }
            }
        } else if (token_len >= matched_size) {
            ESP_LOGW(TAG, "Ignoring trigger token too large for match buffer");
        }
        if (!next) {
            break;
        }
        cursor = next + delim_len;
    }
    return false;
}

static bool parse_skill_rule_line(const char *line,
                                  const char *skill_name,
                                  const char *message,
                                  skill_rule_match_t *match)
{
    if (!line || !skill_name || !message || !match ||
        strstr(line, "@rule") == NULL) {
        return false;
    }

    skill_rule_match_t local = {0};
    snprintf(local.skill_name, sizeof(local.skill_name), "%s", skill_name);

    if (!parse_rule_value(line, "trigger", local.trigger,
                          sizeof(local.trigger)) ||
        !parse_rule_value(line, "target_role", local.target_role,
                          sizeof(local.target_role)) ||
        !parse_rule_value(line, "action", local.action,
                          sizeof(local.action))) {
        return false;
    }
    if (!parse_rule_value(line, "args", local.args_json,
                          sizeof(local.args_json))) {
        snprintf(local.args_json, sizeof(local.args_json), "{}");
    }

    char matched_trigger[sizeof(local.trigger)] = {0};
    if (!trigger_token_matches_message(local.trigger, message,
                                       matched_trigger,
                                       sizeof(matched_trigger))) {
        return false;
    }
    if (!role_is_allowed_for_skill_rule(local.target_role) ||
        !action_is_allowed_for_skill_rule(local.action) ||
        !args_json_looks_safe_object(local.args_json)) {
        ESP_LOGW(TAG, "Ignoring invalid skill rule skill=%s role=%s action=%s",
                 skill_name, local.target_role, local.action);
        return false;
    }

    snprintf(local.trigger, sizeof(local.trigger), "%s", matched_trigger);
    *match = local;
    return true;
}

static int enumerate_skill_files(skill_file_info_t *items, int max_items)
{
    if (!items || max_items <= 0) {
        return 0;
    }

    DIR *dir = opendir(ESPAGENT_SPIFFS_BASE);
    if (!dir) {
        return 0;
    }

    int count = 0;
    struct dirent *ent;
    const char *skills_subdir = "skills/";
    const size_t subdir_len = strlen(skills_subdir);
    while ((ent = readdir(dir)) != NULL && count < max_items) {
        const char *name = ent->d_name;
        if (strncmp(name, skills_subdir, subdir_len) != 0) {
            continue;
        }
        size_t name_len = strlen(name);
        if (name_len < subdir_len + 4 || strcmp(name + name_len - 3, ".md") != 0) {
            continue;
        }

        skill_file_info_t *item = &items[count];
        memset(item, 0, sizeof(*item));
        snprintf(item->rel_name, sizeof(item->rel_name), "%s", name + subdir_len);
        char *dot = strrchr(item->rel_name, '.');
        if (dot) {
            *dot = '\0';
        }
        snprintf(item->path, sizeof(item->path), "%s/%s", ESPAGENT_SPIFFS_BASE, name);

        FILE *f = fopen(item->path, "r");
        if (!f) {
            continue;
        }
        char first_line[128];
        if (fgets(first_line, sizeof(first_line), f)) {
            extract_title(first_line, strlen(first_line), item->title, sizeof(item->title));
            extract_description(f, item->desc, sizeof(item->desc));
        }
        fclose(f);
        count++;
    }
    closedir(dir);
    return count;
}

static int count_skill_files(void)
{
    DIR *dir = opendir(ESPAGENT_SPIFFS_BASE);
    if (!dir) {
        return 0;
    }

    int count = 0;
    struct dirent *ent;
    const char *skills_subdir = "skills/";
    const size_t subdir_len = strlen(skills_subdir);
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        if (strncmp(name, skills_subdir, subdir_len) != 0) {
            continue;
        }
        size_t name_len = strlen(name);
        if (name_len >= subdir_len + 4 &&
            strcmp(name + name_len - 3, ".md") == 0) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

static esp_err_t load_skill_file_infos(skill_file_info_t **items_out,
                                       int *count_out)
{
    if (!items_out || !count_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *items_out = NULL;
    *count_out = 0;

    int capacity = count_skill_files();
    if (capacity <= 0) {
        return ESP_ERR_NOT_FOUND;
    }

    skill_file_info_t *items = calloc(capacity, sizeof(skill_file_info_t));
    if (!items) {
        return ESP_ERR_NO_MEM;
    }

    int count = enumerate_skill_files(items, capacity);
    if (count <= 0) {
        free(items);
        return ESP_ERR_NOT_FOUND;
    }

    *items_out = items;
    *count_out = count;
    return ESP_OK;
}

static bool skill_file_key_is_valid(const char *name)
{
    if (!name || !name[0] || strstr(name, "..")) {
        return false;
    }

    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        bool ok = (c >= '0' && c <= '9') ||
                  (c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z') ||
                  c == '_' || c == '-' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool build_exact_skill_path(const char *name, char *path, size_t path_size,
                                   char *rel_name, size_t rel_name_size)
{
    if (!skill_file_key_is_valid(name) || !path || path_size == 0) {
        return false;
    }

    char key[96] = {0};
    snprintf(key, sizeof(key), "%s", name);
    size_t len = strlen(key);
    char *dot = strchr(key, '.');
    if (dot && (len <= 3 || strcmp(key + len - 3, ".md") != 0)) {
        return false;
    }
    if (len > 3 && strcmp(key + len - 3, ".md") == 0) {
        key[len - 3] = '\0';
    }
    if (!key[0] || !skill_file_key_is_valid(key) || strchr(key, '.')) {
        return false;
    }

    int n = snprintf(path, path_size, "%s%s.md", ESPAGENT_SKILLS_PREFIX, key);
    if (n < 0 || (size_t)n >= path_size) {
        return false;
    }
    if (rel_name && rel_name_size > 0) {
        snprintf(rel_name, rel_name_size, "%s", key);
    }
    return true;
}

static esp_err_t read_skill_file(const char *path,
                                 const char *fallback_title,
                                 char *buf,
                                 size_t size,
                                 char *resolved_title,
                                 size_t resolved_title_size)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t n = fread(buf, 1, size - 1, f);
    buf[n] = '\0';
    fclose(f);

    if (resolved_title && resolved_title_size > 0) {
        char title[96] = {0};
        const char *line_end = strpbrk(buf, "\r\n");
        size_t first_len = line_end ? (size_t)(line_end - buf) : strlen(buf);
        if (first_len > 0) {
            extract_title(buf, first_len, title, sizeof(title));
        }
        snprintf(resolved_title, resolved_title_size, "%s",
                 title[0] ? title : (fallback_title ? fallback_title : ""));
    }
    return ESP_OK;
}

static size_t build_summary_uncached(char *buf, size_t size)
{
    DIR *dir = opendir(ESPAGENT_SPIFFS_BASE);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open SPIFFS for skill enumeration");
        if (size > 0) {
            buf[0] = '\0';
        }
        return 0;
    }

    size_t off = 0;
    struct dirent *ent;
    /* SPIFFS readdir returns filenames relative to the mount point (e.g. "skills/weather.md").
       We match entries that start with "skills/" and end with ".md". */
    const char *skills_subdir = "skills/";
    const size_t subdir_len = strlen(skills_subdir);

    while ((ent = readdir(dir)) != NULL && off < size - 1) {
        const char *name = ent->d_name;

        /* Match files under skills/ with .md extension */
        if (strncmp(name, skills_subdir, subdir_len) != 0) continue;

        size_t name_len = strlen(name);
        if (name_len < subdir_len + 4) continue;  /* at least "skills/x.md" */
        if (strcmp(name + name_len - 3, ".md") != 0) continue;

        /* Build full path */
        char full_path[296];
        snprintf(full_path, sizeof(full_path), "%s/%s", ESPAGENT_SPIFFS_BASE, name);

        FILE *f = fopen(full_path, "r");
        if (!f) continue;

        /* Read first line for title */
        char first_line[128];
        if (!fgets(first_line, sizeof(first_line), f)) {
            fclose(f);
            continue;
        }

        char title[64];
        extract_title(first_line, strlen(first_line), title, sizeof(title));

        /* Read description (until blank line) */
        char desc[256];
        extract_description(f, desc, sizeof(desc));
        fclose(f);

        /* Append to summary */
        off += snprintf(buf + off, size - off,
            "- **%s**: %s (read with: read_file %s)\n",
            title, desc, full_path);
    }

    closedir(dir);

    buf[off] = '\0';
    ESP_LOGI(TAG, "Skills summary: %d bytes", (int)off);
    return off;
}

size_t skill_loader_build_summary(char *buf, size_t size)
{
    if (!buf || size == 0) {
        return 0;
    }

    esp_err_t cache_err = cache_get(SKILLS_SUMMARY_CACHE_KEY, buf, size);
    if (cache_err == ESP_OK) {
        ESP_LOGD(TAG, "Skills summary cache hit: %d bytes", (int)strlen(buf));
        return strlen(buf);
    }

    size_t off = build_summary_uncached(buf, size);
    if (off > 0) {
        esp_err_t put_err = cache_put(SKILLS_SUMMARY_CACHE_KEY, buf, ESPAGENT_CACHE_SKILLS_TTL_S);
        if (put_err != ESP_OK) {
            ESP_LOGD(TAG, "Skills summary cache put skipped: %s", esp_err_to_name(put_err));
        }
    }
    return off;
}

esp_err_t skill_loader_build_index_text(char *buf, size_t size)
{
    if (!buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    skill_file_info_t *items = NULL;
    int count = 0;
    esp_err_t err = load_skill_file_infos(&items, &count);
    if (err != ESP_OK) {
        buf[0] = '\0';
        return err;
    }

    size_t off = 0;
    for (int i = 0; i < count && off < size - 1; i++) {
        off += snprintf(buf + off, size - off,
                        "- %s: %s%s%s\n",
                        items[i].rel_name,
                        items[i].title[0] ? items[i].title : "(untitled)",
                        items[i].desc[0] ? " | " : "",
                        items[i].desc);
    }
    free(items);
    return off > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t skill_loader_read_skill_by_name(const char *name,
                                          char *buf,
                                          size_t size,
                                          char *resolved_title,
                                          size_t resolved_title_size)
{
    if (!name || !name[0] || !buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';

    char exact_path[160] = {0};
    char exact_rel_name[96] = {0};
    if (build_exact_skill_path(name, exact_path, sizeof(exact_path),
                               exact_rel_name, sizeof(exact_rel_name))) {
        esp_err_t exact_err = read_skill_file(exact_path, exact_rel_name,
                                             buf, size,
                                             resolved_title,
                                             resolved_title_size);
        if (exact_err == ESP_OK) {
            return ESP_OK;
        }
    }

    skill_file_info_t *items = NULL;
    int count = 0;
    esp_err_t err = load_skill_file_infos(&items, &count);
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < count; i++) {
        if (strcmp(items[i].title, name) != 0) {
            continue;
        }

        err = read_skill_file(items[i].path,
                              items[i].title[0] ? items[i].title : items[i].rel_name,
                              buf, size,
                              resolved_title,
                              resolved_title_size);
        free(items);
        return err == ESP_ERR_NOT_FOUND ? ESP_FAIL : err;
    }
    free(items);
    buf[0] = '\0';
    return ESP_ERR_NOT_FOUND;
}

esp_err_t skill_loader_build_relevant_details(const char *query,
                                              char *buf,
                                              size_t size,
                                              int max_skills)
{
    if (!query || !query[0] || !buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (max_skills <= 0) {
        max_skills = 2;
    }

    skill_file_info_t *items = NULL;
    char *file_text = calloc(1, SKILL_LOADER_MAX_FILE_TEXT);
    if (!file_text) {
        free(file_text);
        buf[0] = '\0';
        return ESP_ERR_NO_MEM;
    }

    int count = 0;
    esp_err_t load_err = load_skill_file_infos(&items, &count);
    if (load_err != ESP_OK) {
        buf[0] = '\0';
        free(file_text);
        return load_err;
    }

    int *best_index = calloc(count, sizeof(int));
    int *best_score = calloc(count, sizeof(int));
    if (!best_index || !best_score) {
        free(best_index);
        free(best_score);
        free(items);
        free(file_text);
        buf[0] = '\0';
        return ESP_ERR_NO_MEM;
    }

    int matched = 0;

    for (int i = 0; i < count; i++) {
        file_text[0] = '\0';
        FILE *f = fopen(items[i].path, "r");
        if (f) {
            size_t n = fread(file_text, 1, SKILL_LOADER_MAX_FILE_TEXT - 1, f);
            file_text[n] = '\0';
            fclose(f);
        }
        int score = skill_query_score(query,
                                      items[i].rel_name,
                                      items[i].title,
                                      items[i].desc,
                                      file_text);
        if (score <= 0) {
            continue;
        }
        int pos = matched++;
        best_index[pos] = i;
        best_score[pos] = score;
        for (int j = pos; j > 0; j--) {
            if (best_score[j] > best_score[j - 1]) {
                int tmp_score = best_score[j];
                int tmp_index = best_index[j];
                best_score[j] = best_score[j - 1];
                best_index[j] = best_index[j - 1];
                best_score[j - 1] = tmp_score;
                best_index[j - 1] = tmp_index;
            }
        }
    }

    if (matched <= 0) {
        buf[0] = '\0';
        free(best_index);
        free(best_score);
        free(items);
        free(file_text);
        return ESP_ERR_NOT_FOUND;
    }

    size_t off = 0;
    int limit = matched < max_skills ? matched : max_skills;
    for (int r = 0; r < limit && off < size - 1; r++) {
        int idx = best_index[r];
        file_text[0] = '\0';
        FILE *f = fopen(items[idx].path, "r");
        if (!f) {
            continue;
        }
        size_t n = fread(file_text, 1, SKILL_LOADER_MAX_FILE_TEXT - 1, f);
        file_text[n] = '\0';
        fclose(f);
        off += snprintf(buf + off, size - off,
                        "### %s (%s)\n\n%s\n\n",
                        items[idx].title[0] ? items[idx].title : items[idx].rel_name,
                        items[idx].rel_name,
                        file_text);
    }
    free(best_index);
    free(best_score);
    free(items);
    free(file_text);
    return off > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t skill_loader_find_matching_rule(const char *user_message,
                                          skill_rule_match_t *match)
{
    if (!user_message || !user_message[0] || !match) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(match, 0, sizeof(*match));

    skill_file_info_t *items = NULL;
    int count = 0;
    esp_err_t err = load_skill_file_infos(&items, &count);
    if (err != ESP_OK) {
        return err;
    }

    char *file_text = calloc(1, SKILL_LOADER_MAX_FILE_TEXT);
    if (!file_text) {
        free(items);
        return ESP_ERR_NO_MEM;
    }

    bool found = false;
    size_t best_trigger_len = 0;
    skill_rule_match_t best = {0};

    for (int i = 0; i < count; i++) {
        file_text[0] = '\0';
        FILE *f = fopen(items[i].path, "r");
        if (!f) {
            continue;
        }
        size_t n = fread(file_text, 1, SKILL_LOADER_MAX_FILE_TEXT - 1, f);
        file_text[n] = '\0';
        fclose(f);

        char *saveptr = NULL;
        for (char *line = strtok_r(file_text, "\r\n", &saveptr);
             line != NULL;
             line = strtok_r(NULL, "\r\n", &saveptr)) {
            skill_rule_match_t candidate = {0};
            if (parse_skill_rule_line(line, items[i].rel_name,
                                      user_message, &candidate)) {
                size_t trigger_len = strlen(candidate.trigger);
                if (!found || trigger_len > best_trigger_len) {
                    best = candidate;
                    best_trigger_len = trigger_len;
                    found = true;
                }
            }
        }
    }

    free(file_text);
    free(items);
    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }
    *match = best;
    return ESP_OK;
}

void skill_loader_invalidate_cache(void)
{
    cache_delete(SKILLS_SUMMARY_CACHE_KEY);
}
