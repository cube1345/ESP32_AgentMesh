#include "serial_cli.h"
#include "espagent_config.h"
#include "wifi/wifi_manager.h"
#include "channels/feishu/feishu_bot.h"
#include "llm/llm_proxy.h"
#include "cache/cache_store.h"
#include "bus/message_bus.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "proxy/http_proxy.h"
#include "tools/tool_registry.h"
#include "tools/tool_web_search.h"
#include "tools/tool_amap_weather.h"
#include "drivers/max98357.h"
#include "cron/cron_service.h"
#include "heartbeat/heartbeat.h"
#include "proactive/proactive_service.h"
#include "skills/skill_loader.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "cli";

/* --- wifi_set command --- */
static int cmd_wifi_set(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: set_wifi <ssid> <password>\n");
        printf("  SSID may contain spaces, the last argument is the password.\n");
        return 1;
    }

    /* SSID is argv[1..argc-2] joined by spaces, password is argv[argc-1] */
    char ssid[33] = {0};
    size_t pos = 0;
    for (int i = 1; i < argc - 1; i++) {
        if (pos > 0 && pos < sizeof(ssid) - 1) {
            ssid[pos++] = ' ';
        }
        size_t remain = sizeof(ssid) - pos - 1;
        size_t copy = strnlen(argv[i], remain);
        memcpy(ssid + pos, argv[i], copy);
        pos += copy;
        if (pos >= sizeof(ssid) - 1) break;
    }
    ssid[sizeof(ssid) - 1] = '\0';

    wifi_manager_set_credentials(ssid, argv[argc - 1]);
    printf("WiFi credentials saved to SSID: %s\n", ssid);
    printf("Restart to apply.\n");
    return 0;
}

/* --- wifi_status command --- */
static int cmd_wifi_status(int argc, char **argv)
{
    printf("WiFi connected: %s\n", wifi_manager_is_connected() ? "yes" : "no");
    printf("IP: %s\n", wifi_manager_get_ip());
    return 0;
}

/* --- set_feishu_creds command --- */
static struct {
    struct arg_str *app_id;
    struct arg_str *app_secret;
    struct arg_end *end;
} feishu_creds_args;

/* --- feishu_send command --- */
static struct {
    struct arg_str *receive_id;
    struct arg_str *text;
    struct arg_end *end;
} feishu_send_args;

static int cmd_set_feishu_creds(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&feishu_creds_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, feishu_creds_args.end, argv[0]);
        return 1;
    }
    feishu_set_credentials(feishu_creds_args.app_id->sval[0],
                          feishu_creds_args.app_secret->sval[0]);
    printf("Feishu credentials saved.\n");
    return 0;
}

static int cmd_feishu_send(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&feishu_send_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, feishu_send_args.end, argv[0]);
        return 1;
    }

    esp_err_t err = feishu_send_message(feishu_send_args.receive_id->sval[0],
                                        feishu_send_args.text->sval[0]);
    printf("feishu_send status: %s\n", esp_err_to_name(err));
    return (err == ESP_OK) ? 0 : 1;
}

/* --- set_api_key command --- */
static struct {
    struct arg_str *key;
    struct arg_end *end;
} api_key_args;

static int cmd_set_api_key(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&api_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, api_key_args.end, argv[0]);
        return 1;
    }
    llm_set_api_key(api_key_args.key->sval[0]);
    printf("API key saved.\n");
    return 0;
}

/* --- set_model command --- */
static struct {
    struct arg_str *model;
    struct arg_end *end;
} model_args;

static int cmd_set_model(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&model_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, model_args.end, argv[0]);
        return 1;
    }
    llm_set_model(model_args.model->sval[0]);
    printf("Model set.\n");
    return 0;
}

/* --- set_model_provider command --- */
static struct {
    struct arg_str *provider;
    struct arg_end *end;
} provider_args;

static int cmd_set_model_provider(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&provider_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, provider_args.end, argv[0]);
        return 1;
    }
    llm_set_provider(provider_args.provider->sval[0]);
    printf("Model provider set.\n");
    return 0;
}

/* --- memory_read command --- */
static int cmd_memory_read(int argc, char **argv)
{
    char *buf = malloc(4096);
    if (!buf) {
        printf("Out of memory.\n");
        return 1;
    }
    if (memory_read_long_term(buf, 4096) == ESP_OK && buf[0]) {
        printf("=== MEMORY.md ===\n%s\n=================\n", buf);
    } else {
        printf("MEMORY.md is empty or not found.\n");
    }
    free(buf);
    return 0;
}

/* --- memory_write command --- */
static struct {
    struct arg_str *content;
    struct arg_end *end;
} memory_write_args;

static int cmd_memory_write(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&memory_write_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, memory_write_args.end, argv[0]);
        return 1;
    }
    memory_write_long_term(memory_write_args.content->sval[0]);
    printf("MEMORY.md updated.\n");
    return 0;
}

/* --- session_list command --- */
static int cmd_session_list(int argc, char **argv)
{
    printf("Sessions:\n");
    session_list();
    return 0;
}

/* --- session_clear command --- */
static struct {
    struct arg_str *chat_id;
    struct arg_end *end;
} session_clear_args;

static int cmd_session_clear(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&session_clear_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, session_clear_args.end, argv[0]);
        return 1;
    }
    if (session_clear(session_clear_args.chat_id->sval[0]) == ESP_OK) {
        printf("Session cleared.\n");
    } else {
        printf("Session not found.\n");
    }
    return 0;
}

