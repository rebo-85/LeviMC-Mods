#include "mod/EntityDebugOverlay.h"
#include "mod/CreatorTools.h"
#include "mod/GameplayState.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ServerThreadExecutor.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/input/MouseAction.h"
#include "mc/entity/components/BehaviorComponent.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/network/packet/BoxDataPayload.h"
#include "mc/network/packet/PrimitiveShapeDataPayload.h"
#include "mc/network/packet/PrimitiveShapesPacket.h"
#include "mc/network/packet/TextDataPayload.h"
#include "mc/scripting/modules/minecraft/primitiveshapes/ScriptPrimitiveShapeType.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/bhave/BehaviorStatus.h"
#include "mc/world/actor/bhave/definition/BehaviorDefinition.h"
#include "mc/world/actor/bhave/definition/BehaviorTreeDefinition.h"
#include "mc/world/actor/bhave/definition/CompositeDefinition.h"
#include "mc/world/actor/bhave/definition/DecoratorDefinition.h"
#include "mc/world/actor/bhave/definition/SubtreeDefinition.h"
#include "mc/world/actor/bhave/node/BehaviorNode.h"
#include "mc/world/actor/bhave/node/InverterNode.h"
#include "mc/world/actor/bhave/node/RepeatUntilFailureNode.h"
#include "mc/world/actor/bhave/node/RepeatUntilSuccessNode.h"
#include "mc/world/actor/bhave/node/SelectorBehaviorNode.h"
#include "mc/world/actor/bhave/node/SequenceBehaviorNode.h"
#include "mc/world/actor/bhave/node/SubtreeNode.h"
#include "mc/world/level/Level.h"
#include "mc/world/phys/AABB.h"
#include "mc/world/phys/HitResult.h"

#include <algorithm>
#include <chrono>
#include <fmt/format.h>
#include <utility>
#include <vector>

#include <Windows.h>

