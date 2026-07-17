import { Empty, Segmented } from 'antd';
import { useState } from 'react';
import {
  CartesianGrid,
  Legend,
  Line,
  LineChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis
} from 'recharts';
import type { EnvironmentHistoryPoint } from '../types';

type ChartMode = 'comfort' | 'air' | 'light';
type MetricKey = Exclude<keyof EnvironmentHistoryPoint, 'timestamp' | 'nodeId'>;

interface SeriesDefinition {
  key: MetricKey;
  name: string;
  color: string;
  axis: 'left' | 'right';
}

const modeOptions = [
  { label: '温湿度', value: 'comfort' },
  { label: '空气质量', value: 'air' },
  { label: '光照与存在', value: 'light' }
];

const seriesByMode: Record<ChartMode, SeriesDefinition[]> = {
  comfort: [
    { key: 'temperatureC', name: '温度 (°C)', color: '#d85745', axis: 'left' },
    { key: 'humidityPercent', name: '湿度 (%)', color: '#2f6fed', axis: 'right' }
  ],
  air: [
    { key: 'eco2Ppm', name: 'eCO2 (ppm)', color: '#16866d', axis: 'left' },
    { key: 'tvocPpb', name: 'TVOC (ppb)', color: '#d48323', axis: 'right' }
  ],
  light: [
    { key: 'lightLux', name: '光照 (lux)', color: '#b58b11', axis: 'left' },
    { key: 'presence', name: '人体存在', color: '#7867a8', axis: 'right' }
  ]
};

function formatTime(value: string): string {
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return value;
  return date.toLocaleTimeString('zh-CN', {
    hour12: false,
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit'
  });
}

export default function EnvironmentHistoryChart({ points }: { points: EnvironmentHistoryPoint[] }) {
  const [mode, setMode] = useState<ChartMode>('comfort');
  const series = seriesByMode[mode];
  const rightDomain: [number | 'auto', number | 'auto'] = mode === 'light'
    ? [0, 1]
    : ['auto', 'auto'];

  return (
    <div className="environment-chart">
      <div className="environment-chart-toolbar">
        <Segmented
          value={mode}
          options={modeOptions}
          onChange={(value) => setMode(value as ChartMode)}
        />
        <span>{points.length} 个采样点</span>
      </div>
      {points.length < 2 ? (
        <Empty image={Empty.PRESENTED_IMAGE_SIMPLE} description="等待更多环境采样" />
      ) : (
        <div className="environment-chart-canvas" role="img" aria-label="环境数据时间趋势图">
          <ResponsiveContainer width="100%" height="100%">
            <LineChart data={points} margin={{ top: 8, right: 12, bottom: 4, left: 0 }}>
              <CartesianGrid stroke="#e4e9e6" strokeDasharray="3 4" vertical={false} />
              <XAxis
                dataKey="timestamp"
                tickFormatter={formatTime}
                minTickGap={30}
                tick={{ fill: '#78837d', fontSize: 10 }}
                axisLine={{ stroke: '#cfd7d3' }}
                tickLine={false}
              />
              <YAxis
                yAxisId="left"
                width={48}
                tick={{ fill: '#78837d', fontSize: 10 }}
                axisLine={false}
                tickLine={false}
                domain={['auto', 'auto']}
              />
              <YAxis
                yAxisId="right"
                orientation="right"
                width={48}
                tick={{ fill: '#78837d', fontSize: 10 }}
                axisLine={false}
                tickLine={false}
                domain={rightDomain}
                allowDecimals={mode !== 'light'}
              />
              <Tooltip
                labelFormatter={(label) => formatTime(String(label))}
                contentStyle={{ border: '1px solid #d8dfdc', borderRadius: 6, fontSize: 11 }}
              />
              <Legend verticalAlign="top" align="right" height={34} iconType="plainline" />
              {series.map((item) => (
                <Line
                  key={item.key}
                  type="monotone"
                  dataKey={item.key}
                  name={item.name}
                  yAxisId={item.axis}
                  stroke={item.color}
                  strokeWidth={2}
                  dot={false}
                  activeDot={{ r: 3 }}
                  connectNulls
                  isAnimationActive={false}
                />
              ))}
            </LineChart>
          </ResponsiveContainer>
        </div>
      )}
    </div>
  );
}