/* --- heap_info command --- */
static int cmd_heap_info(int argc, char **argv)
{
    printf("Internal free: %d bytes\n",
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    printf("PSRAM free:    %d bytes\n",
           (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("Total free:    %d bytes\n",
           (int)esp_get_free_heap_size());
    return 0;
}

/* --- max98357_test command --- */
static int cmd_max98357_test(int argc, char **argv)
{
    max98357_config_t cfg;
    max98357_default_config(&cfg);

    uint32_t frequency_hz = ESPAGENT_MAX98357_DEFAULT_TONE_HZ;
    uint32_t duration_ms = ESPAGENT_MAX98357_DEFAULT_DURATION_MS;
    uint8_t volume_pct = ESPAGENT_MAX98357_DEFAULT_VOLUME_PCT;

    if (argc == 4) {
        cfg.bclk_gpio = (gpio_num_t)strtol(argv[1], NULL, 10);
        cfg.ws_gpio = (gpio_num_t)strtol(argv[2], NULL, 10);
        cfg.din_gpio = (gpio_num_t)strtol(argv[3], NULL, 10);
    } else if (argc >= 7 && argc <= 10) {
        frequency_hz = (uint32_t)strtoul(argv[1], NULL, 10);
        duration_ms = (uint32_t)strtoul(argv[2], NULL, 10);
        volume_pct = (uint8_t)strtoul(argv[3], NULL, 10);
        cfg.bclk_gpio = (gpio_num_t)strtol(argv[4], NULL, 10);
        cfg.ws_gpio = (gpio_num_t)strtol(argv[5], NULL, 10);
        cfg.din_gpio = (gpio_num_t)strtol(argv[6], NULL, 10);
        if (argc >= 8) {
            cfg.sd_gpio = (gpio_num_t)strtol(argv[7], NULL, 10);
        }
        if (argc >= 9) {
            cfg.i2s_port = (int)strtol(argv[8], NULL, 10);
        }
        if (argc >= 10) {
            cfg.sample_rate_hz = (uint32_t)strtoul(argv[9], NULL, 10);
        }
    } else if (argc != 1) {
        printf("Usage:\n");
        printf("  max98357_test\n");
        printf("  max98357_test <bclk_gpio> <ws_gpio> <din_gpio>\n");
        printf("  max98357_test <freq_hz> <duration_ms> <volume_pct> <bclk_gpio> <ws_gpio> <din_gpio> [sd_gpio] [i2s_port] [sample_rate_hz]\n");
        return 1;
    }

    char result[256];
    esp_err_t err = max98357_play_tone(&cfg, frequency_hz, duration_ms, volume_pct,
                                       result, sizeof(result));
    printf("max98357_test status: %s\n", esp_err_to_name(err));
    printf("%s\n", result[0] ? result : "(empty)");
    return (err == ESP_OK) ? 0 : 1;
}

/* --- cache_stats command --- */
static int cmd_cache_stats(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    cache_stats_t stats;
    cache_stats(&stats);
    uint32_t total_lookups = stats.hits + stats.misses;
    uint32_t hit_rate_x100 = total_lookups ? (stats.hits * 10000U) / total_lookups : 0;

    printf("Cache entries:   %u/%u\n", (unsigned)stats.entries, (unsigned)ESPAGENT_CACHE_MAX_ENTRIES);
    printf("Cache bytes:     %u/%u\n", (unsigned)stats.bytes, (unsigned)ESPAGENT_CACHE_MAX_TOTAL_BYTES);
    printf("Cache hits:      %u\n", (unsigned)stats.hits);
    printf("Cache misses:    %u\n", (unsigned)stats.misses);
    printf("Cache hit rate:  %u.%02u%% (%u lookups)\n",
           (unsigned)(hit_rate_x100 / 100U),
           (unsigned)(hit_rate_x100 % 100U),
           (unsigned)total_lookups);
    printf("Cache evictions: %u\n", (unsigned)stats.evictions);
    printf("Cache expired:   %u\n", (unsigned)stats.expired);
    printf("Cache truncated: %u\n", (unsigned)stats.truncated);
    return 0;
}

/* --- cache_dump command --- */
static int cmd_cache_dump(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    char *buf = calloc(1, 2048);
    if (!buf) {
        printf("Out of memory.\n");
        return 1;
    }

    cache_dump(buf, 2048);
    printf("=== Cache Entries ===\n%s", buf);
    free(buf);
    return 0;
}

/* --- cache_clear command --- */
static int cmd_cache_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    cache_clear();
    printf("Cache cleared.\n");
    return 0;
}

/* --- set_proxy command --- */
static struct {
    struct arg_str *host;
    struct arg_int *port;
    struct arg_str *type;
    struct arg_end *end;
} proxy_args;

static int cmd_set_proxy(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&proxy_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, proxy_args.end, argv[0]);
        return 1;
    }
    const char *proxy_type = "http";
    if (proxy_args.type->count > 0 && proxy_args.type->sval[0] && proxy_args.type->sval[0][0]) {
        proxy_type = proxy_args.type->sval[0];
    }
    if (strcmp(proxy_type, "http") != 0 && strcmp(proxy_type, "socks5") != 0) {
        printf("Invalid proxy type: %s. Use http or socks5.\n", proxy_type);
        return 1;
    }

    http_proxy_set(proxy_args.host->sval[0], (uint16_t)proxy_args.port->ival[0], proxy_type);
    printf("Proxy set. Restart to apply.\n");
    return 0;
}

/* --- clear_proxy command --- */
static int cmd_clear_proxy(int argc, char **argv)
{
    http_proxy_clear();
    printf("Proxy cleared. Restart to apply.\n");
    return 0;
}

/* --- set_search_key command --- */
static struct {
    struct arg_str *key;
    struct arg_end *end;
} search_key_args;

