# Bot Commands

Bot functionality for companion radio devices. Provides automated responses to help users understand message routing and test connectivity.

## Configuration

Bot configuration commands work via **Direct Message only** for security.

### Commands

- `!bot` or `!bot status` - Show bot enabled/disabled state and location
- `!bot on` or `!bot enable` - Enable bot responses
- `!bot off` or `!bot disable` - Disable bot responses
- `!bot location <text>` - Set location (e.g., "Rasa (VA)", "Milano : JN45ab")

State is persisted to `/meshbot` file on device.

## Active Channels

Bot responds to commands in these channels only:
- **#bot** - Technical/debugging channel with detailed output
- **#test** - Testing channel
- **#ping** - Ping testing channel
- **#prove** - Italian testing channel

## Channel Commands

All commands are case-insensitive and accept text after the command (e.g., "Test mobile", "ping da casa").

### ping / !ping

Simple connectivity test with visual hop indicator.

**Response (all channels):**
- Direct connection: `@[Bob] 🏓 pong! 🤖`
- 1-5 hops: `@[Bob] ⌢ ⌢ ⌢ 🏓 pong! 🤖` (one arc per hop)
- 6+ hops: `@[Bob] … 10 hops … 🏓 pong! 🤖`

The arc (⌢) representation makes hop count instantly scannable for nearby connections.

### test / !test / prova / !prova

Shows hop count and location. "prova" is Italian alternative to "test".

**In #bot channel:**
- Shows full path with hex hashes
- Direct: `@[Bob] direct 📍 Rasa (VA) 🤖`
- With hops: `@[Bob] 3 hops a1b2→c3d4 📍 Rasa (VA) 🤖`

**In other channels (#test, #ping, #prove):**
- Shows simplified format with best repeater
- Direct: `@[Bob] direct 📍 Rasa (VA) 🤖`
- With repeater: `@[Bob] 3 hops via IT-LIG-MteBeigua-D 📍 Rasa (VA) 🤖`
- Without repeater: `@[Bob] 3 hops 📍 Rasa (VA) 🤖`

**Warnings:**
- If sender hasn't set region: Adds `⚠️ set region it`
- If using 1-byte hashes: Adds `⚠️ use 2-bytes`
- Both warnings appear if both conditions are true

### path / !path

Shows path information and warns about configuration issues.

**With 1-byte hashes:**
- Shows full hex path (resolution unreliable with 1-byte)
- `@[Bob] 3 hops a1→b2→c3 📍 Rasa (VA) | ⚠️ use 2-bytes 🤖`

**With 2-byte or 3-byte hashes:**
- Shows resolved node names in order: first → middle (prioritize backbone) → last
- Uses `...` for gaps where nodes aren't in contacts
- Examples:
  - `@[Bob] 1 hop IT-LIG-Rasa-R 📍 Rasa (VA) 🤖`
  - `@[Bob] 3 hops FirstHop→...→LastHop 📍 Rasa (VA) 🤖`
  - `@[Bob] 5 hops FirstHop→...→IT-LIG-Backbone-D→...→LastHop 📍 Rasa (VA) 🤖`
  - `@[Bob] 5 hops ...→IT-LIG-Backbone-D→...→LastHop 📍 Rasa (VA) 🤖` (first unknown)

**Warnings:**
- If sender hasn't set region: Adds `⚠️ set region it`
- If using 1-byte hashes: Adds `⚠️ use 2-bytes`
- Both warnings appear if both conditions are true

### !echo <text>

Echoes back the provided text.

**Response (all channels):**
```
@[Bob] Echo: your text 🤖
```

## Repeater Selection

When showing "via X" in messages, the bot selects the most relevant repeater using this priority:

1. **Backbone router** - Routers ending in `-D` (e.g., `IT-LIG-MteBeigua-D`)
   - These are dorsale/backbone infrastructure
2. **First hop** - The initial repeater that picked up the message
   - Shows which of your nearby repeaters was used
3. **Any known hop** - Any repeater found in the device's contact list

**Special cases:**
- With 1 hop: Shows that hop (the single repeater used)
- With 2+ hops: Excludes last hop (destination router)
- If no repeaters are in contacts: Falls back to hop count only

## Message Format

All bot responses follow this pattern:
- `@[SenderName]` - Indicates who the response is for
- Message content - Hop count, repeater info, etc.
- `📍 Location` - Location pin emoji before location (if set)
- `🤖` - Bot emoji at end to indicate automated response

The pin emoji (📍) keeps the format language-neutral while clearly indicating the destination.

## Examples

```
User: ping
Bot: @[Alice] ⌢ ⌢ ⌢ 🏓 pong! 🤖

User: Test dalla Valceresio
Bot: @[Alice] 3 hops via IT-LIG-MteBeigua-D 📍 Rasa (VA) 🤖

User: Prova mobile
Bot: @[Bob] 1 hop via IT-LOM-VA-Rasa-R 📍 Rasa (VA) 🤖

User (in #bot): path
Bot: @[Charlie] 2 hops IT-LOM-Varese-R→...→IT-LIG-MteBeigua-D 📍 Rasa (VA) 🤖

# With warnings:
User: test
Bot (sender no region): @[Dave] 2 hops via IT-LIG-MteBeigua-D ⚠️ set region it 🤖

User (in #bot): path
Bot (1-byte hash, sender no region): @[Eve] 3 hops a1→b2→c3 ⚠️ use 2-bytes & set region it 🤖
```