namespace creator_tools {

namespace {

constexpr auto        kBehaviorRefreshInterval = std::chrono::milliseconds{250};
constexpr auto        kShapeRefreshInterval    = std::chrono::milliseconds{100};
constexpr std::size_t kMaxDisplayedBehaviors   = 8;
constexpr std::size_t kMaxDebugTextLines       = 16;
constexpr std::uint64_t kShapeIdBase           = 0x43544F4F4C530000ULL;
constexpr std::uint64_t kBoxShapeId            = kShapeIdBase + kMaxDebugTextLines;

using DebugTextLine = std::pair<std::string, bool>;

void collectBehaviorNames(BehaviorDefinition const* definition, std::vector<std::string>& names) {
    if (!definition || names.size() >= 64) {
        return;
    }

    if (auto const* composite = dynamic_cast<CompositeDefinition const*>(definition)) {
        for (auto const& child : composite->mChildren.get()) {
            collectBehaviorNames(child.get(), names);
        }
        return;
    }
    if (auto const* decorator = dynamic_cast<DecoratorDefinition const*>(definition)) {
        collectBehaviorNames(decorator->mChild.get(), names);
        return;
    }
    if (auto const* subtree = dynamic_cast<SubtreeDefinition const*>(definition)) {
        auto const& name = subtree->mSubtreeId.get();
        if (!name.empty()) {
            names.push_back(name);
        }
        return;
    }

    auto const& name = definition->mName.get();
    if (!name.empty()) {
        names.push_back(name);
    }
}

BehaviorNode const* getActiveChild(BehaviorNode const& node) {
    if (auto const* sequence = dynamic_cast<SequenceBehaviorNode const*>(&node)) {
        return sequence->mActiveChild.get();
    }
    if (auto const* selector = dynamic_cast<SelectorBehaviorNode const*>(&node)) {
        return selector->mActiveChild.get();
    }
    if (auto const* repeat = dynamic_cast<RepeatUntilFailureNode const*>(&node)) {
        return repeat->mActiveChild.get();
    }
    if (auto const* repeat = dynamic_cast<RepeatUntilSuccessNode const*>(&node)) {
        return repeat->mActiveChild.get();
    }
    if (auto const* inverter = dynamic_cast<InverterNode const*>(&node)) {
        return inverter->mActiveChild.get();
    }
    if (auto const* subtree = dynamic_cast<SubtreeNode const*>(&node)) {
        return subtree->mSubtreeHead.get();
    }
    return nullptr;
}

BehaviorNode const* getRunningLeaf(BehaviorNode const* node) {
    if (!node) {
        return nullptr;
    }
    if (auto const* child = getActiveChild(*node)) {
        if (auto const* leaf = getRunningLeaf(child)) {
            return leaf;
        }
    }
    return node->mStatus == BehaviorStatus::Running ? node : nullptr;
}

PrimitiveShapeDataPayload makeTextShape(
    std::uint64_t  id,
    Vec3 const&    locationOffset,
    std::string    text,
    mce::Color     color,
    DimensionType  dimension,
    ActorUniqueID  attachedTo
) {
    PrimitiveShapeDataPayload shape{};
    shape.mNetworkId          = id;
    shape.mShapeType          = ScriptModuleMinecraft::ScriptPrimitiveShapeType::Text;
    shape.mLocation           = locationOffset;
    shape.mColor              = color;
    shape.mDimensionId        = dimension;
    shape.mAttachedToId       = attachedTo;
    shape.mMaxRenderDistance = 64.0f;

    TextDataPayload textPayload{};
    textPayload.mText        = std::move(text);
    textPayload.mUseRotation = false;
    shape.mExtraDataPayload  = std::move(textPayload);
    return shape;
}

PrimitiveShapeDataPayload makeBoxShape(
    std::uint64_t id,
    AABB const&  bounds,
    Vec3 const&  locationOffset,
    DimensionType dimension,
    ActorUniqueID attachedTo
) {
    PrimitiveShapeDataPayload shape{};
    shape.mNetworkId          = id;
    shape.mShapeType          = ScriptModuleMinecraft::ScriptPrimitiveShapeType::Box;
    shape.mLocation           = locationOffset;
    shape.mColor              = mce::Color{0.2f, 0.9f, 1.0f, 1.0f};
    shape.mDimensionId        = dimension;
    shape.mAttachedToId       = attachedTo;
    shape.mMaxRenderDistance = 64.0f;
    shape.mExtraDataPayload  = BoxDataPayload{Vec3{
        bounds.max.x - bounds.min.x,
        bounds.max.y - bounds.min.y,
        bounds.max.z - bounds.min.z
    }};
    return shape;
}

PrimitiveShapeDataPayload makeRemovedShape(std::uint64_t id) {
    PrimitiveShapeDataPayload shape{};
    shape.mNetworkId = id;
    return shape;
}

void sendShapes(std::vector<PrimitiveShapeDataPayload> shapes) {
    PrimitiveShapesPacket packet{};
    packet.setSerializationMode(SerializationMode::CerealOnly);
    packet.mShapes.get() = std::move(shapes);
    packet.sendToClients();
}

std::vector<PrimitiveShapeDataPayload> makeRemovalShapes() {
    std::vector<PrimitiveShapeDataPayload> shapes;
    shapes.reserve(kMaxDebugTextLines + 1);
    for (std::size_t i = 0; i < kMaxDebugTextLines; ++i) {
        shapes.push_back(makeRemovedShape(kShapeIdBase + i));
    }
    shapes.push_back(makeRemovedShape(kBoxShapeId));
    return shapes;
}

} // namespace

bool EntityDebugOverlay::enable() {
    mMouseListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::MouseInputEvent>(
        [this](ll::event::MouseInputEvent& event) {
            if (event.actionButtonId() != MouseAction::ActionRight || event.buttonData() != MouseAction::DataDown) {
                return;
            }
            if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0) {
                return;
            }
            if (!isMinecraftForeground() || !isGameplayActive()) {
                return;
            }

            auto client = ll::service::getClientInstance();
            if (!client) {
                return;
            }
            auto const& hitResult = client->getLatestHitResult();
            if (hitResult.mType != HitResultType::Entity) {
                CreatorTools::getInstance().getSelf().getLogger().info("Entity debug: no entity targeted");
                return;
            }
            auto* target = hitResult.getEntity();
            if (!target) {
                CreatorTools::getInstance().getSelf().getLogger().warn("Entity debug: target is unavailable");
                return;
            }

            auto const runtimeId = static_cast<std::uint64_t>(target->getRuntimeID());
            auto const selected  = mSelectedRuntimeId.load();
            auto const next      = selected == runtimeId ? 0 : runtimeId;
            mSelectedRuntimeId.store(next);
            auto const generation = mSelectionGeneration.fetch_add(1) + 1;
            {
                std::scoped_lock lock(mBehaviorRefreshMutex);
                mNextBehaviorRefresh = {};
            }
            {
                std::scoped_lock lock(mBehaviorSnapshotMutex);
                mBehaviorSnapshot = {};
            }
            {
                std::scoped_lock lock(mShapeRefreshMutex);
                mNextShapeRefresh = {};
            }
            if (next == 0) {
                queueShapeRemoval(generation);
            }
            CreatorTools::getInstance().getSelf().getLogger().info(
                "Entity debug label {} for runtime ID {}",
                next == 0 ? "hidden" : "enabled",
                runtimeId
            );
        }
    );

    mRenderListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::BeforeUIRenderEvent>(
        [this](ll::event::BeforeUIRenderEvent&) {
            auto client = ll::service::getClientInstance();
            if (!client || !client->isWorldActive()) {
                clearSelection();
                return;
            }
            if (!isGameplayActive()) {
                return;
            }

            auto const runtimeId = mSelectedRuntimeId.load();
            if (runtimeId == 0) {
                return;
            }
            auto const generation = mSelectionGeneration.load();
            auto&      logger     = CreatorTools::getInstance().getSelf().getLogger();
            if (mLastRenderLogGeneration != generation) {
                logger.info("Entity debug world-space renderer active for runtime ID {}", runtimeId);
                mLastRenderLogGeneration = generation;
            }

            auto level = ll::service::getMultiPlayerLevel();
            if (!level) {
                logger.warn("Entity debug: no active client level for runtime ID {}", runtimeId);
                clearSelection();
                return;
            }
            auto* target = level->getRuntimeEntity(ActorRuntimeID{runtimeId}, false);
            if (!target) {
                logger.warn("Entity debug: entity {} is not present in the client level", runtimeId);
                clearSelection();
                return;
            }

            requestBehaviorSnapshot(runtimeId);
            queueShapeUpdate(runtimeId, generation);
        }
    );

    return true;
}

void EntityDebugOverlay::disable() {
    mMouseListener.reset();
    mRenderListener.reset();
    clearSelection();
}

void EntityDebugOverlay::clearSelection() {
    if (mSelectedRuntimeId.exchange(0) == 0) {
        return;
    }

    auto const generation = mSelectionGeneration.fetch_add(1) + 1;
    {
        std::scoped_lock lock(mBehaviorRefreshMutex);
        mNextBehaviorRefresh = {};
    }
    {
        std::scoped_lock lock(mBehaviorSnapshotMutex);
        mBehaviorSnapshot = {};
    }
    {
        std::scoped_lock lock(mShapeRefreshMutex);
        mNextShapeRefresh = {};
    }
    queueShapeRemoval(generation);
}

