// ABOUTME: Bot command handler for companion radio devices
// ABOUTME: Handles bot configuration and automated message responses

#include "BotHandler.h"
#include "MyMesh.h"
#include <Arduino.h>
#include <Mesh.h>

#ifdef ESP32
  #include <SPIFFS.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
#endif

static constexpr const char* BOT_STATE_FILE = "/meshbot";
static bool bot_enabled = false;
static char bot_location[64] = {0};

void botInit() {
#ifdef ESP32
  File file = SPIFFS.open(BOT_STATE_FILE);
#elif defined(RP2040_PLATFORM)
  File file = LittleFS.open(BOT_STATE_FILE, "r");
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  File file = InternalFS.open(BOT_STATE_FILE, FILE_O_READ);
#endif

  if (!file) {
    bot_enabled = false;
    return;
  }

  char buf[256] = {0};
  size_t len = file.readBytes(buf, sizeof(buf) - 1);
  file.close();

  // Parse line by line: "enabled=0/1" and "location=..."
  char* ctx = nullptr;
  char* line = strtok_r(buf, "\n", &ctx);
  while (line != nullptr) {
    if (strncmp(line, "enabled=", 8) == 0) {
      bot_enabled = (line[8] == '1');
    } else if (strncmp(line, "location=", 9) == 0) {
      strncpy(bot_location, line + 9, sizeof(bot_location) - 1);
      bot_location[sizeof(bot_location) - 1] = '\0';
    }
    line = strtok_r(nullptr, "\n", &ctx);
  }
}

static bool botSaveState() {
#ifdef ESP32
  File file = SPIFFS.open(BOT_STATE_FILE, "w");
#elif defined(RP2040_PLATFORM)
  File file = LittleFS.open(BOT_STATE_FILE, "w");
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  File file = InternalFS.open(BOT_STATE_FILE, FILE_O_WRITE);
#endif

  if (!file) return false;

  file.printf("enabled=%d\n", bot_enabled ? 1 : 0);
  file.printf("location=%s\n", bot_location);
  file.close();
  return true;
}

bool botIsEnabled() {
  return bot_enabled;
}

const char* botGetLocation() {
  return bot_location;
}

static const char* getBotWarnings(uint8_t hash_size, bool has_region) {
  bool needs_bytes_warning = (hash_size == 1);
  bool needs_region_warning = !has_region;

  if (needs_bytes_warning && needs_region_warning) {
    return "\n⚠ set 2-byte & region it";
  } else if (needs_bytes_warning) {
    return "\n⚠ set 2-byte";
  } else if (needs_region_warning) {
    return "\n⚠ set region it";
  } else {
    return "";
  }
}

bool botHandleConfig(const char* text, char* reply, size_t reply_len) {
  if (text == nullptr || reply == nullptr || reply_len == 0) {
    return false;
  }

  // Check for !bot commands
  if (strcmp(text, "!bot") == 0 || strcmp(text, "!bot status") == 0) {
    if (bot_location[0] != '\0') {
      snprintf(reply, reply_len, "bot: %s, location: %s",
               bot_enabled ? "enabled" : "disabled", bot_location);
    } else {
      snprintf(reply, reply_len, "bot: %s", bot_enabled ? "enabled" : "disabled");
    }
    return true;
  }

  if (strcmp(text, "!bot on") == 0 || strcmp(text, "!bot enable") == 0) {
    bot_enabled = true;
    if (!botSaveState()) {
      snprintf(reply, reply_len, "Error: could not save bot state");
      return true;
    }
    snprintf(reply, reply_len, "OK - bot enabled");
    return true;
  }

  if (strcmp(text, "!bot off") == 0 || strcmp(text, "!bot disable") == 0) {
    bot_enabled = false;
    if (!botSaveState()) {
      snprintf(reply, reply_len, "Error: could not save bot state");
      return true;
    }
    snprintf(reply, reply_len, "OK - bot disabled");
    return true;
  }

  if (strncmp(text, "!bot location ", 14) == 0) {
    const char* location = text + 14;
    strncpy(bot_location, location, sizeof(bot_location) - 1);
    bot_location[sizeof(bot_location) - 1] = '\0';
    if (!botSaveState()) {
      snprintf(reply, reply_len, "Error: could not save bot state");
      return true;
    }
    snprintf(reply, reply_len, "OK - location set to: %s", bot_location);
    return true;
  }

  return false;  // Not a bot config command
}

