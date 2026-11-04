#include "mod/CreatorTools.h"

#include "mod/Gamemode.h"
#include "mod/GameplayState.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ClientThreadExecutor.h"
#include "ll/api/thread/ServerThreadExecutor.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/game/IMinecraftGame.h"
#include "mc/client/gui/FontRepository.h"
#include "mc/client/gui/interface/IUIRepository.h"
#include "mc/common/SubClientId.h"
#include "mc/network/ClientNetworkSystem.h"
#include "mc/network/packet/CommandRequestPacket.h"
#include "mc/resources/IResourcePackRepository.h"
#include "mc/client/renderer/TextureGroup.h"
#include "mc/server/commands/CommandContext.h"
#include "mc/server/commands/CurrentCmdVersion.h"
#include "mc/server/commands/PlayerCommandOrigin.h"
#include "mc/server/ServerInstance.h"
#include "mc/world/level/Level.h"

#include <chrono>
#include <memory>
#include <string_view>
#include <utility>

#include <Windows.h>

namespace creator_tools {

namespace {
constexpr int kKeyG = 0x47;
constexpr int kKeyR = 0x52;

void sendReloadCommand() {
    auto& creatorTools = CreatorTools::getInstance();
    auto& log          = creatorTools.getSelf().getLogger();

    auto client = ll::service::getClientInstance();
    auto level  = ll::service::getMultiPlayerLevel();
    if (!client || !level) {
        log.warn("Cannot send /reload: no active client level");
        return;
    }

    auto* player = level->getPrimaryLocalPlayer();
    if (!player) {
        log.warn("Cannot send /reload: no local player");
        return;
    }

    auto& network = client->getClientNetworkSystem();
    auto networkId = network.tryGetLocalNetworkId();
    if (!networkId) {
        log.warn("Cannot send /reload: no active network connection");
        return;
    }

    CommandContext context{
        "/reload",
        std::make_unique<PlayerCommandOrigin>(*level, player->getOrCreateUniqueID()),
        static_cast<int>(CurrentCmdVersion::Latest)
    };
    CommandRequestPacketPayload payload{context, false};
    CommandRequestPacket packet{std::move(payload)};
    network.send(*networkId, packet, SubClientId{});
    log.info("Sent /reload command to the server");
}

void reloadBehaviorResources() {
    auto server = ll::service::getServerInstance();
    if (!server) {
        sendReloadCommand();
        return;
    }

    ll::thread::ServerThreadExecutor::getDefault().execute([] {
        auto server = ll::service::getServerInstance();
        if (!server) {
            CreatorTools::getInstance().getSelf().getLogger().warn(
                "Cannot reload behavior resources: no active server"
            );
            return;
        }

        server->onRequestResourceReload();
        CreatorTools::getInstance().getSelf().getLogger().info("Requested behavior resource reload");
    });
}

void reloadClientAssets() {
    auto& creatorTools = CreatorTools::getInstance();
    auto  client       = ll::service::getClientInstance();
    if (!client) {
        creatorTools.getSelf().getLogger().warn("Cannot reload client assets: no active client");
        return;
    }

    auto refresh = client->getResourcePackRepository().refreshPacksAsync();
    if (!refresh.mResult) {
        creatorTools.getSelf().getLogger().warn("Cannot reload client assets: pack refresh did not start");
        return;
    }

    refresh.mResult->addOnComplete([](Bedrock::Threading::IAsyncResult<void> const& result) {
        auto& logger = CreatorTools::getInstance().getSelf().getLogger();
        auto const error = result.getError();
        if (error) {
            logger.warn("Cannot refresh client resource packs: {}", error.message());
            return;
        }

        ll::thread::ClientThreadExecutor::getDefault().execute([] {
            auto client = ll::service::getClientInstance();
            if (!client) {
                CreatorTools::getInstance().getSelf().getLogger().warn(
                    "Cannot reload client assets: no active client after pack refresh"
                );
                return;
            }

            client->onRequestResourceReload();

            auto& game = client->getMinecraftGame();
            game.reloadAnimationAndGeometryData(true);
            game.handleReloadUIDefinitions();
            client->getUIRepository()->reloadGroup();

            auto textureGroup = client->getTextureGroup();
            if (!textureGroup) {
                CreatorTools::getInstance().getSelf().getLogger().warn(
                    "Cannot reload client textures: no texture group"
                );
                return;
            }
            textureGroup->reloadAllTextures();
            const_cast<FontRepository&>(*game.getFontRepository()).reloadFontTextures(true);
            CreatorTools::getInstance().getSelf().getLogger().info(
                "Reloaded client animations, geometries, textures, fonts, and UI resources"
            );
        });
    });
    creatorTools.getSelf().getLogger().info("Refreshing client resource packs");
}
} // namespace

CreatorTools& CreatorTools::getInstance() {
    static CreatorTools instance;
    return instance;
}

bool CreatorTools::load() {
    getSelf().getLogger().info("Loading");
    getSelf().getLogger().info("Loaded");
    return true;
}

bool CreatorTools::enable() {
    mEntityDebugOverlay.enable();
    mListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::KeyInputEvent>(
        [this](ll::event::KeyInputEvent& event) {
            if (event.keyCode() == kKeyR) {
                if (!isMinecraftForeground()) {
                    mRPressed = false;
                    return;
                }

                if (!event.isDown()) {
                    if (mRPressed) {
                        event.cancel();
                    }
                    mRPressed = false;
                    return;
                }

                if (!isGameplayActive()) {
                    return;
                }

                bool const controlPressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                bool const shiftPressed   = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                if (!controlPressed && !shiftPressed) {
                    return;
                }

                if (mRPressed) {
                    event.cancel();
                    return;
                }

                mRPressed = true;
                event.cancel();
                ll::thread::ClientThreadExecutor::getDefault().execute(
                    controlPressed ? reloadBehaviorResources : reloadClientAssets
                );
                return;
            }

            if (event.keyCode() != kKeyG) {
                return;
            }

            if (!isMinecraftForeground()) {
                mGPressed = false;
                event.cancel();
                return;
            }

            if (!isGameplayActive()) {
                mGPressed = false;
                return;
            }

            if (event.isDown()) {
                if (mGPressed) {
                    return;
                }
                mGPressed      = true;
                auto const now = std::chrono::steady_clock::now();
                if (now - mLastGamemodeSwitch < std::chrono::milliseconds{250}) {
                    return;
                }
                mLastGamemodeSwitch = now;
                ll::thread::ClientThreadExecutor::getDefault().execute([] { gamemode::cycleLocalPlayer(); });
            } else {
                mGPressed = false;
            }
        }
    );

    getSelf().getLogger().info("Enabled");
    return true;
}

bool CreatorTools::disable() {
    mListener.reset();
    mEntityDebugOverlay.disable();
    mGPressed = false;
    mRPressed = false;
    getSelf().getLogger().info("Disabled");
    return true;
}

} // namespace creator_tools

LL_REGISTER_MOD(creator_tools::CreatorTools, creator_tools::CreatorTools::getInstance());