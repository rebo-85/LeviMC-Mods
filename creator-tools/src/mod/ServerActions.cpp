#include "mod/ServerActions.h"

#include "mod/ClientCommands.h"
#include "mod/CreatorTools.h"

#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ClientThreadExecutor.h"
#include "ll/api/thread/ServerThreadExecutor.h"

#include "mc/deps/core/utility/MCRESULT.h"
#include "mc/server/ServerInstance.h"
#include "mc/server/ServerLevel.h"
#include "mc/server/commands/CommandContext.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/server/commands/CurrentCmdVersion.h"
#include "mc/server/commands/MinecraftCommands.h"
#include "mc/server/commands/PlayerCommandOrigin.h"
#include "mc/server/commands/ServerCommandOrigin.h"
#include "mc/world/Minecraft.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace creator_tools::server {

namespace {

void notifyOnClient(std::string message) {
    ll::thread::ClientThreadExecutor::getDefault().execute([message = std::move(message)] { notify(message); });
}

} // namespace

bool hasLocalServer() { return ll::service::getServerInstance() != nullptr; }

bool setLocalGameType(GameType mode) {
    if (!hasLocalServer()) {
        return false;
    }
    auto  clientLevel  = ll::service::getMultiPlayerLevel();
    auto* clientPlayer = clientLevel ? clientLevel->getPrimaryLocalPlayer() : nullptr;
    if (!clientPlayer) {
        return false;
    }

    // The client-side player and the server-side player are different objects; match them by UUID.
    auto const uuid  = clientPlayer->getUuid();
    auto       alive = CreatorTools::getInstance().alive();

    ll::thread::ServerThreadExecutor::getDefault().execute([alive, uuid, mode] {
        if (!alive->load()) {
            return;
        }
        auto  level  = ll::service::getLevel();
        auto* player = level ? level->getPlayer(uuid) : nullptr;
        if (!player) {
            notifyOnClient("Cannot change game mode: the server has no matching player");
            return;
        }
        player->setPlayerGameType(mode);
    });
    return true;
}

bool reloadLocal(bool all) {
    if (!hasLocalServer()) {
        return false;
    }
    auto alive = CreatorTools::getInstance().alive();

    // `/reload all` only makes the client re-join the world (which is what actually refreshes pack
    // data) when it comes from a player, so run it as the local player. Matched by UUID on the server.
    std::optional<mce::UUID> playerUuid;
    if (all) {
        auto clientLevel = ll::service::getMultiPlayerLevel();
        if (auto* clientPlayer = clientLevel ? clientLevel->getPrimaryLocalPlayer() : nullptr) {
            playerUuid = clientPlayer->getUuid();
        }
    }

    ll::thread::ServerThreadExecutor::getDefault().execute([alive, all, playerUuid] {
        if (!alive->load()) {
            return;
        }
        auto server    = ll::service::getServerInstance();
        auto level     = ll::service::getLevel();
        auto minecraft = ll::service::getMinecraft();
        if (!server || !level || !minecraft) {
            notifyOnClient("The local server stopped");
            return;
        }

        // The server thread's level is always a ServerLevel, which the origin requires.
        auto&                          serverLevel = static_cast<ServerLevel&>(*level);
        std::unique_ptr<CommandOrigin> origin;
        if (playerUuid) {
            if (auto* player = level->getPlayer(*playerUuid)) {
                origin = std::make_unique<PlayerCommandOrigin>(*level, player->getOrCreateUniqueID());
            }
        }
        if (!origin) {
            origin = std::make_unique<ServerCommandOrigin>("Server", serverLevel, CommandPermissionLevel::Internal, 0);
        }
        CommandContext context{
            all ? "/reload all" : "/reload",
            std::move(origin),
            static_cast<int>(CurrentCmdVersion::Latest)
        };
        auto* commands = minecraft->mCommands.get();
        if (!commands) {
            notifyOnClient("The local server has no command dispatcher");
            return;
        }
        commands->executeCommand(context, false);
        if (!all) {
            notifyOnClient("Reloaded scripts and functions (/reload)");
        }
    });
    return true;
}

} // namespace creator_tools::server
