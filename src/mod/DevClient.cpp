#include "mod/DevClient.h"

#include "mod/Gamemode.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ClientThreadExecutor.h"

#include "mc/client/game/ClientInstance.h"

#include <chrono>
#include <fstream>
#include <string_view>

#include <Windows.h>

namespace dev_client {

namespace {
bool isMinecraftForeground() {
    HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
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

DevClient& DevClient::getInstance() {
    static DevClient instance;
    return instance;
}

void DevClient::debug(std::string_view message) const {
    std::ofstream output(getSelf().getModDir() / "debug.txt", std::ios::app);
    if (output) {
        output << message << '\n';
    }
}

bool DevClient::load() {
    debug("load()");
    getSelf().getLogger().info("Loaded");
    return true;
}

bool DevClient::enable() {
    mListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::KeyInputEvent>(
        [this](ll::event::KeyInputEvent& event) {
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

    debug("enable()");
    getSelf().getLogger().info("Enabled");
    return true;
}

bool DevClient::disable() {
    mListener.reset();
    mGPressed = false;
    debug("disable()");
    getSelf().getLogger().info("Disabled");
    return true;
}

} // namespace dev_client

LL_REGISTER_MOD(dev_client::DevClient, dev_client::DevClient::getInstance());