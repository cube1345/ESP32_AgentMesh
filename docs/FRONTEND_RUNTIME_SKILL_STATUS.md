# ESPAgent 前端与 Runtime Skill 接入现状

本文档说明当前 React 前端已经做到的 Runtime Skill 相关能力、当前这条链路的边界，以及 MCU 真正识别前端 skill 还差哪些板端环节。

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

当前和 skill 相关的前端能力已经包括以下几部分。

### 1.1 Skills Studio 草稿编辑

当前前端已经有一个 Skills Studio 页面，可以：

- 新建 skill 草稿
- 编辑名称
- 编辑 scope
- 编辑 trigger
- 编辑 policy
- 编辑 prompt 内容
- 开启或关闭 enabled 状态
- 保存草稿到前端本地与聚合服务回显

### 1.2 Runtime install 按钮

当前前端已经补上：

```text
安装到 Runtime
```

按钮。

这意味着前端不再只是“写草稿”，而是已经具备“发起安装请求”的 UI 入口。

### 1.3 Runtime Skill 状态面板

当前前端已经补上一个 Runtime Skill 安装状态面板，用来展示：

- 当前安装链路说明
- 当前 runtime source
- 当前选中草稿会映射成什么 runtime 文件名
- 已安装 runtime skill 列表
- 安装时间
- cache 状态
- source
- 最后一次消息说明

### 1.4 Runtime Skill 查询接口接入

当前前端已经接入：

- `GET /api/skills/runtime`

用于刷新并展示“当前 runtime 层已知的已安装 skills”。

### 1.5 Runtime Skill 安装接口接入

当前前端已经接入：

- `POST /api/skills/install`

安装时前端会发送：

- skill 草稿内容
- `confirmed=true`

也就是说，从“前端有没有安装动作”这件事看，这部分已经补齐。

## 2. 当前本地聚合服务已经做到什么

当前本地 Node 聚合服务：

```text
frontend/server/index.mjs
```

已经补上了 skill 相关接口。

### 2.1 草稿保存接口

当前仍保留：

- `POST /api/skills`

这条接口现在只负责：

- 接收前端草稿
- 回显草稿数据

它不是 Runtime 安装接口。

### 2.2 Runtime 列表接口

当前已新增：

- `GET /api/skills/runtime`

用于给前端返回当前 runtime 层已知的 skill 列表。

### 2.3 Runtime 安装接口

当前已新增：

- `POST /api/skills/install`

这条接口负责：

- 接收一个 skill 草稿
- 生成 runtime 文件名
- 生成对应的 Markdown 内容
- 记录到 runtime skill 列表
- 返回安装结果

### 2.4 source 区分

当前 runtime skill 安装有两种 source：

- `local_mock`
- `proxy`

含义如下：

#### `local_mock`

表示：

- 只完成了前端与本地聚合服务闭环
- 没有真正写到 ESP32
- 用于把前端安装流程、状态管理、列表展示先做完整

#### `proxy`

表示：

- 聚合服务配置了上游 runtime 网关
- 安装请求会继续转发到上游接口
- runtime skill 列表也会直接代理上游 `GET /api/skills`

当前通过环境变量：

```text
ESPAGENT_SKILLS_API_BASE
```

来决定是否启用 proxy 模式。

当前建议直接配置为：

```text
ESPAGENT_SKILLS_API_BASE=http://<ESP32_IP>
```

对于当前 coordinator，这是正确的配置，因为 skill runtime 接口现在挂在 admin HTTP server 的 80 端口：

```text
http://<ESP32_IP>/api/skills
```

本轮实测 coordinator IP 为：

```text
http://172.29.231.55/api/skills
```

## 3. 当前这条链路的边界

虽然前端和本地聚合服务这部分已经补了很多，但现在仍然要清楚区分：

### 3.1 当前已经完成

