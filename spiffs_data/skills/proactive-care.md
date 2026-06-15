# Proactive Care

Set up proactive messages such as daily weather, morning briefings, reminders,
and lightweight check-ins.

## When to use

Use this when the user asks ESPAgent to actively send messages later or on a
routine, for example:

- "每天早上8点告诉我天气"
- "每天关心我一下"
- "晚上提醒我早点休息"
- "每天上午给我一个简短 briefing"

## How to use

1. If the request needs a city, location, or preference that is not known from
   memory, ask one brief clarification question before scheduling.
2. Use `cron_add` with `schedule_type="daily"` for daily local-time jobs.
3. Keep `channel` and `chat_id` omitted unless the user explicitly names a
   destination; the agent loop will patch them to the current conversation.
4. Put the future instruction in `message`, not the final answer.

## Examples

Daily weather:

```json
{
  "name": "daily_weather",
  "schedule_type": "daily",
  "hour": 8,
  "minute": 0,
  "message": "请使用get_weather查询今天本地天气，告诉我当前温度、天气状况，并给一句出门建议。"
}
```

Daily care:

```json
{
  "name": "daily_check_in",
  "schedule_type": "daily",
  "hour": 21,
  "minute": 30,
  "message": "请根据记忆和今天日期，发一条简短自然的关心消息。不要像任务机器人。"
}
```
