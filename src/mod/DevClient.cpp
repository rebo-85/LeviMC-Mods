#include "mod/DevClient.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/input/KeyRegistry.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/Level.h"

#include <fstream>
#include <string>

namespace dev_client {

static void dbg(std::string const& msg) {
    std::ofstream f("C:/Users/catal/Documents/GitHub/ReBo/Minecraft Dev Client/debug.txt", std::ios::app);
    f << msg << "\n";
}

static constexpr int kKeyG = 0x47; // Windows VK_G

static GameType const kModes[] = {
    GameType::Survival,
    GameType::Creative,
    GameType::Adventure,
    GameType::Spectator,
};

static void toggleGamemode() {
    auto level = ll::service::getMultiPlayerLevel();
    if (!level) {
        dbg("no multiplayer level");
        return;
    }

    auto* player = level->getPrimaryLocalPlayer();
    if (!player) {
        dbg("no player");
        return;
    }

    auto current = player->getPlayerGameType();

    size_t idx = 0;
    for (size_t i = 0; i < std::size(kModes); ++i) {
        if (kModes[i] == current) {
            idx = i;
            break;
        }
    }
    GameType next = kModes[(idx + 1) % std::size(kModes)];

    player->setPlayerGameType(next);
    dbg("gamemode " + std::to_string(static_cast<int>(current)) + " -> " + std::to_string(static_cast<int>(next)));
}

DevClient& DevClient::getInstance() {
    static DevClient instance;
    return instance;
}

bool DevClient::load() {
    dbg("load()");
    getSelf().getLogger().info("Loaded");
    return true;
}

bool DevClient::enable() {
    dbg("enable()");

    auto& toggle = ll::input::KeyRegistry::getInstance().getOrCreateKey("dev-client.toggle-gamemode", {'G'});

    toggle.registerButtonDownHandler([](FocusImpact, IClientInstance&) {
        dbg("G pressed through KeyRegistry");
        toggleGamemode();
    });

    mListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::KeyInputEvent>(
        [](ll::event::KeyInputEvent& event) {
            if (event.isDown() && event.keyCode() == kKeyG) {
                dbg("G pressed through KeyInputEvent");
                toggleGamemode();
            }
        }
    );
    return true;
}
bool DevClient::disable() {
    dbg("disable()");
    return true;
}

} // namespace dev_client

LL_REGISTER_MOD(dev_client::DevClient, dev_client::DevClient::getInstance());