#include "mod/CreatorTools.h"

#include "mod/Gamemode.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ClientThreadExecutor.h"

#include "mc/client/game/ClientInstance.h"

#include <chrono>
#include <string_view>

#include <Windows.h>

namespace creator_tools {

namespace {
bool isMinecraftForeground() {
    HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }

    HWND window = GetAncestor(foreground, GA_ROOT);
    if (window == nullptr) {
        window = foreground;
    }
    if (!IsWindowVisible(window) || IsIconic(window)) {
        return false;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    return processId == GetCurrentProcessId();
}

bool isGameplayActive() {
    auto client = ll::service::getClientInstance();
    if (!client || !client->isWorldActive()) {
        return false;
    }

    auto const screenName = client->getTopScreenName();
    for (std::string_view menuName : {"pause", "chat", "inventory", "start", "death", "trade", "container"}) {
        if (screenName.find(menuName) != std::string::npos) {
            return false;
        }
    }
    return true;
}

constexpr int kKeyG = 0x47;
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
    mListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::KeyInputEvent>(
        [this](ll::event::KeyInputEvent& event) {
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
    mGPressed = false;
    getSelf().getLogger().info("Disabled");
    return true;
}

} // namespace creator_tools

LL_REGISTER_MOD(creator_tools::CreatorTools, creator_tools::CreatorTools::getInstance());