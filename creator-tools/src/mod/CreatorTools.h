#pragma once

#include "mod/EntityDebugOverlay.h"

#include "ll/api/event/ListenerBase.h"
#include "ll/api/mod/NativeMod.h"

#include <atomic>
#include <chrono>
#include <optional>
#include <string>

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

    // TODO: Implement this method if you need to unload the mod.
    // /// @return True if the mod is unloaded successfully.
    // bool unload();

private:
    ll::mod::NativeMod&                   mSelf;
    EntityDebugOverlay                    mEntityDebugOverlay;
    ll::event::ListenerPtr                mListener;
    ll::event::ListenerPtr                mWorldReentryListener;
    std::optional<std::string>            mWorldToReenter;
    bool                                  mGPressed{false};
    bool                                  mRPressed{false};
    std::atomic_bool                      mWorldReentryCheckQueued{false};
    std::chrono::steady_clock::time_point mLastGamemodeSwitch{};

    void reloadBehaviorResourcesAndReenterWorld();
    void startWorldAfterShutdown();
};

} // namespace creator_tools