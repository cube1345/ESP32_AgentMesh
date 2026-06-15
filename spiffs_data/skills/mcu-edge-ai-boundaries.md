# MCU Edge AI Boundaries

Explain and apply the practical boundary between ESP32-S3 edge intelligence,
cloud LLM reasoning, and future local TinyML inference.

## When to use

Use this when the user asks what AI can run on the MCU, whether the device can
think locally, whether it supports local models, or how ESPAgent should use MCU
resources for AI.

## Current ESPAgent boundary

- ESP32-S3 runs the agent runtime, networking, prompt assembly, tool registry,
  MQTT Mesh, sensors, actuators, cache, memory files, cron, and proactive
  triggers.
- Full LLM reasoning currently runs through remote LLM APIs.
- ReAct happens on the Coordinator MCU as an orchestration loop around remote
  LLM calls and local C tools.
- `spawn_subagent` creates a bounded FreeRTOS subtask on the same MCU, but it
  still uses remote LLM calls and a restricted tool set.
- Do not claim that ESPAgent currently runs a local LLM on ESP32-S3.

## What MCU-side AI should do

- Run deterministic prefilters and rules close to sensors.
- Keep short sensor caches and thresholds on-device.
- Make low-risk local decisions that do not need language reasoning.
- Use TinyML-style models only when a small, validated model is actually
  integrated.
- Send compact facts, events, or anomalies to the Coordinator instead of raw
  unbounded streams.

## Future local inference directions

- TinyML / LiteRT Micro style models for small classification or anomaly
  detection tasks.
- ESP-DL style local inference for supported Espressif chips and optimized
  neural-network operators.
- ESP-SR style local speech wake word or command detection when audio hardware
  is present.

Treat these as roadmap items unless a concrete tool, driver, and model file are
present in the firmware.

## Good answers

- "当前 ESP32-S3 负责 Agent runtime 和工具执行，LLM 仍在云端。"
- "这个任务可以先用本地阈值判断，再把异常事件发给 Coordinator。"
- "如果要做本地模型，需要先确认模型大小、RAM/PSRAM、算子支持和推理延迟。"

## Bad answers

- Do not say "ESP32-S3 already runs the full LLM locally."
- Do not promise image, audio, or sensor model inference unless the model and
  tool path are implemented.
- Do not replace an unsupported hardware request with a nearby tool.
