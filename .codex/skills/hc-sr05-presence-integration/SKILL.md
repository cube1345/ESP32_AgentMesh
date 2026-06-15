---
name: hc-sr05-presence-integration
description: Integrate or debug HC-SR05 ultrasonic modules on ESP32 or ESPAgent, especially when using distance thresholds for human presence, obstacle proximity, or chat-callable presence sensing. Use when wiring Trig and Echo pins, protecting ESP32 inputs from 5V Echo, timing ultrasonic pulses, filtering false detections, or clarifying confusion with HC-SR501 and HC-SR505 PIR human body sensors.
---

# Hc Sr05 Presence Integration

Use this skill when a task mentions `HC-SR05`, ultrasonic ranging, Trig/Echo distance sensing, or using a nearby-object distance threshold as a simple human-presence signal.

## Identify The Module First

- `HC-SR05` is normally an ultrasonic ranging module, similar to HC-SR04. It detects distance, not body heat.
- Common PIR "human body" modules are `HC-SR501` and `HC-SR505`. They expose a digital motion output and do not use Trig/Echo timing.
- If the user says "人体传感器 HC-SR05", treat it as likely ultrasonic unless the board markings or pins show PIR-style `VCC/GND/OUT`.
- Ask for a photo or pin labels only when the requested implementation depends on the exact board variant.

## Electrical Rules

- Power the HC-SR05 from `5V` unless the specific board is documented for `3.3V`.
- Share `GND` with the ESP32.
- Drive `Trig` from an ESP32 GPIO; a 3.3V high is usually accepted by common modules.
- Protect ESP32 `Echo`: many HC-SR05 boards output a 5V pulse. Use a resistor divider or level shifter before connecting to an ESP32 GPIO.
- Avoid ESP32 boot strap pins for Trig/Echo unless the board design already proves they are safe.

## Timing Pattern

1. Hold `Trig` low for a short idle period.
2. Set `Trig` high for about `10 us`.
3. Set `Trig` low.
4. Measure the high pulse width on `Echo`.
5. Convert pulse width to distance:
   `distance_cm = echo_high_us / 58.0`

Use a bounded timeout, commonly around `25-35 ms`, so missing echoes never block the agent loop.

## Presence Detection Pattern

Do not report raw single-sample distance as "human present". Use filtering:

- sample repeatedly at a modest interval, for example `5-10 Hz`
- discard timeout, zero, and out-of-range values
- median or moving average the last few valid readings
- use threshold plus hysteresis, for example present below `80 cm`, absent above `100 cm`
- require several consecutive present or absent samples before changing state
- expose both `present` and `distance_cm` when returning a tool result

This makes the behavior usable for a nearby hand, person, or obstacle while being honest that ultrasonic sensing cannot confirm a human specifically.

## ESP-IDF Implementation Guidance

- Use GPIO output for `Trig` and GPIO input for `Echo`.
- Prefer `esp_timer_get_time()` for microsecond timestamps in a simple blocking measurement.
- Keep each read bounded by timeout loops; return `timeout` or `out_of_range` explicitly.
- If high-rate or low-jitter measurement becomes important, consider RMT or interrupt capture, but do not start there for ESPAgent agent tools.
- Keep the hardware-facing code separate from the chat-facing tool wrapper.

## ESPAgent Touch Points

For this repo, follow the existing hardware tool pattern:

- defaults and pin configuration:
  [`../../../main/espagent_config.h`](../../../main/espagent_config.h)
- tool registration and schema:
  [`../../../main/tools/tool_registry.c`](../../../main/tools/tool_registry.c)
- direct hardware implementation:
  add a focused file near existing tool implementations, for example `main/tools/tool_hc_sr05.c`
- prompt/tool preference:
  [`../../../main/agent/context_builder.c`](../../../main/agent/context_builder.c)

Suggested chat-facing tool name: `read_presence`.

Suggested lower-level technical tool name: `hc_sr05_read_distance`.

Return concise results such as:

```text
present=true distance_cm=42.6 source=hc-sr05 threshold_cm=80
```

## PIR Variant Fallback

If the board is actually `HC-SR501` or `HC-SR505`:

- wire `VCC`, `GND`, and digital `OUT`
- read `OUT` as a GPIO input
- expect a warm-up period after power-on
- handle retrigger and hold-time behavior from the module hardware
- do not use ultrasonic Trig/Echo code

For PIR modules, expose results as motion or occupancy-like signals, not distance.
