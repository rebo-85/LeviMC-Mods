#pragma once

#include "ll/api/service/Bedrock.h"

#include "mc/client/game/ClientInstance.h"

#include <string_view>

#include <Windows.h>

namespace creator_tools {

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

} // namespace creator_tools