bool botHandleDM(MyMesh& mesh, const ContactInfo& from, mesh::Packet* pkt, const char* text) {
  if (text == nullptr || from.out_path_len == OUT_PATH_UNKNOWN) {
    return false;
  }

  // Handle !echo command (case-insensitive)
  if (strncasecmp(text, "!echo ", 6) == 0) {
    const char* echo_text = text + 6;  // Skip "!echo "

    // Create reply message: "Echo: <text>"
    char reply[MAX_TEXT_LEN + 1];
    snprintf(reply, sizeof(reply), "Echo: %s", echo_text);

    // Send reply back to sender
    uint8_t temp[5 + MAX_TEXT_LEN + 1];
    uint32_t timestamp = mesh.getRTCClock()->getCurrentTime();
    memcpy(temp, &timestamp, 4);
    temp[4] = 0;  // attempt = 0
    size_t reply_len = strlen(reply);
    memcpy(&temp[5], reply, reply_len + 1);

    auto reply_pkt = mesh.createDatagram(PAYLOAD_TYPE_TXT_MSG, from.id,
                                         from.getSharedSecret(mesh.self_id), temp, 5 + reply_len);
    if (reply_pkt) {
      mesh.sendDirect(reply_pkt, from.out_path, from.out_path_len);
      Serial.printf("[BOT] Sent echo reply to %s\n", from.name);
    }
    return true;
  }

  return false;  // Not a bot command
}

// Find best repeater to show in "via X" format
// Priority: backbone router (-D) > first hop > any known hop
// Excludes last hop (destination)
// Returns nullptr if no suitable repeater found
static const char* findBestRepeater(MyMesh& mesh, mesh::Packet* pkt) {
  uint8_t hop_count = pkt->getPathHashCount();
  uint8_t hash_size = pkt->getPathHashSize();

  if (hop_count == 0) return nullptr;  // Direct connection, no intermediate nodes

  const char* backbone_repeater = nullptr;
  const char* first_hop = nullptr;
  const char* any_known = nullptr;

  // If multiple hops, exclude last one (destination). If only 1 hop, check it (it's the repeater)
  uint8_t hops_to_check = (hop_count > 1) ? hop_count - 1 : hop_count;

  Serial.printf("[BOT] findBestRepeater: checking %d hops (total %d):\n", hops_to_check, hop_count);

  uint8_t found_count = 0;
  for (uint8_t i = 0; i < hops_to_check; i++) {
    const uint8_t* hash = &pkt->path[i * hash_size];

    // Print hash
    Serial.printf("[BOT]   Hop %d: ", i);
    for (uint8_t j = 0; j < hash_size; j++) {
      Serial.printf("%02x", hash[j]);
    }

    ContactInfo* contact = mesh.lookupContactByPubKey(hash, hash_size);

    if (contact) {
      found_count++;

      // Check if backbone router (ends with -D or -d)
      size_t len = strlen(contact->name);
      bool is_backbone = (len >= 2 && contact->name[len-2] == '-' &&
                         (contact->name[len-1] == 'D' || contact->name[len-1] == 'd'));

      if (is_backbone) {
        Serial.printf(" -> Found: %s (BACKBONE)\n", contact->name);
        if (!backbone_repeater) backbone_repeater = contact->name;
      } else {
        Serial.printf(" -> Found: %s\n", contact->name);
      }

      // Track first hop
      if (i == 0 && !first_hop) first_hop = contact->name;

      // Track any known
      if (!any_known) any_known = contact->name;
    } else {
      Serial.printf(" -> Not found\n");
    }
  }

  Serial.printf("[BOT] Path resolution: %d/%d hops found\n", found_count, hops_to_check);

  // Priority: backbone > first hop > any known
  const char* selected = nullptr;
  const char* reason = nullptr;

  if (backbone_repeater) {
    selected = backbone_repeater;
    reason = "backbone (-D)";
  } else if (first_hop) {
    selected = first_hop;
    reason = "first hop";
  } else if (any_known) {
    selected = any_known;
    reason = "any known";
  }

  if (selected) {
    Serial.printf("[BOT] Selected repeater: %s (%s)\n", selected, reason);
  } else {
    Serial.printf("[BOT] No repeater found in path\n");
  }

  return selected;
}

