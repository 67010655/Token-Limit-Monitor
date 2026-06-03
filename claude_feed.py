#!/usr/bin/env python3
"""Feed REAL usage to the TokenLimitMonitor board -- two cards, no API keys.

    CLAUDE CODE : context-window fill + estimated cost today
                  (from ~/.claude/projects/**/*.jsonl)
    CODEX       : context fill + real 5h / weekly rate-limit % + reset time
                  (from ~/.codex/sessions/**/*.jsonl)

Both read local session logs and stream the JSON the firmware expects over USB.
Cost is an API-equivalent estimate, not your actual bill if you're on a plan.

Usage:
    pip install pyserial
    python claude_feed.py                 # dry run: print the JSON, don't send
    python claude_feed.py --port COM5     # stream to the board
"""

import argparse
import glob
import json
import os
import time
from datetime import datetime, timedelta, timezone

# --- Tunables --------------------------------------------------------------
CLAUDE_DIR = os.path.expanduser("~/.claude/projects")
CODEX_DIR = os.path.expanduser("~/.codex/sessions")
DAILY_COST_BUDGET = 50.0         # USD/day (Claude "Cost Today" gauge)

# Rough public per-million-token prices (USD): input, output, cache write, cache read.
PRICING = {
    "opus":   (15.0, 75.0, 18.75, 1.50),
    "sonnet": (3.0,  15.0,  3.75, 0.30),
    "haiku":  (0.80,  4.0,  1.00, 0.08),
}
DEFAULT_PRICE = PRICING["opus"]


def price_for(model: str):
    m = (model or "").lower()
    for key, p in PRICING.items():
        if key in m:
            return p
    return DEFAULT_PRICE


def msg_cost(model, u):
    pin, pout, pcw, pcr = price_for(model)
    return (
        u.get("input_tokens", 0) / 1e6 * pin
        + u.get("output_tokens", 0) / 1e6 * pout
        + u.get("cache_creation_input_tokens", 0) / 1e6 * pcw
        + u.get("cache_read_input_tokens", 0) / 1e6 * pcr
    )


def msg_tokens(u):
    """"Work" tokens for a turn: input + output + cache writes (NOT cache reads,
    which re-count the same context every turn)."""
    return (
        u.get("input_tokens", 0)
        + u.get("output_tokens", 0)
        + u.get("cache_creation_input_tokens", 0)
    )


def fmt(n):
    n = int(n)
    if n >= 1_000_000:
        return f"{n / 1_000_000:.1f}M"
    if n >= 1_000:
        return f"{n / 1_000:.0f}k"
    return str(n)


def pct(used, total):
    if total <= 0:
        return 0
    return max(0, min(100, round(used / total * 100)))


def detect_window(ctx):
    """Pick the context-window denominator that fits the latest turn."""
    for w in (200_000, 1_000_000):
        if ctx <= w:
            return w
    return 1_000_000


def reltime(unix_ts):
    """Human 'in 2h 13m' until a unix timestamp."""
    if not unix_ts:
        return "--"
    d = int(unix_ts - time.time())
    if d <= 0:
        return "now"
    if d >= 86400:
        return f"in {d // 86400}d {(d % 86400) // 3600}h"
    if d >= 3600:
        return f"in {d // 3600}h {(d % 3600) // 60}m"
    return f"in {max(1, d // 60)}m"


def claude_scan():
    """Latest Claude Code context window + today's estimated cost."""
    today = datetime.now().astimezone().date()
    today_cost = 0.0
    newest_dt = None
    ctx = 0

    for path in glob.glob(os.path.join(CLAUDE_DIR, "**", "*.jsonl"), recursive=True):
        try:
            fh = open(path, encoding="utf-8", errors="ignore")
        except OSError:
            continue
        with fh:
            for line in fh:
                try:
                    o = json.loads(line)
                except ValueError:
                    continue
                if o.get("type") != "assistant":
                    continue
                msg = o.get("message")
                if not isinstance(msg, dict) or not msg.get("usage"):
                    continue
                u = msg["usage"]
                ts = o.get("timestamp")
                dt = None
                if ts:
                    try:
                        dt = datetime.fromisoformat(ts.replace("Z", "+00:00")).astimezone()
                    except ValueError:
                        dt = None
                if dt and dt.date() == today:
                    today_cost += msg_cost(msg.get("model", ""), u)
                if dt and (newest_dt is None or dt > newest_dt):
                    newest_dt = dt
                    ctx = (u.get("input_tokens", 0)
                           + u.get("cache_read_input_tokens", 0)
                           + u.get("cache_creation_input_tokens", 0))
    return {"today_cost": today_cost, "ctx": ctx}


