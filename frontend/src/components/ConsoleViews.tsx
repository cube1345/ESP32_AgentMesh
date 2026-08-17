import {
  ApiOutlined,
  AuditOutlined,
  BranchesOutlined,
  CloudUploadOutlined,
  ControlOutlined,
  DeleteOutlined,
  DeploymentUnitOutlined,
  PlusOutlined,
  RadarChartOutlined,
  ReloadOutlined,
  SafetyCertificateOutlined,
  SaveOutlined
} from '@ant-design/icons';
import { Alert, Button, Empty, Form, Input, Popconfirm, Progress, Segmented, Select, Slider, Spin, Switch, Tag, Typography } from 'antd';
import { lazy, Suspense } from 'react';
import type {
  AgentNode,
  DashboardPayload,
  EnvironmentMetric,
  RuntimeAssetSource,
  RuntimeDeviceManifestRecord,
  RuntimeSkillRecord,
  SkillDraft,
  TimelineEvent,
  UserPreferenceProfile
} from '../types';

const EnvironmentHistoryChart = lazy(() => import('./EnvironmentHistoryChart'));

export type TimelineFilter = '全部' | 'telemetry' | 'state' | 'policy' | 'sandbox' | 'reply' | 'error';

export interface NodePanel {
  node: AgentNode;
  recognized: boolean;
  sentSummary: string[];
  recvSummary: string[];
  subagentLabel: string;
  extraNodes: NodePanel[];
}

const roleLabels: Record<AgentNode['role'], { title: string; short: string; icon: React.ReactNode }> = {
  coordinator_agent: { title: '调度节点', short: 'Coordinator', icon: <DeploymentUnitOutlined /> },
  sensor_agent: { title: '传感节点', short: 'Sensor', icon: <RadarChartOutlined /> },
  control_agent: { title: '控制节点', short: 'Control', icon: <ControlOutlined /> },
  guardian_agent: { title: '安全节点', short: 'Guardian', icon: <SafetyCertificateOutlined /> },
  display_terminal: { title: '展示终端', short: 'Display', icon: <ApiOutlined /> }
};

function roleClass(role: AgentNode['role']): string {
  return `role-${role.replace('_agent', '').replace('_terminal', '')}`;
}

function statusLabel(status: AgentNode['status']): string {
  return status === 'online' ? '在线' : status === 'degraded' ? '降级' : '离线';
}

function metricLabel(status: EnvironmentMetric['status']): string {
  return status === 'good' ? '正常' : status === 'attention' ? '注意' : '严重';
}

function metricPercent(metric: EnvironmentMetric): number {
  if (metric.label === '温度') return Math.min((metric.value / 40) * 100, 100);
  if (metric.label === '湿度') return Math.min(metric.value, 100);
  if (metric.label === 'eCO2') return Math.min((metric.value / 1200) * 100, 100);
  if (metric.label === 'TVOC') return Math.min((metric.value / 220) * 100, 100);
  if (metric.label === '光照') return Math.min((metric.value / 300) * 100, 100);
  return metric.value > 0 ? 100 : 0;
}

function eventKey(event: TimelineEvent, index: number): string {
  return `${event.time}-${event.stage}-${event.source}-${event.target}-${index}`;
}

function runtimeErrorLabel(error: string): string {
  if (/9009|python3|not recognized/i.test(error)) return '串口工具不可用，请检查 Python 与 USB0';
  return error;
}

function NodeCard({ panel, compact = false }: { panel: NodePanel; compact?: boolean }) {
  const meta = roleLabels[panel.node.role];
  return (
    <article className={`node-card ${roleClass(panel.node.role)} ${panel.recognized ? '' : 'node-card-unrecognized'} ${compact ? 'node-card-compact' : ''}`}>
      <div className="node-card-header">
        <span className="role-icon">{meta.icon}</span>
        <div>
          <strong>{meta.title}</strong>
          <small>{meta.short}</small>
        </div>
        <span className={`status-badge status-${panel.node.status}`}>{statusLabel(panel.node.status)}</span>
      </div>
      <code>{panel.node.id}</code>
      {!compact && (
        <div className="node-card-detail">
          <span>{panel.node.transport[0] || 'MQTT Mesh'}</span>
          <p>{panel.node.responsibilities.slice(0, 3).join(' · ') || panel.subagentLabel}</p>
        </div>
      )}
    </article>
  );
}

