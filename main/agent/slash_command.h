#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    ESPAGENT_SLASH_NONE = 0,
    ESPAGENT_SLASH_REWRITE,
    ESPAGENT_SLASH_HELP,
    ESPAGENT_SLASH_ERROR,
} espagent_slash_result_type_t;

typedef struct {
    espagent_slash_result_type_t type;
    char text[1536];
    char command[32];
} espagent_slash_result_t;

bool espagent_slash_try_handle(const char *input, espagent_slash_result_t *result);

