#pragma once

#include "ll/api/service/Bedrock.h"

#include "mc/client/game/ClientInstance.h"

#include <string_view>

#include <Windows.h>

namespace creator_tools {

/// The only screen on which hotkeys are active: the in-game HUD.
///
/// This is a whitelist on purpose. A blocklist of menu names misses text-entry screens such as the
/// anvil rename box, sign editor, book editor and command block screen, so typing a "g" there
/// would switch game mode. If hotkeys never fire, enable debug logging: the mod logs the name of
/// the top screen whenever it rejects a matching key press, so you can correct this value.
inline constexpr std::string_view kGameplayScreenName = "hud_screen";

inline bool isMinecraftForeground() {
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

inline bool isGameplayActive() {
    auto client = ll::service::getClientInstance();
    return client && client->isWorldActive() && client->getTopScreenName() == kGameplayScreenName;
}

} // namespace creator_tools