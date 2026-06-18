# Resume Project Intro

## 基于 ESP32/RPi 的多 Agent 智能管家系统

项目负责人 | 2025/11 - 2026/04  
AIoT 边缘智能应用 | 嵌入式 Agent Runtime | 多 Agent 协作系统

- 基于 ESP32-S3、ESP32-P4/C6 与 RPi 构建多 Agent 智能管家系统，覆盖移动端对话入口、边缘硬件控制、环境感知、可视化终端与本地 AI 网关。
- ESP32-S3 侧基于 ESP-IDF、FreeRTOS、cJSON 实现轻量级 LLM Agent Runtime，支持 ReAct 风格 tool calling、Tool Registry、结构化 OutputMessage、Agent Memory、Skills、上下文缓存与串口 CLI 调试。
- 设计四角色 ESP32-S3 Agent Mesh：Coordinator 负责 Feishu/LLM/任务调度，Sensor 负责温湿度、空气质量、光照与人体存在采集，Control 负责 WS2812/GPIO/舵机/继电器等执行器控制，Guardian 负责 policy_check、权限裁决、审计、隐私与 watchdog。
- 基于 MQTT Mesh 实现多节点协作，支持 node/role topic、结构化 command/result、timeline 事件流、Guardian policy gate 与跨节点 ReAct 闭环；ESP32-P4+C6 与 Android 上位机用于展示 Agent 推理过程、数据通信链路、设备状态和最终执行结果。
- 硬件链路采用模块化驱动架构，接入 AHT10/AHT20、SGP30、BH1750、HC-SR05、WS2812、MAX98357、舵机等外设，并通过白名单、参数校验、tool guard 和 Guardian 审批约束高风险动作。
- 运维能力补充 HTTPS OTA 固件升级底座，支持双 OTA app 分区、串口 `ota_info` / `ota_update` 维护命令；规划由 Agent 执行版本发现、角色匹配、Guardian 审批、用户确认、升级触发、重启健康检查与结果汇报。
- RPi 侧规划作为边缘 AI 网关，部署 YOLOv8 人脸识别与 llama.cpp/Qwen 本地模型，承担隐私数据本地处理、MQTT 内网服务、知识库/MCP 扩展和 ESP32 节点状态聚合。

## 精简版

基于 ESP32-S3、ESP32-P4/C6 与 RPi 构建多 Agent 智能管家系统。ESP32-S3 侧基于 ESP-IDF/FreeRTOS/cJSON 实现轻量级 LLM Agent Runtime，支持 ReAct tool calling、Tool Registry、Memory、Skills、结构化 OutputMessage 与 MQTT Mesh 多节点协作；四块 ESP32-S3 分别承担 Coordinator、Sensor、Control、Guardian 角色，实现 Feishu 对话入口、环境感知、硬件执行、权限审计与跨节点调度。系统通过 Guardian policy gate、工具白名单和参数校验约束高风险硬件操作，并由 ESP32-P4/Android 展示 Agent 推理、通信链路和执行结果；同时补充 HTTPS OTA 固件升级底座，为后续 Agent 编排式版本发现、审批、部署和健康检查提供运维能力。
