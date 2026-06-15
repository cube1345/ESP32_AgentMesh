# Amap Weather Setup

This guide documents ESPAgent's structured weather lookup through Amap
WebService.

## What It Provides

The `get_weather` tool can return:

- current live weather with `extensions="base"`
- multi-day forecast with `extensions="all"`
- default weather for the configured home location
- weather for another city, district, address, or 6-digit Amap adcode

The current default location is Nanjing Qixia District:

```text
location: 南京市栖霞区
adcode: 320113
```

## Build-Time Configuration

Put the private key in `main/espagent_secrets.h`:

```c
#define ESPAGENT_SECRET_AMAP_KEY        "your-amap-webservice-key"
#define ESPAGENT_SECRET_AMAP_DEFAULT_LOCATION "南京市栖霞区"
#define ESPAGENT_SECRET_AMAP_DEFAULT_ADCODE "320113"
```

Do not put real keys in `espagent_secrets.h.example`, README, docs, or commits.

## Runtime Configuration

The serial CLI can save values into NVS:

```text
set_amap_key <key>
set_amap_location 南京市栖霞区 320113
config_show
```

NVS values override build-time defaults until `config_reset` clears them.

## Agent Usage

Typical tool inputs:

```json
{}
```

Uses the default location and live weather.

```json
{"extensions":"all"}
```

Uses the default location and returns forecast weather.

```json
{"location":"北京市海淀区","extensions":"base"}
```

First resolves the location through Amap geocoding, then queries weather.

```json
{"adcode":"320113","extensions":"all"}
```

Directly queries forecast weather for a known adcode.

## Fallback

Use `web_search` only when `get_weather` is not configured or when the user
asks for weather news/context that is not available from the structured Amap
weather response.
