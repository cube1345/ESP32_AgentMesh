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
import type { RefObject } from 'react';
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
              <Input
                value={chatInput}
                onChange={(event) => setChatInput(event.target.value)}
                onPressEnter={send}
                placeholder={connected ? '向 ESP32 Agent 发送消息' : '连接后发送消息'}
                disabled={!canUseChat}
              />
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
