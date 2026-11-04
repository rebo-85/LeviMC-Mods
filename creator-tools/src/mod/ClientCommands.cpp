#include "mod/ClientCommands.h"

#include "mod/CreatorTools.h"

#include "ll/api/service/Bedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/common/SubClientId.h"
#include "mc/network/ClientNetworkSystem.h"
#include "mc/network/packet/CommandRequestPacket.h"
#include "mc/server/commands/CommandContext.h"
#include "mc/server/commands/CurrentCmdVersion.h"
#include "mc/server/commands/PlayerCommandOrigin.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include <memory>
#include <utility>

namespace creator_tools {

void notify(std::string const& message) {
    CreatorTools::getInstance().getSelf().getLogger().info("{}", message);
    auto level = ll::service::getMultiPlayerLevel();
    if (!level) {
        return;
    }
    if (auto* player = level->getPrimaryLocalPlayer()) {
        player->sendMessage(message);
    }
}

bool sendCommand(std::string const& command) {
    auto& log = CreatorTools::getInstance().getSelf().getLogger();

    auto client = ll::service::getClientInstance();
    auto level  = ll::service::getMultiPlayerLevel();
    if (!client || !level) {
        log.warn("Cannot send {}: no active client level", command);
        return false;
    }

    auto* player = level->getPrimaryLocalPlayer();
    if (!player) {
        log.warn("Cannot send {}: no local player", command);
        return false;
    }

    auto& network   = client->getClientNetworkSystem();
    auto  networkId = network.tryGetLocalNetworkId();
    if (!networkId) {
        log.warn("Cannot send {}: no active network connection", command);
        return false;
    }

    CommandContext context{
        command,
        std::make_unique<PlayerCommandOrigin>(*level, player->getOrCreateUniqueID()),
        static_cast<int>(CurrentCmdVersion::Latest)
    };
    CommandRequestPacketPayload payload{context, false};
    CommandRequestPacket        packet{std::move(payload)};
    network.send(*networkId, packet, SubClientId{});
    log.info("Sent {} to the server", command);
    return true;
}

} // namespace creator_tools