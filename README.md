# Token Limit Monitor

A desk dashboard for an **ESP32 + 3.5" TFT** that shows **live AI coding-agent usage**
for **Claude Code** and **Codex** — context window, rate limits, cost, and reset time —
read straight from each tool's local logs. **No API keys.** Plus a Shiba Inu that runs
around and naps. 🐕

```
┌─ TOKEN MONITOR ───────────────────────── ● LIVE ─┐
│  CLAUDE CODE   29%        CODEX        37%        │
│  Context  ███░░░░░░       5h Limit ████░░░░       │
│  290k / 1.0M              63% left                │
│  Cost Today ██████████    Weekly  █████░░░        │
│  $136 / $50              45% left                 │
│  Up 2h 14m        🐕💨           Updated 0.3s     │
└───────────────────────────────────────────────────┘
```

## How it works

```
PC (Python) ──reads ~/.claude & ~/.codex logs──► JSON over USB ──► ESP32 renders it live
```

`claude_feed.py` re-reads the logs every few seconds and sends one line of JSON;
the firmware parses each line and re-draws in real time.

## Hardware

- **ESP32** dev board (USB serial)
- **3.5" TFT, 480×320**, driven by [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI)

## Setup

```bash
git clone https://github.com/67010655/Token-Limit-Monitor.git
```

**1. Flash the firmware**
- Install libraries (Arduino Library Manager): **TFT_eSPI** (configure `User_Setup`
  for your panel; keep `SMOOTH_FONT` on) and **ArduinoJson v7**.
- Open `TokenLimitMonitor.ino`, select your board + port, **Upload**.
- The bundled `NotoSansBold*.h` fonts are picked up automatically.

**2. Run the feeder**
```bash
pip install pyserial
python claude_feed.py              # dry run: prints the JSON, sends nothing
python claude_feed.py --port COM5  # stream to the board (use your real port)
```
Find the port in Arduino IDE → **Tools → Port**, and **close the Serial Monitor first**
(one program can hold the port at a time).

## The two cards

| Card | Headline | Second metric | Source |
|------|----------|---------------|--------|
| **CLAUDE CODE** | Context window (matches Claude's status popup) | Cost today vs `DAILY_COST_BUDGET` | `~/.claude` token logs |
| **CODEX** | 5-hour limit + reset time | Weekly limit | `~/.codex` rate-limit logs (real) |

> Claude Code doesn't store its real 5h/weekly limits on disk (only token counts),
> so the context window is shown instead. Codex *does*, so those numbers are exact.
> "Est. Cost" is an API-equivalent estimate, not your actual subscription bill.

## Configuration

- **Firmware** (`TokenLimitMonitor.ino`): `DATA_SERIAL` (serial port), `NUM_SERVICES`,
  the layout `*_Y`/`CARD_*`/`LANE_*` constants, the color palette, and mascot timing
  (`MASCOT_ACTIVE_MS` = 10 min, `MASCOT_SLEEP_MS` = 30 min).
- **Feeder** (`claude_feed.py`): `DAILY_COST_BUDGET`, plus `--port` / `--interval` flags.

## Custom data

To feed your own source, send one JSON object per line (see `token_feed.py`):

```json
{"services":[{"name":"CLAUDE CODE","provider":"Anthropic","reset":"live",
  "metrics":[{"label":"Context","pct":29,"value":"290k / 1.0M"},
             {"label":"Cost Today","pct":80,"value":"$40 / $50"}]}]}
```
Up to `NUM_SERVICES` services, 2 metrics each; `pct` is 0–100 and the first metric is the headline.

## Credits

[TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) · [ArduinoJson](https://arduinojson.org/) · Noto Sans (Google)

## License

MIT
