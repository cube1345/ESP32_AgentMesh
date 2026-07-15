# Zhangsan Home Automation Scenes

为张三提供默认智能家居场景模板，用于把自然语言转成多步任务、条件任务或一次性硬件动作。

## 常用场景

- 晨间舒适检查：读取环境数据，汇总温湿度、空气质量、光照，再给出出门建议。
- 回家准备：检查空气质量和湿度；若空气质量偏差，建议打开风扇；若偏干，建议加湿。
- 夜间安静模式：关闭或调暗状态灯，避免非必要提醒。
- 设备可见测试：短时间设置 WS2812 颜色、转动舵机或切换 GPIO6 LED，用于现场演示。

## 多步任务

当张三说“先 A，过 N 秒后 B”时，使用 `automation_create_workflow`。

示例：

```json
[
  {"action":"set_status_light","target_role":"control_agent","args":{"color":"blue"},"delay_ms":0},
  {"action":"set_fan","target_role":"control_agent","args":{"state":1},"delay_ms":10000}
]
```

## 条件任务

当张三说“如果湿度低于 40% 就打开加湿器”时，使用 `automation_create_rule`。当前分支默认一触发就自动删除规则。

推荐触发指标：

- `humidity_percent`
- `temperature_c`

推荐动作：

- `set_humidifier`
- `set_fan`
- `set_status_light`
- `set_device_led`
- `servo_write`

## 边界

- 不要用 LLM 自己等待。
- 不要把一次性动作错误理解成每日 cron。
- 持续或高风险规则需要张三确认。
