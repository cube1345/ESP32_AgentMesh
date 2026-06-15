#!/usr/bin/env bash
# ESPAgent Deployment Validator
# Usage: ./skills/deploy/scripts/validate.sh
#
# Checks that all prerequisites are met before building.

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass() { echo -e "  ${GREEN}✓${NC} $*"; }
fail() { echo -e "  ${RED}✗${NC} $*"; ERRORS=$((ERRORS + 1)); }
warn() { echo -e "  ${YELLOW}!${NC} $*"; }

ERRORS=0

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$PROJECT_ROOT"

echo "ESPAgent Deployment Validator"
echo "============================="
echo ""

# 1. ESP-IDF
echo "ESP-IDF:"
if command -v idf.py &>/dev/null; then
    VER=$(idf.py --version 2>&1 | head -1)
    pass "idf.py found: $VER"
else
    fail "idf.py not found — source \$IDF_PATH/export.sh"
fi

# 2. Project files
echo "Project:"
if [ -f main/espagent_config.h ]; then
    pass "main/espagent_config.h exists"
else
    fail "main/espagent_config.h missing — wrong directory?"
fi

if [ -f partitions.csv ]; then
    pass "partitions.csv exists"
else
    fail "partitions.csv missing"
fi

# 3. Secrets
echo "Secrets:"
if [ -f main/espagent_secrets.h ]; then
    pass "main/espagent_secrets.h exists"

    # Check individual fields
    if grep -q 'ESPAGENT_SECRET_WIFI_SSID.*""' main/espagent_secrets.h; then
        fail "WiFi SSID is empty"
    else
        pass "WiFi SSID configured"
    fi

    if grep -q 'ESPAGENT_SECRET_FEISHU_APP_ID.*""' main/espagent_secrets.h ||
       grep -q 'ESPAGENT_SECRET_FEISHU_APP_SECRET.*""' main/espagent_secrets.h; then
        warn "Feishu credentials are empty (Feishu channel will be disabled)"
    else
        pass "Feishu credentials configured"
    fi

    if grep -q 'ESPAGENT_SECRET_API_KEY.*""' main/espagent_secrets.h; then
        fail "LLM API key is empty"
    else
        pass "LLM API key configured"
    fi

    if grep -q 'ESPAGENT_SECRET_TAVILY_KEY.*""' main/espagent_secrets.h &&
       grep -q 'ESPAGENT_SECRET_SEARCH_KEY.*""' main/espagent_secrets.h; then
        warn "No Tavily or Brave Search key set (web_search will be unavailable)"
    else
        pass "Search key configured"
    fi
else
    fail "main/espagent_secrets.h missing — run: cp main/espagent_secrets.h.example main/espagent_secrets.h"
fi

# 4. Serial port
echo "Hardware:"
PORTS=""
if [ "$(uname)" = "Darwin" ]; then
    PORTS=$(ls /dev/cu.usbmodem* 2>/dev/null || true)
else
    PORTS=$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || true)
fi

if [ -n "$PORTS" ]; then
    pass "Serial port found: $(echo "$PORTS" | head -1)"
else
    warn "No ESP32 serial port detected (plug in the board to flash)"
fi

# Summary
echo ""
if [ $ERRORS -eq 0 ]; then
    echo -e "${GREEN}All checks passed!${NC} Ready to build and flash."
    echo "  Run: ./skills/deploy/scripts/deploy.sh"
else
    echo -e "${RED}$ERRORS issue(s) found.${NC} Fix them before deploying."
fi
