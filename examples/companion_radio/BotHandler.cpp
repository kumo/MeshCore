// ABOUTME: Bot command handler for companion radio devices
// ABOUTME: Handles bot configuration and automated message responses

#include "BotHandler.h"
#include "MyMesh.h"
#include <Arduino.h>
#include <Mesh.h>
#include <SHA256.h>

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
static bool reply_all_channels = false;
static bool warnings_enabled = true;
static bool match_sender_region = false;
static uint8_t home_repeater_hash[3] = {0};
static uint8_t home_repeater_hash_len = 0;

// Region matching: store region names and derived TransportKeys
static constexpr uint8_t MAX_MATCH_REGIONS = 8;
static uint8_t match_region_count = 0;
static char match_region_names[MAX_MATCH_REGIONS][32] = {0};
static TransportKey match_region_keys[MAX_MATCH_REGIONS];

// Reply tracking for non-bot channels (prevent spam)
static constexpr uint8_t MAX_REPLY_TRACKING = 16;
static constexpr uint32_t REPLY_WINDOW_SEC = 600;  // 10 minutes

struct ReplyState {
  uint32_t sender_id;
  uint32_t last_reply_time;
  uint8_t reply_count;
};

static ReplyState reply_tracking[MAX_REPLY_TRACKING] = {0};
static uint8_t reply_tracking_count = 0;

// Forward declarations
static void generateRegionKeys();
static const TransportKey* matchIncomingRegion(mesh::Packet* pkt);

// Convert hex char to value (0-15), returns 255 on error
static uint8_t hexCharToValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 255;
}

// Parse hex string to bytes (e.g. "8dbb" -> {0x8d, 0xbb})
// Returns number of bytes parsed, 0 on error
static uint8_t parseHexString(const char* hex_str, uint8_t* dest, uint8_t max_len) {
  if (hex_str == nullptr || dest == nullptr) return 0;

  uint8_t len = 0;
  while (*hex_str && len < max_len) {
    uint8_t high = hexCharToValue(*hex_str++);
    if (high == 255) return 0;

    if (*hex_str == '\0') return 0;  // Odd number of chars
    uint8_t low = hexCharToValue(*hex_str++);
    if (low == 255) return 0;

    dest[len++] = (high << 4) | low;
  }

  return len;
}

// Convert bytes to hex string (e.g. {0x8d, 0xbb} -> "8dbb")
static void bytesToHexString(const uint8_t* bytes, uint8_t len, char* dest, size_t dest_len) {
  if (bytes == nullptr || dest == nullptr || dest_len == 0) return;

  size_t pos = 0;
  for (uint8_t i = 0; i < len && pos + 2 < dest_len; i++) {
    snprintf(dest + pos, dest_len - pos, "%02x", bytes[i]);
    pos += 2;
  }
  dest[pos] = '\0';
}

// Generate TransportKeys from region names using SHA256
// Similar to NessoN1 approach: SHA256("#regionname") -> TransportKey
static void generateRegionKeys() {
  match_region_count = 0;

  for (uint8_t i = 0; i < MAX_MATCH_REGIONS; i++) {
    if (match_region_names[i][0] == '\0') break;

    // Prepend '#' to region name for hashing (standard format)
    char hash_input[33];
    snprintf(hash_input, sizeof(hash_input), "#%s", match_region_names[i]);

    // Calculate SHA256 to derive TransportKey
    SHA256 sha;
    sha.update(hash_input, strlen(hash_input));
    sha.finalize(match_region_keys[i].key, sizeof(match_region_keys[i].key));

    match_region_count++;
    Serial.printf("[BOT] Generated key for region '%s'\n", match_region_names[i]);
  }
}

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

  // Parse line by line: "enabled=0/1", "location=...", "reply_all=0/1", "warnings=0/1", "match_region=0/1", "match_regions=...", "home=..."
  char* ctx = nullptr;
  char* line = strtok_r(buf, "\n", &ctx);
  while (line != nullptr) {
    if (strncmp(line, "enabled=", 8) == 0) {
      bot_enabled = (line[8] == '1');
    } else if (strncmp(line, "location=", 9) == 0) {
      strncpy(bot_location, line + 9, sizeof(bot_location) - 1);
      bot_location[sizeof(bot_location) - 1] = '\0';
    } else if (strncmp(line, "reply_all=", 10) == 0) {
      reply_all_channels = (line[10] == '1');
    } else if (strncmp(line, "warnings=", 9) == 0) {
      warnings_enabled = (line[9] == '1');
    } else if (strncmp(line, "match_region=", 13) == 0) {
      match_sender_region = (line[13] == '1');
    } else if (strncmp(line, "match_regions=", 14) == 0) {
      // Parse comma-separated region names (e.g., "it-lom, europe, it")
      char* region_ctx = nullptr;
      char* region_str = line + 14;
      char* region = strtok_r(region_str, ",", &region_ctx);
      match_region_count = 0;

      while (region != nullptr && match_region_count < MAX_MATCH_REGIONS) {
        // Trim leading spaces
        while (*region == ' ') region++;

        // Trim trailing spaces
        char* end = region + strlen(region) - 1;
        while (end > region && *end == ' ') *end-- = '\0';

        if (*region != '\0') {
          strncpy(match_region_names[match_region_count], region, sizeof(match_region_names[0]) - 1);
          match_region_names[match_region_count][sizeof(match_region_names[0]) - 1] = '\0';
          match_region_count++;
        }

        region = strtok_r(nullptr, ",", &region_ctx);
      }
    } else if (strncmp(line, "home=", 5) == 0) {
      home_repeater_hash_len = parseHexString(line + 5, home_repeater_hash, sizeof(home_repeater_hash));
    }
    line = strtok_r(nullptr, "\n", &ctx);
  }

  // Generate TransportKeys from region names
  if (match_region_count > 0) {
    generateRegionKeys();
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
  file.printf("reply_all=%d\n", reply_all_channels ? 1 : 0);
  file.printf("warnings=%d\n", warnings_enabled ? 1 : 0);
  file.printf("match_region=%d\n", match_sender_region ? 1 : 0);

  // Save match_regions as comma-separated list
  if (match_region_count > 0) {
    file.printf("match_regions=");
    for (uint8_t i = 0; i < match_region_count; i++) {
      file.printf("%s", match_region_names[i]);
      if (i < match_region_count - 1) {
        file.printf(",");
      }
    }
    file.printf("\n");
  }

  if (home_repeater_hash_len > 0) {
    char hex_str[8];
    bytesToHexString(home_repeater_hash, home_repeater_hash_len, hex_str, sizeof(hex_str));
    file.printf("home=%s\n", hex_str);
  }

  file.close();
  return true;
}

