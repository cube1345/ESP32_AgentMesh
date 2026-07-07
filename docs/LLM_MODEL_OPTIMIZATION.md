# ESPAgent AI 模型优化说明

本文档基于当前仓库代码和当前 coordinator 板端配置，说明本项目的 AI 模型接入方式、已经确认的问题，以及建议的优化方向。

## 0. 当前已落地的代码级优化

截至当前仓库版本，已经实际落地的优化包括：

- `main/agent/agent_loop.c`
  - coordinator 对显式 `no tools`、简单寒暄、普通通用问答启用轻量直答 prompt
  - 对 `13.8 和 13.11 谁大` 这类纯数值比较问题增加 deterministic fast-path
  - 直答类问题默认缩短历史，减少旧上下文和 mesh 编排提示对普通问答的干扰
- `main/llm/llm_proxy.c`
  - OpenAI-compatible 请求显式设置 `temperature=0`
  - 显式设置 `top_p=1`
  - 记录上游实际返回的 `model`，便于确认真实命中的模型

这意味着当前“模型回答质量低”的问题，已经不再只是 provider 参数问题，还和：

- 当前实际模型能力
- prompt 分流是否正确
- 普通问答是否被误当成设备编排

直接相关。

## 1. 当前实际运行模型

已通过板端串口 CLI `config_show` 确认，当前 coordinator 实际运行配置为：

- `Model`: `deepseek-chat`
- `Provider`: `openai`

这两个值当前来自：

```text
[build]
```

说明：

- 当前并不是默认配置里的 `claude-opus-4-5`
- 也不是 NVS 覆盖值
- 而是当前固件编译时直接写入的模型配置

## 2. 当前项目里的 provider 含义

本项目里的 `provider=openai`，不等于“只能调用 OpenAI 官方模型”。

在当前实现中：

- `provider=openai`
  - 使用 OpenAI-compatible 的 `chat/completions` 接口格式
  - 使用 `Bearer` 鉴权头
  - 使用 `messages` + `tools`
- `provider=anthropic`
  - 使用 Anthropic 的 `messages` 接口格式
  - 使用 `x-api-key`
  - 使用 `anthropic-version`

当前代码里：

- `ESPAGENT_OPENAI_API_URL`
  - `https://api.deepseek.com/chat/completions`
- `ESPAGENT_LLM_API_URL`
  - `https://api.anthropic.com/v1/messages`

因此，如果你当前使用的是 DeepSeek 的 OpenAI-compatible 接口，继续使用：

```text
provider = openai
```

是正确的，不需要因为模型换成 DeepSeek 就把 provider 改成别的值。

## 3. 当前代码里已经确认的问题

### 3.1 实际运行模型与默认模型不一致

配置默认值在：

- `main/espagent_config.h`

当前默认值仍然是：

- `ESPAGENT_LLM_DEFAULT_MODEL = "claude-opus-4-5"`
- `ESPAGENT_LLM_PROVIDER_DEFAULT = "anthropic"`

但板端实际运行的是：

- `deepseek-chat`
- `openai`

这会导致：

- 看代码时容易误判当前模型能力
- 以为自己还在用 Claude，实际上已经切到了 DeepSeek

### 3.2 代理分支 host/path 写死

当前 `llm_proxy.c` 中：

- 直连路径使用 `ESPAGENT_OPENAI_API_URL`
- 但代理路径仍然写死：
  - host: `api.openai.com`
  - path: `/v1/chat/completions`

这意味着：

- 直连 DeepSeek 时通常没问题
- 一旦启用 HTTP proxy，可能会把请求发错目标主机

这是当前最明确的兼容性隐患之一。

### 3.3 没有显式设置低随机性参数

当前 `llm_chat_tools()` 组包时只传了：

- `model`
- `max_completion_tokens` 或 `max_tokens`
- `messages`
- `tools`

没有显式传：

- `temperature`
- `top_p`

这会导致：

- 简单判断题稳定性差
- 同类问题多轮表现不一致
- “13.8 和 13.11 谁大”这类基础题更容易出错

### 3.4 当前没有确定性数值工具

项目现有工具主要是：

- 搜索
- 天气
- Mesh
- 自动化
- GPIO / 传感器 / 舵机 / 红外
- 文件 / Lua / Gateway

但没有：

- calculator
- number_compare
- expression_eval

所以所有数值比较、简单运算、排序判断，当前都交给 LLM 自己完成。

这会带来一个非常典型的问题：

```text
模型做设备编排很强
但基础数值判断仍然可能犯低级错误
```

### 3.5 当前 prompt 更偏“设备编排器”，不是“严格判题器”

当前 coordinator 使用的是 compact prompt。

它重点强调的是：

