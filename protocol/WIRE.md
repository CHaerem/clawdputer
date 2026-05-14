# clawdputer wire protocol

Source of truth for what the Cardputer firmware and the Mac-side
`clawd-bridge` daemon send each other. Both sides MUST track changes here.

## Transports

Two BLE services live on the Cardputer simultaneously, so Claude Desktop and
`clawd-bridge` can be connected at the same time without protocol crosstalk.

### 1. Nordic UART Service — buddy (Claude Desktop ↔ firmware)

This is the Anthropic-defined protocol from
[`anthropics/claude-desktop-buddy`](https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md).
The Cardputer implements it under `firmware/src/apps/buddy/`. We do not
extend it — Claude Desktop is the only peer that speaks NUS to us.

|                                | UUID                                   |
| ------------------------------ | -------------------------------------- |
| Service                        | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` |
| RX (Desktop → Cardputer)       | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` |
| TX (Cardputer → Desktop)       | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` |

### 2. Clawd Bridge Service — `clawd-bridge` ↔ firmware

Our extended protocol, used by the Mac daemon to drive Cardputer apps
(`apps/chat`, `apps/ssh`, …). Same line-buffered JSON framing as NUS, but a
separate service UUID so the two links don't conflict.

|                                | UUID                                   |
| ------------------------------ | -------------------------------------- |
| Service                        | `c1aedb01-1d0c-4adc-9b1a-c1aedb010000` |
| RX (bridge → Cardputer)        | `c1aedb01-1d0c-4adc-9b1a-c1aedb010001` |
| TX (Cardputer → bridge)        | `c1aedb01-1d0c-4adc-9b1a-c1aedb010002` |

> Status (2026-05-14): firmware exposes both services. Advertising only
> carries the NUS UUID (so Claude Desktop's scan filter matches), and the
> bridge service is discovered after connecting.

## Framing

UTF-8 JSON objects, one per line, terminated by `\n`. Either side may
fragment writes across BLE notifications; the receiver accumulates bytes
and parses each newline-terminated chunk.

## Bridge message set (v0, work in progress)

### Hello

On connect, bridge announces itself and Cardputer replies with its current
state:

```json
// bridge → device
{"cmd":"hello","bridge":"clawd-bridge","ver":1}

// device → bridge
{"ack":"hello","ok":true,"device":"Claude-Cardputer-AB12","apps":["buddy","chat"]}
```

### App focus

Bridge tells the device which app should be active so further messages are
routed to it.

```json
// bridge → device
{"cmd":"focus","app":"chat"}

// device → bridge
{"ack":"focus","ok":true,"app":"chat"}
```

### Chat — line in, stream out

The bridge owns the `claude` CLI process on the Mac. Cardputer's `apps/chat`
sends typed lines; bridge pipes them into the running session and streams
output back as `chat.chunk` events terminated by `chat.end`.

```json
// device → bridge
{"evt":"chat.send","text":"summarise the diff on this branch"}

// bridge → device (zero or more)
{"evt":"chat.chunk","text":"On branch main…"}

// bridge → device (one, marks turn complete)
{"evt":"chat.end","tokens":1820}
```

Chunks are best-effort newline boundaries — bridge MAY split a line across
multiple `chat.chunk` events if a token boundary lands mid-line, but SHOULD
prefer line-aligned splits when output is line-buffered.

### Errors

```json
{"evt":"error","where":"chat","msg":"claude exited with status 1"}
```

### Claude usage report

Cardputer asks for a usage summary; bridge responds with figures pulled
from `~/.claude/stats-cache.json` (daily activity) and
`claude --print --output-format json "/cost"` (cost + tokens).

```json
// device → bridge
{"evt":"usage.request"}

// bridge → device
{
  "evt":"usage.response",
  "today":   {"messages": 8901, "sessions": 5, "tools": 1234},
  "week":    {"messages": 45678, "sessions": 23, "tools": 6789},
  "month":   {"messages": 234567, "sessions": 89, "tools": 23456},
  "cost":    {"usd": 0, "tier": "subscription"},
  "tokens":  {"input": 0, "output": 0, "cacheRead": 0, "cacheCreate": 0},
  "asOf":    "2026-05-14"
}
```

Either side MAY omit fields that aren't available (e.g. older stats-cache
versions without `toolCallCount`).

## Open questions

- Session resume across reconnects: does the bridge keep the `claude` CLI
  alive while BLE is dropped, or restart on each connect?
- Multi-session (multiple `claude` CLI instances behind the bridge): not in
  v0. `apps/chat` is single-session.
- Auth between bridge and Cardputer: BLE pairing is sufficient on the link
  layer; we don't layer additional auth on top yet.
