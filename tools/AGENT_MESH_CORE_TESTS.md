# Agent Mesh 五大能力无串口测试

这些脚本通过 Feishu、MQTT 和 Dashboard HTTP API 验证五大核心能力，不访问串口。硬件执行测试只使用第三角色板载 WS2812。

运行脚本会向测试群发送真实消息，Workflow、Rule 会实际改变 WS2812 状态。请一次只运行一个脚本，避免并发回复干扰消息关联。

## 前置条件

- 四个角色节点在线，Coordinator 的 Feishu 通道可收发消息。
- 已安装并登录 `lark-cli`，用户身份具备发送和读取测试群消息的权限。
- Workflow 和 Rule 测试前启动 Dashboard 聚合服务：

```bash
cd frontend
npm run server
```

- 默认测试群为现有 ESPAgent 测试群。可通过环境变量覆盖：

```bash
export ESPAGENT_FEISHU_CHAT_ID=oc_xxx
```

## 独立运行

在项目根目录执行：

```bash
python3 tools/test_core_subagent_feishu.py
python3 tools/test_core_sandbox_feishu.py
python3 tools/test_core_workflow_feishu.py
python3 tools/test_core_rule_feishu.py
python3 tools/test_core_runtime_skill_feishu.py
```

每个脚本输出一段 JSON，`passed: true` 且进程退出码为 `0` 表示通过；`passed: false` 且退出码为 `1` 表示能力未完整复现。

## 第二组互补场景

```bash
python3 tools/test_core_subagent_reasoning_feishu.py
python3 tools/test_core_sandbox_path_feishu.py
python3 tools/test_core_workflow_colors_feishu.py
python3 tools/test_core_rule_else_feishu.py
python3 tools/test_core_runtime_skill_update_feishu.py
```

- `subagent_reasoning`：委托子代理完成结果固定的计算任务。
- `sandbox_path`：带 `confirmed=true` 请求仍必须拒绝包含 `..` 的路径。
- `workflow_colors`：验证紫、黄、青、关闭四步 WS2812 流程。
- `rule_else`：读取当前湿度并动态设置更高阈值，验证条件规则的 `else` 分支。
- `runtime_skill_update`：连续覆盖同一个 Skill，验证文件更新和 Agent 上下文缓存失效。

## 判定标准

### Subagent

- 使用显式 `/subagent` 路由。
- 子代理查询当前时间。
- Agent 回复包含合法时间，且不包含子代理执行失败信息。

### Sandbox

- 请求在没有 `confirmed=true` 时写入唯一测试 Skill 路径。
- Agent 必须拒绝或拦截。
- 随后通过 `/skills_show` 确认测试 Skill 不存在。

### Workflow

- 创建蓝色、绿色、关闭三步 WS2812 流程，每步间隔 3 秒。
- 飞书回复必须确认 Workflow 创建成功。
- Dashboard 的新 MQTT 时间线必须依次包含三种 Control 执行结果。

### Rule

- 创建基于湿度数据的 WS2812 条件规则。
- 飞书回复必须确认 Rule 创建成功。
- Dashboard 新时间线必须包含 Sensor telemetry、Guardian allow 和红色 WS2812 Control 结果。
- 测试完成后通过 Feishu 发送 WS2812 关闭命令进行清理。

### Runtime Skill

- Python 使用 MQTT 3.1.1 request/reply topic 直接写入，不调用前端串口回退。
- 固定覆盖 `runtime-verify-basic.md`，避免连续测试积累文件。
- `/skills_show` 必须返回本轮随机事实。
- 自然语言问答必须正确使用本轮随机事实。

## 参数覆盖

```bash
python3 tools/test_core_workflow_feishu.py \
  --chat-id oc_xxx \
  --dashboard-url http://127.0.0.1:4175/api/dashboard

python3 tools/test_core_runtime_skill_feishu.py \
  --mqtt-host broker.emqx.io \
  --mqtt-port 1883 \
  --topic-prefix espagent/cube1345 \
  --mesh-auth-key 'shared-key'
```

对应环境变量：

- `ESPAGENT_FEISHU_CHAT_ID`
- `ESPAGENT_DASHBOARD_URL`
- `ESPAGENT_MQTT_HOST`
- `ESPAGENT_MQTT_PORT`
- `ESPAGENT_TOPIC_PREFIX`
- `ESPAGENT_MESH_AUTH_KEY`

公网 MQTT 环境应配置 `ESPAGENT_MESH_AUTH_KEY`，并确保固件和测试脚本使用同一个值。未配置时 Runtime Skill 写请求没有 HMAC 保护，只适合当前受控演示环境。
