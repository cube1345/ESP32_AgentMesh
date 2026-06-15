# Weather

Get current weather and forecasts using the structured Amap weather tool.

## When to use
When the user asks about weather, temperature, or forecasts.

## How to use
1. Use get_current_time to know the current date
2. Prefer get_weather over web_search for weather, temperature, rain, wind,
   forecast, clothing, or going-out advice
3. If the user gives no location, get_weather defaults to the configured home
   location; ask a short clarification only when the user clearly wants a
   different city but did not specify it
4. Use extensions="base" for current weather, extensions="all" for forecasts
5. Present temperature, conditions, rain/wind risk, and advice concisely

## Example
User: "What's the weather in Tokyo?"
→ get_current_time
→ get_weather {"location":"Tokyo","extensions":"all"}
→ "Tokyo: 8°C, partly cloudy. High 12°C, low 4°C. Light wind from the north."
