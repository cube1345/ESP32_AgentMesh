import {
  CheckCircleFilled,
  DisconnectOutlined,
  LinkOutlined,
  MessageOutlined,
  SendOutlined,
  SettingOutlined,
  WarningFilled
} from '@ant-design/icons';
import { Button, Drawer, Input, Tag } from 'antd';
import { useMemo, useState } from 'react';
import type { KeyboardEvent, RefObject } from 'react';
import type { DashboardPayload, TimelineEvent } from '../types';

export interface ChatMessage {
  id: string;
  role: 'user' | 'assistant' | 'system';
  tone?: 'info' | 'success' | 'warning' | 'error';
  text: string;
  time: string;
}

interface ChatWorkspaceProps {
  open: boolean;
  onClose: () => void;
  username: string;
  role: string;
  canUseChat: boolean;
  wsUrl: string;
  setWsUrl: (value: string) => void;
  chatId: string;
  setChatId: (value: string) => void;
  connected: boolean;
  connect: () => void;
  disconnect: () => void;
  chatInput: string;
  setChatInput: (value: string) => void;
  send: () => void;
  messages: ChatMessage[];
  timeline: TimelineEvent[];
  chatGateway: DashboardPayload['chatGateway'];
  scrollRef: RefObject<HTMLDivElement>;
}

interface SlashCommandOption {
  command: string;
  description: string;
  needsPrompt?: boolean;
}

const slashCommands: SlashCommandOption[] = [
  { command: '/help', description: '查看可用命令和能力' },
  { command: '/clear', description: '清除当前会话上下文' },
  { command: '/skills', description: '展示可用 Skills' },
  { command: '/skills_list', description: '列出 Runtime Skills' },
  { command: '/skills_show', description: '查看指定 Skill', needsPrompt: true },
  { command: '/sensor', description: '显式路由到感知角色', needsPrompt: true },
  { command: '/control', description: '显式路由到执行角色', needsPrompt: true },
  { command: '/guardian', description: '显式路由到安全审查角色', needsPrompt: true },
  { command: '/subagent', description: '调用子代理处理任务', needsPrompt: true },
  { command: '/workflow', description: '创建或查看多步任务', needsPrompt: true },
  { command: '/rule', description: '创建或查看条件任务', needsPrompt: true },
  { command: '/mesh', description: '显式执行 Mesh 调度', needsPrompt: true },
  { command: '/status', description: '查看系统状态' },
  { command: '/device', description: '查看运行时设备' },
  { command: '/profile', description: '查看或修改用户偏好', needsPrompt: true },
  { command: '/privacy', description: '查看隐私与权限策略' },
  { command: '/lua', description: '查看 Lua 运行时能力', needsPrompt: true },
  { command: '/trace', description: '查看最近执行链路' },
  { command: '/ota', description: '查看 OTA 计划能力' },
  { command: '/clear_all_memory', description: '清除全部长期记忆' }
];

function messageClass(message: ChatMessage): string {
  if (message.role === 'user') return 'chat-message chat-message-user';
  if (message.role === 'assistant') return 'chat-message chat-message-agent';
  return `chat-message chat-message-system tone-${message.tone || 'info'}`;
}

