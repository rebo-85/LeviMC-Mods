#pragma once

#include "mc/world/level/GameType.h"

#include <string_view>

namespace creator_tools::gamemode {

/// Next mode in the Survival -> Creative -> Adventure -> Spectator cycle.
/// Unknown modes restart the cycle at Survival.
[[nodiscard]] GameType next(GameType current);

[[nodiscard]] std::string_view name(GameType mode);

/// Cycles the local player's gamemode. Returns false if no player/level is available.
bool cycleLocalPlayer();

} // namespace creator_tools::gamemode