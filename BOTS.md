# Bot Commands

Bot functionality for companion radio devices. Provides automated responses to help users understand message routing and test connectivity.

## Configuration

Bot configuration commands work via **Direct Message only** for security.

### Commands

- `!bot` or `!bot status` - Show bot enabled/disabled state, location, reply-all, and home repeater setting
- `!bot on` or `!bot enable` - Enable bot responses
- `!bot off` or `!bot disable` - Disable bot responses
- `!bot location <text>` - Set location (e.g., "Rasa (VA)", "Milano : JN45ab")
- `!bot reply-all on` - Enable bot responses on non-bot channels (Public, etc.)
- `!bot reply-all off` - Disable bot responses on non-bot channels (default)
- `!bot home <hash>` - Set home repeater hash (1-3 bytes hex, e.g., "8d" or "8dbb")
- `!bot home clear` - Clear home repeater setting (reply everywhere)
- `!bot clear` - Clear reply tracking state (resets spam prevention)

State is persisted to `/meshbot` file on device.

**Home Repeater:** When configured, the bot will not send replies when the last hop matches the home repeater. This prevents the bot from replying to local messages when at home. The hash comparison uses the minimum of configured and path hash lengths, so "8dbb" will match both 1-byte (8d) and 2-byte (8dbb) paths.

## Active Channels

Bot responds to commands in these channels:
- **#bot** - Technical/debugging channel with detailed output
- **#test** - Testing channel
- **#ping** - Ping testing channel
- **#prove** - Italian testing channel

On **other channels** (e.g. Public), only `test` / `prova` get a short casual reply when `reply-all` is enabled. Rate limiting prevents spam:
- First message: Always replies
- Second strict command ("test"/"prova" alone): Replies with gentle nudge to use bot channels
- Subsequent messages: No reply
- State resets after 10 minutes

## Channel Commands

All commands are case-insensitive and accept text after the command (e.g., "Test mobile", "ping da casa"). A leading `@[companion name]` or `@[companion name]:` is accepted when it matches this node.

### ping / !ping

Simple connectivity test with visual hop indicator.

**Response (bot channels):**
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
- Shows simplified format with best repeater and path distance
- Direct: `@[Bob] direct 📍 Rasa (VA) 🤖`
- With repeater: `@[Bob] 3 hops via IT-LIG-MteBeigua-D (45km) 📍 Rasa (VA) 🤖`
- Incomplete GPS data: `@[Bob] 3 hops via IT-LIG-MteBeigua-D (~45km) 📍 Rasa (VA) 🤖`
- Without repeater: `@[Bob] 3 hops (78km) 📍 Rasa (VA) 🤖`
- Insufficient GPS data, 1-byte hashes, or low GPS coverage: `@[Bob] 3 hops 📍 Rasa (VA) 🤖`

**Path Distance:** Total distance calculated by summing distances between consecutive hops using GPS coordinates from contacts. Shows "45km" when all data available, "~45km" (approximately) when some repeaters lack GPS data. Distance is only shown when at least 40% of hops have valid GPS coordinates. With 1-byte hashes, distance is not shown because repeater resolution is unreliable.

**In Public (when reply-all is enabled):**
- First reply: `@[Bob] 3 salti, Rasa (VA) 🤖`
- Second strict command: `@[Bob] 3 salti, Rasa (VA) (scrivi in #bot o #test) 🤖`

**Warnings:**
- If sender hasn't set region: Adds `⚠ set region it`
- If using 1-byte hashes: Adds `⚠ set 2-byte`
- Both warnings appear if both conditions are true

### path / !path

Shows path information and warns about configuration issues.

**With 1-byte hashes:**
- Shows full hex path (resolution unreliable with 1-byte)
- Example:
  ```
  @[Bob] 3 hops: a1→b2→c3
  📍 Rasa (VA) 🤖
  ⚠ set 2-byte
  ```

**With 2-byte or 3-byte hashes:**
- Shows known repeater names only: `→` between adjacent hops, `⇢` when hops are skipped or unknown
- Format: `N hops: First⇢Last` or `N hops: First→Second⇢Last`
- Location is omitted (path names use the available budget; use `test`/`prova` for location)
- Examples:
  - `@[Bob] 1 hop: IT-LIG-Rasa-R 🤖`
  - `@[Bob] 3 hops: FirstHop⇢LastHop 🤖`
  - `@[Bob] 5 hops: FirstHop→Second⇢IT-LIG-Backbone-D⇢LastHop 🤖`
  - `@[Bob] 5 hops: ⇢IT-LIG-Backbone-D⇢LastHop 🤖` (unknown hops before first name)

**Warnings:**
- If sender hasn't set region: Adds `⚠ set region it`
- If using 1-byte hashes: Adds `⚠ set 2-byte`
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

(The `path` command uses path order instead of this priority.)

## Message Format

All bot responses follow this pattern:
- `@[SenderName]` - Indicates who the response is for
- Message content - Hop count, repeater info, etc.
- `📍 Location` - Location pin emoji before location (if set; omitted on named `path` replies)
- `🤖` - Bot emoji at end to indicate automated response

The pin emoji (📍) keeps the format language-neutral while clearly indicating the destination.

## Reply Rate Limiting (Non-Bot Channels)

When `reply-all` is enabled, the bot uses smart rate limiting to welcome new users without spamming regulars:

1. **First contact** - Bot replies to any message starting with "test"/"prova" (even "prova prima di pranzo")
2. **Second strict command** - If sender sends just "test" or "prova" again within 10 minutes, bot replies with nudge to use #bot/#test channels
3. **Conversational messages** - If second message has text after "test"/"prova", bot stays silent (assumes conversation, not testing)
4. **Further messages** - Bot stays silent for remaining messages in 10-minute window
5. **Reset** - After 10 minutes, tracking resets and bot will reply again

This balances welcoming newcomers (who often test in Public) with reducing noise for active users. Use `!bot clear` to manually reset tracking if needed.

## Examples

```
User: ping
Bot: @[Alice] ⌢ ⌢ ⌢ 🏓 pong! 🤖

User: Test dalla Valceresio
Bot: @[Alice] 3 hops via IT-LIG-MteBeigua-D (45km) 📍 Rasa (VA) 🤖

User: Prova mobile
Bot: @[Bob] 1 hop via IT-LOM-VA-Rasa-R (12km) 📍 Rasa (VA) 🤖

User (in #bot): path
Bot: @[Charlie] 2 hops: IT-LIG-MteBeigua-D→IT-LOM-VA-Rasa-R 🤖

# With warnings:
User: test
Bot (sender no region):
  @[Dave] 2 hops via IT-LIG-MteBeigua-D
  📍 Rasa (VA) 🤖
  ⚠ set region it

User (in #bot): path
Bot (1-byte hash, sender no region):
  @[Eve] 3 hops: a1→b2→c3
  📍 Rasa (VA) 🤖
  ⚠ set 2-byte & region it

# In Public channel (reply-all enabled):
User: prova prima di uscire
Bot: @[Alice] 3 salti, Rasa (VA) 🤖

User (same person, <10 min later): test
Bot: @[Alice] 3 salti, Rasa (VA) (scrivi in #bot o #test) 🤖

User (same person, <10 min later): test again
Bot: (no reply - limit reached)

User (same person, >10 min later): prova
Bot: @[Alice] 2 salti, Rasa (VA) 🤖
```
