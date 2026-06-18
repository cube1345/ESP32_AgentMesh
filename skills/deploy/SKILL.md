--- name: deploy
description: Deploy ESPAgent firmware to an ESP32-S3 board. Covers prerequisites, configuration, build, flash, verification, and troubleshooting.
---

# Deploy ESPAgent

End-to-end guide for deploying ESPAgent to an ESP32-S3 dev board.

## Prerequisites

### Hardware
- ESP32-S3 dev board with **16 MB flash + 8 MB PSRAM** (e.g. Xiaozhi AI board, ~$5-10)
- USB Type-C data cable (not charge-only)

### Software
- **ESP-IDF v5.5+** installed and working
  ```bash
  # Install: https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/
  # Verify:
  idf.py --version   # should show >= 5.5
  ```

### Credentials (get these first)
- **WiFi SSID + password** — the network the ESP32 will connect to
- **Feishu App ID + App Secret** — from the Feishu/Lark developer console
- **LLM API Key** — from your configured model provider
- *(Optional)* Tavily or Brave Search API key — for the `web_search` tool
- *(Optional)* HTTP proxy host:port — if your network requires one

## Step 1: Clone and Set Target

```bash
git clone https://github.com/memovai/ESPAgent.git
cd ESPAgent
idf.py set-target esp32s3
```

## Step 2: Configure Secrets

```bash
cp main/espagent_secrets.h.example main/espagent_secrets.h
```

Edit `main/espagent_secrets.h` — fill in ALL required fields:

```c
#define ESPAGENT_SECRET_WIFI_SSID       "YourWiFiName"        // REQUIRED
#define ESPAGENT_SECRET_WIFI_PASS       "YourWiFiPassword"     // REQUIRED
#define ESPAGENT_SECRET_FEISHU_APP_ID   "cli_xxxxxxxxxxxxxx"   // REQUIRED for Feishu
#define ESPAGENT_SECRET_FEISHU_APP_SECRET "xxxxxxxxxxxxxxxx"    // REQUIRED for Feishu
#define ESPAGENT_SECRET_API_KEY         "sk-..."               // REQUIRED
#define ESPAGENT_SECRET_MODEL           ""                     // optional, defaults to claude-opus-4-5
#define ESPAGENT_SECRET_TAVILY_KEY      ""                     // optional: Tavily Search API key
#define ESPAGENT_SECRET_SEARCH_KEY      ""                     // optional: Brave Search API key fallback
#define ESPAGENT_SECRET_PROXY_HOST      ""                     // optional: e.g. "192.168.1.83"
#define ESPAGENT_SECRET_PROXY_PORT      ""                     // optional: e.g. "7897"
```

**Proxy setup:**
If you need a proxy to reach Feishu, your LLM provider, or search APIs, set both `PROXY_HOST` and `PROXY_PORT`. The proxy machine must:
- Be on the same LAN as the ESP32
- Support HTTP CONNECT method (Clash, V2Ray, etc.)
- Have "Allow LAN connections" enabled

## Step 3: Build

```bash
idf.py fullclean && idf.py build
```

**IMPORTANT:** Always `fullclean` after changing `espagent_secrets.h` — the secrets are compiled into the binary.

Expected output: `Project build complete. To flash, run: idf.py flash`

### Build Troubleshooting

| Error | Fix |
|-------|-----|
| `espagent_secrets.h: No such file` | Run `cp main/espagent_secrets.h.example main/espagent_secrets.h` |
| `esp_websocket_client not found` | Run `idf.py fullclean` then `idf.py build` (managed component auto-downloads) |
| `Toolchain not found` | Re-run ESP-IDF `install.sh` and `source export.sh` |
| Build runs out of memory | Close other apps, ESP-IDF build needs ~2GB RAM |

## Step 4: Find Serial Port

```bash
# macOS
ls /dev/cu.usb*

# Linux
ls /dev/ttyACM* /dev/ttyUSB*
```

