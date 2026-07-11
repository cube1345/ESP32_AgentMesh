#include "agent/slash_command.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *usage;
    const char *template_text;
} slash_command_spec_t;

static bool is_action_command(const char *command)
{
    return command &&
           (strcmp(command, "clear") == 0 ||
            strcmp(command, "clear_all_memory") == 0 ||
            strcmp(command, "context_status") == 0 ||
            strcmp(command, "skills_list") == 0 ||
            strcmp(command, "skills_show") == 0);
}

static const slash_command_spec_t s_commands[] = {
    {
        .name = "clear",
        .usage = "/clear",
        .template_text = NULL,
    },
    {
        .name = "clear_all_memory",
        .usage = "/clear_all_memory",
        .template_text = NULL,
    },
    {
        .name = "context_status",
        .usage = "/context_status",
        .template_text = NULL,
    },
    {
        .name = "skills_list",
        .usage = "/skills_list",
        .template_text = NULL,
    },
    {
        .name = "skills_show",
        .usage = "/skills_show <skill_name>",
        .template_text = NULL,
    },
    {
        .name = "sensor",
        .usage = "/sensor <自然语言任务>",
        .template_text =
            "这是一个显式的 /sensor 指令。请只从 sensor_agent 的职责出发处理这条请求，"
            "优先使用传感器、环境读取、虚拟只读设备、温湿度、光照、空气质量、人体存在等能力。"
            "除非用户明确要求，否则不要把任务转成 control_agent 执行器控制。"
            "用户原始请求：%s",
    },
    {
        .name = "control",
        .usage = "/control <自然语言任务>",
        .template_text =
            "这是一个显式的 /control 指令。请只从 control_agent 的职责出发处理这条请求，"
            "优先使用状态灯、WS2812、GPIO、舵机、继电器、虚拟控制设备等执行能力。"
            "如果需要远程执行，应优先走 control_agent，而不是本地 coordinator 直接声称已执行。"
            "用户原始请求：%s",
    },
    {
        .name = "guardian",
        .usage = "/guardian <自然语言任务>",
        .template_text =
            "这是一个显式的 /guardian 指令。请只从 guardian_agent 的安全、权限、隐私、审计、策略判断视角处理这条请求。"
            "重点给出 policy、risk、privacy、approval、sandbox 相关结论。"
            "用户原始请求：%s",
    },
    {
        .name = "subagent",
        .usage = "/subagent <自然语言任务>",
        .template_text =
            "这是一个显式的 /subagent 指令。请优先调用 spawn_subagent 处理下面这个聚焦任务，"
            "然后由主 Agent 用简短结果总结返回。用户原始请求：%s",
    },
    {
        .name = "workflow",
        .usage = "/workflow <自然语言任务>",
        .template_text =
            "这是一个显式的 /workflow 指令。请优先把下面的请求实现为 deterministic 多步 workflow，"
            "而不是只执行最后一步。优先考虑 automation_create_workflow。用户原始请求：%s",
    },
    {
        .name = "rule",
        .usage = "/rule <自然语言任务>",
        .template_text =
            "这是一个显式的 /rule 指令。请优先把下面的请求实现为 persistent 条件规则，"
            "而不是一次性动作。优先考虑 automation_create_rule；若属于持久后台规则，需要明确确认要求。"
            "用户原始请求：%s",
    },
    {
        .name = "local",
        .usage = "/local <自然语言任务>",
        .template_text =
            "这是一个显式的 /local 指令。请只在当前节点本地处理下面这条请求，"
            "不要默认路由到其它 AgentMesh 角色。若调用支持 local=true 的工具，应显式使用 local=true。"
            "用户原始请求：%s",
    },
    {
        .name = "mesh",
        .usage = "/mesh <自然语言任务>",
        .template_text =
            "这是一个显式的 /mesh 指令。请优先把下面这条请求理解为跨节点 AgentMesh 协作任务。"
            "若属于确定性的远程传感器读取、执行器控制、Guardian 审计或控制状态查询，优先调用 mesh_send_command，"
            "不要只给口头分析。用户原始请求：%s",
    },
    {
        .name = "status",
        .usage = "/status <自然语言任务>",
        .template_text =
            "这是一个显式的 /status 指令。请优先检查状态、连接、队列、自动化、Guardian 或控制面状态。"
            "优先考虑 automation_list，"
            "以及对 control_agent 使用 mesh_send_command(action=control_state)。用户原始请求：%s",
    },
    {
        .name = "stop",
        .usage = "/stop <自然语言任务>",
        .template_text =
            "这是一个显式的 /stop 指令。请把下面请求优先理解为停止、刹停、取消或进入安全状态。"
            "若是停止第三角色的硬件动作或进入执行器锁定，优先调用 mesh_send_command 到 control_agent，"
            "action=control_emergency_stop。若是停止 Lua 任务，优先使用 lua_stop_job。"
            "若是停止 workflow/rule，优先使用 automation_list/automation_remove。用户原始请求：%s",
    },
    {
        .name = "resume",
        .usage = "/resume <自然语言任务>",
        .template_text =
            "这是一个显式的 /resume 指令。请把下面请求优先理解为恢复、解锁或解除刹停。"
            "若是恢复第三角色的硬件执行权限，优先调用 mesh_send_command 到 control_agent，"
            "action=control_clear_emergency_stop。用户原始请求：%s",
    },
    {
        .name = "device",
        .usage = "/device <自然语言任务>",
        .template_text =
            "这是一个显式的 /device 指令。请优先从 runtime device manifest、协议外设、virtual_device_read、"
            "virtual_device_control、GPIO/UART/I2C/SPI 扩展设备角度处理下面请求。不要假装设备已经被内建驱动支持。"
            "用户原始请求：%s",
    },
    {
        .name = "profile",
        .usage = "/profile <自然语言任务>",
        .template_text =
            "这是一个显式的 /profile 指令。请优先从用户画像、长期偏好、习惯、约束、冲突修正的角度处理下面请求。"
            "若用户透露稳定偏好或修正旧偏好，优先使用 memory_profile_set；不要只把信息留在临时对话里。"
            "用户原始请求：%s",
    },
    {
        .name = "skills",
        .usage = "/skills <自然语言任务>",
        .template_text =
            "这是一个显式的 /skills 指令。请优先从 skills、benchmark、验证记录、硬件支持边界、"
            "manifest capability 和已知限制的角度处理下面请求。必要时优先使用 skill_observation_add 记录结果。"
            "用户原始请求：%s",
    },
    {
        .name = "privacy",
        .usage = "/privacy <自然语言任务>",
        .template_text =
            "这是一个显式的 /privacy 指令。请从隐私、脱敏、最小化上云、权限、Guardian policy 和数据边界角度处理下面请求。"
            "若涉及分享或上传敏感信息，先给出风险判断和更安全方案。用户原始请求：%s",
    },
    {
        .name = "lua",
        .usage = "/lua <自然语言任务>",
        .template_text =
            "这是一个显式的 /lua 指令。请优先从 Lua runtime、脚本列表、Lua job 管理角度处理下面请求。"
            "先确认 lua_runtime_info，再决定是否需要 lua_list_scripts、lua_run_script、lua_run_script_async 或 lua_stop_job。"
            "用户原始请求：%s",
    },
    {
        .name = "trace",
        .usage = "/trace <自然语言任务>",
        .template_text =
            "这是一个显式的 /trace 指令。请优先从 timeline、StateBoard、Mesh trace、控制状态、"
            "自动化状态角度处理下面请求。必要时组合 automation_list、"
            "以及对 control_agent 的 control_state 查询。用户原始请求：%s",
    },
};