bool botIsEnabled() {
  return bot_enabled;
}

const char* botGetLocation() {
  return bot_location;
}

bool botGetReplyAll() {
  return reply_all_channels;
}

bool botGetWarningsEnabled() {
  return warnings_enabled;
}

bool botGetMatchSenderRegion() {
  return match_sender_region;
}

void botGetMatchRegions(char* buf, size_t max_len) {
  if (buf == nullptr || max_len == 0) return;

  buf[0] = '\0';

  if (match_region_count == 0) return;

  size_t pos = 0;
  for (uint8_t i = 0; i < match_region_count && pos < max_len - 1; i++) {
    size_t region_len = strlen(match_region_names[i]);

    // Add comma if not first region and there's space
    if (i > 0 && pos + 1 < max_len - 1) {
      buf[pos++] = ',';
    }

    // Copy region name if there's space
    if (pos + region_len < max_len) {
      strcpy(buf + pos, match_region_names[i]);
      pos += region_len;
    } else {
      break;  // Not enough space
    }
  }

  buf[pos] = '\0';
}

// Get or create reply state for a sender in non-bot channels
// Returns nullptr if tracking is full and sender not found
static ReplyState* getReplyState(uint32_t sender_id, uint32_t current_time) {
  // Check if sender exists and is within time window
  for (uint8_t i = 0; i < reply_tracking_count; i++) {
    if (reply_tracking[i].sender_id == sender_id) {
      // Check if outside time window - reset if so
      if (current_time - reply_tracking[i].last_reply_time > REPLY_WINDOW_SEC) {
        reply_tracking[i].last_reply_time = 0;
        reply_tracking[i].reply_count = 0;
      }
      return &reply_tracking[i];
    }
  }

  // Not found - add new entry if space available
  if (reply_tracking_count < MAX_REPLY_TRACKING) {
    ReplyState* state = &reply_tracking[reply_tracking_count++];
    state->sender_id = sender_id;
    state->last_reply_time = 0;
    state->reply_count = 0;
    return state;
  }

  // Tracking full - evict oldest entry
  ReplyState* oldest = &reply_tracking[0];
  for (uint8_t i = 1; i < MAX_REPLY_TRACKING; i++) {
    if (reply_tracking[i].last_reply_time < oldest->last_reply_time) {
      oldest = &reply_tracking[i];
    }
  }
  oldest->sender_id = sender_id;
  oldest->last_reply_time = 0;
  oldest->reply_count = 0;
  return oldest;
}

// Check if message is a strict command (just "test"/"prova" alone)
static bool isStrictTestCommand(const char* cmd) {
  if (cmd == nullptr) return false;

  // Check for exact "test" or "prova" (case-insensitive, no trailing text)
  if ((strncasecmp(cmd, "test", 4) == 0 && cmd[4] == '\0') ||
      (strncasecmp(cmd, "prova", 5) == 0 && cmd[5] == '\0') ||
      (strncasecmp(cmd, "!test", 5) == 0 && cmd[5] == '\0') ||
      (strncasecmp(cmd, "!prova", 6) == 0 && cmd[6] == '\0')) {
    return true;
  }
  return false;
}

static void clearReplyTracking() {
  reply_tracking_count = 0;
  memset(reply_tracking, 0, sizeof(reply_tracking));
}

// Simple hash function for sender names
static uint32_t hashSenderName(const char* name) {
  if (name == nullptr) return 0;
  uint32_t hash = 5381;
  while (*name) {
    hash = ((hash << 5) + hash) + (uint8_t)(*name++);  // hash * 33 + c
  }
  return hash;
}

// Check if last hop in path matches configured home repeater
// Returns true if at home, false otherwise
static bool isAtHomeRepeater(mesh::Packet* pkt) {
  // Not configured - never at home
  if (home_repeater_hash_len == 0) return false;

  uint8_t hop_count = pkt->getPathHashCount();
  if (hop_count == 0) return false;  // Direct connection, not via repeater

  uint8_t hash_size = pkt->getPathHashSize();
  const uint8_t* last_hop = &pkt->path[(hop_count - 1) * hash_size];

  // Compare configured hash with last hop
  // Compare only up to the minimum of config length and path hash size
  // This way "8dbb" matches both 1-byte "8d" and 2-byte "8dbb"
  uint8_t compare_len = (home_repeater_hash_len < hash_size) ? home_repeater_hash_len : hash_size;

  for (uint8_t i = 0; i < compare_len; i++) {
    if (home_repeater_hash[i] != last_hop[i]) return false;
  }

  return true;
}

