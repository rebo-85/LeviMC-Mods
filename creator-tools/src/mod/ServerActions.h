#pragma once

#include "mc/world/level/GameType.h"

namespace creator_tools::server {

/// True when the world is hosted by this client's own integrated server, i.e. we can call the
/// server directly instead of talking to it over the network.
[[nodiscard]] bool hasLocalServer();

/// Sets the local player's game mode by calling the integrated server's Player API directly
/// (the same call /gamemode makes). No command packet, no cheats/operator needed.
/// Returns false if there is no local server (remote world) so the caller can fall back.
/// Client thread only.
bool setLocalGameType(GameType mode);

/// Runs `/reload` (all == false) or `/reload all` (all == true) on the integrated server in-process,
/// through the server's own command dispatcher with an internal-permission origin.
/// Returns false if there is no local server. Client thread only.
bool reloadLocal(bool all);

} // namespace creator_tools::server
