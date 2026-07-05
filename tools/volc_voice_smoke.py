#!/usr/bin/env python3
"""
Minimal configurable smoke test for Volcengine / Doubao voice APIs.

Purpose:
- verify endpoint reachability
- verify AppID / token placement
- inspect HTTP status / response headers / body preview

This script is intentionally generic because different Volc voice products
may use different endpoints and auth fields.

Examples:

1) POST JSON
python3 tools/volc_voice_smoke.py \
  --url "https://example.com/api" \
  --method POST \
  --appid "2398722370" \
  --token "YOUR_TOKEN" \
  --auth-mode bearer \
  --json '{"app_id":"2398722370","text":"你好"}'

2) Custom token header
python3 tools/volc_voice_smoke.py \
  --url "https://example.com/api" \
  --method POST \
  --appid "2398722370" \
  --token "YOUR_TOKEN" \
  --token-header "Access-Token" \
  --json '{"text":"hello"}'

3) Load body from file
python3 tools/volc_voice_smoke.py \
  --url "https://example.com/api" \
  --method POST \
  --token "YOUR_TOKEN" \
  --token-header "Authorization" \
  --body-file /tmp/request.json \
  --content-type application/json
"""

from __future__ import annotations

import argparse
import json
import ssl
import sys
import textwrap
import urllib.error
import urllib.request
from pathlib import Path


def mask_secret(value: str, keep: int = 4) -> str:
    if not value:
        return ""
    if len(value) <= keep * 2:
        return "*" * len(value)
    return value[:keep] + "*" * (len(value) - keep * 2) + value[-keep:]


def build_headers(args: argparse.Namespace) -> dict[str, str]:
    headers: dict[str, str] = {}

    if args.content_type:
        headers["Content-Type"] = args.content_type

    if args.appid:
        headers[args.appid_header] = args.appid

    if args.token:
        if args.auth_mode == "bearer":
            headers["Authorization"] = f"Bearer {args.token}"
        elif args.auth_mode == "raw":
            headers["Authorization"] = args.token
        elif args.auth_mode == "header":
            headers[args.token_header] = args.token

    for item in args.header:
        if ":" not in item:
            raise ValueError(f"Invalid --header value: {item!r}")
        k, v = item.split(":", 1)
        headers[k.strip()] = v.strip()

    return headers


def build_body(args: argparse.Namespace) -> bytes | None:
    if args.json and args.body_file:
        raise ValueError("Use either --json or --body-file, not both.")

    if args.json:
        parsed = json.loads(args.json)
        return json.dumps(parsed, ensure_ascii=False).encode("utf-8")

    if args.body_file:
        return Path(args.body_file).read_bytes()

    return None


def response_preview(data: bytes, limit: int = 800) -> str:
    clipped = data[:limit]
    try:
        return clipped.decode("utf-8", errors="replace")
    except Exception:
        return repr(clipped)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generic smoke test for Volc voice HTTP endpoints."
    )
    parser.add_argument("--url", required=True, help="Target API URL")
    parser.add_argument("--method", default="POST", choices=["GET", "POST", "PUT"])
    parser.add_argument("--appid", default="", help="Voice AppID")
    parser.add_argument(
        "--appid-header",
        default="X-Appid",
        help="Header name for AppID when the API expects one",
    )
    parser.add_argument("--token", default="", help="Access token / bearer token")
    parser.add_argument(
        "--auth-mode",
        default="bearer",
        choices=["bearer", "raw", "header", "none"],
        help="How to place the token",
    )
    parser.add_argument(
        "--token-header",
        default="Access-Token",
        help="Header name when --auth-mode=header",
    )
    parser.add_argument(
        "--content-type",
        default="application/json",
        help="Content-Type header",
    )
    parser.add_argument("--json", default="", help="Inline JSON body")
    parser.add_argument("--body-file", default="", help="Raw request body file")
    parser.add_argument(
        "--header",
        action="append",
        default=[],
        help="Extra header, format: 'Key: Value'",
    )
    parser.add_argument(
        "--insecure",
        action="store_true",
        help="Disable TLS certificate verification for local experiments only",
    )
    args = parser.parse_args()

    try:
        headers = build_headers(args)
        body = build_body(args)
    except Exception as exc:
        print(f"[config-error] {exc}", file=sys.stderr)
        return 2

    print("=== volc voice smoke test ===")
    print(f"url: {args.url}")
    print(f"method: {args.method}")
    print(f"appid: {mask_secret(args.appid)}")
    print(f"token: {mask_secret(args.token)}")
    print(f"auth_mode: {args.auth_mode}")
    print("headers:")
    for k, v in headers.items():
        safe_v = mask_secret(v) if "token" in k.lower() or k.lower() == "authorization" else v
        print(f"  {k}: {safe_v}")
    if body is None:
        print("body: <empty>")
    else:
        preview = response_preview(body, limit=400)
        print(f"body_bytes: {len(body)}")
        print("body_preview:")
        print(textwrap.indent(preview, "  "))

    request = urllib.request.Request(
        url=args.url,
        data=body,
        headers=headers,
        method=args.method,
    )

    context = None
    if args.insecure:
        context = ssl._create_unverified_context()

    try:
        with urllib.request.urlopen(request, timeout=20, context=context) as resp:
            data = resp.read()
            print(f"status: {resp.status}")
            print("response_headers:")
            for k, v in resp.headers.items():
                print(f"  {k}: {v}")
            print(f"response_bytes: {len(data)}")
            print("response_preview:")
            print(textwrap.indent(response_preview(data), "  "))
            return 0
    except urllib.error.HTTPError as exc:
        data = exc.read()
        print(f"status: {exc.code}")
        print("response_headers:")
        for k, v in exc.headers.items():
            print(f"  {k}: {v}")
        print(f"response_bytes: {len(data)}")
        print("response_preview:")
        print(textwrap.indent(response_preview(data), "  "))
        return 1
    except Exception as exc:
        print(f"[request-error] {type(exc).__name__}: {exc}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
