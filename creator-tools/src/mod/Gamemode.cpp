#include "mod/Gamemode.h"

#include "mod/ClientCommands.h"
#include "mod/CreatorTools.h"

#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include <algorithm>
#include <array>
#include <string>

namespace creator_tools::gamemode {

namespace {

constexpr std::array kCycle{
    GameType::Survival,
    GameType::Creative,
    GameType::Adventure,
    GameType::Spectator,
};

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

    auto const current = player->getPlayerGameType();
    auto const target  = next(current);
    auto const arg     = commandArg(target);
    if (arg.empty()) {
        log.warn("No /gamemode argument for target mode {}", name(target));
        return false;
    }

    // Go through the server instead of calling setPlayerGameType on the client-side player, which
    // only changes client state and can desync from the server. The server replies with its own
    // confirmation (or a permission error) in chat, so no extra message is shown here.
    if (!sendCommand("/gamemode " + std::string{arg})) {
        return false;
    }

    log.info("Requested gamemode {} -> {}", name(current), name(target));
    return true;
}

} // namespace creator_tools::gamemode