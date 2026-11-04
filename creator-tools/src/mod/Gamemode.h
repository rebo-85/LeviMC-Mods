#pragma once

#include "mc/world/level/GameType.h"

#include <string_view>

namespace creator_tools::gamemode {

/// Next mode in the Survival -> Creative -> Adventure -> Spectator cycle.
/// Unknown modes restart the cycle at Survival.
[[nodiscard]] GameType next(GameType current);

[[nodiscard]] std::string_view name(GameType mode);

/// Requests the next gamemode in the cycle by sending `/gamemode <mode>` to the server, so it works on
/// local and remote worlds but needs permission to use the command (cheats / operator).
/// Returns false if no player/level/connection is available. Must be called on the client thread.
bool cycleLocalPlayer();

} // namespace creator_tools::gamemode