function EventList({ events, empty }: { events: TimelineEvent[]; empty: string }) {
  if (!events.length) return <Empty image={Empty.PRESENTED_IMAGE_SIMPLE} description={empty} />;
  return (
    <div className="event-list panel-scroll">
      {events.map((event, index) => (
        <details className="event-row" key={eventKey(event, index)}>
          <summary>
            <span className={`event-dot status-${event.status}`} />
            <span className="event-stage">{event.stage}</span>
            <span className="event-route">{event.source} → {event.target}</span>
            <time>{event.time}</time>
          </summary>
          <p>{event.payload}</p>
        </details>
      ))}
    </div>
  );
}

interface OverviewProps {
  payload: DashboardPayload;
  nodes: NodePanel[];
  isLive: boolean;
}

export function OverviewView({ payload, nodes, isLive }: OverviewProps) {
  return (
    <div className="view-stack view-enter">
      <div className="overview-grid">
        <section className="workspace-surface topology-surface">
          <div className="section-heading">
            <div><span>运行拓扑</span><h2>四节点协作</h2></div>
            <span className={`source-indicator ${isLive ? 'is-live' : ''}`}>{isLive ? '实时 MQTT' : '演示数据'}</span>
          </div>
          <div className="topology-stage">
            <div className="topology-grid">
              {nodes.map((panel) => (
                <div className="topology-lane" key={panel.node.role}>
                  {panel.extraNodes.length > 0 && (
                    <div className="topology-extra-stack">
                      {panel.extraNodes.map((extraPanel) => <NodeCard key={extraPanel.node.id} panel={extraPanel} compact />)}
                    </div>
                  )}
                  <NodeCard panel={panel} />
                </div>
              ))}
            </div>
          </div>
        </section>

        <section className="workspace-surface environment-surface">
          <div className="section-heading">
            <div><span>Sensor Agent</span><h2>环境状态</h2></div>
            <small>{payload.mqtt?.lastEventAt ? `更新 ${new Date(payload.mqtt.lastEventAt).toLocaleTimeString('zh-CN', { hour12: false })}` : '等待更新'}</small>
          </div>
          <div className="environment-list">
            {payload.environment.map((metric) => (
              <div className="environment-row" key={metric.label}>
                <div><span>{metric.label}</span><strong>{metric.value}<small>{metric.unit}</small></strong></div>
                <div className="metric-track"><Progress percent={Math.round(metricPercent(metric))} showInfo={false} strokeColor={metric.status === 'good' ? '#1a8b70' : metric.status === 'attention' ? '#d88b26' : '#c94b4b'} /></div>
                <span className={`metric-state metric-${metric.status}`}>{metricLabel(metric.status)}</span>
              </div>
            ))}
          </div>
        </section>
      </div>

      <section className="workspace-surface environment-history-surface">
        <div className="section-heading">
          <div><span>Sensor History</span><h2>环境趋势</h2></div>
          <small>最近 {payload.environmentHistory?.length || 0} 次 MQTT 采样</small>
        </div>
        <Suspense fallback={<div className="environment-chart-loading"><Spin size="small" /></div>}>
          <EnvironmentHistoryChart points={payload.environmentHistory || []} />
        </Suspense>
      </section>

      <div className="overview-grid overview-grid-secondary">
        <section className="workspace-surface activity-surface">
          <div className="section-heading">
            <div><span>Timeline</span><h2>最近执行</h2></div>
            <small>点击事件查看 payload</small>
          </div>
          <EventList events={payload.timeline.slice(0, 8)} empty="暂无运行事件" />
        </section>
        <section className="workspace-surface capability-surface">
          <div className="section-heading"><div><span>Capability</span><h2>能力边界</h2></div><strong>{payload.capabilities.length}</strong></div>
          <div className="capability-list panel-scroll">
            {payload.capabilities.slice(0, 7).map((capability) => (
              <div className="capability-row" key={capability.name}>
                <span className={`capability-mark ${roleClass(capability.role)}`} />
                <div><strong>{capability.name}</strong><small>{capability.role}</small></div>
                <span>{capability.maturity}</span>
              </div>
            ))}
          </div>
        </section>
      </div>
    </div>
  );
}