static int cmd_set_search_key(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&search_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, search_key_args.end, argv[0]);
        return 1;
    }
    tool_web_search_set_key(search_key_args.key->sval[0]);
    printf("Search API key saved.\n");
    return 0;
}

/* --- set_tavily_key command --- */
static struct {
    struct arg_str *key;
    struct arg_end *end;
} tavily_key_args;

static int cmd_set_tavily_key(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&tavily_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, tavily_key_args.end, argv[0]);
        return 1;
    }
    tool_web_search_set_tavily_key(tavily_key_args.key->sval[0]);
    printf("Tavily API key saved.\n");
    return 0;
}

/* --- set_amap_key command --- */
static struct {
    struct arg_str *key;
    struct arg_end *end;
} amap_key_args;

static int cmd_set_amap_key(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&amap_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, amap_key_args.end, argv[0]);
        return 1;
    }
    tool_amap_weather_set_key(amap_key_args.key->sval[0]);
    printf("Amap API key saved.\n");
    return 0;
}

/* --- set_amap_location command --- */
static struct {
    struct arg_str *location;
    struct arg_str *adcode;
    struct arg_end *end;
} amap_location_args;

static int cmd_set_amap_location(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&amap_location_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, amap_location_args.end, argv[0]);
        return 1;
    }
    esp_err_t err = tool_amap_weather_set_default_location(
        amap_location_args.location->sval[0], amap_location_args.adcode->sval[0]);
    if (err != ESP_OK) {
        printf("Failed to save Amap default location: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Amap default location saved.\n");
    return 0;
}

/* --- wifi_scan command --- */
static int cmd_wifi_scan(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    wifi_manager_scan_and_print();
    return 0;
}

/* --- skill_list command --- */
static int cmd_skill_list(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    char *buf = malloc(4096);
    if (!buf) {
        printf("Out of memory.\n");
        return 1;
    }

    size_t n = skill_loader_build_summary(buf, 4096);
    if (n == 0) {
        printf("No skills found under " ESPAGENT_SKILLS_PREFIX ".\n");
    } else {
        printf("=== Skills ===\n%s", buf);
    }
    free(buf);
    return 0;
}

/* --- skill_show command --- */
static struct {
    struct arg_str *name;
    struct arg_end *end;
} skill_show_args;

static bool has_md_suffix(const char *name)
{
    size_t len = strlen(name);
    return (len >= 3) && strcmp(name + len - 3, ".md") == 0;
}

static bool build_skill_path(const char *name, char *out, size_t out_size)
{
    if (!name || !name[0]) return false;
    if (strstr(name, "..") != NULL) return false;
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) return false;

    if (has_md_suffix(name)) {
        snprintf(out, out_size, ESPAGENT_SKILLS_PREFIX "%s", name);
    } else {
        snprintf(out, out_size, ESPAGENT_SKILLS_PREFIX "%s.md", name);
    }
    return true;
}

static int cmd_skill_show(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&skill_show_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, skill_show_args.end, argv[0]);
        return 1;
    }

    char path[128];
    if (!build_skill_path(skill_show_args.name->sval[0], path, sizeof(path))) {
        printf("Invalid skill name.\n");
        return 1;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        printf("Skill not found: %s\n", path);
        return 1;
    }

    printf("=== %s ===\n", path);
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        fputs(line, stdout);
    }
    fclose(f);
    printf("\n============\n");
    return 0;
}

/* --- skill_search command --- */
static struct {
    struct arg_str *keyword;
    struct arg_end *end;
} skill_search_args;

static bool contains_nocase(const char *text, const char *keyword)
{
    if (!text || !keyword || !keyword[0]) return false;

    size_t key_len = strlen(keyword);
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (i < key_len && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)keyword[i])) {
            i++;
        }
        if (i == key_len) return true;
    }
    return false;
}

static int cmd_skill_search(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&skill_search_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, skill_search_args.end, argv[0]);
        return 1;
    }

    const char *keyword = skill_search_args.keyword->sval[0];
    DIR *dir = opendir(ESPAGENT_SPIFFS_BASE);
    if (!dir) {
        printf("Cannot open " ESPAGENT_SPIFFS_BASE ".\n");
        return 1;
    }

    const char *prefix = "skills/";
    const size_t prefix_len = strlen(prefix);
    int matches = 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        size_t name_len = strlen(name);

        if (strncmp(name, prefix, prefix_len) != 0) continue;
        if (name_len < prefix_len + 4) continue;
        if (strcmp(name + name_len - 3, ".md") != 0) continue;

        char full_path[296];
        snprintf(full_path, sizeof(full_path), ESPAGENT_SPIFFS_BASE "/%s", name);

        bool file_matched = contains_nocase(name, keyword);
        int matched_line = 0;

        FILE *f = fopen(full_path, "r");
        if (!f) continue;

        char line[256];
        int line_no = 0;
        while (!file_matched && fgets(line, sizeof(line), f)) {
            line_no++;
            if (contains_nocase(line, keyword)) {
                file_matched = true;
                matched_line = line_no;
            }
        }
        fclose(f);

        if (file_matched) {
            matches++;
            if (matched_line > 0) {
                printf("- %s (matched at line %d)\n", full_path, matched_line);
            } else {
                printf("- %s (matched in filename)\n", full_path);
            }
        }
    }

    closedir(dir);
    if (matches == 0) {
        printf("No skills matched keyword: %s\n", keyword);
    } else {
        printf("Total matches: %d\n", matches);
    }
    return 0;
}