static void buildHopPath(char* body, size_t body_len, mesh::Packet* pkt) {
  uint8_t hop_count = pkt->getPathHashCount();
  uint8_t hash_size = pkt->getPathHashSize();

  if (hop_count == 0) {
    snprintf(body, body_len, "direct");
    return;
  }

  // Build "N hops a1b2→c3d4→..." format
  char path_str[128] = {0};
  char* out = path_str;
  size_t remaining = sizeof(path_str);

  for (uint8_t i = 0; i < hop_count && remaining > 10; i++) {
    if (i > 0) {
      int written = snprintf(out, remaining, "→");
      out += written;
      remaining -= written;
    }

    const uint8_t* hash = &pkt->path[i * hash_size];
    for (uint8_t j = 0; j < hash_size && remaining > 3; j++) {
      int written = snprintf(out, remaining, "%02x", hash[j]);
      out += written;
      remaining -= written;
    }
  }

  snprintf(body, body_len, "%d %s %s", hop_count, hop_count == 1 ? "hop" : "hops", path_str);
}

static void buildHopSummary(char* body, size_t body_len, MyMesh& mesh, mesh::Packet* pkt) {
  uint8_t hop_count = pkt->getPathHashCount();

  if (hop_count == 0) {
    snprintf(body, body_len, "direct");
    return;
  }

  const char* repeater = findBestRepeater(mesh, pkt);
  if (repeater) {
    snprintf(body, body_len, "%d %s via %s", hop_count, hop_count == 1 ? "hop" : "hops", repeater);
  } else {
    snprintf(body, body_len, "%d %s", hop_count, hop_count == 1 ? "hop" : "hops");
  }
}

// Match command word at start; allows trailing text (e.g. "test da Como")
static bool matchesCommandWord(const char* message, const char* word) {
  size_t len = strlen(word);
  if (strncasecmp(message, word, len) != 0) return false;
  char next = message[len];
  return next == '\0' || next == ' ' || next == '\t';
}

static bool matchesBangCommandWord(const char* message, const char* word) {
  if (message == nullptr || message[0] != '!') return false;
  return matchesCommandWord(message + 1, word);
}

static bool matchesTestOrProva(const char* message) {
  return matchesCommandWord(message, "test") ||
         matchesBangCommandWord(message, "test") ||
         matchesCommandWord(message, "prova") ||
         matchesBangCommandWord(message, "prova");
}

static bool mentionMatchesNode(const char* mention, size_t mention_len, const char* node_name) {
  if (mention == nullptr || node_name == nullptr || mention_len == 0) return false;

  size_t node_len = strlen(node_name);
  if (node_len == mention_len && strncmp(mention, node_name, mention_len) == 0) {
    return true;
  }

  static constexpr const char* BOT_SUFFIX = " 🤖";
  size_t suffix_len = strlen(BOT_SUFFIX);
  if (node_len > suffix_len && strcmp(node_name + node_len - suffix_len, BOT_SUFFIX) == 0) {
    size_t base_len = node_len - suffix_len;
    if (base_len == mention_len && strncmp(mention, node_name, mention_len) == 0) {
      return true;
    }
  }

  return false;
}

// Skip leading @[node name] when it matches this companion
// (e.g. "@[Rasa-R 🤖] path" or "@[Rasa-R 🤖]: path")
static const char* stripLeadingBotMention(const char* message, const char* node_name) {
  if (message == nullptr || node_name == nullptr || strncmp(message, "@[", 2) != 0) {
    return message;
  }

  const char* end = strchr(message + 2, ']');
  if (end == nullptr) return message;

  size_t mention_len = (size_t)(end - (message + 2));
  if (!mentionMatchesNode(message + 2, mention_len, node_name)) {
    return message;
  }

  const char* cmd = end + 1;
  while (*cmd == ' ' || *cmd == '\t') cmd++;
  if (*cmd == ':') {
    cmd++;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
  }
  return cmd;
}

