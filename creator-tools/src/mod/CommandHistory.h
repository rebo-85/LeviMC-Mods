#pragma once

#include "ll/api/event/ListenerBase.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <optional>
#include <string>

namespace creator_tools {

/// Keeps the chat Up-arrow command history across game sessions.
///
/// Vanilla keeps sent messages in ClientInstance::getSentMessageHistory() for the current session only.
/// This module restores saved commands into that same container on startup and saves it to disk
/// whenever it changes, so the normal Up arrow in chat just works.
class CommandHistory {
public:
    bool enable();
    void disable();

private:
    void pollOnClientThread();

    ll::event::ListenerPtr                mRenderListener;
    std::atomic_bool                      mPollQueued{false};
    std::chrono::steady_clock::time_point mNextPoll{}; // render listener only

    // Everything below: client thread only.
    bool                    mLoaded{false};
    bool                    mPendingSave{false};
    std::optional<bool>     mNewestAtFront; // game's deque order; learned from observation, then stored in the file
    std::deque<std::string> mLastRaw;       // full copy of the game's history at the last poll
    std::deque<std::string> mLastSaved;     // the commands last written to disk, in game order
};

} // namespace creator_tools