- 前端草稿编辑
- 前端安装按钮
- Runtime skill 列表展示
- Runtime skill 安装结果提示
- 本地聚合服务安装 API
- 本地 mock / proxy source 区分

### 3.2 当前还没有保证

当前并不能保证：

- skill 已真实写入 ESP32
- skill 已真实进入 `/spiffs/skills/*.md`
- MCU 已经重新扫描 skill
- prompt 下一轮一定能看到这个 skill

尤其当当前 source 是：

```text
local_mock
```

时，含义非常明确：

```text
前端闭环已完成
但 MCU 真实生效闭环还没完成
```

## 4. MCU 真正识别前端 skill 需要什么

MCU 现在识别 skill 的方式不是读取前端状态，而是读取：

```text
/spiffs/skills/*.md
```

板端 skill loader 会扫描这些 Markdown 文件，并构建 summary 进入 prompt。

所以 MCU 真正识别前端 skill，必须满足：

1. 前端草稿被转换为 Markdown
2. Markdown 真正写入 ESP32 的 `/spiffs/skills/<name>.md`
3. skill summary cache 被失效
4. 下一轮 prompt 重新构建

当前前端已经做到了第 1 步和安装 UI，但第 2 到第 4 步是否真实发生，仍取决于板端接口。

## 5. 当前还差哪些板端能力

如果目标是：

> 在前端写一个 skill，不重烧录 ESP32，ESP32 就能拿到并在下一轮对话里使用

那么板端至少还需要这些能力。

### 5.1 一个真实可调用的 runtime skill 写入接口

当前最关键的缺口是：

- 把外部请求真正写入 `/spiffs/skills/*.md`

可以是：

- HTTP 接口
- WebSocket 管理指令
- 受控 tool gateway

但必须真实落到板端 SPIFFS。

### 5.2 写入后触发 skill cache 失效

写入 skill 文件后，需要触发：

```c
skill_loader_invalidate_cache()
```

否则下一轮 prompt 可能仍看到旧 summary。

### 5.3 列出板端已安装 skill 的真实接口

当前前端已经有 runtime 列表 UI。

下一步板端最好提供真实接口，用于列出：

- 当前 `/spiffs/skills/*.md`
- 文件名
- 标题
- 修改时间

这样前端就可以把当前的 mock/runtime 列表，替换成真实板端列表。

### 5.4 安装结果回执

前端已经支持显示安装消息。

下一步板端最好明确返回：

- 写入成功 / 失败
- sandbox 是否拒绝
- 是否触发 cache invalidate
- 是否将在下一轮 prompt 可见

## 6. 当前推荐的完整链路

推荐链路如下：

```text
Skills Studio 草稿
-> POST /api/skills/install
-> 聚合服务
-> 上游 runtime gateway
-> ESP32 写入 /spiffs/skills/<name>.md
-> skill_loader_invalidate_cache()
-> 前端刷新 runtime 列表
```

当前已经完成的是：

```text
Skills Studio 草稿
-> POST /api/skills/install
-> 聚合服务
-> runtime 列表 / 状态回显
```

还没完全打通的是：

```text
聚合服务
-> ESP32 SPIFFS
-> MCU prompt 生效
```

## 7. 当前结论

一句话总结：

### 前端现在已经不只是草稿编辑器

而是已经具备：

- Runtime install 按钮
- Runtime list 面板
- install API 对接
- source 区分

### 但当前 MCU 是否真正识别 skill

取决于 runtime source：

- `local_mock`：还没有真实写入 ESP32
- `proxy`：聚合服务会把请求转发到真实板端接口；本轮已验证 coordinator 能真实写入 SPIFFS

### 所以当前状态最准确的表述是

```text
前端 Runtime Skill 闭环已完成
且已经可以代理 ESP32 原生 /api/skills
MCU Runtime Skill 真生效取决于是否正确配置 ESPAGENT_SKILLS_API_BASE，并连通 coordinator 的 `http://<ESP32_IP>/api/skills`
```