Common ports:
- macOS USB-OTG: `/dev/cu.usbmodem1101` or `/dev/cu.usbmodem11401`
- Linux: `/dev/ttyACM0`

**If no port shows up:**
- Try a different USB cable (must be data cable, not charge-only)
- Try a different USB port
- Check if board has a power LED lit

## Step 5: Flash

```bash
idf.py -p PORT flash monitor
```

Replace `PORT` with your actual port. Example:
```bash
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

The monitor shows boot logs. Look for:
```
I (xxx) ESPAgent: ESPAgent firmware for ESP32-S3
I (xxx) ESPAgent: PSRAM free: ~8000000 bytes
I (xxx) wifi: WiFi connected: 192.168.x.x
I (xxx) feishu: Feishu credentials loaded
I (xxx) ESPAgent: All services started!
```

**Exit monitor:** `Ctrl+]`

## Step 6: Verify

1. Open Feishu/Lark and send a message to the configured ESPAgent bot
2. Send: `Hello`
3. You should see "ESPAgent is processing your request..." followed by a response
4. Send: `What time is it?` — tests the get_current_time tool
5. Send: `Search for latest news about ESP32` — tests web_search if a Tavily or Brave key is set

## Post-Deploy: Runtime Configuration

Connect via serial (`idf.py -p PORT monitor`) and use CLI commands:

```
ESPAgent> config_show                  # see current config
ESPAgent> wifi_set NewSSID NewPass     # change WiFi
ESPAgent> set_feishu_creds cli_xxx xxx # change Feishu credentials
ESPAgent> set_api_key sk-...           # change API key
ESPAgent> set_model claude-sonnet-4-5  # change model
ESPAgent> set_proxy 192.168.1.83 7897  # set proxy
ESPAgent> clear_proxy                  # remove proxy
ESPAgent> heap_info                    # check memory
ESPAgent> restart                      # reboot
```

CLI settings are stored in NVS flash and take priority over build-time values.

## OTA Update (over WiFi)

After initial USB flash, current firmware can update over WiFi only from an HTTPS app `.bin` URL through the serial CLI:

1. Build new firmware: `idf.py build`
2. Publish `build/ESPAgent.bin` at an ESP32-reachable HTTPS URL.
3. On the serial console, check partitions:
   ```
   ota_info
   ```
4. Start OTA:
   ```
   ota_update https://YOUR_HOST/path/ESPAgent.bin
   ```

Current code rejects plain HTTP URLs. Do not expose OTA through Feishu/LLM until Guardian policy, human confirmation, firmware provenance checks, version checks, and role-profile handling are implemented.

## Flash Layout

```
16 MB Flash:
├── 0x009000  NVS (24 KB) — runtime config
├── 0x020000  OTA_0 (2 MB) — active firmware
├── 0x220000  OTA_1 (2 MB) — update slot
├── 0x420000  SPIFFS (12 MB) — memory, sessions, config
└── 0xFF0000  Coredump (64 KB)
```

## Common Issues

| Symptom | Cause | Fix |
|---------|-------|-----|
| No WiFi connection | Wrong SSID/password | Check `espagent_secrets.h`, `idf.py fullclean && build && flash` |
| Feishu not configured | Empty Feishu App ID/Secret | Set via `espagent_secrets.h` or CLI `set_feishu_creds` |
| Bot doesn't respond | API key invalid | Check the configured LLM provider key, set via CLI |
| "Markdown send failed" | Normal with Markdown mode | Non-critical, falls back to plain text |
| Proxy timeout | Proxy not reachable | Ensure same LAN, proxy allows LAN connections |
| SPIFFS mount failed | First boot or corruption | Normal on first boot (auto-formats) |
| Port busy/not found | Wrong port or cable | Try different USB port/cable, check `ls /dev/cu.usb*` |
| Boot loop | Firmware crash | Flash via USB again, check serial logs for crash info |
