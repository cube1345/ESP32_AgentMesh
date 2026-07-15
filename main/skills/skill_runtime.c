#include "skills/skill_runtime.h"

#include "skills/skill_loader.h"
#include "espagent_config.h"

#include "cJSON.h"
#include "esp_log.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "skill_runtime";

#define SKILL_RUNTIME_MAX_NAME_CHARS 48
#define SKILL_RUNTIME_MAX_CONTENT_BYTES (12 * 1024)

static bool skill_name_is_valid(const char *name)
{
    if (!name || name[0] == '\0') {
        return false;
    }

    size_t len = strlen(name);
    if (len == 0 || len > SKILL_RUNTIME_MAX_NAME_CHARS) {
        return false;
    }

    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') ||
            (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') ||
            *p == '_' || *p == '-') {
            continue;
        }
        return false;
    }
    return true;
}

static bool skill_content_is_valid(const char *content)
{
    if (!content || content[0] == '\0') {
        return false;
    }

    size_t len = strlen(content);
    if (len == 0 || len > SKILL_RUNTIME_MAX_CONTENT_BYTES) {
        return false;
    }

    while (*content && isspace((unsigned char)*content)) {
        content++;
    }
    return content[0] == '#' && content[1] == ' ';
}

static esp_err_t build_skill_path(const char *name, char *path, size_t path_size)
{
    if (!skill_name_is_valid(name) || !path || path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int n = snprintf(path, path_size, "%s%s.md", ESPAGENT_SKILLS_PREFIX, name);
    if (n < 0 || (size_t)n >= path_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void extract_title_from_content(const char *content, char *title, size_t title_size)
{
    if (!title || title_size == 0) {
        return;
    }
    title[0] = '\0';
    if (!content) {
        return;
    }

    while (*content && isspace((unsigned char)*content)) {
        content++;
    }
    if (content[0] == '#' && content[1] == ' ') {
        content += 2;
    }

    size_t len = 0;
    while (content[len] && content[len] != '\n' && content[len] != '\r') {
        len++;
    }
    if (len >= title_size) {
        len = title_size - 1;
    }
    memcpy(title, content, len);
    title[len] = '\0';
}

static void extract_title_from_file(const char *path, char *title, size_t title_size)
{
    if (!title || title_size == 0) {
        return;
    }
    title[0] = '\0';

    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[160];
    if (fgets(line, sizeof(line), f)) {
        extract_title_from_content(line, title, title_size);
    }
    fclose(f);
}

esp_err_t skill_runtime_list_json(char *buf, size_t size)
{
    if (!buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    DIR *dir = opendir(ESPAGENT_SPIFFS_BASE);
    if (!dir) {
        snprintf(buf, size, "{\"ok\":false,\"skills\":[]}");
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (!root || !items) {
        closedir(dir);
        cJSON_Delete(root);
        cJSON_Delete(items);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "skills", items);

    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        size_t name_len = strlen(name);
        if (strncmp(name, "skills/", 7) != 0 || name_len <= 10 ||
            strcmp(name + name_len - 3, ".md") != 0) {
            continue;
        }

        char full_path[320];
        int n = snprintf(full_path, sizeof(full_path), "%s/%s", ESPAGENT_SPIFFS_BASE, name);
        if (n < 0 || (size_t)n >= sizeof(full_path)) {
            continue;
        }

        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }

        char skill_name[SKILL_RUNTIME_MAX_NAME_CHARS + 1];
        size_t copy_len = name_len - strlen("skills/") - strlen(".md");
        if (copy_len > sizeof(skill_name) - 1) {
            copy_len = sizeof(skill_name) - 1;
        }
        memcpy(skill_name, name + strlen("skills/"), copy_len);
        skill_name[copy_len] = '\0';

        char title[96];
        extract_title_from_file(full_path, title, sizeof(title));

        cJSON_AddStringToObject(item, "name", skill_name);
        cJSON_AddStringToObject(item, "path", full_path);
        cJSON_AddStringToObject(item, "title", title[0] ? title : skill_name);

        struct stat st;
        if (stat(full_path, &st) == 0) {
            cJSON_AddNumberToObject(item, "size_bytes", (double)st.st_size);
        }

        cJSON_AddItemToArray(items, item);
    }
    closedir(dir);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(buf, size, "%s", json);
    cJSON_free(json);
    return ESP_OK;
}

esp_err_t skill_runtime_get_json(const char *name, char **json_out)
{
    /* Keep content reads per-skill so the admin UI does not build one large SPIFFS JSON payload. */
    if (!json_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *json_out = NULL;
    if (!skill_name_is_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[160];
    esp_err_t err = build_skill_path(name, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size < 0 || st.st_size > SKILL_RUNTIME_MAX_CONTENT_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    char *content = calloc(1, (size_t)st.st_size + 1);
    if (!content) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(content, 1, (size_t)st.st_size, f);
    fclose(f);
    content[read] = '\0';

    char title[96];
    extract_title_from_content(content, title, sizeof(title));

    cJSON *root = cJSON_CreateObject();
    cJSON *skill = cJSON_CreateObject();
    if (!root || !skill) {
        cJSON_Delete(root);
        cJSON_Delete(skill);
        free(content);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "skill", skill);
    cJSON_AddStringToObject(skill, "name", name);
    cJSON_AddStringToObject(skill, "path", path);
    cJSON_AddStringToObject(skill, "title", title[0] ? title : name);
    cJSON_AddNumberToObject(skill, "size_bytes", (double)read);
    cJSON_AddStringToObject(skill, "content", content);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(content);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    *json_out = json;
    return ESP_OK;
}

esp_err_t skill_runtime_upsert(const char *name,
                               const char *content,
                               bool confirmed,
                               char *message,
                               size_t message_size)
{
    if (!confirmed) {
        if (message && message_size > 0) {
            snprintf(message, message_size, "skill update requires confirmed=true");
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (!skill_name_is_valid(name)) {
        if (message && message_size > 0) {
            snprintf(message, message_size,
                     "invalid skill name; use 1-%d chars of [A-Za-z0-9_-]",
                     SKILL_RUNTIME_MAX_NAME_CHARS);
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (!skill_content_is_valid(content)) {
        if (message && message_size > 0) {
            snprintf(message, message_size,
                     "invalid skill content; first non-empty line must start with '# ' and size <= %d bytes",
                     SKILL_RUNTIME_MAX_CONTENT_BYTES);
        }
        return ESP_ERR_INVALID_ARG;
    }

    char path[160];
    esp_err_t err = build_skill_path(name, path, sizeof(path));
    if (err != ESP_OK) {
        if (message && message_size > 0) {
            snprintf(message, message_size, "failed to build skill path for %s", name ? name : "(null)");
        }
        return err;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        if (message && message_size > 0) {
            snprintf(message, message_size, "cannot open %s for writing", path);
        }
        return ESP_FAIL;
    }

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);
    if (written != len) {
        if (message && message_size > 0) {
            snprintf(message, message_size, "short write to %s (%d/%d bytes)",
                     path, (int)written, (int)len);
        }
        return ESP_FAIL;
    }

    skill_loader_invalidate_cache();

    char title[96];
    extract_title_from_content(content, title, sizeof(title));
    if (message && message_size > 0) {
        snprintf(message, message_size, "OK: skill %s saved to %s (title=%s, %d bytes)",
                 name, path, title[0] ? title : name, (int)written);
    }
    ESP_LOGI(TAG, "Skill saved: %s (%d bytes)", path, (int)written);
    return ESP_OK;
}

esp_err_t skill_runtime_delete(const char *name,
                               bool confirmed,
                               char *message,
                               size_t message_size)
{
    if (!confirmed) {
        if (message && message_size > 0) {
            snprintf(message, message_size, "skill delete requires confirmed=true");
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (!skill_name_is_valid(name)) {
        if (message && message_size > 0) {
            snprintf(message, message_size,
                     "invalid skill name; use 1-%d chars of [A-Za-z0-9_-]",
                     SKILL_RUNTIME_MAX_NAME_CHARS);
        }
        return ESP_ERR_INVALID_ARG;
    }

    char path[160];
    esp_err_t err = build_skill_path(name, path, sizeof(path));
    if (err != ESP_OK) {
        if (message && message_size > 0) {
            snprintf(message, message_size, "failed to build skill path for %s", name ? name : "(null)");
        }
        return err;
    }
    if (remove(path) != 0) {
        if (message && message_size > 0) {
            snprintf(message, message_size, "skill not found: %s", path);
        }
        return ESP_ERR_NOT_FOUND;
    }

    skill_loader_invalidate_cache();
    if (message && message_size > 0) {
        snprintf(message, message_size, "OK: skill %s deleted from %s", name, path);
    }
    ESP_LOGI(TAG, "Skill deleted: %s", path);
    return ESP_OK;
}