static constexpr size_t BOT_REPLY_MARGIN = 8;
static constexpr const char* PATH_UNKNOWN = "...";
static constexpr const char* PATH_SEP = "→";
static constexpr const char* PATH_GAP = "→...→";
static constexpr uint8_t PATH_MAX_SELECTED = 32;

static void sortPositions(uint8_t* positions, uint8_t count) {
  for (uint8_t i = 1; i < count; i++) {
    uint8_t key = positions[i];
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && positions[j] > key) {
      positions[j + 1] = positions[j];
      j--;
    }
    positions[j + 1] = key;
  }
}

static size_t measureJoinedPathLen(const uint8_t* positions, uint8_t count, const char** labels) {
  if (count == 0) return 0;

  const char* first_label = labels[positions[0]] ? labels[positions[0]] : PATH_UNKNOWN;
  size_t len = strlen(first_label);

  for (uint8_t i = 1; i < count; i++) {
    uint8_t gap = positions[i] - positions[i - 1];
    len += (gap == 1) ? strlen(PATH_SEP) : strlen(PATH_GAP);
    const char* label = labels[positions[i]] ? labels[positions[i]] : PATH_UNKNOWN;
    len += strlen(label);
  }
  return len;
}

static bool selectionContains(const uint8_t* positions, uint8_t count, uint8_t pos) {
  for (uint8_t i = 0; i < count; i++) {
    if (positions[i] == pos) return true;
  }
  return false;
}

static bool trySelectHop(uint8_t* positions, uint8_t& count, const char** labels, size_t path_budget,
                         uint8_t pos) {
  if (selectionContains(positions, count, pos)) return true;
  if (count >= PATH_MAX_SELECTED) return false;

  uint8_t trial[PATH_MAX_SELECTED];
  memcpy(trial, positions, count);
  trial[count] = pos;
  uint8_t trial_count = count + 1;
  sortPositions(trial, trial_count);

  if (measureJoinedPathLen(trial, trial_count, labels) > path_budget) {
    return false;
  }

  positions[count++] = pos;
  sortPositions(positions, count);
  return true;
}

static size_t measureBotPathBudget(const char* sender_name, const char* location,
                                   const char* warnings, uint8_t hop_count,
                                   const char* node_name) {
  char hop_prefix[16];
  snprintf(hop_prefix, sizeof(hop_prefix), "%d %s ", hop_count, hop_count == 1 ? "hop" : "hops");

  size_t reply_fixed = 2 + strlen(sender_name ? sender_name : "") + 1 + strlen(hop_prefix);
  if (location != nullptr && location[0] != '\0') {
    reply_fixed += 1 + strlen("📍 ") + strlen(location) + strlen(" 🤖");
  } else {
    reply_fixed += strlen(" 🤖");
  }
  if (warnings != nullptr && warnings[0] != '\0') {
    reply_fixed += strlen(warnings);
  }

  // sendGroupMessage prepends "node_name: " before the reply text
  size_t node_prefix = strlen(node_name ? node_name : "") + 2;
  size_t packet_limit = MAX_TEXT_LEN;
  if (node_prefix < packet_limit) {
    packet_limit -= node_prefix;
  }

  if (packet_limit <= reply_fixed + BOT_REPLY_MARGIN) return 32;
  return packet_limit - reply_fixed - BOT_REPLY_MARGIN;
}

static constexpr const char* PATH_TRAIL = "→...";