- routing
- mesh_send_command
- tool choice
- guardian / policy
- workflow / rule
- voice / gateway / skill / memory

这对多 Agent 调度是合理的，但并没有专门强化：

- 数值比较
- 简单数学
- 严格逻辑判断

所以当请求不触发工具，而只是一个短问答时，效果高度依赖底层模型本身。

## 4. 为什么会出现“连 13.8 和 13.11 谁大都分不清”

这类问题通常不是单一 bug，而是几个因素叠加：

### 4.1 当前模型本身并不是最强推理模型

当前实际运行的是：

```text
deepseek-chat
```

而不是默认代码里看起来像在用的高端 Claude 配置。

### 4.2 问题没有走 deterministic tool

因为没有数值比较工具，所以它只能靠 LLM 纯文本推理。

### 4.3 请求没压低温度

这使得简单判断问题的稳定性进一步下降。

### 4.4 prompt 的重点不在这种任务上

当前 prompt 结构更偏：

- 调度硬件
- 选工具
- 走 Mesh
- 受 Guardian 限制

而不是进行数学型规则推理。

## 5. 建议的优化方向

下面是按优先级排序的建议。

### 5.1 第一优先级：修复代理分支兼容性

建议修改 `main/llm/llm_proxy.c`，避免在 `provider=openai` 分支里把：

- host
- path

写死为 OpenAI 官方地址。

更稳妥的方案：

- 从 `ESPAGENT_OPENAI_API_URL` 解析 host/path
- 或单独新增：
  - `ESPAGENT_OPENAI_API_HOST`
  - `ESPAGENT_OPENAI_API_PATH`

这样可以兼容：

- DeepSeek
- OpenAI
- 其他 OpenAI-compatible 服务

### 5.2 第二优先级：显式设置低温度

建议在 OpenAI-compatible 请求体中新增：

```json
"temperature": 0
```

或保守一点：

```json
"temperature": 0.1
```

用途：

- 提高简单判断题稳定性
- 降低多轮同问不同答
- 减少数值比较类低级错误

如果需要，也可以同时加：

```json
"top_p": 1
```

### 5.3 第三优先级：确认 `max_completion_tokens` 兼容性

当前 OpenAI-compatible 分支使用的是：

```text
max_completion_tokens
```

对一部分兼容服务这可能没问题，但也有服务更偏向：

```text
max_tokens
```

建议：

- 确认当前 DeepSeek 是否稳定接受 `max_completion_tokens`
- 若兼容性不稳，改成：
  - 统一使用 `max_tokens`
  - 或按 provider/model 类型切换字段

### 5.4 第四优先级：补一个确定性数值工具

建议新增一个非常小而稳的工具，例如：

- `number_compare`
- `calculator`

可支持：

- 比较两个整数或小数大小
- 加减乘除
- 简单排序

这样以下问题就不再依赖 LLM 自己猜：

- `13.8 和 13.11 谁大`
- `27.4 加 1.5 等于多少`
- `把三个温度从小到大排序`

### 5.5 第五优先级：为简单数值请求增加 fast path

如果不想让模型连这种题都参与，可以在 `agent_loop` 中增加轻量规则：

- 识别“谁大”“比较大小”“加减乘除”“排序”这类请求
- 直接调用 deterministic tool
- 不进入完整 ReAct 流程

这样可以：

- 更快
- 更便宜
- 更稳

## 6. 当前最推荐的修改顺序

建议按这个顺序落地：

### 第一步

修复 `llm_proxy.c` 的代理分支 host/path 问题。

### 第二步

为 OpenAI-compatible 请求显式加入：

- `temperature=0`

### 第三步

确认 `max_completion_tokens` 是否适配当前 DeepSeek 接口。

### 第四步

补充一个最小可用的 deterministic 数值比较/计算工具。

## 7. 当前结论

一句话总结：

### 当前 DeepSeek 接入方式本身没有选错 provider

如果你用的是 DeepSeek 的 OpenAI-compatible 接口：

```text
provider = openai
```

是对的。

### 当前问题不在“provider 选错”

而在于：

- 实际模型能力预期和默认配置认知不一致
- 代理分支仍然写死 OpenAI host/path
- 没有低温度约束
- 没有 deterministic 数值工具

### 当前最值得优先做的优化

是：

1. 修代理分支
2. 加 `temperature=0`
3. 视情况补数值工具

## 8. 后续可继续补的内容

如果后续要继续提升模型稳定性，还可以继续做这些事情：

- 增加 `provider/model/base_url` 的统一配置抽象
- 为不同 provider 定制请求字段
- 记录模型调用失败和空响应的统计
- 对短问答与硬件编排走不同的 prompt 或 tool 策略
- 对“已知易错题型”建立 deterministic fallback
