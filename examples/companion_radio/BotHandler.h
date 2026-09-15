// ABOUTME: Bot command handler for companion radio devices
// ABOUTME: Handles bot configuration and automated message responses

#pragma once

#include <stdint.h>
#include <stddef.h>

// Forward declarations
class ContactInfo;
namespace mesh {
  class Packet;
  class GroupChannel;
}
class MyMesh;

// Initialize bot handler with data store
void botInit();

// Check if bot is currently enabled
bool botIsEnabled();

// Get bot location (returns empty string if not set)
const char* botGetLocation();

// Check if reply-all is enabled for non-bot channels
bool botGetReplyAll();

// Check if warnings are enabled
bool botGetWarningsEnabled();

// Handle !bot configuration commands (on/off/status)
// Returns true if command was handled, false if not a bot config command
bool botHandleConfig(const char* text, char* reply, size_t reply_len);

// Handle bot commands in direct messages
// Returns true if command was handled and reply sent
bool botHandleDM(MyMesh& mesh, const ContactInfo& from, mesh::Packet* pkt, const char* text);

// Handle bot commands in channels
// Returns true if command was handled and reply sent
// Bot channels (#bot, #test, #ping, #prove): full bot replies.
// Other channels: casual test/prova replies when location is set.
bool botHandleChannel(MyMesh& mesh, const char* channel_name, mesh::GroupChannel& channel,
                      mesh::Packet* pkt, const char* text);
