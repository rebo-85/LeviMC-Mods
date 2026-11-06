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

class Actor;

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

/// Ctrl+right-click an entity to inspect it: a text label (health, position, behaviors), its collision box,
/// and the path it is currently walking, drawn as world-space debug shapes.
class EntityInspector {
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

    /// What the server thread last sent for the label/box, so unchanged frames can be skipped.
    struct SentState {
        std::uint64_t                         generation{std::numeric_limits<std::uint64_t>::max()};
        std::vector<DebugLine>                lines;
        std::array<float, 6>                  bounds{};
        std::chrono::steady_clock::time_point sentAt{};
    };

    /// What the server thread last sent for the path.
    struct SentPathState {
        std::uint64_t                         generation{std::numeric_limits<std::uint64_t>::max()};
        std::vector<std::array<float, 3>>     points;
        bool                                  truncated{};
        std::chrono::steady_clock::time_point sentAt{};
    };

    void clearSelection();
    void resetSelectionState();
    void requestBehaviorSnapshot(std::uint64_t runtimeId, std::int64_t uniqueId);
    void queueShapeUpdate(std::uint64_t runtimeId, std::uint64_t generation);
    void queueShapeRemoval(std::uint64_t generation);

    // Server thread only.
    void updateInfoShapes(Actor* target, std::uint64_t runtimeId, std::uint64_t generation, std::int64_t localPlayerId);
    void updatePathShapes(Actor& target, std::uint64_t generation, std::int64_t localPlayerId);
    void clearShapesOnServer();
    void resetPathState();

    ll::event::ListenerPtr                mMouseListener;
    ll::event::ListenerPtr                mRenderListener;
    std::atomic<std::uint64_t>            mSelectedRuntimeId{0};
    std::atomic<std::uint64_t>            mSelectionGeneration{0};
    std::atomic<std::int64_t>             mLocalPlayerUniqueId{0};
    std::uint64_t                         mLastRenderLogGeneration{std::numeric_limits<std::uint64_t>::max()};
    std::mutex                            mShapeRefreshMutex;
    std::chrono::steady_clock::time_point mNextShapeRefresh{};
    std::atomic<std::uint64_t>            mLastShapeWarningGeneration{std::numeric_limits<std::uint64_t>::max()};
    std::mutex                            mBehaviorRefreshMutex;
    std::mutex                            mBehaviorSnapshotMutex;
    BehaviorSnapshot                      mBehaviorSnapshot;
    std::chrono::steady_clock::time_point mNextBehaviorRefresh{};

    // Server thread only.
    std::size_t   mActiveTextShapeCount{};
    SentState     mLastSent;
    std::size_t   mActivePathSegmentCount{};
    bool          mTargetMarkerActive{};
    bool          mDestinationMarkerActive{};
    SentPathState mLastPathSent;

    // Queued server-thread tasks check this so they do nothing after the mod is disabled.
    std::shared_ptr<std::atomic_bool> const mAlive{std::make_shared<std::atomic_bool>(false)};
};

} // namespace creator_tools