interface CollaborationProps {
  payload: DashboardPayload;
  filteredTimeline: TimelineEvent[];
  filter: TimelineFilter;
  setFilter: (value: TimelineFilter) => void;
}

export function CollaborationView({ payload, filteredTimeline, filter, setFilter }: CollaborationProps) {
  return (
    <div className="collaboration-grid view-enter">
      <section className="workspace-surface flow-surface">
        <div className="section-heading"><div><span>Message Flow</span><h2>协作链路</h2></div><BranchesOutlined /></div>
        <div className="flow-list panel-scroll">
          {payload.flows.map((flow, index) => (
            <div className="flow-step" key={flow.id}>
              <span className="flow-index">{String(index + 1).padStart(2, '0')}</span>
              <div><strong>{flow.step}</strong><small>{flow.transport}</small><p>{flow.producer} → {flow.consumer}</p><code>{flow.topic}</code></div>
            </div>
          ))}
        </div>
      </section>
      <section className="workspace-surface timeline-surface">
        <div className="section-heading timeline-heading">
          <div><span>Event Stream</span><h2>事件时间线</h2></div>
          <Segmented value={filter} onChange={(value) => setFilter(value as TimelineFilter)} options={['全部', 'telemetry', 'state', 'policy', 'sandbox', 'reply', 'error']} />
        </div>
        <EventList events={filteredTimeline.slice(0, 60)} empty="当前筛选条件下没有事件" />
      </section>
    </div>
  );
}

interface SandboxProps {
  events: TimelineEvent[];
  warnCount: number;
  latest: TimelineEvent | null;
  command: string;
}

export function SandboxView({ events, warnCount, latest, command }: SandboxProps) {
  const policies = [
    ['Skill 文件写入', '需要确认', '/spiffs/skills 写入必须携带 confirmed=true。'],
    ['配置与密钥', '禁止写入', 'config 与 secrets 路径不开放给通用文件工具。'],
    ['Mesh 控制', 'TTL 限制', '命令 TTL 不超过 30 秒，action 必须进入白名单。'],
    ['高风险自动化', '需要确认', '持久化规则创建必须经过确认流。'],
    ['音频执行', '参数限幅', '播放时长和音量保持在设备安全范围。']
  ];
  return (
    <div className="view-stack view-enter">
      <div className="security-summary">
        <div><AuditOutlined /><span>拦截事件<strong>{events.length}</strong></span></div>
        <div><SafetyCertificateOutlined /><span>风险告警<strong>{warnCount}</strong></span></div>
        <div><ControlOutlined /><span>最近工具<strong>{latest?.target || '无'}</strong></span></div>
      </div>
      <div className="security-grid">
        <section className="workspace-surface policy-surface">
          <div className="section-heading"><div><span>Guard Rails</span><h2>执行策略</h2></div></div>
          <div className="policy-list">
            {policies.map(([title, state, detail]) => <div className="policy-row" key={title}><span className="policy-check"><SafetyCertificateOutlined /></span><div><strong>{title}</strong><p>{detail}</p></div><Tag>{state}</Tag></div>)}
          </div>
        </section>
        <section className="workspace-surface">
          <div className="section-heading"><div><span>Audit</span><h2>拦截记录</h2></div></div>
          <EventList events={events} empty="还没有收到 Sandbox 拦截事件" />
        </section>
      </div>
      <section className="command-band">
        <div><span>验证命令</span><strong>预期被 Sandbox 拦截</strong></div>
        <Typography.Paragraph copyable className="command-code">{command}</Typography.Paragraph>
      </section>
    </div>
  );
}

