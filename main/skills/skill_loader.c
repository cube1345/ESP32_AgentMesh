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

static int skill_query_score(const char *query,
                             const char *name,
                             const char *title,
                             const char *desc,
                             const char *content)
{
    if (!query || !query[0]) {
        return 0;
    }

    int score = 0;
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

void skill_loader_invalidate_cache(void)
{
    cache_delete(SKILLS_SUMMARY_CACHE_KEY);
}