void EntityDebugOverlay::requestBehaviorSnapshot(std::uint64_t runtimeId) {
    auto const now = std::chrono::steady_clock::now();
    {
        std::scoped_lock lock(mBehaviorRefreshMutex);
        if (now < mNextBehaviorRefresh) {
            return;
        }
        mNextBehaviorRefresh = now + kBehaviorRefreshInterval;
    }

    auto const generation = mSelectionGeneration.load();
    if (!ll::service::getServerInstance()) {
        {
            std::scoped_lock lock(mBehaviorRefreshMutex);
            if (mSelectionGeneration.load() != generation || mSelectedRuntimeId.load() != runtimeId) {
                return;
            }
            mNextBehaviorRefresh = std::chrono::steady_clock::time_point::max();
        }
        std::scoped_lock lock(mBehaviorSnapshotMutex);
        if (mSelectionGeneration.load() != generation || mSelectedRuntimeId.load() != runtimeId) {
            return;
        }
        mBehaviorSnapshot = BehaviorSnapshot{runtimeId, true, false, {}, {}, {}};
        return;
    }

    ll::thread::ServerThreadExecutor::getDefault().execute([this, runtimeId, generation] {
        BehaviorSnapshot snapshot;
        snapshot.runtimeId = runtimeId;
        snapshot.ready     = true;

        auto level = ll::service::getLevel();
        if (level) {
            auto* target = level->getRuntimeEntity(ActorRuntimeID{runtimeId}, false);
            if (target) {
                auto component = target->getEntityContext().tryGetComponent<BehaviorComponent>();
                if (component) {
                    snapshot.available = true;

                    auto const* tree = component->mTreeDefinition->mPtr;
                    if (tree) {
                        snapshot.treeName = tree->mTreeName.get();
                        collectBehaviorNames(tree->mRoot.get(), snapshot.names);
                    }

                    if (auto const* active = getRunningLeaf(component->mRoot.get());
                        active && active->mNodeDefinition) {
                        snapshot.activeName = active->mNodeDefinition->mName.get();
                    }
                }
            }
        }

        std::scoped_lock lock(mBehaviorSnapshotMutex);
        if (mSelectionGeneration.load() != generation || mSelectedRuntimeId.load() != runtimeId) {
            return;
        }
        mBehaviorSnapshot = std::move(snapshot);
    });
}