/* --- config_show command --- */
static void print_config(const char *label, const char *ns, const char *key,
                         const char *build_val, bool mask)
{
    char nvs_val[128] = {0};
    const char *source = "not set";
    const char *display = "(empty)";

    /* NVS takes highest priority */
    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(nvs_val);
        if (nvs_get_str(nvs, key, nvs_val, &len) == ESP_OK && nvs_val[0]) {
            source = "NVS";
            display = nvs_val;
        }
        nvs_close(nvs);
    }

    /* Fall back to build-time value */
    if (strcmp(source, "not set") == 0 && build_val[0] != '\0') {
        source = "build";
        display = build_val;
    }

    if (mask && strlen(display) > 6 && strcmp(display, "(empty)") != 0) {
        printf("  %-14s: %.4s****  [%s]\n", label, display, source);
    } else {
        printf("  %-14s: %s  [%s]\n", label, display, source);
    }
}

static void print_config_u16(const char *label, const char *ns, const char *key,
                             const char *build_val)
{
    char nvs_val[16] = {0};
    const char *source = "not set";
    const char *display = "(empty)";

    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        uint16_t value = 0;
        if (nvs_get_u16(nvs, key, &value) == ESP_OK && value > 0) {
            snprintf(nvs_val, sizeof(nvs_val), "%u", (unsigned)value);
            source = "NVS";
            display = nvs_val;
        }
        nvs_close(nvs);
    }

    if (strcmp(source, "not set") == 0 && build_val[0] != '\0') {
        source = "build";
        display = build_val;
    }

    printf("  %-14s: %s  [%s]\n", label, display, source);
}

static int cmd_config_show(int argc, char **argv)
{
    printf("=== Current Configuration ===\n");
    print_config("WiFi SSID",  ESPAGENT_NVS_WIFI,   ESPAGENT_NVS_KEY_SSID,     ESPAGENT_SECRET_WIFI_SSID,  false);
    print_config("WiFi Pass",  ESPAGENT_NVS_WIFI,   ESPAGENT_NVS_KEY_PASS,     ESPAGENT_SECRET_WIFI_PASS,  true);
    print_config("API Key",    ESPAGENT_NVS_LLM,    ESPAGENT_NVS_KEY_API_KEY,  ESPAGENT_SECRET_API_KEY,    true);
    print_config("Model",      ESPAGENT_NVS_LLM,    ESPAGENT_NVS_KEY_MODEL,    ESPAGENT_SECRET_MODEL,      false);
    print_config("Provider",   ESPAGENT_NVS_LLM,    ESPAGENT_NVS_KEY_PROVIDER, ESPAGENT_SECRET_MODEL_PROVIDER, false);
    print_config("Proxy Host", ESPAGENT_NVS_PROXY,  ESPAGENT_NVS_KEY_PROXY_HOST, ESPAGENT_SECRET_PROXY_HOST, false);
    print_config_u16("Proxy Port", ESPAGENT_NVS_PROXY, ESPAGENT_NVS_KEY_PROXY_PORT, ESPAGENT_SECRET_PROXY_PORT);
    print_config("Search Key", ESPAGENT_NVS_SEARCH, ESPAGENT_NVS_KEY_API_KEY,  ESPAGENT_SECRET_SEARCH_KEY, true);
    print_config("Tavily Key", ESPAGENT_NVS_SEARCH, ESPAGENT_NVS_KEY_TAVILY_KEY, ESPAGENT_SECRET_TAVILY_KEY, true);
    print_config("Amap Key", ESPAGENT_NVS_AMAP, ESPAGENT_NVS_KEY_API_KEY, ESPAGENT_SECRET_AMAP_KEY, true);
    print_config("Amap Loc", ESPAGENT_NVS_AMAP, ESPAGENT_NVS_KEY_LOCATION, ESPAGENT_AMAP_DEFAULT_LOCATION, false);
    print_config("Amap Code", ESPAGENT_NVS_AMAP, ESPAGENT_NVS_KEY_ADCODE, ESPAGENT_AMAP_DEFAULT_ADCODE, false);
    print_config("Node ID", ESPAGENT_NVS_NODE, ESPAGENT_NVS_KEY_NODE_ID, ESPAGENT_NODE_ID, false);
    print_config("Node Role", ESPAGENT_NVS_NODE, ESPAGENT_NVS_KEY_NODE_ROLE, ESPAGENT_NODE_ROLE, false);
    print_config("Node Loc", ESPAGENT_NVS_NODE, ESPAGENT_NVS_KEY_NODE_LOCATION, ESPAGENT_NODE_LOCATION, false);
    print_config("Node Caps", ESPAGENT_NVS_NODE, ESPAGENT_NVS_KEY_NODE_CAPABILITIES, ESPAGENT_NODE_CAPABILITIES, false);
    print_config("Node Duties", ESPAGENT_NVS_NODE, ESPAGENT_NVS_KEY_NODE_RESPONSIBILITIES, ESPAGENT_NODE_RESPONSIBILITIES, false);
    printf("  %-14s: %s\n", "MQTT State", ESPAGENT_SENSOR_MQTT_TOPIC_STATE);
    printf("  %-14s: %s\n", "MQTT Telemetry", ESPAGENT_SENSOR_MQTT_TOPIC_TELEMETRY);
    printf("  %-14s: %s\n", "MQTT Command", ESPAGENT_SENSOR_MQTT_TOPIC_COMMAND);
    printf("  %-14s: %s\n", "MQTT Role Cmd", ESPAGENT_MESH_TOPIC_ROLE_COMMAND);
    print_config("Proactive Ch", ESPAGENT_NVS_PROACTIVE, ESPAGENT_NVS_KEY_CHANNEL, "", false);
    print_config("Proactive ID", ESPAGENT_NVS_PROACTIVE, ESPAGENT_NVS_KEY_CHAT_ID, "", false);
    printf("=============================\n");
    return 0;
}

