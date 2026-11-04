#pragma once

#include <string>

namespace creator_tools {

/// Sends a slash command (e.g. "/gamemode creative") to the server the client is connected to,
/// as the primary local player. Works on both local and remote worlds.
/// Must be called on the client thread. Returns false (and logs why) if it could not be sent.
bool sendCommand(std::string const& command);

/// Logs the message and also shows it in chat so creators don't have to watch the log.
/// Must be called on the client thread.
void notify(std::string const& message);

} // namespace creator_tools