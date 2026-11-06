#pragma once

#include "mod/CommandHistory.h"
#include "mod/EntityInspector.h"

#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/mod/NativeMod.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace creator_tools {

class CreatorTools {

public:
    static CreatorTools& getInstance();

    CreatorTools() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    /// @return True if the mod is loaded successfully.
    bool load();

    /// @return True if the mod is enabled successfully.
    bool enable();

    /// @return True if the mod is disabled successfully.
    bool disable();

    /// Queued executor tasks check this so they do nothing if the mod was disabled in the meantime.
    [[nodiscard]] std::shared_ptr<std::atomic_bool> alive() const { return mAlive; }

private:
    /// One hotkey. Modifiers (Ctrl/Shift/Alt) must match exactly, so Ctrl+R and Ctrl+Shift+R never overlap.
    struct Binding {
        int                   key;
        unsigned              mods;
        bool                  consume; // swallow the key event so the game doesn't also see it
        std::function<void()> action;  // always run on the client thread
        bool                  held{false};
    };

    ll::mod::NativeMod&                   mSelf;
    EntityInspector                       mEntityInspector;
    CommandHistory                        mCommandHistory;
    ll::event::ListenerPtr                mListener;
    std::vector<Binding>                  mBindings;
    std::string                           mLastRejectedScreen;
    std::chrono::steady_clock::time_point mLastGamemodeSwitch{};

    // Queued executor tasks check this so they do nothing if the mod was disabled in the meantime.
    std::shared_ptr<std::atomic_bool> const mAlive{std::make_shared<std::atomic_bool>(false)};

    void handleKeyInput(ll::event::KeyInputEvent& event);
    void requestGamemodeCycle();
    void reloadBehaviorResources();
    void reloadAll();
};

} // namespace creator_tools