static void joinPathFragments(char* path_str, size_t path_str_len, const uint8_t* positions,
                              uint8_t count, const char** labels, uint8_t hop_count,
                              bool trailing_ellipsis) {
  path_str[0] = '\0';
  if (count == 0) return;

  char* out = path_str;
  size_t remaining = path_str_len;

  const char* first_label = labels[positions[0]] ? labels[positions[0]] : PATH_UNKNOWN;
  int written = snprintf(out, remaining, "%s", first_label);
  out += written;
  remaining -= written;

  for (uint8_t i = 1; i < count && remaining > 1; i++) {
    uint8_t gap = positions[i] - positions[i - 1];
    const char* sep = (gap == 1) ? PATH_SEP : PATH_GAP;
    const char* label = labels[positions[i]] ? labels[positions[i]] : PATH_UNKNOWN;

    written = snprintf(out, remaining, "%s%s", sep, label);
    out += written;
    remaining -= written;
  }

  if (trailing_ellipsis && remaining > strlen(PATH_TRAIL)) {
    snprintf(out, remaining, "%s", PATH_TRAIL);
  } else if (hop_count > 1 && count > 0 && positions[count - 1] < hop_count - 1 &&
             remaining > strlen(PATH_TRAIL)) {
    snprintf(out, remaining, "%s", PATH_TRAIL);
  }
}

static void buildHopPathWithNames(char* body, size_t body_len, MyMesh& mesh, mesh::Packet* pkt,
                                  size_t path_budget) {
  uint8_t hop_count = pkt->getPathHashCount();
  uint8_t hash_size = pkt->getPathHashSize();

  if (hop_count == 0) {
    snprintf(body, body_len, "direct");
    return;
  }

  if (hop_count > PATH_MAX_SELECTED) {
    snprintf(body, body_len, "%d %s ...", hop_count, hop_count == 1 ? "hop" : "hops");
    return;
  }

  const char* labels[PATH_MAX_SELECTED] = {0};
  for (uint8_t i = 0; i < hop_count && i < PATH_MAX_SELECTED; i++) {
    ContactInfo* contact = mesh.lookupContactByPubKey(&pkt->path[i * hash_size], hash_size);
    if (contact) labels[i] = contact->name;
  }

  uint8_t positions[PATH_MAX_SELECTED];
  uint8_t count = 0;
  bool trailing_ellipsis = false;

  positions[count++] = 0;
  if (hop_count > 1) {
    uint8_t anchors[2] = {0, (uint8_t)(hop_count - 1)};
    if (measureJoinedPathLen(anchors, 2, labels) <= path_budget) {
      positions[count++] = hop_count - 1;
      sortPositions(positions, count);
    } else {
      trailing_ellipsis = true;
    }
  }

  if (hop_count > 2 && !trailing_ellipsis) {
    uint8_t fwd = 1;
    uint8_t bwd = hop_count - 2;
    bool forward_turn = true;
    bool budget_exhausted = false;

    while (fwd <= bwd && !budget_exhausted) {
      bool added = false;

      if (forward_turn) {
        for (uint8_t i = fwd; i <= bwd; i++) {
          if (labels[i] == nullptr) continue;
          if (trySelectHop(positions, count, labels, path_budget, i)) {
            fwd = i + 1;
            added = true;
            break;
          }
          budget_exhausted = true;
          break;
        }
        if (!added && !budget_exhausted) fwd = bwd + 1;
      } else {
        for (int16_t i = bwd; i >= (int16_t)fwd; i--) {
          if (labels[(uint8_t)i] == nullptr) continue;
          if (trySelectHop(positions, count, labels, path_budget, (uint8_t)i)) {
            bwd = (uint8_t)i - 1;
            added = true;
            break;
          }
          budget_exhausted = true;
          break;
        }
        if (!added && !budget_exhausted) bwd = fwd - 1;
      }

      forward_turn = !forward_turn;
    }
  }

  char path_str[128];
  joinPathFragments(path_str, sizeof(path_str), positions, count, labels, hop_count,
                    trailing_ellipsis);
  snprintf(body, body_len, "%d %s %s", hop_count, hop_count == 1 ? "hop" : "hops", path_str);
}

static bool isBotChannel(const char* channel_name) {
  return strcmp(channel_name, "#bot") == 0 ||
         strcmp(channel_name, "#test") == 0 ||
         strcmp(channel_name, "#ping") == 0 ||
         strcmp(channel_name, "#prove") == 0;
}