export default function ChatWorkspace(props: ChatWorkspaceProps) {
  const {
    open,
    onClose,
    username,
    role,
    canUseChat,
    wsUrl,
    setWsUrl,
    chatId,
    setChatId,
    connected,
    connect,
    disconnect,
    chatInput,
    setChatInput,
    send,
    messages,
    timeline,
    chatGateway,
    scrollRef
  } = props;

  const [highlightedSlashIndex, setHighlightedSlashIndex] = useState(0);
  const slashQueryMatch = chatInput.match(/^\/([^\s]*)$/);
  const slashSuggestions = useMemo(() => {
    if (!slashQueryMatch) return [];
    const query = slashQueryMatch[1].toLowerCase();
    return slashCommands
      .map((item) => {
        const command = item.command.toLowerCase();
        let score = 0;
        if (query && command === `/${query}`) score = 100;
        else if (query && command.startsWith(`/${query}`)) score = 80;
        else if (query && command.includes(query)) score = 40;
        else if (!query) score = 10;
        return { ...item, score };
      })
      .filter((item) => item.score > 0)
      .sort((a, b) => b.score - a.score || a.command.length - b.command.length || a.command.localeCompare(b.command))
      .slice(0, 5);
  }, [chatInput, slashQueryMatch]);
  const showSlashSuggestions = slashSuggestions.length > 0;

  function applySlashSuggestion(option: SlashCommandOption) {
    setChatInput(option.needsPrompt ? `${option.command} ` : option.command);
    setHighlightedSlashIndex(0);
  }

  function handleInputChange(value: string) {
    setChatInput(value);
    setHighlightedSlashIndex(0);
  }

  function handleInputKeyDown(event: KeyboardEvent<HTMLInputElement>) {
    if (showSlashSuggestions) {
      if (event.key === 'ArrowDown') {
        event.preventDefault();
        setHighlightedSlashIndex((current) => (current + 1) % slashSuggestions.length);
        return;
      }
      if (event.key === 'ArrowUp') {
        event.preventDefault();
        setHighlightedSlashIndex((current) => (current - 1 + slashSuggestions.length) % slashSuggestions.length);
        return;
      }
      if (event.key === 'Tab' || event.key === 'Enter') {
        event.preventDefault();
        applySlashSuggestion(slashSuggestions[Math.min(highlightedSlashIndex, slashSuggestions.length - 1)]);
        return;
      }
      if (event.key === 'Escape') {
        event.preventDefault();
        setChatInput('');
        setHighlightedSlashIndex(0);
        return;
      }
    }
    if (event.key === 'Enter') {
      send();
    }
  }

  return (
    <Drawer
      className="chat-drawer"
      open={open}
      onClose={onClose}
      placement="right"
      width="min(780px, 100vw)"
      title={(
        <div className="drawer-title">
          <span className="drawer-icon"><MessageOutlined /></span>
          <div>
            <strong>Agent 通信</strong>
            <span>{username} · {role}</span>
          </div>
        </div>
      )}
      extra={(
        <Tag color={connected ? 'success' : 'default'}>
          {connected ? <CheckCircleFilled /> : <WarningFilled />} {connected ? '已连接' : '未连接'}
        </Tag>
      )}
    >
      <div className="chat-workspace">
        <details className="connection-settings">
          <summary><SettingOutlined /> 连接设置</summary>
          <div className="connection-grid">
            <label>
              <span>Gateway WebSocket</span>
              <Input value={wsUrl} onChange={(event) => setWsUrl(event.target.value)} />
            </label>
            <label>
              <span>Chat ID</span>
              <Input value={chatId} onChange={(event) => setChatId(event.target.value)} />
            </label>
            <div className="connection-actions">
              <Button type="primary" icon={<LinkOutlined />} onClick={connect} disabled={!canUseChat}>连接</Button>
              <Button icon={<DisconnectOutlined />} onClick={disconnect}>断开</Button>
            </div>
          </div>
        </details>

        <div className="chat-main-grid">
          <section className="conversation-pane" aria-label="Agent 会话">
            <div ref={scrollRef} className="conversation-scroll panel-scroll">
              {messages.map((item) => (
                <div key={item.id} className={messageClass(item)}>
                  <div className="chat-message-meta">
                    <span>{item.role === 'user' ? '你' : item.role === 'assistant' ? 'ESPAgent' : '系统'}</span>
                    <time>{item.time}</time>
                  </div>
                  <div>{item.text}</div>
                </div>
              ))}
            </div>
            <div className="chat-composer">
              <div className="chat-input-wrap">
                {showSlashSuggestions ? (
                  <div className="slash-command-menu" role="listbox" aria-label="斜杠命令候选">
                    {slashSuggestions.map((item, index) => (
                      <button
                        type="button"
                        key={item.command}
                        className={index === highlightedSlashIndex ? 'is-active' : ''}
                        onMouseDown={(event) => {
                          event.preventDefault();
                          applySlashSuggestion(item);
                        }}
                      >
                        <strong>{item.command}</strong>
                        <span>{item.description}</span>
                      </button>
                    ))}
                  </div>
                ) : null}
                <Input
                  value={chatInput}
                  onChange={(event) => handleInputChange(event.target.value)}
                  onKeyDown={handleInputKeyDown}
                  placeholder={connected ? '向 ESP32 Agent 发送消息，输入 / 可选择命令' : '连接后发送消息'}
                  disabled={!canUseChat}
                />
              </div>
              <Button type="primary" icon={<SendOutlined />} onClick={send} disabled={!canUseChat || !connected} aria-label="发送消息" />
            </div>
          </section>

          <aside className="conversation-context">
            <div className="context-status">
              <span>上游网关</span>
              <strong>{chatGateway?.enabled ? '已启用' : '未启用'}</strong>
              <small>{chatGateway?.lastError || chatGateway?.lastEventAt || '暂无事件'}</small>
            </div>
            <div className="context-heading">相关事件</div>
            <div className="compact-timeline panel-scroll">
              {timeline.slice(0, 8).map((event, index) => (
                <div className="compact-event" key={`${event.time}-${event.stage}-${event.source}-${index}`}>
                  <span className={`event-dot status-${event.status}`} />
                  <div>
                    <strong>{event.stage}</strong>
                    <small>{event.source} → {event.target}</small>
                    <p>{event.payload}</p>
                  </div>
                </div>
              ))}
            </div>
          </aside>
        </div>
      </div>
    </Drawer>
  );
}
