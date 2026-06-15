# Tool Setup Guides

Configuration guides for ESPAgent's external service integrations.

Related local setup guide:

- [Wi-Fi AP Onboarding Guide](../WIFI_ONBOARDING_AP.md) — configure firmware builds that expose the local `ESPAgent-XXXX` onboarding/admin access point

## Guides

| Guide | Service | Description |
|-------|---------|-------------|
| [Tavily Setup](TAVILY_SETUP.md) | [Tavily](https://tavily.com) | Web search API — preferred search provider for the `web_search` tool |
| [Amap Weather Setup](AMAP_WEATHER_SETUP.md) | Amap WebService | Structured weather lookup for the `get_weather` tool |

## Overview

ESPAgent integrates with external services to extend its capabilities. Each guide below walks through obtaining API credentials, configuring ESPAgent (build-time or runtime), and verifying the integration.

All credentials can be set in two ways:

1. **Build-time** — define in `main/espagent_secrets.h` and rebuild
2. **Runtime** — use serial CLI commands (saved to NVS flash, no rebuild needed)

See [espagent_secrets.h.example](../../main/espagent_secrets.h.example) for the full list of configurable secrets.