static bool sendCasualReply(MyMesh& mesh, mesh::GroupChannel& channel,
                            const char* sender_name, const char* text) {
  char reply[MAX_TEXT_LEN + 1];
  if (sender_name != nullptr && sender_name[0] != '\0') {
    snprintf(reply, sizeof(reply), "@[%s] %s 🤖", sender_name, text);
  } else {
    snprintf(reply, sizeof(reply), "%s 🤖", text);
  }

  uint32_t timestamp = mesh.getRTCClock()->getCurrentTime();
  if (mesh.sendGroupMessage(timestamp, channel, mesh.getNodeName(), reply, strlen(reply))) {
    Serial.printf("[BOT] Sent casual reply\n");
    return true;
  }
  return false;
}

static bool sendBotReply(MyMesh& mesh, const char* channel_name, mesh::GroupChannel& channel,
                         const char* sender_name, const char* body,
                         const char* location, const char* warnings) {
  char reply[MAX_TEXT_LEN + 1];

  // Assemble: @[sender] {body}\n{location} 🤖\n{warnings}
  // Note: warnings includes "\n" prefix if non-empty
  if (location && location[0] != '\0') {
    snprintf(reply, sizeof(reply), "@[%s] %s\n📍 %s 🤖%s", sender_name, body, location, warnings);
  } else {
    snprintf(reply, sizeof(reply), "@[%s] %s 🤖%s", sender_name, body, warnings);
  }

  uint32_t timestamp = mesh.getRTCClock()->getCurrentTime();
  if (mesh.sendGroupMessage(timestamp, channel, mesh.getNodeName(), reply, strlen(reply))) {
    Serial.printf("[BOT] Sent reply to channel %s\n", channel_name);
    return true;
  }
  return false;
}

