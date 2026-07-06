# ESPAgent 前端与运行时 Skill 接入现状

本文档说明当前 React 前端已经实现的能力、ESP32 固件侧已经具备的基础能力，以及“前端编写 skill 后不重烧录就让 ESP32 生效”这件事还差哪些环节。

## 1. 当前前端已经做到什么

当前前端目录：

```text
frontend/
```

当前前端技术栈：

- React
- TypeScript
- Vite
- Ant Design
- Tailwind CSS
- axios

当前前端已经实现的能力如下。

### 1.1 多节点总览展示

当前页面已经可以展示：

- `coordinator_agent`
- `sensor_agent`
- `control_agent`
- `guardian_agent`

展示内容包括：

- 节点 ID
- 角色
- 状态
- 基本职责
- 通信面

### 1.2 真实环境数据展示

当前前端已经不只是 mock。

现在通过本地聚合服务订阅真实 MQTT 数据后，前端可以展示：

- 温度
- 湿度
- eCO2
- TVOC
- 光照
- Presence

这些数据来自真实 MQTT telemetry，而不是静态假数据。

### 1.3 节点状态与部分通信过程展示

当前前端已经能展示一部分真实通信过程：

- `nodes/+/state`
- `nodes/+/telemetry`
- `agent/timeline`
- `guardian/stateboard`

也就是说，现在前端已经能看到：

- 节点 online/offline 状态变化
- sensor 节点 telemetry 上报
- 一部分 timeline 事件
- guardian 的 stateboard 更新

### 1.4 Skills Studio 前端草稿编辑

当前前端已经有一个 Skills Studio 页面，可以：

- 新建 skill 草稿
- 编辑名称
- 编辑 scope
- 编辑 trigger
- 编辑 policy
- 编辑 prompt 内容
- 开启或关闭 enabled 状态

但是这部分目前仍然只是前端/本地聚合服务层的“草稿数据”。

### 1.5 用户偏好配置页面

当前前端已经有用户偏好配置页，可以编辑：

- 首选交互通道
- 语言
- 隐私模式
- 摘要风格
- 自动化积极度
- 语音输出开关

目前这部分也主要是前端态和本地聚合服务回显，没有真正同步到 ESP32 的 `/spiffs/config/` 或 memory/profile。

## 2. 当前前端还没有做到什么

虽然前端页面已经比较完整，但以下能力还没有真正落到板端。

### 2.1 前端 skill 还没有下发到 ESP32

当前前端的 `POST /api/skills` 只是把数据回给前端自己：

- 没有写入 ESP32
- 没有写入 `/spiffs/skills/*.md`
- 没有触发板端 skill cache 刷新

所以当前前端里的 skill：

- 前端可见
- ESP32 不可见

### 2.2 用户偏好还没有同步到 ESP32 运行时配置

当前前端偏好配置没有真正写入以下位置：

- `/spiffs/config/USER.md`
- `/spiffs/memory/profile.jsonl`
- 其他偏好配置持久化结构

所以现在前端偏好只是 UI 层面的配置草稿。

### 2.3 还没有完整的任务链路可视化

当前已经拿到真实 timeline 数据，但还没有把单次任务整理成完整链路，例如：

```text
User
-> coordinator_agent
-> guardian_agent
-> control_agent
-> output message
-> final reply
```

现在前端更像“事件流展示”，还不是“按一次任务回放完整协作链”。

### 2.4 还没有直接操作 ESP32 SPIFFS 的运行时写入链路

这是当前最关键的缺口。

想做到“不重烧录 ESP32，让前端写入的新 skill 立即被 ESP32 识别”，必须要有一条运行时写入链路，把前端内容真正写到：

```text
/spiffs/skills/<name>.md
```

当前这条链路还没有接起来。

## 3. 当前 ESP32 已经具备哪些基础能力

虽然前端还没和板端完全打通，但 ESP32 固件侧其实已经具备了不少基础能力。

### 3.1 Skill Loader 已经存在

ESP32 固件现在已经支持从 SPIFFS 扫描 skill：

```text
/spiffs/skills/*.md
```

相关代码：

- `main/skills/skill_loader.c`
- `main/skills/skill_loader.h`

这说明 skill 不是写死在固件里的，而是运行时从 SPIFFS 读出来的。

