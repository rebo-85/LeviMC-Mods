#pragma once

#include "ll/api/event/ListenerBase.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace creator_tools {

/// One node of the entity's behavior tree, flattened for display.
struct BehaviorEntry {
    std::string name;
    int         depth{};
    bool        active{}; // this is the node that is currently running
};

/// One line of world-space text above the entity.
struct DebugLine {
    std::string text;
    bool        highlighted{};
    bool        operator==(DebugLine const&) const = default;
};

class EntityDebugOverlay {
public:
    bool enable();
    void disable();

private:
    struct BehaviorSnapshot {
        std::uint64_t              runtimeId{};
        bool                       ready{};
        bool                       available{};
        bool                       runtimeStateAvailable{};
        std::string                treeName;
        std::vector<BehaviorEntry> entries;
        std::string                activeName;
    };

    /// What the server thread last sent, so unchanged frames can be skipped.
    struct SentState {
        std::uint64_t                         generation{std::numeric_limits<std::uint64_t>::max()};
        std::vector<DebugLine>                lines;
        std::array<float, 6>                  bounds{};
        std::chrono::steady_clock::time_point sentAt{};
    };

    void clearSelection();
    void resetSelectionState();
    void requestBehaviorSnapshot(std::uint64_t runtimeId, std::int64_t uniqueId);
    void queueShapeUpdate(std::uint64_t runtimeId, std::uint64_t generation);
    void queueShapeRemoval(std::uint64_t generation);

    ll::event::ListenerPtr                mMouseListener;
    ll::event::ListenerPtr                mRenderListener;
    std::atomic<std::uint64_t>            mSelectedRuntimeId{0};
    std::atomic<std::uint64_t>            mSelectionGeneration{0};
    std::atomic<std::int64_t>             mLocalPlayerUniqueId{0};
    std::uint64_t                         mLastRenderLogGeneration{std::numeric_limits<std::uint64_t>::max()};
    std::mutex                            mShapeRefreshMutex;
    std::chrono::steady_clock::time_point mNextShapeRefresh{};
    std::atomic<std::uint64_t>            mLastShapeWarningGeneration{std::numeric_limits<std::uint64_t>::max()};
    std::size_t                           mActiveTextShapeCount{}; // server thread only
    SentState                             mLastSent;               // server thread only
    std::mutex                            mBehaviorRefreshMutex;
    std::mutex                            mBehaviorSnapshotMutex;
    BehaviorSnapshot                      mBehaviorSnapshot;
    std::chrono::steady_clock::time_point mNextBehaviorRefresh{};

    // Queued server-thread tasks check this so they do nothing after the mod is disabled.
    std::shared_ptr<std::atomic_bool> const mAlive{std::make_shared<std::atomic_bool>(false)};
};

} // namespace creator_tools