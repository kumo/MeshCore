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

  char buf[32] = {0};
  size_t len = file.readBytes(buf, sizeof(buf) - 1);
  file.close();

  // Parse "enabled=0" or "enabled=1"
  if (strncmp(buf, "enabled=", 8) == 0) {
    bot_enabled = (buf[8] == '1');
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
  file.close();
  return true;
}

bool botIsEnabled() {
  return bot_enabled;
}

bool botHandleConfig(const char* text, char* reply, size_t reply_len) {
  if (text == nullptr || reply == nullptr || reply_len == 0) {
    return false;
  }

  // Check for !bot commands
  if (strcmp(text, "!bot") == 0 || strcmp(text, "!bot status") == 0) {
    snprintf(reply, reply_len, "bot: %s", bot_enabled ? "enabled" : "disabled");
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

bool botHandleChannel(MyMesh& mesh, const char* channel_name, mesh::GroupChannel& channel, const char* text) {
  if (channel_name == nullptr || text == nullptr) {
    Serial.printf("[BOT] Channel handler called: channel=%s, text=%s\n",
                  channel_name ? channel_name : "null",
                  text ? text : "null");
    return false;
  }

  Serial.printf("[BOT] Channel: %s, Text: %s\n", channel_name, text);

  // Only respond in allowed channels
  if (strcmp(channel_name, "#bot") != 0 &&
      strcmp(channel_name, "#test") != 0 &&
      strcmp(channel_name, "#prove") != 0) {
    Serial.printf("[BOT] Not an allowed channel, ignoring\n");
    return false;  // Not an allowed channel
  }

  // Skip sender prefix "Name: " in channel messages
  const char* message = strchr(text, ':');
  if (message != nullptr) {
    message++;  // Skip the ':'
    while (*message == ' ') message++;  // Skip spaces after ':'
  } else {
    message = text;  // No prefix, use whole text
  }

  Serial.printf("[BOT] Parsed message: %s\n", message);

  // Handle !echo command
  if (message[0] == '!' && strncmp(message, "!echo ", 6) == 0) {
    const char* echo_text = message + 6;  // Skip "!echo "

    // Create reply message: "Echo: <text>"
    char reply[MAX_TEXT_LEN + 1];
    snprintf(reply, sizeof(reply), "[BOT] Echo: %s", echo_text);

    uint32_t timestamp = mesh.getRTCClock()->getCurrentTime();
    if (mesh.sendGroupMessage(timestamp, channel, mesh.getNodeName(), reply, strlen(reply))) {
      Serial.printf("[BOT] Sent echo reply to channel %s\n", channel_name);
    }
    return true;
  }

  return false;  // Not a bot command
}