/* --- config_reset command --- */
static int cmd_config_reset(int argc, char **argv)
{
    const char *namespaces[] = {
        ESPAGENT_NVS_WIFI, ESPAGENT_NVS_LLM, ESPAGENT_NVS_PROXY,
        ESPAGENT_NVS_SEARCH, ESPAGENT_NVS_PROACTIVE, ESPAGENT_NVS_AMAP,
        ESPAGENT_NVS_NODE
    };
    for (size_t i = 0; i < sizeof(namespaces) / sizeof(namespaces[0]); i++) {
        nvs_handle_t nvs;
        if (nvs_open(namespaces[i], NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_erase_all(nvs);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }
    printf("All NVS config cleared. Build-time defaults will be used on restart.\n");
    return 0;
}

/* --- heartbeat_trigger command --- */
static int cmd_heartbeat_trigger(int argc, char **argv)
{
    printf("Checking HEARTBEAT.md...\n");
    if (heartbeat_trigger()) {
        printf("Heartbeat: agent prompted with pending tasks.\n");
    } else {
        printf("Heartbeat: no actionable tasks found.\n");
    }
    return 0;
}

/* --- cron_start command --- */
static int cmd_cron_start(int argc, char **argv)
{
    esp_err_t err = cron_service_start();
    if (err == ESP_OK) {
        printf("Cron service started.\n");
        return 0;
    }

    printf("Failed to start cron service: %s\n", esp_err_to_name(err));
    return 1;
}

static int cmd_proactive_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    char status[192] = {0};
    proactive_service_status(status, sizeof(status));
    printf("Proactive: %s\n", status);
    return 0;
}

static int cmd_proactive_set_target(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: proactive_set_target <channel> <chat_id>\n");
        return 1;
    }

    esp_err_t err = proactive_service_note_contact(argv[1], argv[2]);
    printf("proactive_set_target status: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_proactive_trigger(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    esp_err_t err = proactive_service_trigger_now();
    printf("proactive_trigger status: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_tool_exec(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: tool_exec <name> [json]\n");
        return 1;
    }

    const char *tool_name = argv[1];
    const char *input_json = (argc >= 3) ? argv[2] : "{}";

    char *output = calloc(1, 4096);
    if (!output) {
        printf("Out of memory.\n");
        return 1;
    }

    esp_err_t err = tool_registry_execute(tool_name, input_json, output, 4096);
    printf("tool_exec status: %s\n", esp_err_to_name(err));
    printf("%s\n", output[0] ? output : "(empty)");
    free(output);
    return (err == ESP_OK) ? 0 : 1;
}

