#!/usr/bin/env python3
"""Feed live token/quota usage to the TokenLimitMonitor board over USB serial.

Sends one JSON snapshot per line. The firmware parses each complete line and
re-renders in real time. Replace `read_usage()` with your real data source
(API calls, log scraping, etc.) -- everything else is plumbing.

Usage:
    pip install pyserial
    python token_feed.py --port COM5            # Windows
    python token_feed.py --port /dev/ttyUSB0    # Linux/macOS
"""

import argparse
import json
import time

import serial  # pip install pyserial


def read_usage():
    """Return the current snapshot. Plug your real metrics in here.

    Each service has up to 2 metrics; pct is 0-100 and drives the bar/colour.
    """
    return {
        "services": [
            {
                "name": "CLAUDE CODE",
                "provider": "Anthropic",
                "reset": "8:00 PM",
                "metrics": [
                    {"label": "Context Window", "pct": 35, "value": "75k / 200k"},
                    {"label": "Monthly Cost", "pct": 55, "value": "$5.50 / $10"},
                ],
            },
            {
                "name": "ANTIGRAVITY",
                "provider": "Internal API",
                "reset": "--",
                "metrics": [
                    {"label": "API Requests", "pct": 12, "value": "1,200 / 10,000"},
                    {"label": "Est. Cost", "pct": 20, "value": "$1.25 / $25"},
                ],
            },
            {
                "name": "CODEX",
                "provider": "OpenAI",
                "reset": "in 3d 11h",
                "metrics": [
                    {"label": "Daily Tokens", "pct": 87, "value": "435k / 500k"},
                    {"label": "Quota Reset", "pct": 39, "value": "in 3d 11h"},
                ],
            },
        ]
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="serial port, e.g. COM5 or /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--interval", type=float, default=2.0, help="seconds between snapshots")
    args = ap.parse_args()

    with serial.Serial(args.port, args.baud, timeout=1) as ser:
        time.sleep(2)  # let the board reset/settle after the port opens
        print(f"Feeding {args.port} @ {args.baud} every {args.interval}s. Ctrl+C to stop.")
        while True:
            line = json.dumps(read_usage(), separators=(",", ":")) + "\n"
            ser.write(line.encode("utf-8"))
            ser.flush()
            time.sleep(args.interval)


if __name__ == "__main__":
    main()
