#include "mod/Gamemode.h"

#include "mod/ClientCommands.h"
#include "mod/CreatorTools.h"
#include "mod/ServerActions.h"

#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <string>

namespace creator_tools::gamemode {

namespace {

constexpr std::array kCycle{
    GameType::Survival,
    GameType::Creative,
    GameType::Adventure,
    GameType::Spectator,
};

// How long a requested mode is trusted before we fall back to the player's actual mode. Covers the
// round trip to the server so rapid presses keep advancing the cycle instead of repeating one target.
constexpr auto kPendingTimeout = std::chrono::seconds{2};

struct PendingMode {
    GameType                              target;
    std::chrono::steady_clock::time_point expires;
};

// Client thread only.
std::optional<PendingMode> gPending;

// Argument accepted by the /gamemode command, or empty for modes outside the cycle.
std::string_view commandArg(GameType mode) {
    switch (mode) {
    case GameType::Survival:
        return "survival";
    case GameType::Creative:
        return "creative";
    case GameType::Adventure:
        return "adventure";
    case GameType::Spectator:
        return "spectator";
    default:
        return {};
    }
}

} // namespace

GameType next(GameType current) {
    auto it = std::ranges::find(kCycle, current);
    if (it == kCycle.end()) {
        return kCycle.front();
    }
    return *(++it == kCycle.end() ? kCycle.begin() : it);
}

std::string_view name(GameType mode) {
    switch (mode) {
    case GameType::Survival:
        return "Survival";
    case GameType::Creative:
        return "Creative";
    case GameType::Adventure:
        return "Adventure";
    case GameType::Spectator:
        return "Spectator";
    default:
        return "Unknown";
    }
}

bool cycleLocalPlayer() {
    auto& creatorTools = CreatorTools::getInstance();
    auto& log          = creatorTools.getSelf().getLogger();

    auto level = ll::service::getMultiPlayerLevel();
    if (!level) {
        log.warn("No multiplayer level");
        return false;
    }

    Player* player = level->getPrimaryLocalPlayer();
    if (!player) {
        log.warn("No local player");
        return false;
    }

    auto const now     = std::chrono::steady_clock::now();
    auto const current = player->getPlayerGameType();

    // If we already asked for a mode and the server hasn't applied it yet, continue from that mode.
    auto base = current;
    if (gPending && now < gPending->expires && current != gPending->target) {
        base = gPending->target;
    }

    auto const target = next(base);
    auto const arg    = commandArg(target);
    if (arg.empty()) {
        log.warn("No /gamemode argument for target mode {}", name(target));
        return false;
    }

    // Local world: call the integrated server's Player API directly (no command packet, no cheats
    // needed). Remote world: there is no API to reach the server, so a /gamemode command is the only way.
    bool const ok =
        server::hasLocalServer() ? server::setLocalGameType(target) : sendCommand("/gamemode " + std::string{arg});
    if (!ok) {
        return false;
    }

    gPending = PendingMode{target, now + kPendingTimeout};
    notify("Game mode: " + std::string{name(target)});
    log.info("Requested gamemode {} -> {}", name(base), name(target));
    return true;
}

} // namespace creator_tools::gamemode