static int cmd_inject_msg(int argc, char **argv)
{
    if (argc < 4) {
        printf("Usage: inject_msg <channel> <chat_id> <text>\n");
        return 1;
    }

    espagent_msg_t msg = {0};
    strncpy(msg.channel, argv[1], sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, argv[2], sizeof(msg.chat_id) - 1);

    size_t text_len = 0;
    for (int i = 3; i < argc; i++) {
        text_len += strlen(argv[i]) + 1;
    }

    msg.content = calloc(1, text_len + 1);
    if (!msg.content) {
        printf("Out of memory.\n");
        return 1;
    }

    for (int i = 3; i < argc; i++) {
        if (i > 3) {
            strncat(msg.content, " ", text_len - strlen(msg.content));
        }
        strncat(msg.content, argv[i], text_len - strlen(msg.content));
    }

    esp_err_t err = message_bus_push_inbound(&msg);
    if (err != ESP_OK) {
        free(msg.content);
        printf("inject_msg status: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("inject_msg status: ESP_OK (%s:%s)\n", msg.channel, msg.chat_id);
    return 0;
}

static int cmd_inject_search_zh(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    espagent_msg_t msg = {0};
    strncpy(msg.channel, "system", sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, "debug", sizeof(msg.chat_id) - 1);
    msg.content = strdup("请联网搜索 ESP-IDF 最新版本 并总结一句话");
    if (!msg.content) {
        printf("Out of memory.\n");
        return 1;
    }

    esp_err_t err = message_bus_push_inbound(&msg);
    if (err != ESP_OK) {
        free(msg.content);
        printf("inject_search_zh status: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("inject_search_zh status: ESP_OK (system:debug)\n");
    return 0;
}

/* --- web_search command --- */
static struct {
    struct arg_str *query;
    struct arg_end *end;
} web_search_args;

typedef struct {
    const char *input_json;
    char *output;
    size_t output_size;
    esp_err_t err;
    SemaphoreHandle_t done;
} web_search_task_ctx_t;

static void web_search_task(void *arg)
{
    web_search_task_ctx_t *task_ctx = (web_search_task_ctx_t *)arg;
    task_ctx->err = tool_web_search_execute(task_ctx->input_json, task_ctx->output, task_ctx->output_size);
    xSemaphoreGive(task_ctx->done);
    vTaskDelete(NULL);
}

static bool json_escape_string(const char *in, char *out, size_t out_size)
{
    if (!in || !out || out_size == 0) return false;
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0'; ++i) {
        const char c = in[i];
        const char *esc = NULL;
        switch (c) {
            case '\\': esc = "\\\\"; break;
            case '\"': esc = "\\\""; break;
            case '\n': esc = "\\n"; break;
            case '\r': esc = "\\r"; break;
            case '\t': esc = "\\t"; break;
            default: break;
        }
        if (esc) {
            size_t n = strlen(esc);
            if (o + n >= out_size) return false;
            memcpy(&out[o], esc, n);
            o += n;
            continue;
        }
        if ((unsigned char)c < 0x20) {
            continue;
        }
        if (o + 1 >= out_size) return false;
        out[o++] = c;
    }
    out[o] = '\0';
    return true;
}

static int cmd_web_search(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&web_search_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, web_search_args.end, argv[0]);
        return 1;
    }

    char escaped_query[512];
    if (!json_escape_string(web_search_args.query->sval[0], escaped_query, sizeof(escaped_query))) {
        printf("Query too long.\n");
        return 1;
    }

    char input_json[640];
    int n = snprintf(input_json, sizeof(input_json), "{\"query\":\"%s\"}", escaped_query);
    if (n <= 0 || n >= (int)sizeof(input_json)) {
        printf("Query too long.\n");
        return 1;
    }

    char *output = calloc(1, 4096);
    if (!output) {
        printf("Out of memory.\n");
        return 1;
    }

    web_search_task_ctx_t *ctx = calloc(1, sizeof(*ctx));
    char *input_copy = strdup(input_json);
    if (!ctx || !input_copy) {
        free(input_copy);
        free(ctx);
        free(output);
        printf("Out of memory.\n");
        return 1;
    }

    ctx->input_json = input_copy;
    ctx->output = output;
    ctx->output_size = 4096;
    ctx->done = xSemaphoreCreateBinary();
    if (!ctx->done) {
        free(input_copy);
        free(ctx);
        free(output);
        printf("Out of memory.\n");
        return 1;
    }

    if (xTaskCreate(web_search_task, "cli_web_search", 20 * 1024, ctx, 5, NULL) != pdPASS) {
        vSemaphoreDelete(ctx->done);
        free(input_copy);
        free(ctx);
        free(output);
        printf("Failed to start web_search task.\n");
        return 1;
    }

    if (xSemaphoreTake(ctx->done, pdMS_TO_TICKS(45000)) != pdTRUE) {
        printf("web_search status: timeout\n");
        vSemaphoreDelete(ctx->done);
        free(input_copy);
        free(ctx);
        free(output);
        return 1;
    }
    esp_err_t err = ctx->err;
    vSemaphoreDelete(ctx->done);
    free(input_copy);
    free(ctx);

    printf("web_search status: %s\n", esp_err_to_name(err));
    printf("%s\n", output[0] ? output : "(empty)");
    free(output);
    return (err == ESP_OK) ? 0 : 1;
}

/* --- restart command --- */
static int cmd_restart(int argc, char **argv)
{
    printf("Restarting...\n");
    esp_restart();
    return 0;  /* unreachable */
}

esp_err_t serial_cli_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "ESPAgent> ";
    repl_config.max_cmdline_length = 256;

#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl));
#else
    ESP_LOGE(TAG, "No supported console backend is enabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif

    /* Register commands */
    esp_console_register_help_command();

    /* set_wifi */
    esp_console_cmd_t wifi_set_cmd = {
        .command = "set_wifi",
        .help = "Set WiFi SSID and password. SSID may contain spaces, last argument is password.",
        .func = &cmd_wifi_set,
    };
    esp_console_cmd_register(&wifi_set_cmd);

    /* wifi_status */
    esp_console_cmd_t wifi_status_cmd = {
        .command = "wifi_status",
        .help = "Show WiFi connection status",
        .func = &cmd_wifi_status,
    };
    esp_console_cmd_register(&wifi_status_cmd);

    /* wifi_scan */
    esp_console_cmd_t wifi_scan_cmd = {
        .command = "wifi_scan",
        .help = "Scan and list nearby WiFi APs",
        .func = &cmd_wifi_scan,
    };
    esp_console_cmd_register(&wifi_scan_cmd);

    /* set_feishu_creds */
    feishu_creds_args.app_id = arg_str1(NULL, NULL, "<app_id>", "Feishu App ID");
    feishu_creds_args.app_secret = arg_str1(NULL, NULL, "<app_secret>", "Feishu App Secret");
    feishu_creds_args.end = arg_end(2);
    esp_console_cmd_t feishu_creds_cmd = {
        .command = "set_feishu_creds",
        .help = "Set Feishu app credentials (app_id app_secret)",
        .func = &cmd_set_feishu_creds,
        .argtable = &feishu_creds_args,
    };
    esp_console_cmd_register(&feishu_creds_cmd);

    /* feishu_send */
    feishu_send_args.receive_id = arg_str1(NULL, NULL, "<receive_id>", "Feishu open_id/chat_id");
    feishu_send_args.text = arg_str1(NULL, NULL, "<text>", "Text message (quote if contains spaces)");
    feishu_send_args.end = arg_end(2);
    esp_console_cmd_t feishu_send_cmd = {
        .command = "feishu_send",
        .help = "Send Feishu text: feishu_send <open_id|chat_id> \"hello\"",
        .func = &cmd_feishu_send,
        .argtable = &feishu_send_args,
    };
    esp_console_cmd_register(&feishu_send_cmd);

    /* set_api_key */
    api_key_args.key = arg_str1(NULL, NULL, "<key>", "LLM API key");
    api_key_args.end = arg_end(1);
    esp_console_cmd_t api_key_cmd = {
        .command = "set_api_key",
        .help = "Set LLM API key",
        .func = &cmd_set_api_key,
        .argtable = &api_key_args,
    };
    esp_console_cmd_register(&api_key_cmd);

    /* set_model */
    model_args.model = arg_str1(NULL, NULL, "<model>", "Model identifier");
    model_args.end = arg_end(1);
    esp_console_cmd_t model_cmd = {
        .command = "set_model",
        .help = "Set LLM model (default: " ESPAGENT_LLM_DEFAULT_MODEL ")",
        .func = &cmd_set_model,
        .argtable = &model_args,
    };
    esp_console_cmd_register(&model_cmd);

    /* set_model_provider */
    provider_args.provider = arg_str1(NULL, NULL, "<provider>", "Model provider (anthropic|openai)");
    provider_args.end = arg_end(1);
    esp_console_cmd_t provider_cmd = {
        .command = "set_model_provider",
        .help = "Set LLM model provider (default: " ESPAGENT_LLM_PROVIDER_DEFAULT ")",
        .func = &cmd_set_model_provider,
        .argtable = &provider_args,
    };
    esp_console_cmd_register(&provider_cmd);

    /* skill_list */
    esp_console_cmd_t skill_list_cmd = {
        .command = "skill_list",
        .help = "List installed skills from " ESPAGENT_SKILLS_PREFIX,
        .func = &cmd_skill_list,
    };
    esp_console_cmd_register(&skill_list_cmd);

    /* skill_show */
    skill_show_args.name = arg_str1(NULL, NULL, "<name>", "Skill name (e.g. weather or weather.md)");
    skill_show_args.end = arg_end(1);
    esp_console_cmd_t skill_show_cmd = {
        .command = "skill_show",
        .help = "Print full content of one skill file",
        .func = &cmd_skill_show,
        .argtable = &skill_show_args,
    };
    esp_console_cmd_register(&skill_show_cmd);

    /* skill_search */
    skill_search_args.keyword = arg_str1(NULL, NULL, "<keyword>", "Keyword to search in skills");
    skill_search_args.end = arg_end(1);
    esp_console_cmd_t skill_search_cmd = {
        .command = "skill_search",
        .help = "Search skill files by keyword (filename + content)",
        .func = &cmd_skill_search,
        .argtable = &skill_search_args,
    };
    esp_console_cmd_register(&skill_search_cmd);

    /* memory_read */
    esp_console_cmd_t mem_read_cmd = {
        .command = "memory_read",
        .help = "Read MEMORY.md",
        .func = &cmd_memory_read,
    };
    esp_console_cmd_register(&mem_read_cmd);

    /* memory_write */
    memory_write_args.content = arg_str1(NULL, NULL, "<content>", "Content to write");
    memory_write_args.end = arg_end(1);
    esp_console_cmd_t mem_write_cmd = {
        .command = "memory_write",
        .help = "Write to MEMORY.md",
        .func = &cmd_memory_write,
        .argtable = &memory_write_args,
    };
    esp_console_cmd_register(&mem_write_cmd);

    /* session_list */
    esp_console_cmd_t sess_list_cmd = {
        .command = "session_list",
        .help = "List all sessions",
        .func = &cmd_session_list,
    };
    esp_console_cmd_register(&sess_list_cmd);

    /* session_clear */
    session_clear_args.chat_id = arg_str1(NULL, NULL, "<chat_id>", "Chat ID to clear");
    session_clear_args.end = arg_end(1);
    esp_console_cmd_t sess_clear_cmd = {
        .command = "session_clear",
        .help = "Clear a session",
        .func = &cmd_session_clear,
        .argtable = &session_clear_args,
    };
    esp_console_cmd_register(&sess_clear_cmd);

    /* heap_info */
    esp_console_cmd_t heap_cmd = {
        .command = "heap_info",
        .help = "Show heap memory usage",
        .func = &cmd_heap_info,
    };
    esp_console_cmd_register(&heap_cmd);

    /* max98357_test */
    esp_console_cmd_t max98357_cmd = {
        .command = "max98357_test",
        .help = "Play a short test tone through the MAX98357 audio amplifier",
        .func = &cmd_max98357_test,
    };
    esp_console_cmd_register(&max98357_cmd);

    /* cache_stats */
    esp_console_cmd_t cache_stats_cmd = {
        .command = "cache_stats",
        .help = "Show agent RAM KV cache usage and hit/miss counters",
        .func = &cmd_cache_stats,
    };
    esp_console_cmd_register(&cache_stats_cmd);

    /* cache_dump */
    esp_console_cmd_t cache_dump_cmd = {
        .command = "cache_dump",
        .help = "List agent RAM KV cache entries with TTL and hit counters",
        .func = &cmd_cache_dump,
    };
    esp_console_cmd_register(&cache_dump_cmd);

    /* cache_clear */
    esp_console_cmd_t cache_clear_cmd = {
        .command = "cache_clear",
        .help = "Clear agent RAM KV cache and counters",
        .func = &cmd_cache_clear,
    };
    esp_console_cmd_register(&cache_clear_cmd);

    /* set_search_key */
    search_key_args.key = arg_str1(NULL, NULL, "<key>", "Brave Search API key");
    search_key_args.end = arg_end(1);
    esp_console_cmd_t search_key_cmd = {
        .command = "set_search_key",
        .help = "Set Brave Search API key for web_search tool",
        .func = &cmd_set_search_key,
        .argtable = &search_key_args,
    };
    esp_console_cmd_register(&search_key_cmd);

    /* set_tavily_key */
    tavily_key_args.key = arg_str1(NULL, NULL, "<key>", "Tavily Search API key");
    tavily_key_args.end = arg_end(1);
    esp_console_cmd_t tavily_key_cmd = {
        .command = "set_tavily_key",
        .help = "Set Tavily API key for web_search tool",
        .func = &cmd_set_tavily_key,
        .argtable = &tavily_key_args,
    };
    esp_console_cmd_register(&tavily_key_cmd);

    /* set_amap_key */
    amap_key_args.key = arg_str1(NULL, NULL, "<key>", "Amap WebService API key");
    amap_key_args.end = arg_end(1);
    esp_console_cmd_t amap_key_cmd = {
        .command = "set_amap_key",
        .help = "Set Amap WebService API key for get_weather tool",
        .func = &cmd_set_amap_key,
        .argtable = &amap_key_args,
    };
    esp_console_cmd_register(&amap_key_cmd);

    /* set_amap_location */
    amap_location_args.location = arg_str1(NULL, NULL, "<location>", "Default location label, e.g. 南京市栖霞区");
    amap_location_args.adcode = arg_str1(NULL, NULL, "<adcode>", "Default 6-digit Amap adcode");
    amap_location_args.end = arg_end(2);
    esp_console_cmd_t amap_location_cmd = {
        .command = "set_amap_location",
        .help = "Set default Amap weather location (e.g. set_amap_location 南京市栖霞区 320113)",
        .func = &cmd_set_amap_location,
        .argtable = &amap_location_args,
    };
    esp_console_cmd_register(&amap_location_cmd);

    /* set_proxy */
    proxy_args.host = arg_str1(NULL, NULL, "<host>", "Proxy host/IP");
    proxy_args.port = arg_int1(NULL, NULL, "<port>", "Proxy port");
    proxy_args.type = arg_str0(NULL, NULL, "<type>", "Proxy type: http|socks5 (default: http)");
    proxy_args.end = arg_end(3);
    esp_console_cmd_t proxy_cmd = {
        .command = "set_proxy",
        .help = "Set proxy (e.g. set_proxy 192.168.1.83 7897 [http|socks5])",
        .func = &cmd_set_proxy,
        .argtable = &proxy_args,
    };
    esp_console_cmd_register(&proxy_cmd);

    /* clear_proxy */
    esp_console_cmd_t clear_proxy_cmd = {
        .command = "clear_proxy",
        .help = "Remove proxy configuration",
        .func = &cmd_clear_proxy,
    };
    esp_console_cmd_register(&clear_proxy_cmd);

    /* config_show */
    esp_console_cmd_t config_show_cmd = {
        .command = "config_show",
        .help = "Show current configuration (build-time + NVS)",
        .func = &cmd_config_show,
    };
    esp_console_cmd_register(&config_show_cmd);

    /* config_reset */
    esp_console_cmd_t config_reset_cmd = {
        .command = "config_reset",
        .help = "Clear all NVS overrides, revert to build-time defaults",
        .func = &cmd_config_reset,
    };
    esp_console_cmd_register(&config_reset_cmd);

    /* heartbeat_trigger */
    esp_console_cmd_t heartbeat_cmd = {
        .command = "heartbeat_trigger",
        .help = "Manually trigger a heartbeat check",
        .func = &cmd_heartbeat_trigger,
    };
    esp_console_cmd_register(&heartbeat_cmd);

    /* cron_start */
    esp_console_cmd_t cron_start_cmd = {
        .command = "cron_start",
        .help = "Start cron scheduler timer now",
        .func = &cmd_cron_start,
    };
    esp_console_cmd_register(&cron_start_cmd);

    /* proactive_status */
    esp_console_cmd_t proactive_status_cmd = {
        .command = "proactive_status",
        .help = "Show proactive agent target and interval",
        .func = &cmd_proactive_status,
    };
    esp_console_cmd_register(&proactive_status_cmd);

    /* proactive_set_target */
    esp_console_cmd_t proactive_set_target_cmd = {
        .command = "proactive_set_target",
        .help = "Set proactive target: proactive_set_target <channel> <chat_id>",
        .func = &cmd_proactive_set_target,
    };
    esp_console_cmd_register(&proactive_set_target_cmd);

    /* proactive_trigger */
    esp_console_cmd_t proactive_trigger_cmd = {
        .command = "proactive_trigger",
        .help = "Trigger a proactive LLM check now",
        .func = &cmd_proactive_trigger,
    };
    esp_console_cmd_register(&proactive_trigger_cmd);

    /* tool_exec */
    esp_console_cmd_t tool_exec_cmd = {
        .command = "tool_exec",
        .help = "Execute a registered tool: tool_exec <name> '{...json...}'",
        .func = &cmd_tool_exec,
    };
    esp_console_cmd_register(&tool_exec_cmd);

    /* inject_msg */
    esp_console_cmd_t inject_msg_cmd = {
        .command = "inject_msg",
        .help = "Inject a message into agent loop: inject_msg <channel> <chat_id> <text>",
        .func = &cmd_inject_msg,
    };
    esp_console_cmd_register(&inject_msg_cmd);

    /* inject_search_zh */
    esp_console_cmd_t inject_search_zh_cmd = {
        .command = "inject_search_zh",
        .help = "Inject a fixed Chinese web-search request into agent loop",
        .func = &cmd_inject_search_zh,
    };
    esp_console_cmd_register(&inject_search_zh_cmd);

    /* web_search */
    web_search_args.query = arg_str1(NULL, NULL, "<query>", "Search query");
    web_search_args.end = arg_end(1);
    esp_console_cmd_t web_search_cmd = {
        .command = "web_search",
        .help = "Run web search tool directly (e.g. web_search \"latest esp-idf\")",
        .func = &cmd_web_search,
        .argtable = &web_search_args,
    };
    esp_console_cmd_register(&web_search_cmd);

    /* restart */
    esp_console_cmd_t restart_cmd = {
        .command = "restart",
        .help = "Restart the device",
        .func = &cmd_restart,
    };
    esp_console_cmd_register(&restart_cmd);

    /* Start REPL */
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Serial CLI started");

    return ESP_OK;
}
