#include "tools/tool_automation.h"

#include "automation/automation_engine.h"

#include "cJSON.h"

#include <stdio.h>

esp_err_t tool_automation_create_workflow_execute(const char *input_json,
                                                  char *output,
                                                  size_t output_size)
{
    return automation_engine_create_workflow(input_json, output, output_size);
}

esp_err_t tool_automation_create_rule_execute(const char *input_json,
                                             char *output,
                                             size_t output_size)
{
    return automation_engine_create_rule(input_json, output, output_size);
}

esp_err_t tool_automation_list_execute(const char *input_json,
                                      char *output,
                                      size_t output_size)
{
    (void)input_json;
    return automation_engine_list(output, output_size);
}

esp_err_t tool_automation_remove_execute(const char *input_json,
                                         char *output,
                                         size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *id = cJSON_GetObjectItem(root, "id");
    const char *id_str = cJSON_IsString(id) ? id->valuestring : NULL;
    esp_err_t err = automation_engine_remove(id_str, output, output_size);
    cJSON_Delete(root);
    return err;
}