interface SkillsProps {
  drafts: SkillDraft[];
  active: SkillDraft | null;
  activeId: string;
  editorMode: 'draft' | 'runtime' | 'device';
  setEditorMode: (mode: 'draft' | 'runtime' | 'device') => void;
  setActiveId: (id: string) => void;
  patchSkill: (patch: Partial<SkillDraft>) => void;
  canEdit: boolean;
  addSkill: () => void;
  deleteSkill: (id: string) => void;
  saveSkills: () => void;
  saving: boolean;
  install: () => void;
  installing: boolean;
  runtimeSkills: RuntimeSkillRecord[];
  activeRuntime: RuntimeSkillRecord | null;
  activeRuntimeId: string;
  setActiveRuntimeId: (id: string) => void;
  patchRuntimeSkill: (patch: Partial<RuntimeSkillRecord>) => void;
  saveRuntimeSkill: () => void;
  savingRuntime: boolean;
  runtimeSource: RuntimeAssetSource;
  runtimeError: string;
  runtimeLoading: boolean;
  runtimeDetailLoading: boolean;
  runtimeDetailError: string;
  refreshRuntime: () => void;
  runtimeDevices: RuntimeDeviceManifestRecord[];
  activeRuntimeDevice: RuntimeDeviceManifestRecord | null;
  activeRuntimeDeviceId: string;
  setActiveRuntimeDeviceId: (id: string) => void;
  patchRuntimeDevice: (patch: Partial<RuntimeDeviceManifestRecord>) => void;
  saveRuntimeDevice: () => void;
  savingRuntimeDevice: boolean;
  runtimeDeviceSource: 'local_mock' | 'proxy' | 'serial';
  runtimeDeviceError: string;
  runtimeDevicesLoading: boolean;
  refreshDevices: () => void;
  refresh: () => void;
}

