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

  // Handle !echo command
  if (text[0] == '!' && strncmp(text, "!echo ", 6) == 0) {
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

bool botHandleChannel(MyMesh& mesh, const char* channel_name, mesh::GroupChannel& channel,
                      mesh::Packet* pkt, const char* text) {
  if (channel_name == nullptr || text == nullptr || pkt == nullptr) {
    Serial.printf("[BOT] Channel handler called: channel=%s, text=%s\n",
                  channel_name ? channel_name : "null",
                  text ? text : "null");
    return false;
  }

  Serial.printf("[BOT] Channel: %s, Text: %s\n", channel_name, text);

  // Only respond in allowed channels
  if (strcmp(channel_name, "#bot") != 0 &&
      strcmp(channel_name, "#test") != 0 &&
      strcmp(channel_name, "#ping") != 0 &&
      strcmp(channel_name, "#prove") != 0) {
    Serial.printf("[BOT] Not an allowed channel, ignoring\n");
    return false;  // Not an allowed channel
  }

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

  // Handle !ping command (case-insensitive)
  if ((message[0] == '!' && (strncasecmp(message, "!ping", 5) == 0 && (message[5] == '\0' || message[5] == ' '))) ||
      (strcasecmp(message, "ping") == 0)) {
    char reply[MAX_TEXT_LEN + 1];
    snprintf(reply, sizeof(reply), "@[%s] pong 🤖", sender_name);

    uint32_t timestamp = mesh.getRTCClock()->getCurrentTime();
    if (mesh.sendGroupMessage(timestamp, channel, mesh.getNodeName(), reply, strlen(reply))) {
      Serial.printf("[BOT] Sent ping reply to channel %s\n", channel_name);
    }
    return true;
  }

  // Handle !test or test command (case-insensitive)
  if ((message[0] == '!' && (strncasecmp(message, "!test", 5) == 0 && (message[5] == '\0' || message[5] == ' '))) ||
      (strcasecmp(message, "test") == 0)) {
    char reply[MAX_TEXT_LEN + 1];

    // Get hop count from packet
    uint8_t hop_count = pkt->getPathHashCount();
    uint8_t hash_size = pkt->getPathHashSize();
    const char* location = botGetLocation();

    // Format based on channel
    if (strcmp(channel_name, "#bot") == 0) {
      // Detailed format for #bot channel with path
      if (hop_count == 0) {
        if (location[0] != '\0') {
          snprintf(reply, sizeof(reply), "@[%s] direct a %s 🤖", sender_name, location);
        } else {
          snprintf(reply, sizeof(reply), "@[%s] direct 🤖", sender_name);
        }
      } else {
        // Build path string with hex hashes
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

        if (location[0] != '\0') {
          snprintf(reply, sizeof(reply), "@[%s] %d %s %s a %s 🤖",
                   sender_name, hop_count, hop_count == 1 ? "hop" : "hops", path_str, location);
        } else {
          snprintf(reply, sizeof(reply), "@[%s] %d %s %s 🤖",
                   sender_name, hop_count, hop_count == 1 ? "hop" : "hops", path_str);
        }
      }
    } else {
      // Simple format for other channels
      if (hop_count == 0) {
        if (location[0] != '\0') {
          snprintf(reply, sizeof(reply), "@[%s] direct a %s 🤖", sender_name, location);
        } else {
          snprintf(reply, sizeof(reply), "@[%s] direct 🤖", sender_name);
        }
      } else {
        if (location[0] != '\0') {
          snprintf(reply, sizeof(reply), "@[%s] %d %s a %s 🤖",
                   sender_name, hop_count, hop_count == 1 ? "hop" : "hops", location);
        } else {
          snprintf(reply, sizeof(reply), "@[%s] %d %s 🤖",
                   sender_name, hop_count, hop_count == 1 ? "hop" : "hops");
        }
      }
    }

    uint32_t timestamp = mesh.getRTCClock()->getCurrentTime();
    if (mesh.sendGroupMessage(timestamp, channel, mesh.getNodeName(), reply, strlen(reply))) {
      Serial.printf("[BOT] Sent test reply to channel %s\n", channel_name);
    }
    return true;
  }

  // Handle !echo command
  if (message[0] == '!' && strncmp(message, "!echo ", 6) == 0) {
    const char* echo_text = message + 6;  // Skip "!echo "

    // Create reply message: "@[Sender] Echo: <text> 🤖"
    char reply[MAX_TEXT_LEN + 1];
    snprintf(reply, sizeof(reply), "@[%s] Echo: %s 🤖", sender_name, echo_text);

    uint32_t timestamp = mesh.getRTCClock()->getCurrentTime();
    if (mesh.sendGroupMessage(timestamp, channel, mesh.getNodeName(), reply, strlen(reply))) {
      Serial.printf("[BOT] Sent echo reply to channel %s\n", channel_name);
    }
    return true;
  }

  return false;  // Not a bot command
}
