#include "mod/CreatorTools.h"

#include "mod/ClientCommands.h"
#include "mod/Gamemode.h"
#include "mod/GameplayState.h"
#include "mod/ServerActions.h"

#include "ll/api/Config.h"
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
#include "mc/client/renderer/TextureGroup.h"
#include "mc/resources/IResourcePackRepository.h"
#include "mc/world/level/Level.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fmt/format.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <Windows.h>

namespace creator_tools {

namespace {
constexpr unsigned kModNone  = 0;
constexpr unsigned kModCtrl  = 1;
constexpr unsigned kModShift = 2;
constexpr unsigned kModAlt   = 4;

constexpr auto         kGamemodeDebounce      = std::chrono::milliseconds{250};
constexpr std::int64_t kClientReloadTimeoutMs = 30'000;

// Client asset reload bookkeeping. The generation counter lets a reload that is stuck (its async
// result never completes) be superseded after kClientReloadTimeoutMs without the stale completion
// callback later clearing the flag of the new reload.
std::atomic_bool           gClientReloadInProgress{false};
std::atomic<std::int64_t>  gClientReloadStartedAtMs{0};
std::atomic<std::uint64_t> gClientReloadGeneration{0};

bool isKeyDown(int virtualKey) { return (GetAsyncKeyState(virtualKey) & 0x8000) != 0; }

unsigned currentModifiers() {
    unsigned mods = kModNone;
    if (isKeyDown(VK_CONTROL)) mods |= kModCtrl;
    if (isKeyDown(VK_SHIFT)) mods |= kModShift;
    if (isKeyDown(VK_MENU)) mods |= kModAlt;
    return mods;
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::optional<int> parseKey(std::string_view text) {
    if (text.size() == 1) {
        auto const key = text.front();
        if ((key >= 'A' && key <= 'Z') || (key >= 'a' && key <= 'z')) {
            return key >= 'a' ? key - ('a' - 'A') : key;
        }
        if (key >= '0' && key <= '9') {
            return key;
        }
    }
    if (text.size() >= 2 && (text.front() == 'F' || text.front() == 'f')) {
        int number = 0;
        auto const result = std::from_chars(text.data() + 1, text.data() + text.size(), number);
        if (result.ec == std::errc{} && result.ptr == text.data() + text.size() && number >= 1 && number <= 12) {
            return VK_F1 + number - 1;
        }
    }
    return std::nullopt;
}

int configuredKey(std::string const& text, int fallback, std::string_view name) {
    if (auto key = parseKey(text)) {
        return *key;
    }
    CreatorTools::getInstance().getSelf().getLogger().warn(
        "Invalid hotkeys.{} value '{}'; using the default",
        name,
        text
    );
    return fallback;
}

/// Returns the new reload's generation, or 0 if a reload is already running.
std::uint64_t tryBeginClientReload() {
    auto const now = nowMs();
    if (gClientReloadInProgress.exchange(true, std::memory_order_acq_rel)) {
        if (now - gClientReloadStartedAtMs.load(std::memory_order_acquire) < kClientReloadTimeoutMs) {
            return 0;
        }
        CreatorTools::getInstance().getSelf().getLogger().warn(
            "Previous client asset reload never finished; starting a new one"
        );
    }
    gClientReloadStartedAtMs.store(now, std::memory_order_release);
    return gClientReloadGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
}

void finishClientReload(std::uint64_t generation) {
    if (gClientReloadGeneration.load(std::memory_order_acquire) == generation) {
        gClientReloadInProgress.store(false, std::memory_order_release);
    }
}

void reloadClientAssets() {
    auto& logger = CreatorTools::getInstance().getSelf().getLogger();
    auto  client = ll::service::getClientInstance();
    if (!client) {
        logger.warn("Cannot reload client assets: no active client");
        return;
    }

    auto const generation = tryBeginClientReload();
    if (generation == 0) {
        notify("A client asset reload is already in progress");
        return;
    }

    auto refresh = client->getResourcePackRepository().refreshPacksAsync();
    if (!refresh.mResult) {
        finishClientReload(generation);
        notify("Cannot reload client assets: pack refresh did not start");
        return;
    }

    notify("Reloading client packs and assets...");
    auto const startedAt = std::chrono::steady_clock::now();
    refresh.mResult->addOnComplete([startedAt, generation](Bedrock::Threading::IAsyncResult<void> const& result) {
        auto const error = result.getError();
        if (error) {
            finishClientReload(generation);
            auto message = fmt::format("Cannot refresh client resource packs: {}", error.message());
            ll::thread::ClientThreadExecutor::getDefault().execute([message = std::move(message)] { notify(message); });
            return;
        }

        ll::thread::ClientThreadExecutor::getDefault().execute([startedAt, generation] {
            // Clears the in-progress flag on every exit path below.
            struct Guard {
                explicit Guard(std::uint64_t g) : generation(g) {}
                ~Guard() { finishClientReload(generation); }
                std::uint64_t generation;
            } guard{generation};

            auto& logger = CreatorTools::getInstance().getSelf().getLogger();
            if (gClientReloadGeneration.load(std::memory_order_acquire) != generation) {
                logger.warn("Ignoring a client asset reload that was superseded by a newer one");
                return;
            }

            auto client = ll::service::getClientInstance();
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
                notify("Cannot reload client textures: no texture group");
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
    logger.info("Refreshing client resource packs");
}
} // namespace

CreatorTools& CreatorTools::getInstance() {
    static CreatorTools instance;
    return instance;
}

void CreatorTools::requestGamemodeCycle() {
    auto const now = std::chrono::steady_clock::now();
    if (now - mLastGamemodeSwitch < kGamemodeDebounce) {
        return;
    }
    mLastGamemodeSwitch = now;
    gamemode::cycleLocalPlayer();
}

void CreatorTools::handleKeyInput(ll::event::KeyInputEvent& event) {
    auto const key = static_cast<int>(event.keyCode());

    // Is a binding on this key currently held? Then every repeat/release event belongs to it, even if
    // the modifier keys changed in the meantime (so pressing Ctrl mid-hold can't trigger a second action).
    bool anyHeld     = false;
    bool consumeHeld = false;
    for (auto const& binding : mBindings) {
        if (binding.key == key && binding.held) {
            anyHeld     = true;
            consumeHeld = consumeHeld || binding.consume;
        }
    }
    auto releaseKey = [&] {
        for (auto& binding : mBindings) {
            if (binding.key == key) {
                binding.held = false;
            }
        }
    };

    if (!isMinecraftForeground()) {
        releaseKey();
        // The game must not react to keys typed into another window. Only presses are swallowed;
        // releases still pass through so a key held while switching windows can't get stuck down.
        if (event.isDown()) {
            event.cancel();
        }
        return;
    }

    if (!event.isDown()) {
        if (anyHeld) {
            if (consumeHeld) {
                event.cancel();
            }
            releaseKey();
        }
        return;
    }

    if (anyHeld) {
        if (consumeHeld) {
            event.cancel();
        }
        return;
    }

    auto const mods = currentModifiers();
    for (auto& binding : mBindings) {
        if (binding.key != key || binding.mods != mods) {
            continue;
        }

        if (!isGameplayActive()) {
            // Log the rejected screen once per distinct name so a wrong kGameplayScreenName is easy to spot.
            if (auto client = ll::service::getClientInstance()) {
                auto screen = std::string{client->getTopScreenName()};
                if (screen != mLastRejectedScreen) {
                    getSelf().getLogger().debug("Ignored hotkey: top screen is '{}', not gameplay", screen);
                    mLastRejectedScreen = std::move(screen);
                }
            }
            return;
        }

        binding.held = true;
        if (binding.consume) {
            event.cancel();
        }
        ll::thread::ClientThreadExecutor::getDefault().execute([action = binding.action] { action(); });
        return;
    }
}

void CreatorTools::reloadBehaviorResources() {
    if (!ll::service::getClientInstance() || !ll::service::getMultiPlayerLevel()) {
        getSelf().getLogger().warn("Cannot reload behavior resources: no active client level");
        return;
    }

    // Local world: in-process server call. Remote world: no API reaches the server, so ask it with /reload.
    if (server::reloadLocal(false)) {
        notify("Reloading behavior resources...");
    } else if (sendCommand("/reload")) {
        notify("Requested behavior-resource reload (/reload)");
    }
}

void CreatorTools::reloadAll() {
    if (!ll::service::getClientInstance() || !ll::service::getMultiPlayerLevel()) {
        getSelf().getLogger().warn("Cannot reload all: no active client level");
        return;
    }

    // No leaving/re-entering the world anymore: /reload all runs inside the live server.
    if (server::reloadLocal(true)) {
        notify("Reloading behavior packs (/reload all)...");
    } else if (sendCommand("/reload all")) {
        notify("Requested full reload (/reload all)");
    }
}

bool CreatorTools::load() {
    auto& logger    = getSelf().getLogger();
    auto const path = getSelf().getConfigDir() / "config.json";
    bool needsRewrite;
    try {
        needsRewrite = !ll::config::loadConfig(mConfig, path);
    } catch (std::exception const& error) {
        logger.error("Could not load {}: {}", path.string(), error.what());
        return false;
    }

    auto& entityDebug = mConfig.entityDebug;
    entityDebug.maxBehaviors = std::clamp(entityDebug.maxBehaviors, 1, 64);
    if (!entityDebug.inspectCtrl && !entityDebug.inspectShift && !entityDebug.inspectAlt) {
        logger.warn("entityDebug: no inspect modifier set; using Ctrl+Shift");
        entityDebug.inspectCtrl  = true;
        entityDebug.inspectShift = true;
    }

    auto clampFloat = [&logger](float& value, float fallback, float min, float max, char const* name) {
        if (!std::isfinite(value)) {
            logger.warn("{} must be finite; using {}", name, fallback);
            value = fallback;
        }
        value = std::clamp(value, min, max);
    };
    clampFloat(entityDebug.labelScale, 0.3f, 0.05f, 2.0f, "entityDebug.labelScale");
    clampFloat(entityDebug.pathHeightOffset, 0.1f, 0.0f, 5.0f, "entityDebug.pathHeightOffset");
    clampFloat(entityDebug.pathTargetRadius, 0.25f, 0.01f, 5.0f, "entityDebug.pathTargetRadius");
    clampFloat(entityDebug.pathDestRadius, 0.40f, 0.01f, 5.0f, "entityDebug.pathDestRadius");
    auto clampColor = [&clampFloat](std::array<float, 4>& color, char const* name) {
        for (auto& channel : color) {
            clampFloat(channel, 1.0f, 0.0f, 1.0f, name);
        }
    };
    clampColor(entityDebug.pathLineColor, "entityDebug.pathLineColor");
    clampColor(entityDebug.pathTargetColor, "entityDebug.pathTargetColor");
    clampColor(entityDebug.pathDestColor, "entityDebug.pathDestColor");

    if (needsRewrite && !ll::config::saveConfig(mConfig, path)) {
        logger.error("Could not write updated config to {}", path.string());
        return false;
    }

    getSelf().getLogger().info("Loaded");
    return true;
}

bool CreatorTools::enable() {
    mAlive->store(true);
    mEntityInspector.enable();
    mCommandHistory.enable();

    // G is intentionally not consumed (plain key, only active on the HUD). The R bindings are consumed
    // so the game never sees Ctrl/Shift+R.
    auto const gamemodeKey = configuredKey(mConfig.hotkeys.gamemode, 'G', "gamemode");
    auto const reloadKey   = configuredKey(mConfig.hotkeys.reload, 'R', "reload");
    mBindings = {
        Binding{gamemodeKey, kModNone,             false, [this] { requestGamemodeCycle(); }   },
        Binding{reloadKey,   kModCtrl,             true,  [this] { reloadBehaviorResources(); }},
        Binding{reloadKey,   kModCtrl | kModShift, true,  [this] { reloadAll(); }              },
        Binding{reloadKey,   kModShift,            true,  [] { reloadClientAssets(); }         },
    };

    mListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::KeyInputEvent>(
        [this](ll::event::KeyInputEvent& event) { handleKeyInput(event); }
    );

    getSelf().getLogger().info("Enabled");
    return true;
}

bool CreatorTools::disable() {
    mAlive->store(false);
    mListener.reset();
    mEntityInspector.disable();
    mCommandHistory.disable();
    mBindings.clear();
    getSelf().getLogger().info("Disabled");
    return true;
}

} // namespace creator_tools

LL_REGISTER_MOD(creator_tools::CreatorTools, creator_tools::CreatorTools::getInstance());