def codex_scan():
    """Latest Codex context fill + real rate-limit percentages."""
    latest_ts = latest_info = None
    rl_ts = rate_limits = None
    for path in glob.glob(os.path.join(CODEX_DIR, "**", "*.jsonl"), recursive=True):
        try:
            fh = open(path, encoding="utf-8", errors="ignore")
        except OSError:
            continue
        with fh:
            for line in fh:
                try:
                    o = json.loads(line)
                except ValueError:
                    continue
                pl = o.get("payload")
                if not isinstance(pl, dict) or pl.get("type") != "token_count":
                    continue
                ts = o.get("timestamp", "")
                if pl.get("info") and (latest_ts is None or ts > latest_ts):
                    latest_ts, latest_info = ts, pl["info"]
                if pl.get("rate_limits") and (rl_ts is None or ts > rl_ts):
                    rl_ts, rate_limits = ts, pl["rate_limits"]
    return latest_info, rate_limits


def claude_card():
    # Claude Code logs do NOT expose the real 5h/weekly plan limit -- only token
    # counts. So the headline is the CONTEXT WINDOW (which matches the number in
    # Claude's own status popup); cost today is a secondary, log-derived figure.
    d = claude_scan()
    window = detect_window(d["ctx"])
    return {
        "name": "CLAUDE CODE",
        "provider": "Anthropic",
        "reset": "live",
        "metrics": [
            {"label": "Context", "pct": pct(d["ctx"], window),
             "value": f"{fmt(d['ctx'])} / {fmt(window)}"},
            {"label": "Cost Today", "pct": pct(d["today_cost"], DAILY_COST_BUDGET),
             "value": f"${d['today_cost']:.2f} / ${DAILY_COST_BUDGET:.0f}"},
        ],
    }


def codex_card():
    info, rl = codex_scan()
    if not info and not rl:
        return {"name": "CODEX", "provider": "OpenAI", "reset": "--",
                "metrics": [{"label": "Context", "pct": 0, "value": "no data"},
                            {"label": "5h Limit", "pct": 0, "value": "no data"}]}
    cw = (info or {}).get("model_context_window", 0) or 1
    used = (info or {}).get("last_token_usage", {}).get("total_tokens", 0)
    prim = (rl or {}).get("primary", {}) or {}
    sec = (rl or {}).get("secondary", {}) or {}
    p_pct = int(prim.get("used_percent", 0))
    s_pct = int(sec.get("used_percent", 0))
    return {
        "name": "CODEX",
        "provider": "OpenAI live",
        "reset": reltime(prim.get("resets_at")),    # real 5h reset from OpenAI
        "metrics": [
            {"label": "5h Limit", "pct": p_pct, "value": f"{100 - p_pct}% left"},
            {"label": "Weekly", "pct": s_pct, "value": f"{100 - s_pct}% left"},
        ],
    }


def build_snapshot():
    return {"services": [claude_card(), codex_card()]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="serial port, e.g. COM5 (omit for a dry run)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--interval", type=float, default=5.0)
    args = ap.parse_args()

    if not args.port:
        print(json.dumps(build_snapshot(), indent=2, ensure_ascii=False))
        return

    import serial  # pip install pyserial
    with serial.Serial(args.port, args.baud, timeout=1) as ser:
        time.sleep(2)
        print(f"Feeding {args.port} from {CLAUDE_DIR} every {args.interval}s. Ctrl+C to stop.")
        while True:
            line = json.dumps(build_snapshot(), separators=(",", ":")) + "\n"
            ser.write(line.encode("utf-8"))
            ser.flush()
            time.sleep(args.interval)


if __name__ == "__main__":
    main()