static const char* getBotWarnings(uint8_t hash_size, bool has_region) {
  if (!warnings_enabled) {
    return "";
  }

  bool needs_bytes_warning = (hash_size == 1);
  bool needs_region_warning = !has_region;

  if (needs_bytes_warning && needs_region_warning) {
    return "\n⚠ impostare 2-byte per vedere il path & region it";
  } else if (needs_bytes_warning) {
    return "\n⚠ impostare 2-byte per vedere il path";
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
    char status[256];
    snprintf(status, sizeof(status), "bot: %s", bot_enabled ? "enabled" : "disabled");

    if (bot_location[0] != '\0') {
      size_t len = strlen(status);
      snprintf(status + len, sizeof(status) - len, ", location: %s", bot_location);
    }

    size_t len = strlen(status);
    snprintf(status + len, sizeof(status) - len, ", reply-all: %s",
             reply_all_channels ? "on" : "off");

    len = strlen(status);
    snprintf(status + len, sizeof(status) - len, ", warnings: %s",
             warnings_enabled ? "on" : "off");

    len = strlen(status);
    snprintf(status + len, sizeof(status) - len, ", match-region: %s",
             match_sender_region ? "on" : "off");

    if (match_region_count > 0) {
      len = strlen(status);
      snprintf(status + len, sizeof(status) - len, ", regions: ");
      for (uint8_t i = 0; i < match_region_count; i++) {
        len = strlen(status);
        snprintf(status + len, sizeof(status) - len, "%s%s",
                 match_region_names[i], i < match_region_count - 1 ? "," : "");
      }
    }

    if (home_repeater_hash_len > 0) {
      char hex_str[8];
      bytesToHexString(home_repeater_hash, home_repeater_hash_len, hex_str, sizeof(hex_str));
      len = strlen(status);
      snprintf(status + len, sizeof(status) - len, ", home: %s", hex_str);
    }

    snprintf(reply, reply_len, "%s", status);
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

  if (strcmp(text, "!bot reply-all on") == 0) {
    reply_all_channels = true;
    if (!botSaveState()) {
      snprintf(reply, reply_len, "Error: could not save bot state");
      return true;
    }
    snprintf(reply, reply_len, "OK - reply-all enabled");
    return true;
  }

  if (strcmp(text, "!bot reply-all off") == 0) {
    reply_all_channels = false;
    if (!botSaveState()) {
      snprintf(reply, reply_len, "Error: could not save bot state");
      return true;
    }
    snprintf(reply, reply_len, "OK - reply-all disabled");
    return true;
  }

  if (strcmp(text, "!bot warnings on") == 0) {
    warnings_enabled = true;
    if (!botSaveState()) {
      snprintf(reply, reply_len, "Error: could not save bot state");
      return true;
    }
    snprintf(reply, reply_len, "OK - warnings enabled");
    return true;
  }

  if (strcmp(text, "!bot warnings off") == 0) {
    warnings_enabled = false;
    if (!botSaveState()) {
      snprintf(reply, reply_len, "Error: could not save bot state");
      return true;
    }
    snprintf(reply, reply_len, "OK - warnings disabled");
    return true;
  }

  if (strcmp(text, "!bot match-region on") == 0) {
    match_sender_region = true;
    if (!botSaveState()) {
      snprintf(reply, reply_len, "Error: could not save bot state");
      return true;
    }
    snprintf(reply, reply_len, "OK - match-region enabled");
    return true;
  }

  if (strcmp(text, "!bot match-region off") == 0) {
    match_sender_region = false;
    if (!botSaveState()) {
      snprintf(reply, reply_len, "Error: could not save bot state");
      return true;
    }
    snprintf(reply, reply_len, "OK - match-region disabled");
    return true;
  }

  if (strncmp(text, "!bot home ", 10) == 0) {
    const char* hash_str = text + 10;
    uint8_t new_hash[3];
    uint8_t new_len = parseHexString(hash_str, new_hash, sizeof(new_hash));

    if (new_len == 0 || new_len > 3) {
      snprintf(reply, reply_len, "Error: invalid hex string (use 1-3 bytes, e.g. '8dbb')");
      return true;
    }

    memcpy(home_repeater_hash, new_hash, new_len);
    home_repeater_hash_len = new_len;

    if (!botSaveState()) {
      snprintf(reply, reply_len, "Error: could not save bot state");
      return true;
    }

    char hex_str[8];
    bytesToHexString(home_repeater_hash, home_repeater_hash_len, hex_str, sizeof(hex_str));
    snprintf(reply, reply_len, "OK - home repeater set to: %s", hex_str);
    return true;
  }

  if (strncmp(text, "!bot match region ", 18) == 0) {
    const char* regions_str = text + 18;

    // Parse comma-separated region names
    char temp_buf[256];
    strncpy(temp_buf, regions_str, sizeof(temp_buf) - 1);
    temp_buf[sizeof(temp_buf) - 1] = '\0';

    // Clear existing regions
    for (uint8_t i = 0; i < MAX_MATCH_REGIONS; i++) {
      match_region_names[i][0] = '\0';
    }
    match_region_count = 0;

    char* ctx = nullptr;
    char* region = strtok_r(temp_buf, ",", &ctx);
    while (region != nullptr && match_region_count < MAX_MATCH_REGIONS) {
      // Trim leading spaces
      while (*region == ' ') region++;

      // Trim trailing spaces
      char* end = region + strlen(region) - 1;
      while (end > region && *end == ' ') *end-- = '\0';

      if (*region != '\0') {
        strncpy(match_region_names[match_region_count], region, sizeof(match_region_names[0]) - 1);
        match_region_names[match_region_count][sizeof(match_region_names[0]) - 1] = '\0';
        match_region_count++;
      }

      region = strtok_r(nullptr, ",", &ctx);
    }

    // Generate TransportKeys for the new regions
    generateRegionKeys();

    if (!botSaveState()) {
      snprintf(reply, reply_len, "Error: could not save bot state");
      return true;
    }

    snprintf(reply, reply_len, "OK - match regions set to: %s", regions_str);
    return true;
  }

  if (strcmp(text, "!bot clear location") == 0) {
    bot_location[0] = '\0';

    if (!botSaveState()) {
      snprintf(reply, reply_len, "Error: could not save bot state");
      return true;
    }

    snprintf(reply, reply_len, "OK - location cleared");
    return true;
  }

  if (strcmp(text, "!bot clear home") == 0) {
    home_repeater_hash_len = 0;
    memset(home_repeater_hash, 0, sizeof(home_repeater_hash));

    if (!botSaveState()) {
      snprintf(reply, reply_len, "Error: could not save bot state");
      return true;
    }

    snprintf(reply, reply_len, "OK - home repeater cleared");
    return true;
  }

  if (strcmp(text, "!bot clear regions") == 0) {
    for (uint8_t i = 0; i < MAX_MATCH_REGIONS; i++) {
      match_region_names[i][0] = '\0';
    }
    match_region_count = 0;

    if (!botSaveState()) {
      snprintf(reply, reply_len, "Error: could not save bot state");
      return true;
    }

    snprintf(reply, reply_len, "OK - match regions cleared");
    return true;
  }

  if (strcmp(text, "!bot clear") == 0) {
    clearReplyTracking();
    snprintf(reply, reply_len, "OK - reply tracking cleared");
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

  snprintf(body, body_len, "%d %s: %s", hop_count, hop_count == 1 ? "hop" : "hops", path_str);
}

// Calculate distance between two lat/lon points using haversine formula
// lat/lon are stored as int32_t with 6 decimal places
// Returns distance in kilometers
static float calculateDistance(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2) {
  // Check for invalid coordinates (0,0)
  if ((lat1 == 0 && lon1 == 0) || (lat2 == 0 && lon2 == 0)) {
    return -1.0f;  // Invalid
  }

  // Convert to radians
  float lat1_rad = (lat1 / 1000000.0f) * M_PI / 180.0f;
  float lon1_rad = (lon1 / 1000000.0f) * M_PI / 180.0f;
  float lat2_rad = (lat2 / 1000000.0f) * M_PI / 180.0f;
  float lon2_rad = (lon2 / 1000000.0f) * M_PI / 180.0f;

  // Haversine formula
  float dlat = lat2_rad - lat1_rad;
  float dlon = lon2_rad - lon1_rad;
  float a = sin(dlat/2) * sin(dlat/2) +
            cos(lat1_rad) * cos(lat2_rad) *
            sin(dlon/2) * sin(dlon/2);
  float c = 2 * atan2(sqrt(a), sqrt(1-a));

  return 6371.0f * c;  // Earth radius in km
}

// Calculate total path distance by summing distances between consecutive hops
// Returns total distance in km, and sets incomplete=true if any GPS data is missing
// Sets gps_hop_count to number of hops with valid GPS data
static float calculatePathDistance(MyMesh& mesh, mesh::Packet* pkt, bool& incomplete, uint8_t& gps_hop_count) {
  static constexpr float MAX_HOP_DISTANCE_KM = 200.0f;  // Sanity check for GPS data

  incomplete = false;
  gps_hop_count = 0;
  uint8_t hop_count = pkt->getPathHashCount();
  uint8_t hash_size = pkt->getPathHashSize();

  if (hop_count < 2) return 0.0f;  // Need at least 2 hops to calculate distance

  Serial.printf("[BOT] Calculating path distance for %d hops:\n", hop_count);
  float total_distance = 0.0f;

  // Find first known contact with valid GPS as starting point
  ContactInfo* last_known = nullptr;
  int8_t last_known_idx = -1;

  for (uint8_t i = 0; i < hop_count; i++) {
    const uint8_t* hash = &pkt->path[i * hash_size];
    ContactInfo* contact = mesh.lookupContactByPubKey(hash, hash_size);

    if (contact && !(contact->gps_lat == 0 && contact->gps_lon == 0)) {
      gps_hop_count++;
      // If this is the first known hop and it's not at the start, mark incomplete
      if (!last_known && i > 0) {
        Serial.printf("[BOT]   (skipped %d unknown hop%s at start)\n", i, i == 1 ? "" : "s");
        incomplete = true;
      }

      if (last_known) {
        // We have two known points - calculate distance between them
        Serial.printf("[BOT]   Hop %d->%d: ", last_known_idx, i);

        float dist = calculateDistance(last_known->gps_lat, last_known->gps_lon,
                                      contact->gps_lat, contact->gps_lon);

        Serial.printf("%s (%.6f, %.6f) -> %s (%.6f, %.6f)\n",
                     last_known->name, last_known->gps_lat/1000000.0, last_known->gps_lon/1000000.0,
                     contact->name, contact->gps_lat/1000000.0, contact->gps_lon/1000000.0);

        if (i > last_known_idx + 1) {
          Serial.printf("[BOT]     (skipped %d unknown hop%s)\n",
                       i - last_known_idx - 1,
                       (i - last_known_idx - 1) == 1 ? "" : "s");
          incomplete = true;
        }

        if (dist > 0 && dist <= MAX_HOP_DISTANCE_KM) {
          total_distance += dist;
          Serial.printf("[BOT]     Distance: %.1fkm (total: %.1fkm)\n", dist, total_distance);
        } else if (dist > MAX_HOP_DISTANCE_KM) {
          incomplete = true;  // Unreasonable distance, likely bad GPS data
          Serial.printf("[BOT]     Distance %.1fkm exceeds max %dkm - GPS data suspect, skipping\n",
                       dist, (int)MAX_HOP_DISTANCE_KM);
        } else {
          incomplete = true;  // Missing GPS data
          Serial.printf("[BOT]     Missing GPS data (0,0)\n");
        }
      }

      last_known = contact;
      last_known_idx = i;
    }
  }

  // Check for unknown hops at the end
  if (last_known_idx >= 0 && last_known_idx < hop_count - 1) {
    uint8_t trailing_unknown = hop_count - 1 - last_known_idx;
    Serial.printf("[BOT]   (skipped %d unknown hop%s at end)\n",
                 trailing_unknown, trailing_unknown == 1 ? "" : "s");
    incomplete = true;
  }

  Serial.printf("[BOT] Total path distance: %.1fkm%s (%d/%d hops with GPS)\n",
                total_distance, incomplete ? " (incomplete)" : "", gps_hop_count, hop_count);
  return total_distance;
}

static void buildHopSummary(char* body, size_t body_len, MyMesh& mesh, mesh::Packet* pkt) {
  static constexpr uint8_t MIN_GPS_PERCENTAGE = 40;  // Minimum % of hops with GPS to show distance

  uint8_t hop_count = pkt->getPathHashCount();

  if (hop_count == 0) {
    snprintf(body, body_len, "direct");
    return;
  }

  // Calculate path distance if available
  bool incomplete = false;
  uint8_t gps_hop_count = 0;
  float distance = calculatePathDistance(mesh, pkt, incomplete, gps_hop_count);

  // Only show distance if we have GPS data for enough hops
  bool show_distance = false;
  if (distance > 0 && hop_count > 0) {
    uint8_t gps_percentage = (gps_hop_count * 100) / hop_count;
    show_distance = (gps_percentage >= MIN_GPS_PERCENTAGE);
    Serial.printf("[BOT] GPS coverage: %d%% (%d/%d hops) - %s distance\n",
                  gps_percentage, gps_hop_count, hop_count,
                  show_distance ? "showing" : "hiding");
  }

  const char* repeater = findBestRepeater(mesh, pkt);

  // Format: "N hops via Repeater (45km)" or "N hops via Repeater (~45km)"
  if (repeater) {
    if (show_distance) {
      snprintf(body, body_len, "%d %s via %s (%s%.0fkm)",
               hop_count, hop_count == 1 ? "hop" : "hops", repeater,
               incomplete ? "~" : "", distance);
    } else {
      snprintf(body, body_len, "%d %s via %s", hop_count, hop_count == 1 ? "hop" : "hops", repeater);
    }
  } else {
    if (show_distance) {
      snprintf(body, body_len, "%d %s (%s%.0fkm)",
               hop_count, hop_count == 1 ? "hop" : "hops",
               incomplete ? "~" : "", distance);
    } else {
      snprintf(body, body_len, "%d %s", hop_count, hop_count == 1 ? "hop" : "hops");
    }
  }
}

// Match command word at start; allows trailing text (e.g. "test da Como")
static const char* skipLeadingWhitespace(const char* s) {
  if (s == nullptr) return s;
  while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
  return s;
}

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
  if (*cmd == ':') cmd++;
  return skipLeadingWhitespace(cmd);
}

static constexpr size_t BOT_REPLY_MARGIN = 8;
static constexpr const char* PATH_SEP = "→";   // adjacent known hops
static constexpr const char* PATH_GAP = "⇢";  // non-adjacent / hidden hops
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

static size_t measureJoinedPathLen(const uint8_t* positions, uint8_t count, const char** labels,
                                   uint8_t hop_count, bool trailing_ellipsis) {
  if (count == 0) return 0;

  size_t len = 0;
  if (positions[0] > 0) len += strlen(PATH_GAP);
  len += strlen(labels[positions[0]]);

  for (uint8_t i = 1; i < count; i++) {
    uint8_t gap = positions[i] - positions[i - 1];
    len += (gap == 1) ? strlen(PATH_SEP) : strlen(PATH_GAP);
    len += strlen(labels[positions[i]]);
  }

  if (trailing_ellipsis || positions[count - 1] < hop_count - 1) {
    len += strlen(PATH_GAP);
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
                         uint8_t hop_count, bool trailing_ellipsis, uint8_t pos) {
  if (selectionContains(positions, count, pos)) return true;
  if (count >= PATH_MAX_SELECTED) return false;
  if (labels[pos] == nullptr) return false;

  uint8_t trial[PATH_MAX_SELECTED];
  memcpy(trial, positions, count);
  trial[count] = pos;
  uint8_t trial_count = count + 1;
  sortPositions(trial, trial_count);

  bool trail = trailing_ellipsis || trial[trial_count - 1] < hop_count - 1;
  if (measureJoinedPathLen(trial, trial_count, labels, hop_count, trail) > path_budget) {
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
  snprintf(hop_prefix, sizeof(hop_prefix), "%d %s: ", hop_count, hop_count == 1 ? "hop" : "hops");

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

static void joinPathFragments(char* path_str, size_t path_str_len, const uint8_t* positions,
                              uint8_t count, const char** labels, uint8_t hop_count,
                              bool trailing_ellipsis) {
  path_str[0] = '\0';
  if (count == 0) return;

  char* out = path_str;
  size_t remaining = path_str_len;

  if (positions[0] > 0 && remaining > 1) {
    int written = snprintf(out, remaining, "%s", PATH_GAP);
    out += written;
    remaining -= written;
  }

  int written = snprintf(out, remaining, "%s", labels[positions[0]]);
  out += written;
  remaining -= written;

  for (uint8_t i = 1; i < count && remaining > 1; i++) {
    uint8_t gap = positions[i] - positions[i - 1];
    const char* sep = (gap == 1) ? PATH_SEP : PATH_GAP;

    written = snprintf(out, remaining, "%s%s", sep, labels[positions[i]]);
    out += written;
    remaining -= written;
  }

  if ((trailing_ellipsis || positions[count - 1] < hop_count - 1) &&
      remaining > strlen(PATH_GAP)) {
    snprintf(out, remaining, "%s", PATH_GAP);
  }
}

static void buildHopPathWithNames(char* body, size_t body_len, MyMesh& mesh, mesh::Packet* pkt,
                                  size_t path_budget) {
  uint8_t hop_count = pkt->getPathHashCount();
  uint8_t hash_size = pkt->getPathHashSize();
  const char* hop_word = (hop_count == 1) ? "hop" : "hops";

  if (hop_count == 0) {
    snprintf(body, body_len, "direct");
    return;
  }

  if (hop_count > PATH_MAX_SELECTED) {
    snprintf(body, body_len, "%d %s:", hop_count, hop_word);
    return;
  }

  const char* labels[PATH_MAX_SELECTED] = {0};
  for (uint8_t i = 0; i < hop_count && i < PATH_MAX_SELECTED; i++) {
    ContactInfo* contact = mesh.lookupContactByPubKey(&pkt->path[i * hash_size], hash_size);
    if (contact) labels[i] = contact->name;
  }

  int8_t first_known = -1;
  int8_t last_known = -1;
  for (uint8_t i = 0; i < hop_count; i++) {
    if (labels[i] == nullptr) continue;
    if (first_known < 0) first_known = (int8_t)i;
    last_known = (int8_t)i;
  }

  if (first_known < 0) {
    snprintf(body, body_len, "%d %s:", hop_count, hop_word);
    return;
  }

  uint8_t positions[PATH_MAX_SELECTED];
  uint8_t count = 0;
  bool trailing_ellipsis = false;

  positions[count++] = (uint8_t)first_known;
  if (last_known != first_known) {
    uint8_t anchors[2] = {(uint8_t)first_known, (uint8_t)last_known};
    bool trail = (uint8_t)last_known < hop_count - 1;
    if (measureJoinedPathLen(anchors, 2, labels, hop_count, trail) <= path_budget) {
      positions[count++] = (uint8_t)last_known;
      sortPositions(positions, count);
    } else {
      trailing_ellipsis = true;
    }
  }

  if (last_known > first_known && !trailing_ellipsis) {
    uint8_t fwd = (uint8_t)first_known + 1;
    uint8_t bwd = (uint8_t)last_known - 1;
    bool forward_turn = true;
    bool budget_exhausted = false;

    while (fwd <= bwd && !budget_exhausted) {
      bool added = false;

      if (forward_turn) {
        for (uint8_t i = fwd; i <= bwd; i++) {
          if (labels[i] == nullptr) continue;
          if (trySelectHop(positions, count, labels, path_budget, hop_count, trailing_ellipsis, i)) {
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
          if (trySelectHop(positions, count, labels, path_budget, hop_count, trailing_ellipsis,
                           (uint8_t)i)) {
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
  snprintf(body, body_len, "%d %s: %s", hop_count, hop_word, path_str);
}

static bool isBotChannel(const char* channel_name) {
  return strcmp(channel_name, "#bot") == 0 ||
         strcmp(channel_name, "#test") == 0 ||
         strcmp(channel_name, "#ping") == 0 ||
         strcmp(channel_name, "#prove") == 0;
}

static bool sendCasualReply(MyMesh& mesh, mesh::GroupChannel& channel,
                            const char* sender_name, const char* text, mesh::Packet* pkt) {
  char reply[MAX_TEXT_LEN + 1];
  if (sender_name != nullptr && sender_name[0] != '\0') {
    snprintf(reply, sizeof(reply), "@[%s] %s 🤖", sender_name, text);
  } else {
    snprintf(reply, sizeof(reply), "%s 🤖", text);
  }

  uint32_t timestamp = mesh.getRTCClock()->getCurrentTimeUnique();
  bool success = false;

  // Try to match incoming region if configured
  const TransportKey* matched_key = matchIncomingRegion(pkt);

  if (matched_key != nullptr) {
    // Create packet manually to use specific region key (NessoN1 approach)
    uint8_t temp[5 + MAX_TEXT_LEN + 32];
    memcpy(temp, &timestamp, 4);
    temp[4] = 0;  // TXT_TYPE_PLAIN

    // Add sender name prefix (bot name)
    sprintf((char*)&temp[5], "%s: ", mesh.getNodeName());
    int prefix_len = strlen((char*)&temp[5]);

    int reply_len = strlen(reply);
    if (reply_len + prefix_len > MAX_TEXT_LEN) reply_len = MAX_TEXT_LEN - prefix_len;
    memcpy(&temp[5 + prefix_len], reply, reply_len);

    auto reply_pkt = mesh.createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, channel,
                                              temp, 5 + prefix_len + reply_len);
    if (reply_pkt) {
      mesh.sendFloodScoped(*matched_key, reply_pkt, 0);
      Serial.printf("[BOT] Sent casual reply with matched region\n");
      success = true;
    }
  } else {
    // Use device's default configuration
    success = mesh.sendGroupMessage(timestamp, channel, mesh.getNodeName(), reply, strlen(reply));
    Serial.printf("[BOT] Sent casual reply with device default region\n");
  }

  return success;
}

static bool sendBotReply(MyMesh& mesh, const char* channel_name, mesh::GroupChannel& channel,
                         const char* sender_name, const char* body,
                         const char* location, const char* warnings, mesh::Packet* pkt) {
  char reply[MAX_TEXT_LEN + 1];

  // Assemble: @[sender] {body}\n{location} 🤖\n{warnings}
  // Note: warnings includes "\n" prefix if non-empty
  if (location && location[0] != '\0') {
    snprintf(reply, sizeof(reply), "@[%s] %s\n📍 %s 🤖%s", sender_name, body, location, warnings);
  } else {
    snprintf(reply, sizeof(reply), "@[%s] %s 🤖%s", sender_name, body, warnings);
  }

  uint32_t timestamp = mesh.getRTCClock()->getCurrentTimeUnique();
  bool success = false;

  // Try to match incoming region if configured
  const TransportKey* matched_key = matchIncomingRegion(pkt);

  if (matched_key != nullptr) {
    // Create packet manually to use specific region key (NessoN1 approach)
    uint8_t temp[5 + MAX_TEXT_LEN + 32];
    memcpy(temp, &timestamp, 4);
    temp[4] = 0;  // TXT_TYPE_PLAIN

    // Add sender name prefix (bot name)
    sprintf((char*)&temp[5], "%s: ", mesh.getNodeName());
    int prefix_len = strlen((char*)&temp[5]);

    int reply_len = strlen(reply);
    if (reply_len + prefix_len > MAX_TEXT_LEN) reply_len = MAX_TEXT_LEN - prefix_len;
    memcpy(&temp[5 + prefix_len], reply, reply_len);

    auto reply_pkt = mesh.createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, channel,
                                              temp, 5 + prefix_len + reply_len);
    if (reply_pkt) {
      mesh.sendFloodScoped(*matched_key, reply_pkt, 0);
      Serial.printf("[BOT] Sent reply to channel %s with matched region\n", channel_name);
      success = true;
    }
  } else {
    // Use device's default configuration
    success = mesh.sendGroupMessage(timestamp, channel, mesh.getNodeName(), reply, strlen(reply));
    Serial.printf("[BOT] Sent reply to channel %s with device default region\n", channel_name);
  }

  return success;
}

// Try to match incoming packet's region against configured regions
// Returns matched TransportKey pointer, or nullptr if no match
static const TransportKey* matchIncomingRegion(mesh::Packet* pkt) {
  if (match_region_count == 0 || pkt == nullptr || !pkt->hasTransportCodes()) {
    return nullptr;
  }

  uint16_t incoming_code = pkt->transport_codes[0];

  // Try each configured region
  for (uint8_t i = 0; i < match_region_count; i++) {
    // Calculate what transport code this region would produce for this packet
    uint16_t expected_code = match_region_keys[i].calcTransportCode(pkt);

    Serial.printf("[BOT] Checking region '%s': incoming=%04x, expected=%04x\n",
                  match_region_names[i], incoming_code, expected_code);

    if (incoming_code == expected_code) {
      Serial.printf("[BOT] Matched region: '%s'\n", match_region_names[i]);
      return &match_region_keys[i];
    }
  }

  Serial.printf("[BOT] No region match found for code %04x\n", incoming_code);
  return nullptr;
}

bool botHandleChannel(MyMesh& mesh, const char* channel_name, mesh::GroupChannel& channel,
                      mesh::Packet* pkt, const char* text) {
  if (channel_name == nullptr || text == nullptr || pkt == nullptr) {
    Serial.printf("[BOT] Channel handler called: channel=%s, text=%s\n",
                  channel_name ? channel_name : "null",
                  text ? text : "null");
    return false;
  }

  // Check if bot is enabled
  if (!botIsEnabled()) {
    Serial.printf("[BOT] Bot disabled, ignoring channel message\n");
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
    message = skipLeadingWhitespace(message);
  } else {
    sender_name[0] = '\0';  // No sender name found
    message = text;  // No prefix, use whole text
  }

  Serial.printf("[BOT] Sender: %s, Message: %s\n", sender_name, message);

  // Skip if sender is ourselves (message echoed back via repeater)
  if (sender_name[0] != '\0' && strcmp(sender_name, mesh.getNodeName()) == 0) {
    Serial.printf("[BOT] Ignoring message from self\n");
    return false;
  }

  const char* cmd = skipLeadingWhitespace(
      stripLeadingBotMention(skipLeadingWhitespace(message), mesh.getNodeName()));
  if (cmd != message) {
    Serial.printf("[BOT] Command after mention strip: %s\n", cmd);
  }

  const bool in_bot_channel = isBotChannel(channel_name);
  const char* location = botGetLocation();

  // Public/other channels: casual replies only when reply-all is enabled
  if (!in_bot_channel) {
    if (!reply_all_channels) {
      Serial.printf("[BOT] Not a bot channel and reply-all disabled, ignoring\n");
      return false;
    }

    if (matchesTestOrProva(cmd)) {
      // Skip reply if at home repeater
      if (isAtHomeRepeater(pkt)) {
        Serial.printf("[BOT] At home repeater, skipping reply\n");
        return false;
      }

      // Check reply state for this sender
      uint32_t current_time = mesh.getRTCClock()->getCurrentTime();
      uint32_t sender_id = hashSenderName(sender_name);
      ReplyState* state = getReplyState(sender_id, current_time);

      if (state == nullptr) {
        Serial.printf("[BOT] Reply tracking full, skipping\n");
        return false;
      }

      // State machine: 0 = first reply, 1 = second reply (if strict command), 2+ = no reply
      if (state->reply_count == 0) {
        // First reply: always respond
        uint8_t hop_count = pkt->getPathHashCount();
        char body[128];
        if (hop_count == 0) {
          snprintf(body, sizeof(body), "direct, %s", location);
        } else {
          snprintf(body, sizeof(body), "%d %s, %s", hop_count,
                   hop_count == 1 ? "salto" : "salti", location);
        }
        sendCasualReply(mesh, channel, sender_name, body, pkt);
        state->reply_count++;
        state->last_reply_time = current_time;
        Serial.printf("[BOT] First reply to %s\n", sender_name);
        return true;

      } else if (state->reply_count == 1 && isStrictTestCommand(cmd)) {
        // Second reply: only if strict command, include nudge
        uint8_t hop_count = pkt->getPathHashCount();
        char body[192];
        if (hop_count == 0) {
          snprintf(body, sizeof(body), "direct, %s (scrivi nel canale #bot o #test)", location);
        } else {
          snprintf(body, sizeof(body), "%d %s, %s (scrivi nel canale #bot o #test)",
                   hop_count, hop_count == 1 ? "salto" : "salti", location);
        }
        sendCasualReply(mesh, channel, sender_name, body, pkt);
        state->reply_count++;
        state->last_reply_time = current_time;
        Serial.printf("[BOT] Second reply to %s with nudge\n", sender_name);
        return true;

      } else {
        // Third+ message, or second message but not strict command: no reply
        Serial.printf("[BOT] Skipping reply to %s (count=%d, strict=%d)\n",
                     sender_name, state->reply_count, isStrictTestCommand(cmd) ? 1 : 0);
        return false;
      }
    }

    return false;
  }

  // Bot channels - skip all replies if at home repeater
  if (isAtHomeRepeater(pkt)) {
    Serial.printf("[BOT] At home repeater, skipping reply\n");
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

    sendBotReply(mesh, channel_name, channel, sender_name, body, nullptr, "", pkt);
    return true;
  }

  // Handle !test/test/!prova/prova command (case-insensitive)
  if (matchesTestOrProva(cmd)) {

    uint8_t hash_size = pkt->getPathHashSize();
    const char* warnings = getBotWarnings(hash_size, pkt->hasTransportCodes());

    char body[128];
    const char* test_location = location;
    if (strcmp(channel_name, "#bot") == 0) {
      buildHopPath(body, sizeof(body), pkt);
      test_location = nullptr;  // Don't show location with hash paths
    } else if (hash_size == 1) {
      // 1-byte hashes: unreliable resolution, just show hop count
      uint8_t hop_count = pkt->getPathHashCount();
      if (hop_count == 0) {
        snprintf(body, sizeof(body), "direct");
      } else {
        snprintf(body, sizeof(body), "%d %s", hop_count, hop_count == 1 ? "hop" : "hops");
      }
    } else {
      buildHopSummary(body, sizeof(body), mesh, pkt);
    }

    sendBotReply(mesh, channel_name, channel, sender_name, body, test_location, warnings, pkt);
    return true;
  }

  // Handle !path or path command
  if ((strncasecmp(cmd, "!path", 5) == 0 && (cmd[5] == '\0' || cmd[5] == ' ')) ||
      (strncasecmp(cmd, "path", 4) == 0 && (cmd[4] == '\0' || cmd[4] == ' '))) {

    uint8_t hop_count = pkt->getPathHashCount();
    uint8_t hash_size = pkt->getPathHashSize();
    const char* warnings = getBotWarnings(hash_size, pkt->hasTransportCodes());
    // Don't show location with any path display (hash or named)
    const char* path_location = nullptr;

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

    sendBotReply(mesh, channel_name, channel, sender_name, body, path_location, warnings, pkt);

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
    sendBotReply(mesh, channel_name, channel, sender_name, body, nullptr, "", pkt);
    return true;
  }

  return false;  // Not a bot command
}