export function SkillsView(props: SkillsProps) {
  const {
    drafts,
    active,
    activeId,
    editorMode,
    setEditorMode,
    setActiveId,
    patchSkill,
    canEdit,
    addSkill,
    deleteSkill,
    saveSkills,
    saving,
    install,
    installing,
    runtimeSkills,
    activeRuntime,
    activeRuntimeId,
    setActiveRuntimeId,
    patchRuntimeSkill,
    saveRuntimeSkill,
    savingRuntime,
    runtimeSource,
    runtimeError,
    runtimeLoading,
    runtimeDetailLoading,
    runtimeDetailError,
    refreshRuntime,
    runtimeDevices,
    activeRuntimeDevice,
    activeRuntimeDeviceId,
    setActiveRuntimeDeviceId,
    patchRuntimeDevice,
    saveRuntimeDevice,
    savingRuntimeDevice,
    runtimeDeviceSource,
    runtimeDeviceError,
    runtimeDevicesLoading,
    refreshDevices,
    refresh
  } = props;
  const runtimeSourceLabel = runtimeError
    ? '连接不可用'
    : runtimeSource === 'mqtt'
      ? '无线 MQTT'
      : runtimeSource === 'serial'
      ? '串口实时'
      : runtimeSource === 'proxy'
        ? '网关代理'
        : '本地演示';
  const runtimeDeviceSourceLabel = runtimeDeviceError
    ? '连接不可用'
    : runtimeDeviceSource === 'serial'
      ? '串口实时'
      : runtimeDeviceSource === 'proxy'
        ? '网关代理'
        : '本地演示';

  return (
    <div className="view-stack view-enter">
      <div className="studio-grid">
        <aside className="studio-list">
          <div className="section-heading">
            <div><span>Drafts</span><h2>Skill 草稿</h2></div>
            <Button type="text" icon={<PlusOutlined />} onClick={addSkill} disabled={!canEdit} aria-label="新增 Skill" />
          </div>
          {drafts.map((draft) => (
            <div className="skill-draft-row" key={draft.id}>
              <button
                className={`skill-select ${editorMode === 'draft' && draft.id === activeId ? 'is-active' : ''}`}
                onClick={() => { setEditorMode('draft'); setActiveId(draft.id); }}
              >
                <span><strong>{draft.name}</strong><small>{draft.scope}</small></span>
                <i className={draft.enabled ? 'is-enabled' : ''} />
              </button>
              <Popconfirm
                title="删除这个 Skill 草稿？"
                okText="删除"
                cancelText="取消"
                placement="right"
                onConfirm={() => deleteSkill(draft.id)}
              >
                <Button
                  type="text"
                  danger
                  icon={<DeleteOutlined />}
                  disabled={!canEdit}
                  aria-label={`删除 Skill 草稿 ${draft.name}`}
                />
              </Popconfirm>
            </div>
          ))}

          <div className="studio-list-divider">
            <span>Runtime</span>
            <button onClick={refreshRuntime} aria-label="刷新 Runtime Skills" disabled={runtimeLoading}><ReloadOutlined /></button>
          </div>
          <div className="studio-asset-list">
            {runtimeLoading && !runtimeSkills.length ? (
              <div className="studio-list-note studio-loading"><Spin size="small" /> 正在读取 Runtime Skills...</div>
            ) : runtimeError ? (
              <p className="studio-list-note">{runtimeErrorLabel(runtimeError)}</p>
            ) : runtimeSkills.length ? runtimeSkills.map((item) => (
              <button
                key={item.runtimeName}
                className={`skill-select runtime-select ${editorMode === 'runtime' && item.runtimeName === activeRuntimeId ? 'is-active' : ''}`}
                onClick={() => { setEditorMode('runtime'); setActiveRuntimeId(item.runtimeName); }}
              >
                <span><strong>{item.title || item.runtimeName}</strong><small>{item.path || item.runtimeName}</small></span>
                <i className={item.status === 'installed' ? 'is-enabled' : ''} />
              </button>
            )) : <p className="studio-list-note">当前未读取到 Runtime Skill</p>}
          </div>

          <div className="studio-list-divider">
            <span>Devices</span>
            <button onClick={refreshDevices} aria-label="刷新 Device Manifests" disabled={runtimeDevicesLoading}><ReloadOutlined /></button>
          </div>
          <div className="studio-asset-list">
            {runtimeDevicesLoading && !runtimeDevices.length ? (
              <div className="studio-list-note studio-loading"><Spin size="small" /> 正在读取 Device Manifests...</div>
            ) : runtimeDeviceError ? (
              <p className="studio-list-note">{runtimeErrorLabel(runtimeDeviceError)}</p>
            ) : runtimeDevices.length ? runtimeDevices.map((item) => (
              <button
                key={item.manifestName}
                className={`skill-select runtime-select ${editorMode === 'device' && item.manifestName === activeRuntimeDeviceId ? 'is-active' : ''}`}
                onClick={() => { setEditorMode('device'); setActiveRuntimeDeviceId(item.manifestName); }}
              >
                <span><strong>{item.manifestName}</strong><small>{item.protocol} · {item.role}</small></span>
                <i className={item.content ? 'is-enabled' : ''} />
              </button>
            )) : <p className="studio-list-note">当前未读取到 Device Manifest</p>}
          </div>
        </aside>
        <section className="studio-editor">
          {editorMode === 'draft' ? (
            <>
              <div className="section-heading">
                <div><span>Draft Editor</span><h2>{active?.name || '选择一个 Skill 草稿'}</h2></div>
                <div className="section-actions">
                  <Button icon={<SaveOutlined />} onClick={saveSkills} loading={saving} disabled={!canEdit}>保存草稿</Button>
                  <Button type="primary" icon={<CloudUploadOutlined />} onClick={install} loading={installing} disabled={!canEdit || !active}>安装到 Runtime</Button>
                </div>
              </div>
              {active ? (
                <Form layout="vertical" className="skill-form">
                  <div className="field-grid">
                    <Form.Item label="名称"><Input value={active.name} onChange={(event) => patchSkill({ name: event.target.value })} disabled={!canEdit} /></Form.Item>
                    <Form.Item label="作用域"><Input value={active.scope} onChange={(event) => patchSkill({ scope: event.target.value })} disabled={!canEdit} /></Form.Item>
                  </div>
                  <div className="field-grid">
                    <Form.Item label="触发条件"><Input value={active.trigger} onChange={(event) => patchSkill({ trigger: event.target.value })} disabled={!canEdit} /></Form.Item>
                    <Form.Item label="启用"><Switch checked={active.enabled} onChange={(value) => patchSkill({ enabled: value })} disabled={!canEdit} /></Form.Item>
                  </div>
                  <Form.Item label="策略要求"><Input value={active.policy} onChange={(event) => patchSkill({ policy: event.target.value })} disabled={!canEdit} /></Form.Item>
                  <Form.Item label="Skill 内容"><Input.TextArea rows={9} value={active.prompt} onChange={(event) => patchSkill({ prompt: event.target.value })} disabled={!canEdit} /></Form.Item>
                </Form>
              ) : <Empty description="没有可编辑的 Skill 草稿" />}
            </>
          ) : editorMode === 'runtime' ? (
            <>
              <div className="section-heading">
                <div><span>Runtime Editor</span><h2>{activeRuntime?.title || '选择一个 Runtime Skill'}</h2></div>
                <div className="section-actions">
                  <Button icon={<ReloadOutlined />} onClick={refreshRuntime} loading={runtimeLoading}>刷新</Button>
                  <Button type="primary" icon={<SaveOutlined />} onClick={saveRuntimeSkill} loading={savingRuntime} disabled={!canEdit || !activeRuntime || runtimeDetailLoading || Boolean(runtimeDetailError)}>保存到上位机/板端</Button>
                </div>
              </div>
              {activeRuntime ? (
                <Form layout="vertical" className="skill-form">
                  <div className="field-grid">
                    <Form.Item label="运行时文件名"><Input value={activeRuntime.runtimeName} disabled /></Form.Item>
                    <Form.Item label="标题"><Input value={activeRuntime.title} onChange={(event) => patchRuntimeSkill({ title: event.target.value })} disabled={!canEdit} /></Form.Item>
                  </div>
                  <div className="runtime-detail-grid">
                    <span>Source：{activeRuntime.source === 'mqtt' ? '无线 MQTT' : activeRuntime.source}</span>
                    <span>Status：{activeRuntime.status}</span>
                    <span>Path：{activeRuntime.path}</span>
                  </div>
                  {activeRuntime.lastMessage ? <p className="studio-list-note">{activeRuntime.lastMessage}</p> : null}
                  {runtimeDetailLoading ? <div className="runtime-detail-loading"><Spin size="small" /> 正在从设备读取 Skill 正文...</div> : null}
                  {runtimeDetailError ? <Alert type="error" showIcon message="Runtime Skill 正文读取失败" description={runtimeErrorLabel(runtimeDetailError)} /> : null}
                  <Form.Item label="Runtime Skill Markdown">
                    <Input.TextArea
                      rows={16}
                      value={activeRuntime.content || ''}
                      onChange={(event) => patchRuntimeSkill({ content: event.target.value })}
                      disabled={!canEdit}
                      placeholder={'# Skill 标题\n\n写入该 skill 的完整 Markdown 内容。'}
                    />
                  </Form.Item>
                </Form>
              ) : <Empty description="没有可编辑的 Runtime Skill" />}
            </>
          ) : (
            <>
              <div className="section-heading">
                <div><span>Device Manifest</span><h2>{activeRuntimeDevice?.manifestName || '选择一个 Device Manifest'}</h2></div>
                <div className="section-actions">
                  <Button icon={<ReloadOutlined />} onClick={refresh}>刷新</Button>
                  <Button type="primary" icon={<SaveOutlined />} onClick={saveRuntimeDevice} loading={savingRuntimeDevice} disabled={!canEdit || !activeRuntimeDevice}>保存到上位机/板端</Button>
                </div>
              </div>
              {activeRuntimeDevice ? (
                <Form layout="vertical" className="skill-form">
                  <div className="field-grid">
                    <Form.Item label="Manifest 文件名"><Input value={activeRuntimeDevice.manifestName} disabled /></Form.Item>
                    <Form.Item label="协议"><Input value={activeRuntimeDevice.protocol} disabled /></Form.Item>
                  </div>
                  <div className="runtime-detail-grid">
                    <span>Source：{activeRuntimeDevice.source}</span>
                    <span>Role：{activeRuntimeDevice.role}</span>
                    <span>Risk：{activeRuntimeDevice.risk}</span>
                    <span>Path：{activeRuntimeDevice.path}</span>
                    <span>Signature：{activeRuntimeDevice.signaturePath}</span>
                    <span>Status：{activeRuntimeDevice.status}</span>
                  </div>
                  {activeRuntimeDevice.lastMessage ? <p className="studio-list-note">{activeRuntimeDevice.lastMessage}</p> : null}
                  <Form.Item label="Device Manifest JSON">
                    <Input.TextArea
                      rows={20}
                      value={activeRuntimeDevice.content || ''}
                      onChange={(event) => patchRuntimeDevice({ content: event.target.value })}
                      disabled={!canEdit}
                      placeholder={'{\n  "manifest_version": 1,\n  "name": "aht20_manifest",\n  "protocol": "i2c"\n}'}
                    />
                  </Form.Item>
                </Form>
              ) : <Empty description="没有可编辑的 Device Manifest" />}
            </>
          )}
        </section>
      </div>
      <section className="runtime-band">
        <div><span>Runtime Assets</span><strong>{runtimeSourceLabel} / {runtimeDeviceSourceLabel}</strong></div>
        <div className="runtime-list">
          {runtimeError ? <em>{runtimeErrorLabel(runtimeError)}</em> : runtimeSkills.length ? runtimeSkills.slice(0, 8).map((item) => <span key={item.id || item.runtimeName}>{item.title}<small>{item.status}</small></span>) : <em>未读取到 Runtime Skill</em>}
          {runtimeDeviceError ? <em>{runtimeErrorLabel(runtimeDeviceError)}</em> : runtimeDevices.length ? runtimeDevices.slice(0, 8).map((item) => <span key={item.id || item.manifestName}>{item.manifestName}<small>{item.protocol}</small></span>) : <em>未读取到 Device Manifest</em>}
        </div>
        <Button icon={<ReloadOutlined />} onClick={refresh} loading={runtimeLoading || runtimeDevicesLoading}>刷新</Button>
      </section>
    </div>
  );
}

