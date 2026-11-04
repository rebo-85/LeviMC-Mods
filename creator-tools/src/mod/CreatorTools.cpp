#include "mod/CreatorTools.h"

#include "mod/ClientCommands.h"
#include "mod/Gamemode.h"
#include "mod/GameplayState.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ClientThreadExecutor.h"
#include "ll/api/thread/ServerThreadExecutor.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/game/IMinecraftGame.h"
#include "mc/client/gui/FontRepository.h"
#include "mc/client/gui/interface/IUIRepository.h"
#include "mc/client/renderer/TextureGroup.h"
#include "mc/client/world/LocalWorldStarter.h"
#include "mc/client/world/WorldID.h"
#include "mc/client/world/WorldSystem.h"
#include "mc/resources/IResourcePackRepository.h"
#include "mc/server/ServerInstance.h"
#include "mc/world/level/Level.h"

#include <atomic>
#include <chrono>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <Windows.h>

namespace creator_tools {

namespace {
constexpr int kKeyG = 'G';
constexpr int kKeyR = 'R';

constexpr auto kGamemodeDebounce = std::chrono::milliseconds{250};
constexpr auto kReentryTimeout   = std::chrono::seconds{30};

std::atomic_bool gClientReloadInProgress{false};

bool isKeyDown(int virtualKey) { return (GetAsyncKeyState(virtualKey) & 0x8000) != 0; }

void sendReloadCommand() {
    if (sendCommand("/reload")) {
        notify("Requested behavior-resource reload (/reload)");
    }
}

void reloadClientAssets() {
    auto& creatorTools = CreatorTools::getInstance();
    auto  client       = ll::service::getClientInstance();
    if (!client) {
        creatorTools.getSelf().getLogger().warn("Cannot reload client assets: no active client");
        return;
    }

    if (gClientReloadInProgress.exchange(true, std::memory_order_acq_rel)) {
        creatorTools.getSelf().getLogger().info("Client asset reload already in progress");
        return;
    }

    auto refresh = client->getResourcePackRepository().refreshPacksAsync();
    if (!refresh.mResult) {
        gClientReloadInProgress.store(false, std::memory_order_release);
        creatorTools.getSelf().getLogger().warn("Cannot reload client assets: pack refresh did not start");
        return;
    }

    auto const startedAt = std::chrono::steady_clock::now();
    refresh.mResult->addOnComplete([startedAt](Bedrock::Threading::IAsyncResult<void> const& result) {
        auto const error = result.getError();
        if (error) {
            gClientReloadInProgress.store(false, std::memory_order_release);
            CreatorTools::getInstance().getSelf().getLogger().warn(
                "Cannot refresh client resource packs: {}",
                error.message()
            );
            return;
        }

        ll::thread::ClientThreadExecutor::getDefault().execute([startedAt] {
            // Clears the in-progress flag on every exit path below.
            struct Guard {
                ~Guard() { gClientReloadInProgress.store(false, std::memory_order_release); }
            } guard;

            auto& logger = CreatorTools::getInstance().getSelf().getLogger();
            auto  client = ll::service::getClientInstance();
            if (!client) {
                logger.warn("Cannot reload client assets: no active client after pack refresh");
                return;
            }

            client->onRequestResourceReload();

            auto& game = client->getMinecraftGame();
            game.reloadAnimationAndGeometryData(true);
            game.handleReloadUIDefinitions();
            client->getUIRepository()->reloadGroup();

            auto textureGroup = client->getTextureGroup();
            if (!textureGroup) {
                logger.warn("Cannot reload client textures: no texture group");
                return;
            }
            textureGroup->reloadAllTextures();
            // reloadFontTextures is non-const but getFontRepository() returns a const pointer.
            const_cast<FontRepository&>(*game.getFontRepository()).reloadFontTextures(true);

            auto const elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);
            notify(
                fmt::format(
                    "Reloaded client animations, geometries, textures, fonts, and UI ({:.1f}s)",
                    static_cast<double>(elapsed.count()) / 1000.0
                )
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

void CreatorTools::reloadBehaviorResourcesAndReenterWorld() {
    auto client = ll::service::getClientInstance();
    auto level  = ll::service::getMultiPlayerLevel();
    auto server = ll::service::getServerInstance();
    if (!client || !level) {
        getSelf().getLogger().warn("Cannot reload and re-enter world: no active client level");
        return;
    }

    if (!server) {
        getSelf().getLogger().warn(
            "Cannot re-enter a remote world automatically; requesting behavior-resource reload only"
        );
        sendReloadCommand();
        return;
    }

    if (mPendingReentry) {
        getSelf().getLogger().warn("Cannot reload and re-enter world: a world reload is already in progress");
        return;
    }

    auto const worldId = level->getLevelId();
    if (worldId.empty()) {
        getSelf().getLogger().warn("Cannot reload and re-enter world: the current world has no level ID");
        return;
    }
    beginReentry(worldId);
    notify("Reloading behavior resources and re-entering the world...");
    getSelf().getLogger().info("World re-entry queued for level ID '{}'", worldId);

    ll::thread::ServerThreadExecutor::getDefault().execute([this, worldId] {
        auto server = ll::service::getServerInstance();
        if (!server) {
            ll::thread::ClientThreadExecutor::getDefault().execute([this, worldId] {
                if (mPendingReentry && mPendingReentry->worldId == worldId) {
                    clearReentry();
                }
                getSelf().getLogger().warn("Cannot reload and re-enter world: the local server stopped");
            });
            return;
        }

        server->onRequestResourceReload();
        getSelf().getLogger().info("Requested behavior-resource reload before re-entering the world");
        ll::thread::ClientThreadExecutor::getDefault().execute([this, worldId] {
            auto client = ll::service::getClientInstance();
            if (!client) {
                clearReentry();
                getSelf().getLogger().warn("Cannot re-enter world: no active client");
                return;
            }
            getSelf().getLogger().info("Requesting synchronous world exit");
            client->requestLeaveGame(true, true);
        });
    });
}

void CreatorTools::startWorldAfterShutdown() {
    if (!mPendingReentry) {
        return;
    }

    if (std::chrono::steady_clock::now() > mPendingReentry->deadline) {
        auto const worldId = mPendingReentry->worldId;
        clearReentry();
        getSelf().getLogger().warn(
            "Gave up re-entering level ID '{}': the world did not shut down within {}s",
            worldId,
            std::chrono::duration_cast<std::chrono::seconds>(kReentryTimeout).count()
        );
        return;
    }

    auto client = ll::service::getClientInstance();
    if (!client || !client->getMinecraftGame().isLeaveGameDone()) {
        return;
    }

    auto       server = ll::service::getServerInstance();
    bool const serverStopped =
        !server
        || server->mInstanceState.get().load(std::memory_order_acquire) == ServerInstance::InstanceState::Stopped;
    if (!serverStopped) {
        return;
    }

    auto const worldId = mPendingReentry->worldId;
    getSelf().getLogger().info("World shutdown complete; starting level ID '{}'", worldId);
    auto& localWorldStarter = client->getWorldSystem().mModules.get().mLocalWorldStarter;
    if (!localWorldStarter) {
        clearReentry();
        getSelf().getLogger().warn("Cannot re-enter world: local world starter is unavailable");
        return;
    }

    World::WorldID id;
    id.mValue         = worldId;
    auto const result = localWorldStarter->startLocalWorld(id);
    clearReentry();
    if (result != World::StartLocalWorldResult::Ok) {
        getSelf().getLogger().warn(
            "Cannot re-enter world: starting the local world failed with result {}",
            static_cast<int>(result)
        );
        return;
    }
    getSelf().getLogger().info("Started the world again after behavior-resource reload");
}

void CreatorTools::beginReentry(std::string worldId) {
    mPendingReentry = PendingReentry{std::move(worldId), std::chrono::steady_clock::now() + kReentryTimeout};
    mReentryPending.store(true, std::memory_order_release);
}

void CreatorTools::clearReentry() {
    mPendingReentry.reset();
    mReentryPending.store(false, std::memory_order_release);
}

bool CreatorTools::load() {
    getSelf().getLogger().info("Loading");
    getSelf().getLogger().info("Loaded");
    return true;
}

bool CreatorTools::enable() {
    mEntityDebugOverlay.enable();
    mWorldReentryListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::BeforeUIRenderEvent>(
        [this](ll::event::BeforeUIRenderEvent&) {
            if (!mReentryPending.load(std::memory_order_acquire)) {
                return;
            }
            if (mWorldReentryCheckQueued.exchange(true, std::memory_order_acq_rel)) {
                return;
            }

            ll::thread::ClientThreadExecutor::getDefault().execute([this] {
                mWorldReentryCheckQueued.store(false, std::memory_order_release);
                startWorldAfterShutdown();
            });
        }
    );
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

                bool const controlPressed = isKeyDown(VK_CONTROL);
                bool const shiftPressed   = isKeyDown(VK_SHIFT);
                if (!controlPressed && !shiftPressed) {
                    return;
                }

                if (mRPressed) {
                    event.cancel();
                    return;
                }

                mRPressed = true;
                event.cancel();
                if (controlPressed && shiftPressed) {
                    ll::thread::ClientThreadExecutor::getDefault().execute([this] {
                        reloadBehaviorResourcesAndReenterWorld();
                    });
                } else if (controlPressed) {
                    ll::thread::ClientThreadExecutor::getDefault().execute(sendReloadCommand);
                } else {
                    ll::thread::ClientThreadExecutor::getDefault().execute(reloadClientAssets);
                }
                return;
            }

            if (event.keyCode() != kKeyG) {
                return;
            }

            if (!isMinecraftForeground()) {
                mGPressed = false;
                return;
            }

            if (!isGameplayActive()) {
                mGPressed = false;
                return;
            }

            if (event.isDown()) {
                // Plain G only: Ctrl+G, Shift+G and Alt+G must not switch game mode.
                if (isKeyDown(VK_CONTROL) || isKeyDown(VK_SHIFT) || isKeyDown(VK_MENU)) {
                    return;
                }
                if (mGPressed) {
                    return;
                }
                mGPressed      = true;
                auto const now = std::chrono::steady_clock::now();
                if (now - mLastGamemodeSwitch < kGamemodeDebounce) {
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
    mWorldReentryListener.reset();
    mEntityDebugOverlay.disable();
    mGPressed = false;
    mRPressed = false;
    clearReentry();
    mWorldReentryCheckQueued.store(false, std::memory_order_release);
    getSelf().getLogger().info("Disabled");
    return true;
}

} // namespace creator_tools

LL_REGISTER_MOD(creator_tools::CreatorTools, creator_tools::CreatorTools::getInstance());