static void build_help_text(espagent_slash_result_t *result)
{
    if (!result) {
        return;
    }

    result->type = ESPAGENT_SLASH_HELP;
    snprintf(result->command, sizeof(result->command), "help");

    size_t off = 0;
    off += snprintf(result->text + off, sizeof(result->text) - off,
                    "Slash commands:\n");
    off += snprintf(result->text + off, sizeof(result->text) - off,
                    "/help - show slash command help\n");
    off += snprintf(result->text + off, sizeof(result->text) - off,
                    "/clear - clear current chat_id session/history/brief/trace\n");
    off += snprintf(result->text + off, sizeof(result->text) - off,
                    "/clear_all_memory - clear session + MEMORY/profile/skills/trace\n");
    off += snprintf(result->text + off, sizeof(result->text) - off,
                    "/context_status - show current chat_id history/brief/trace usage\n");
    off += snprintf(result->text + off, sizeof(result->text) - off,
                    "/skills_list - show all loaded skills\n");
    off += snprintf(result->text + off, sizeof(result->text) - off,
                    "/skills_show <skill_name> - show one skill content\n");
    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]) && off < sizeof(result->text); i++) {
        off += snprintf(result->text + off, sizeof(result->text) - off,
                        "%s - %s\n", s_commands[i].usage, s_commands[i].name);
    }
    snprintf(result->text + off, sizeof(result->text) - off,
             "Example: /control 把状态灯设为蓝色");
}

static const char *skip_spaces(const char *p)
{
    while (p && *p && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

bool espagent_slash_try_handle(const char *input, espagent_slash_result_t *result)
{
    if (!input || !result) {
        return false;
    }

    memset(result, 0, sizeof(*result));
    const char *p = skip_spaces(input);
    if (!p || p[0] != '/') {
        result->type = ESPAGENT_SLASH_NONE;
        return false;
    }

    p++;
    char command[32] = {0};
    size_t ci = 0;
    while (*p && !isspace((unsigned char)*p) && ci + 1 < sizeof(command)) {
        command[ci++] = (char)tolower((unsigned char)*p++);
    }
    command[ci] = '\0';
    p = skip_spaces(p);

    if (command[0] == '\0' || strcmp(command, "help") == 0) {
        build_help_text(result);
        return true;
    }

    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); i++) {
        if (strcmp(command, s_commands[i].name) != 0) {
            continue;
        }

        snprintf(result->command, sizeof(result->command), "%s", command);
        if (is_action_command(command)) {
            if (strcmp(command, "skills_show") == 0) {
                if (!p || p[0] == '\0') {
                    result->type = ESPAGENT_SLASH_ERROR;
                    snprintf(result->text, sizeof(result->text),
                             "Missing skill name after /skills_show.\nUsage: /skills_show <skill_name>");
                    return true;
                }
                result->type = ESPAGENT_SLASH_ACTION;
                snprintf(result->text, sizeof(result->text), "%s", p);
                return true;
            }
            result->type = ESPAGENT_SLASH_ACTION;
            snprintf(result->text, sizeof(result->text), "%s", command);
            return true;
        }

        if (!p || p[0] == '\0') {
            result->type = ESPAGENT_SLASH_ERROR;
            snprintf(result->text, sizeof(result->text),
                     "Missing task after /%s.\nUsage: %s",
                     command, s_commands[i].usage);
            return true;
        }

        result->type = ESPAGENT_SLASH_REWRITE;
        snprintf(result->text, sizeof(result->text),
                 s_commands[i].template_text, p);
        return true;
    }

    result->type = ESPAGENT_SLASH_ERROR;
    snprintf(result->text, sizeof(result->text),
             "Unknown slash command: /%s\nUse /help to list supported commands.",
             command);
    snprintf(result->command, sizeof(result->command), "%s", command);
    return true;
}
