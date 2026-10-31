#include "mod/Gamemode.h"

#include "mod/DevClient.h"

#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include <algorithm>
#include <array>
#include <string>

namespace dev_client::gamemode {

namespace {

constexpr std::array kCycle{
    GameType::Survival,
    GameType::Creative,
    GameType::Adventure,
    GameType::Spectator,
};

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

std::string_view gameModeTranslationKey(GameType mode) {
    switch (mode) {
    case GameType::Survival:
        return "gameMode.survival";
    case GameType::Creative:
        return "gameMode.creative";
    case GameType::Adventure:
        return "gameMode.adventure";
    case GameType::Spectator:
        return "gameMode.spectator";
    default:
        return "";
    }
}

void sendGameModeMessage(Player& player, GameType mode) {
    std::string_view modeName = name(mode);
    player.sendMessage("Set own game mode to " + std::string{modeName});
}

bool cycleLocalPlayer() {
    auto& devClient = DevClient::getInstance();
    auto& log       = devClient.getSelf().getLogger();

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

    player->setPlayerGameType(target);
    sendGameModeMessage(*player, target);

    devClient.debug(
        "gamemode " + std::to_string(static_cast<int>(current)) + " -> " + std::to_string(static_cast<int>(target))
    );
    return true;
}

} // namespace dev_client::gamemode