interface PreferencesProps {
  preferences: UserPreferenceProfile;
  setPreferences: (update: (current: UserPreferenceProfile) => UserPreferenceProfile) => void;
  save: () => void;
  saving: boolean;
  username: string;
  role: string;
}

export function PreferencesView({ preferences, setPreferences, save, saving, username, role }: PreferencesProps) {
  return (
    <div className="preferences-grid view-enter">
      <section className="workspace-surface preference-form">
        <div className="section-heading"><div><span>Behavior</span><h2>用户偏好</h2></div><Button type="primary" icon={<SaveOutlined />} onClick={save} loading={saving}>保存偏好</Button></div>
        <Form layout="vertical"><div className="field-grid"><Form.Item label="首选通道"><Select value={preferences.preferredChannel} onChange={(value) => setPreferences((current) => ({ ...current, preferredChannel: value }))} options={['Web Console', 'ESP32-P4', 'Android', 'Feishu'].map((value) => ({ value }))} /></Form.Item><Form.Item label="隐私模式"><Select value={preferences.privacyMode} onChange={(value) => setPreferences((current) => ({ ...current, privacyMode: value }))} options={['metadata_only', 'balanced', 'full_context'].map((value) => ({ value }))} /></Form.Item></div><Form.Item label={`自动化等级 ${preferences.automationAggressiveness}%`}><Slider value={preferences.automationAggressiveness} onChange={(value) => setPreferences((current) => ({ ...current, automationAggressiveness: value }))} /></Form.Item><div className="field-grid"><Form.Item label="摘要风格"><Select value={preferences.summaryStyle} onChange={(value) => setPreferences((current) => ({ ...current, summaryStyle: value }))} options={['concise', 'standard', 'detailed'].map((value) => ({ value }))} /></Form.Item><Form.Item label="语言"><Input value={preferences.preferredLanguage} onChange={(event) => setPreferences((current) => ({ ...current, preferredLanguage: event.target.value }))} /></Form.Item></div></Form>
      </section>
      <aside className="preference-preview">
        <span>当前配置</span><h2>{username}</h2><small>{role}</small>
        <dl><div><dt>交互通道</dt><dd>{preferences.preferredChannel}</dd></div><div><dt>隐私模式</dt><dd>{preferences.privacyMode}</dd></div><div><dt>自动化等级</dt><dd>{preferences.automationAggressiveness}%</dd></div><div><dt>摘要风格</dt><dd>{preferences.summaryStyle}</dd></div></dl>
      </aside>
    </div>
  );
}
