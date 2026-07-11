import type {
  AgentNode,
  Capability,
  DashboardPayload,
  EnvironmentMetric,
  MeshMessageFlow,
  SkillDraft,
  TimelineEvent,
  UserPreferenceProfile
} from '../types';

const nodes: AgentNode[] = [
  {
    id: 'esp32s3-coordinator-01',
    role: 'coordinator_agent',
    transport: ['Feishu WS', 'WebSocket', 'MQTT Mesh'],
    status: 'online',
    location: '/dev/ttyUSB0',
    responsibilities: ['LLM 调度', '工具调用', '任务编排', 'Timeline 聚合'],
    surfaces: ['飞书', 'Web 控制台', '串口 CLI']
  },
  {
    id: 'esp32s3-sensor-01',
    role: 'sensor_agent',
    transport: ['MQTT Mesh', 'ESP-NOW'],
    status: 'online',
    location: '/dev/ttyUSB1',
    responsibilities: ['AHT20', 'SGP30', 'BH1750', 'PIR/存在感知'],
    surfaces: ['环境数据上报', '规则触发输入']
  },
  {
    id: 'esp32s3-control-01',
    role: 'control_agent',
    transport: ['MQTT Mesh', 'RMT/I2S/PWM'],
    status: 'online',
    location: '/dev/ttyUSB2',
    responsibilities: ['GPIO', 'WS2812', '舵机', '格力空调 IR', 'MAX98357'],
    surfaces: ['执行回执', '控制状态快照']
  },
  {
    id: 'esp32s3-guardian-01',
    role: 'guardian_agent',
    transport: ['MQTT Mesh'],
    status: 'degraded',
    location: '/dev/ttyUSB3',
    responsibilities: ['policy_check', 'audit', 'watchdog', 'StateBoard'],
    surfaces: ['安全审计', '风险决策']
  },
  {
    id: 'esp32p4-display-01',
    role: 'display_terminal',
    transport: ['Wi-Fi', 'MQTT', 'Touch UI'],
    status: 'online',
    location: 'display frontend',
    responsibilities: ['调度可视化', '本地交互', '状态展示'],
    surfaces: ['屏幕 UI', '状态大盘']
  }
];

const capabilities: Capability[] = [
  { name: 'read_environment', category: '感知', role: 'sensor_agent', maturity: '已验证', summary: '聚合温湿度、空气质量、光照等环境数据。' },
  { name: 'sgp30_read_air_quality', category: '感知', role: 'sensor_agent', maturity: '已验证', summary: '读取 eCO2 / TVOC，并参与环境规则判断。' },
  { name: 'mesh_send_command', category: '协同', role: 'coordinator_agent', maturity: '已验证', summary: '将自然语言意图路由为跨节点控制任务。' },
  { name: 'automation_create_rule', category: '协同', role: 'coordinator_agent', maturity: '已验证', summary: '创建持久化条件规则，脱离单轮对话持续运行。' },
  { name: 'set_status_light', category: '控制', role: 'control_agent', maturity: '已验证', summary: '控制 WS2812 状态灯或 RGB 指示行为。' },
  { name: 'servo_write', category: '控制', role: 'control_agent', maturity: '已验证', summary: '通过 PWM 驱动舵机执行角度控制。' },
  { name: 'gree_ac_control', category: '控制', role: 'control_agent', maturity: '已验证', summary: '格力空调 IR 发射控制，支持常用模式和温度调整。' },
  { name: 'policy_check', category: '安全', role: 'guardian_agent', maturity: '已验证', summary: '对远程控制请求进行风险判定与审计。' },
  { name: 'lua_run_script', category: '扩展', role: 'coordinator_agent', maturity: '已验证', summary: '通过受限 Lua 运行时扩展板端能力。' },
  { name: 'virtual_device_control', category: '扩展', role: 'control_agent', maturity: '进行中', summary: '以 manifest 驱动受控设备，统一权限与 cooldown。' }
];

