#!/usr/bin/env python3

import argparse
import asyncio
import os
import sys
import uuid

import websockets


async def run(url: str, api_key: str, resource_id: str, timeout_s: float) -> int:
    headers = {
        "X-Api-Key": api_key,
        "X-Api-Resource-Id": resource_id,
        "X-Api-Connect-Id": str(uuid.uuid4()),
        "X-Control-Require-Usage-Tokens-Return": "*",
    }

    try:
        async with websockets.connect(
            url,
            additional_headers=headers,
            max_size=10 * 1024 * 1024,
            open_timeout=timeout_s,
            close_timeout=timeout_s,
        ) as ws:
            logid = ws.response.headers.get("x-tt-logid", "")
            print("handshake_status=ok")
            print(f"logid={logid}")
            return 0
    except Exception as exc:
        print("handshake_status=error")
        print(f"error_type={type(exc).__name__}")
        print(f"error={exc}")
        return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="Volc TTS WebSocket handshake smoke test")
    parser.add_argument(
        "--url",
        default="wss://openspeech.bytedance.com/api/v3/tts/bidirection",
        help="WebSocket URL",
    )
    parser.add_argument("--resource-id", default="seed-tts-2.0", help="X-Api-Resource-Id")
    parser.add_argument("--api-key", default=os.getenv("VOLC_TTS_API_KEY", ""), help="X-Api-Key")
    parser.add_argument("--timeout", type=float, default=15.0, help="Timeout in seconds")
    args = parser.parse_args()

    if not args.api_key:
        print("missing_api_key", file=sys.stderr)
        return 2

    return asyncio.run(run(args.url, args.api_key, args.resource_id, args.timeout))


if __name__ == "__main__":
    raise SystemExit(main())