void EntityDebugOverlay::queueShapeUpdate(std::uint64_t runtimeId, std::uint64_t generation) {
    auto const now = std::chrono::steady_clock::now();
    {
        std::scoped_lock lock(mShapeRefreshMutex);
        if (now < mNextShapeRefresh) {
            return;
        }
        mNextShapeRefresh = now + kShapeRefreshInterval;
    }

    auto& logger = CreatorTools::getInstance().getSelf().getLogger();
    if (!ll::service::getServerInstance()) {
        if (mLastShapeWarningGeneration.exchange(generation) != generation) {
            logger.warn("Entity debug world-space shapes require a local world with an integrated server");
        }
        return;
    }

    ll::thread::ServerThreadExecutor::getDefault().execute([this, runtimeId, generation] {
        if (mSelectionGeneration.load() != generation || mSelectedRuntimeId.load() != runtimeId) {
            return;
        }

        auto level = ll::service::getLevel();
        auto* target = level ? level->getRuntimeEntity(ActorRuntimeID{runtimeId}, false) : nullptr;
        if (!target) {
            if (mLastShapeWarningGeneration.exchange(generation) != generation) {
                CreatorTools::getInstance().getSelf().getLogger().warn(
                    "Entity debug: entity {} is not present in the local server level",
                    runtimeId
                );
            }
            sendShapes(makeRemovalShapes());
            mActiveTextShapeCount = 0;
            return;
        }

        std::vector<DebugTextLine> lines;
        lines.emplace_back(fmt::format("Health: {}/{}", target->getHealth(), target->getMaxHealth()), false);

        auto const position = target->getPosition();
        auto const velocity = target->getVelocity();
        lines.emplace_back(
            fmt::format("Position: {:.1f}, {:.1f}, {:.1f}", position.x, position.y, position.z),
            false
        );
        lines.emplace_back(
            fmt::format("Velocity: {:.1f}, {:.1f}, {:.1f}", velocity.x, velocity.y, velocity.z),
            false
        );

        BehaviorSnapshot snapshot;
        {
            std::scoped_lock lock(mBehaviorSnapshotMutex);
            if (mBehaviorSnapshot.runtimeId == runtimeId) {
                snapshot = mBehaviorSnapshot;
            }
        }

        if (!snapshot.ready) {
            lines.emplace_back("Behaviors: checking...", false);
        } else if (!snapshot.available) {
            lines.emplace_back("Behaviors: unavailable", false);
        } else {
            lines.emplace_back("Behaviors:", false);
            if (!snapshot.treeName.empty()) {
                lines.emplace_back("Behavior tree: " + snapshot.treeName, false);
            }
            auto const visibleCount = std::min(snapshot.names.size(), kMaxDisplayedBehaviors);
            for (std::size_t i = 0; i < visibleCount; ++i) {
                auto const& name = snapshot.names[i];
                lines.emplace_back(name, !snapshot.activeName.empty() && name == snapshot.activeName);
            }
            if (snapshot.names.size() > visibleCount) {
                lines.emplace_back(fmt::format("... and {} more", snapshot.names.size() - visibleCount), false);
            }
            if (snapshot.names.empty() && snapshot.activeName.empty()) {
                lines.emplace_back("No behavior entries", false);
            }
            if (!snapshot.activeName.empty()
                && std::none_of(snapshot.names.begin(), snapshot.names.end(), [&snapshot](std::string const& name) {
                       return name == snapshot.activeName;
                   })) {
                lines.emplace_back(snapshot.activeName, true);
            }
        }

        if (lines.size() > kMaxDebugTextLines) {
            lines.resize(kMaxDebugTextLines);
        }

        auto const& bounds = target->getAABB();
        auto const dimension = target->getDimensionId();
        auto const entityPosition = target->getPosition();
        auto const attachedTo = target->getOrCreateUniqueID();
        auto const centerX = (bounds.min.x + bounds.max.x) * 0.5f;
        auto const centerY = (bounds.min.y + bounds.max.y) * 0.5f;
        auto const centerZ = (bounds.min.z + bounds.max.z) * 0.5f;

        std::vector<PrimitiveShapeDataPayload> shapes;
        shapes.reserve(lines.size() + mActiveTextShapeCount + 1);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            auto const height = bounds.max.y + 0.2f + static_cast<float>(lines.size() - i - 1) * 0.24f;
            auto const color = lines[i].second ? mce::Color{0.2f, 1.0f, 0.2f, 1.0f}
                                               : mce::Color{1.0f, 1.0f, 1.0f, 1.0f};
            shapes.push_back(makeTextShape(
                kShapeIdBase + i,
                Vec3{
                    centerX - entityPosition.x,
                    height - entityPosition.y,
                    centerZ - entityPosition.z
                },
                std::move(lines[i].first),
                color,
                dimension,
                attachedTo
            ));
        }
        for (std::size_t i = lines.size(); i < mActiveTextShapeCount; ++i) {
            shapes.push_back(makeRemovedShape(kShapeIdBase + i));
        }
        shapes.push_back(makeBoxShape(
            kBoxShapeId,
            bounds,
            Vec3{centerX - entityPosition.x, centerY - entityPosition.y, centerZ - entityPosition.z},
            dimension,
            attachedTo
        ));
        sendShapes(std::move(shapes));
        mActiveTextShapeCount = lines.size();
    });
}

void EntityDebugOverlay::queueShapeRemoval(std::uint64_t generation) {
    if (!ll::service::getServerInstance()) {
        return;
    }

    ll::thread::ServerThreadExecutor::getDefault().execute([this, generation] {
        if (mSelectionGeneration.load() != generation || mSelectedRuntimeId.load() != 0) {
            return;
        }
        sendShapes(makeRemovalShapes());
        mActiveTextShapeCount = 0;
    });
}

} // namespace creator_tools