const timeline: TimelineEvent[] = [
  { time: '09:14:02', stage: 'intent_ingest', source: 'Feishu', target: 'coordinator_agent', payload: '请读取环境数据并根据湿度调整状态灯', status: 'ok' },
  { time: '09:14:03', stage: 'reasoning', source: 'coordinator_agent', target: 'tool_registry', payload: '选择 read_environment + automation_create_rule', status: 'ok' },
  { time: '09:14:04', stage: 'policy_check', source: 'coordinator_agent', target: 'guardian_agent', payload: '请求执行规则: humidity > 60 -> set_status_light orange', status: 'queued' },
  { time: '09:14:04', stage: 'policy_decision', source: 'guardian_agent', target: 'control_agent', payload: 'allow, risk_score=0.32, privacy_mode=metadata_only', status: 'ok' },
  { time: '09:14:04', stage: 'sandbox_denied', source: 'tool_registry', target: 'write_file', payload: 'sandbox denied write_file: skill changes require explicit confirmed=true', status: 'warn' },
  { time: '09:14:05', stage: 'telemetry_publish', source: 'sensor_agent', target: 'MQTT Mesh', payload: 'temp=27.4C humidity=64.2% tvoc=21ppb lux=185', status: 'ok' },
  { time: '09:14:06', stage: 'actuation', source: 'control_agent', target: 'WS2812', payload: 'set_status_light(color=orange)', status: 'ok' },
  { time: '09:14:06', stage: 'visual_sync', source: 'MQTT Mesh', target: 'ESP32-P4/Android', payload: 'timeline + control_state + telemetry update', status: 'ok' }
];

const environment: EnvironmentMetric[] = [
  { label: '温度', value: 27.4, unit: '°C', status: 'good', trend: 0.6 },
  { label: '湿度', value: 64.2, unit: '%', status: 'attention', trend: 3.1 },
  { label: 'eCO2', value: 618, unit: 'ppm', status: 'good', trend: -18 },
  { label: 'TVOC', value: 21, unit: 'ppb', status: 'good', trend: -4 },
  { label: '光照', value: 185, unit: 'lux', status: 'attention', trend: 42 },
  { label: 'Presence', value: 1, unit: 'detected', status: 'good', trend: 0 }
];

const skills: SkillDraft[] = [
  {
    id: 'skill-1',
    name: '环境告警联动',
    scope: 'coordinator + display',
    trigger: '空气质量恶化或 Guardian 发出 warn',
    policy: '仅发送结构化事件，不暴露隐私原文',
    prompt: '当 eCO2 超过 900ppm 或 TVOC 超过 120ppb 时，向 control 节点发送黄色状态灯闪烁 5 秒的控制请求，并在 Timeline 记录摘要。',
    enabled: true
  },
  {
    id: 'skill-2',
    name: '夜间节能模式',
    scope: 'automation',
    trigger: '22:00 后有人存在且照度较低',
    policy: '风扇和音频执行需要 Guardian allow',
    prompt: '当时间晚于 22:00，presence=true 且 lux<80 时，将状态灯切为蓝色低亮模式，并降低主动播报频率。',
    enabled: true
  },
  {
    id: 'skill-3',
    name: '调试回放',
    scope: 'display terminal',
    trigger: '用户请求查看上一次跨节点任务链路',
    policy: '仅显示脱敏 timeline',
    prompt: '按时间线展示 dispatch, policy_check, telemetry, tool_result 四类事件，并允许导出为 Markdown。',
    enabled: false
  }
];

const preferences: UserPreferenceProfile = {
  preferredChannel: 'Web Console',
  privacyMode: 'metadata_only',
  automationAggressiveness: 62,
  summaryStyle: 'concise',
  preferredLanguage: 'zh-CN'
};

const flows: MeshMessageFlow[] = [
  {
    id: 'flow-1',
    step: '用户入口',
    transport: 'Feishu WS / WebSocket',
    producer: 'User',
    consumer: 'coordinator_agent',
    topic: 'message_bus inbound',
    detail: '自然语言指令进入统一总线。'
  },
  {
    id: 'flow-2',
    step: 'Agent 编排',
    transport: 'LLM + Tool Registry',
    producer: 'coordinator_agent',
    consumer: 'tool_registry',
    topic: 'tool_use',
    detail: '根据上下文与 skills 选择环境读取、Mesh 调度或自动化工具。'
  },
  {
    id: 'flow-3',
    step: '安全门控',
    transport: 'MQTT Mesh',
    producer: 'coordinator_agent',
    consumer: 'guardian_agent',
    topic: 'espagent/security/policy_check',
    detail: 'Guardian 决定 allow/deny，并记录 risk_score。'
  },
  {
    id: 'flow-4',
    step: '执行与采集',
    transport: 'MQTT Mesh / GPIO / I2C / RMT / I2S',
    producer: 'sensor_agent / control_agent',
    consumer: 'device driver',
    topic: 'telemetry + actuator state',
    detail: '真实硬件采样与执行后回写结构化结果。'
  },
  {
    id: 'flow-5',
    step: '终端展示',
    transport: 'MQTT / Wi-Fi',
    producer: 'Mesh Timeline',
    consumer: 'ESP32-P4 / Android / Web Console',
    topic: 'timeline + state + alerts',
    detail: '多终端同步展示调度过程、设备状态、环境曲线与用户偏好。'
  }
];

export const mockDashboardPayload: DashboardPayload = {
  nodes,
  capabilities,
  timeline,
  environment,
  skills,
  preferences,
  flows
};