### 3.2 Skill Summary 已进入 prompt 构建链路

skill loader 不只是扫描文件，它还会构建 skill summary，用于 system prompt / context prompt。

这意味着：

- skill 文件一旦进入 SPIFFS
- 并且缓存刷新成功
- 下一轮 prompt 就有机会感知到这个新 skill

### 3.3 已有 skill cache 失效机制

当前代码已经有 skill summary cache invalidation 机制。

如果修改的是 `/spiffs/skills/` 下文件，代码会调用：

```c
skill_loader_invalidate_cache()
```

这说明板端已经考虑了“skill 在运行时发生变化”的情况。

### 3.4 已有文件写入和 sandbox 边界

当前工程已经有文件相关工具和 sandbox 边界。

尤其是 skill 文件修改，当前明确要求：

- 需要显式确认
- 不能绕过 sandbox

这说明运行时写入 skill 在架构上是允许的，但必须受控。

## 4. 当前 ESP32 还需要补哪些东西

如果目标是：

> 在前端写一个 skill，不重烧录 ESP32，ESP32 就能拿到并在下一轮对话里使用

那么 ESP32 侧和链路侧还需要补以下内容。

### 4.1 一个真实可调用的“写 skill 文件”接口

当前缺的不是 skill loader，而是“把外部内容安全写入 `/spiffs/skills/*.md`”这条正式接口。

至少需要支持：

- 文件名合法性校验
- Markdown 内容写入
- 覆盖/更新已有 skill
- skill 文件大小限制
- 内容结构基本校验

### 4.2 写入成功后刷新 skill cache

写入 skill 文件后，必须调用：

```text
skill_loader_invalidate_cache()
```

否则下一轮 prompt 仍然可能看到旧的 skills summary。

### 4.3 最好补一个“列出当前板端 skill”接口

前端如果要确认 ESP32 真的识别到了 skill，最好要有一个真实查询接口，例如：

- 列出 `/spiffs/skills/*.md`
- 返回 skill title / path / modified time

这样前端可以清楚展示：

- 前端草稿
- 板端已安装
- 板端已生效

### 4.4 最好补一个“skill 生效状态”反馈

仅仅写入文件还不够，最好还能反馈：

- skill 已写入
- skill summary cache 已失效
- 下一轮 prompt 将可见

否则前端只能猜“应该已经生效了”。

## 5. 要实现这件事，推荐的完整链路

推荐链路如下：

```text
前端 Skills Studio
-> 本地聚合服务 /api/skills/install
-> ESP32 运行时文件写入接口
-> /spiffs/skills/<name>.md
-> skill_loader_invalidate_cache()
-> 板端返回 install ok
-> 前端刷新“已安装 skills”
```

进一步建议拆成两个接口：

### 5.1 草稿保存接口

用于前端本地保存：

```text
POST /api/skills
```

这个接口已经有了，但目前只是简单回显。

### 5.2 板端安装接口

建议新增，例如：

```text
POST /api/skills/install
```

传入：

- skill name
- markdown content
- confirmed

然后由聚合服务转发到 ESP32 运行时文件写入链路。

## 6. 当前结论

一句话总结当前状态：

### 前端现在已经能做到

- 展示真实环境数据
- 展示真实节点状态
- 展示部分真实通信过程
- 编辑 skill 草稿
- 编辑用户偏好

### 但前端现在还做不到

- 让前端写的 skill 自动被 ESP32 识别
- 让用户偏好真正进入 ESP32 运行时配置
- 把单次任务完整回放成可视协作链路

### ESP32 现在已经具备

- SPIFFS skill 存储机制
- skill loader
- skill summary prompt 构建
- skill cache 失效机制
- 受控文件写入边界

### ESP32 还需要补

- 真实的运行时 skill 写入接口
- 写入后的 cache refresh
- skill 安装结果查询接口
- 前端到板端的安装链路

## 7. 推荐下一步

如果接下来只做一件事，我建议优先做：

### 前端 skill -> ESP32 SPIFFS 安装链路

也就是：

1. 前端写 skill
2. 聚合服务提供 install API
3. ESP32 运行时写入 `/spiffs/skills/*.md`
4. 自动刷新 skill cache
5. 前端能查询“板端已安装 skills”

这一步做完后，“不重烧录 ESP32 安装新 skill”这件事才算真正闭环。