bool botHandleChannel(MyMesh& mesh, const char* channel_name, mesh::GroupChannel& channel,
                      mesh::Packet* pkt, const char* text) {
  if (channel_name == nullptr || text == nullptr || pkt == nullptr) {
    Serial.printf("[BOT] Channel handler called: channel=%s, text=%s\n",
                  channel_name ? channel_name : "null",
                  text ? text : "null");
    return false;
  }

  Serial.printf("[BOT] Channel: %s, Text: %s\n", channel_name, text);

  // Extract sender name and message from "SenderName: message" format
  static char sender_name[64];
  const char* message = strchr(text, ':');
  if (message != nullptr) {
    // Extract sender name
    size_t name_len = message - text;
    if (name_len >= sizeof(sender_name)) name_len = sizeof(sender_name) - 1;
    memcpy(sender_name, text, name_len);
    sender_name[name_len] = '\0';

    message++;  // Skip the ':'
    while (*message == ' ') message++;  // Skip spaces after ':'
  } else {
    sender_name[0] = '\0';  // No sender name found
    message = text;  // No prefix, use whole text
  }

  Serial.printf("[BOT] Sender: %s, Message: %s\n", sender_name, message);

  const char* cmd = stripLeadingBotMention(message, mesh.getNodeName());
  if (cmd != message) {
    Serial.printf("[BOT] Command after mention strip: %s\n", cmd);
  }

  const bool in_bot_channel = isBotChannel(channel_name);
  const char* location = botGetLocation();

  // Public/other channels: casual replies only when location is configured
  if (!in_bot_channel) {
    if (location[0] == '\0') {
      Serial.printf("[BOT] Not a bot channel and no location set, ignoring\n");
      return false;
    }

    if (matchesTestOrProva(cmd)) {

      uint8_t hop_count = pkt->getPathHashCount();
      char body[128];
      if (hop_count == 0) {
        snprintf(body, sizeof(body), "direct da %s", location);
      } else {
        snprintf(body, sizeof(body), "%d %s da %s", hop_count,
                 hop_count == 1 ? "hop" : "hops", location);
      }
      sendCasualReply(mesh, channel, sender_name, body);
      return true;
    }

    return false;
  }

  // Handle !ping command (case-insensitive)
  if ((strncasecmp(cmd, "!ping", 5) == 0 && (cmd[5] == '\0' || cmd[5] == ' ')) ||
      (strncasecmp(cmd, "ping", 4) == 0 && (cmd[4] == '\0' || cmd[4] == ' '))) {

    uint8_t hop_count = pkt->getPathHashCount();
    char body[64];

    if (hop_count == 0) {
      // Direct connection
      snprintf(body, sizeof(body), "🏓 pong!");
    } else if (hop_count <= 5) {
      // Visual arc representation for 1-5 hops
      char* out = body;
      size_t remaining = sizeof(body);
      for (uint8_t i = 0; i < hop_count && remaining > 10; i++) {
        if (i > 0) {
          int written = snprintf(out, remaining, " ");
          out += written;
          remaining -= written;
        }
        int written = snprintf(out, remaining, "⌢");
        out += written;
        remaining -= written;
      }
      snprintf(out, remaining, " 🏓 pong!");
    } else {
      // 6+ hops: numeric with ellipsis
      snprintf(body, sizeof(body), "… %d hops … 🏓 pong!", hop_count);
    }

    sendBotReply(mesh, channel_name, channel, sender_name, body, nullptr, "");
    return true;
  }

  // Handle !test/test/!prova/prova command (case-insensitive)
  if (matchesTestOrProva(cmd)) {

    uint8_t hash_size = pkt->getPathHashSize();
    const char* warnings = getBotWarnings(hash_size, pkt->hasTransportCodes());

    char body[128];
    if (strcmp(channel_name, "#bot") == 0) {
      buildHopPath(body, sizeof(body), pkt);
    } else {
      buildHopSummary(body, sizeof(body), mesh, pkt);
    }

    sendBotReply(mesh, channel_name, channel, sender_name, body, location, warnings);
    return true;
  }

  // Handle !path or path command
  if ((strncasecmp(cmd, "!path", 5) == 0 && (cmd[5] == '\0' || cmd[5] == ' ')) ||
      (strncasecmp(cmd, "path", 4) == 0 && (cmd[4] == '\0' || cmd[4] == ' '))) {

    uint8_t hop_count = pkt->getPathHashCount();
    uint8_t hash_size = pkt->getPathHashSize();
    const char* warnings = getBotWarnings(hash_size, pkt->hasTransportCodes());
    // Named paths omit location to free budget for repeater names; hex paths keep it.
    const char* path_location = (hash_size == 1) ? location : nullptr;

    char body[128];
    if (hash_size == 1) {
      // 1-byte hashes: show hex path (unreliable resolution)
      buildHopPath(body, sizeof(body), pkt);
    } else {
      // 2/3-byte hashes: show resolved names with ellipsis
      size_t path_budget = measureBotPathBudget(sender_name, path_location, warnings, hop_count,
                                                mesh.getNodeName());
      buildHopPathWithNames(body, sizeof(body), mesh, pkt, path_budget);
    }

    sendBotReply(mesh, channel_name, channel, sender_name, body, path_location, warnings);

    // Debug logging for 2/3-byte hashes
    if (hash_size == 2 || hash_size == 3) {
      Serial.printf("[BOT] Path command with %d-byte hashes, %d hops:\n", hash_size, hop_count);
      uint8_t found_count = 0;
      for (uint8_t i = 0; i < hop_count; i++) {
        const uint8_t* hash = &pkt->path[i * hash_size];

        Serial.printf("[BOT]   Hop %d: ", i);
        for (uint8_t j = 0; j < hash_size; j++) {
          Serial.printf("%02x", hash[j]);
        }

        ContactInfo* contact = mesh.lookupContactByPubKey(hash, hash_size);
        if (contact) {
          Serial.printf(" -> Found: %s\n", contact->name);
          found_count++;
        } else {
          Serial.printf(" -> Not found\n");
        }
      }
      Serial.printf("[BOT] Path resolution: %d/%d hops found\n", found_count, hop_count);
    }

    return true;
  }

  // Handle !echo command (case-insensitive)
  if (strncasecmp(cmd, "!echo ", 6) == 0) {
    const char* echo_text = cmd + 6;  // Skip "!echo "

    char body[MAX_TEXT_LEN];
    snprintf(body, sizeof(body), "Echo: %s", echo_text);
    sendBotReply(mesh, channel_name, channel, sender_name, body, nullptr, "");
    return true;
  }

  return false;  // Not a bot command
}
