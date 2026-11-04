#pragma once

#include "ll/api/event/ListenerBase.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace creator_tools {

class EntityDebugOverlay {
public:
    bool enable();
    void disable();

private:
    struct BehaviorSnapshot {
        std::uint64_t            runtimeId{};
        bool                     ready{};
        bool                     available{};
        bool                     runtimeStateAvailable{};
        std::string              treeName;
        std::vector<std::string> names;
        std::string              activeName;
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
    std::uint64_t                         mLastRenderLogGeneration{std::numeric_limits<std::uint64_t>::max()};
    std::mutex                            mShapeRefreshMutex;
    std::chrono::steady_clock::time_point mNextShapeRefresh{};
    std::atomic<std::uint64_t>            mLastShapeWarningGeneration{std::numeric_limits<std::uint64_t>::max()};
    std::size_t                           mActiveTextShapeCount{};
    std::mutex                            mBehaviorRefreshMutex;
    std::mutex                            mBehaviorSnapshotMutex;
    BehaviorSnapshot                      mBehaviorSnapshot;
    std::chrono::steady_clock::time_point mNextBehaviorRefresh{};
};

} // namespace creator_tools
