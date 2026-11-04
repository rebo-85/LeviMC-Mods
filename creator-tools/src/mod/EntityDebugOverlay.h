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
        std::string              treeName;
        std::vector<std::string> names;
        std::string              activeName;
    };

    void clearSelection();
    void requestBehaviorSnapshot(std::uint64_t runtimeId);

    ll::event::ListenerPtr                mMouseListener;
    ll::event::ListenerPtr                mRenderListener;
    std::atomic<std::uint64_t>            mSelectedRuntimeId{0};
    std::atomic<std::uint64_t>            mSelectionGeneration{0};
    std::uint64_t                         mLastRenderLogGeneration{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t                         mLastProjectionWarningGeneration{std::numeric_limits<std::uint64_t>::max()};
    std::chrono::steady_clock::time_point mNextProjectionDiagnostic{};
    std::mutex                            mBehaviorRefreshMutex;
    std::mutex                            mBehaviorSnapshotMutex;
    BehaviorSnapshot                      mBehaviorSnapshot;
    std::chrono::steady_clock::time_point mNextBehaviorRefresh{};
};

} // namespace creator_tools
