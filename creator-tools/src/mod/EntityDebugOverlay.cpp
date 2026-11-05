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
#include "mc/util/NamedMolangScript.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinition.h"
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
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "mc/world/phys/AABB.h"
#include "mc/world/phys/HitResult.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fmt/format.h>
#include <utility>
#include <vector>

#include <Windows.h>

namespace creator_tools {

namespace {

constexpr auto          kBehaviorRefreshInterval = std::chrono::milliseconds{250};
constexpr auto          kShapeRefreshInterval    = std::chrono::milliseconds{100};
constexpr auto          kShapeResendInterval     = std::chrono::seconds{2}; // re-send even if unchanged, to self-heal
constexpr std::size_t   kMaxDisplayedBehaviors   = 8;
constexpr std::size_t   kMaxCollectedBehaviors   = 64;
constexpr std::size_t   kMaxDebugTextLines       = 16;
constexpr std::uint64_t kShapeIdBase             = 0x43544F4F4C530000ULL;
constexpr std::uint64_t kBoxShapeId              = kShapeIdBase + kMaxDebugTextLines;

/// Flattens the definition tree into display entries. Containers (composites/decorators) are listed
/// only if they have a name, and only those increase the indentation depth.
/// `activeDefinition` is the definition of the node that is currently running, matched by identity.
void collectBehaviorEntries(
    BehaviorDefinition const*   definition,
    BehaviorDefinition const*   activeDefinition,
    int                         depth,
    std::vector<BehaviorEntry>& entries
) {
    if (!definition || entries.size() >= kMaxCollectedBehaviors) {
        return;
    }

    auto add = [&](std::string const& name) {
        if (name.empty()) {
            return false;
        }
        entries.push_back(BehaviorEntry{name, depth, activeDefinition && definition == activeDefinition});
        return true;
    };

    if (auto const* composite = dynamic_cast<CompositeDefinition const*>(definition)) {
        int const childDepth = add(definition->mName.get()) ? depth + 1 : depth;
        for (auto const& child : composite->mChildren.get()) {
            collectBehaviorEntries(child.get(), activeDefinition, childDepth, entries);
        }
        return;
    }
    if (auto const* decorator = dynamic_cast<DecoratorDefinition const*>(definition)) {
        int const childDepth = add(definition->mName.get()) ? depth + 1 : depth;
        collectBehaviorEntries(decorator->mChild.get(), activeDefinition, childDepth, entries);
        return;
    }
    if (auto const* subtree = dynamic_cast<SubtreeDefinition const*>(definition)) {
        add(subtree->mSubtreeId.get());
        return;
    }

    add(definition->mName.get());
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

std::string formatEntry(BehaviorEntry const& entry) {
    // ">" markers instead of spaces: the text is centered in the world, so leading spaces would vanish.
    if (entry.depth <= 0) {
        return entry.name;
    }
    return std::string(static_cast<std::size_t>(entry.depth), '>') + " " + entry.name;
}

std::int64_t localPlayerUniqueId(Level& level) {
    auto* player = level.getPrimaryLocalPlayer();
    return player ? player->getOrCreateUniqueID().rawID : 0;
}

PrimitiveShapeDataPayload makeTextShape(
    std::uint64_t id,
    Vec3 const&   locationOffset,
    std::string   text,
    mce::Color    color,
    DimensionType dimension,
    ActorUniqueID attachedTo
) {
    PrimitiveShapeDataPayload shape{};
    shape.mNetworkId         = id;
    shape.mShapeType         = ScriptModuleMinecraft::ScriptPrimitiveShapeType::Text;
    shape.mLocation          = locationOffset;
    shape.mColor             = color;
    shape.mDimensionId       = dimension;
    shape.mAttachedToId      = attachedTo;
    shape.mMaxRenderDistance = 64.0f;

    TextDataPayload textPayload{};
    textPayload.mText        = std::move(text);
    textPayload.mUseRotation = false;
    shape.mExtraDataPayload  = std::move(textPayload);
    return shape;
}

PrimitiveShapeDataPayload makeBoxShape(
    std::uint64_t id,
    AABB const&   bounds,
    Vec3 const&   locationOffset,
    DimensionType dimension,
    ActorUniqueID attachedTo
) {
    PrimitiveShapeDataPayload shape{};
    shape.mNetworkId         = id;
    shape.mShapeType         = ScriptModuleMinecraft::ScriptPrimitiveShapeType::Box;
    shape.mLocation          = locationOffset;
    shape.mColor             = mce::Color{0.2f, 0.9f, 1.0f, 1.0f};
    shape.mDimensionId       = dimension;
    shape.mAttachedToId      = attachedTo;
    shape.mMaxRenderDistance = 64.0f;
    shape.mExtraDataPayload  = BoxDataPayload{
        Vec3{bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y, bounds.max.z - bounds.min.z}
    };
    return shape;
}

PrimitiveShapeDataPayload makeRemovedShape(std::uint64_t id) {
    PrimitiveShapeDataPayload shape{};
    shape.mNetworkId = id;
    return shape;
}

/// Sends the shapes to the local player only. The integrated server may have LAN guests, and
/// broadcasting with sendToClients() would show them our debug labels too.
/// Server thread only. Returns false if the local player's server-side entity can't be found.
///
/// NOTE: relies on Packet::sendTo(Player const&); if your LeviLamina version names it differently,
/// this is the one place to adjust.
bool sendShapes(std::vector<PrimitiveShapeDataPayload> shapes, std::int64_t localPlayerId) {
    if (localPlayerId == 0) {
        return false;
    }
    auto level = ll::service::getLevel();
    if (!level) {
        return false;
    }

    // Look the player up in the server's player list instead of dynamic_cast-ing an Actor: Minecraft's
    // own classes have no usable RTTI, so dynamic_cast on them throws std::__non_rtti_object.
    // (Only the behavior-definition/node classes in this file are safe to dynamic_cast.)
    Player* player = nullptr;
    level->forEachPlayer([&](Player& candidate) {
        if (candidate.getOrCreateUniqueID().rawID == localPlayerId) {
            player = &candidate;
            return false; // stop iterating
        }
        return true;
    });
    if (!player) {
        return false;
    }

    PrimitiveShapesPacket packet{};
    packet.setSerializationMode(SerializationMode::CerealOnly);
    packet.mShapes.get() = std::move(shapes);
    packet.sendTo(*player);
    return true;
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
    mAlive->store(true);

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

            // Swallow the click so Ctrl+right-click doesn't also trade with / mount / interact with the entity.
            event.cancel();

            auto const runtimeId = static_cast<std::uint64_t>(target->getRuntimeID());
            auto const selected  = mSelectedRuntimeId.load();
            auto const next      = selected == runtimeId ? 0 : runtimeId;
            mSelectedRuntimeId.store(next);
            auto const generation = mSelectionGeneration.fetch_add(1) + 1;
            resetSelectionState();
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
            // Runs every frame: bail out first when nothing is selected.
            auto const runtimeId = mSelectedRuntimeId.load();
            if (runtimeId == 0) {
                return;
            }

            auto client = ll::service::getClientInstance();
            if (!client || !client->isWorldActive()) {
                clearSelection();
                return;
            }
            if (!isGameplayActive()) {
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

            mLocalPlayerUniqueId.store(localPlayerUniqueId(*level));
            requestBehaviorSnapshot(runtimeId, target->getOrCreateUniqueID().rawID);
            queueShapeUpdate(runtimeId, generation);
        }
    );

    return true;
}

void EntityDebugOverlay::disable() {
    mMouseListener.reset();
    mRenderListener.reset();
    clearSelection(); // queues removal of the on-screen shapes
    mAlive->store(false);
}

void EntityDebugOverlay::clearSelection() {
    if (mSelectedRuntimeId.exchange(0) == 0) {
        return;
    }

    auto const generation = mSelectionGeneration.fetch_add(1) + 1;
    resetSelectionState();
    queueShapeRemoval(generation);
}

void EntityDebugOverlay::resetSelectionState() {
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
}

void EntityDebugOverlay::requestBehaviorSnapshot(std::uint64_t runtimeId, std::int64_t uniqueId) {
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
        mBehaviorSnapshot = BehaviorSnapshot{runtimeId, true, false, false, {}, {}, {}};
        return;
    }

    ll::thread::ServerThreadExecutor::getDefault().execute([this, alive = mAlive, runtimeId, uniqueId, generation] {
        if (!alive->load()) {
            return;
        }

        BehaviorSnapshot snapshot;
        snapshot.runtimeId = runtimeId;
        snapshot.ready     = true;

        auto level = ll::service::getLevel();
        if (level) {
            auto* target = level->fetchEntity(ActorUniqueID{uniqueId}, false);
            if (target) {
                auto const* actorDefinition = target->mActorDefinitionPtr->mPtr;
                if (actorDefinition) {
                    if (auto const* behaviorDescription = actorDefinition->mDescription->mBehavior) {
                        snapshot.treeName = behaviorDescription->mBehaviorTreeId.get();
                    }
                }

                auto component = target->getEntityContext().tryGetComponent<BehaviorComponent>();
                if (component) {
                    snapshot.available             = true;
                    snapshot.runtimeStateAvailable = true;

                    // Find the running leaf first so the matching entry can be flagged by identity.
                    BehaviorDefinition const* activeDefinition = nullptr;
                    if (auto const* active = getRunningLeaf(component->mRoot.get());
                        active && active->mNodeDefinition) {
                        snapshot.activeName = active->mNodeDefinition->mName.get();
                        activeDefinition    = &*active->mNodeDefinition;
                    }

                    auto const* tree = component->mTreeDefinition->mPtr;
                    if (tree) {
                        snapshot.treeName = tree->mTreeName.get();
                        collectBehaviorEntries(tree->mRoot.get(), activeDefinition, 0, snapshot.entries);
                    }

                    // Safety net: if identity matching found nothing, fall back to matching by name.
                    bool const anyActive =
                        std::any_of(snapshot.entries.begin(), snapshot.entries.end(), [](auto const& e) {
                            return e.active;
                        });
                    if (!anyActive && !snapshot.activeName.empty()) {
                        for (auto& entry : snapshot.entries) {
                            entry.active = entry.name == snapshot.activeName;
                        }
                    }
                } else if (!snapshot.treeName.empty()) {
                    snapshot.available = true;
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

    ll::thread::ServerThreadExecutor::getDefault().execute([this, alive = mAlive, runtimeId, generation] {
        if (!alive->load() || mSelectionGeneration.load() != generation || mSelectedRuntimeId.load() != runtimeId) {
            return;
        }

        auto const localPlayerId = mLocalPlayerUniqueId.load();
        auto       level         = ll::service::getLevel();
        auto*      target        = level ? level->getRuntimeEntity(ActorRuntimeID{runtimeId}, false) : nullptr;
        if (!target) {
            if (mLastShapeWarningGeneration.exchange(generation) != generation) {
                CreatorTools::getInstance().getSelf().getLogger().warn(
                    "Entity debug: entity {} is not present in the local server level",
                    runtimeId
                );
            }
            sendShapes(makeRemovalShapes(), localPlayerId);
            mActiveTextShapeCount = 0;
            mLastSent             = {};
            return;
        }

        std::vector<DebugLine> lines;
        auto                   addLine = [&lines](std::string text, bool highlighted = false) {
            lines.push_back(DebugLine{std::move(text), highlighted});
        };

        addLine(fmt::format("Health: {}/{}", target->getHealth(), target->getMaxHealth()));

        auto const position = target->getPosition();
        addLine(fmt::format("Position: {:.1f}, {:.1f}, {:.1f}", position.x, position.y, position.z));

        BehaviorSnapshot snapshot;
        {
            std::scoped_lock lock(mBehaviorSnapshotMutex);
            if (mBehaviorSnapshot.runtimeId == runtimeId) {
                snapshot = mBehaviorSnapshot;
            }
        }

        if (!snapshot.ready) {
            addLine("Behaviors: checking...");
        } else if (!snapshot.available) {
            addLine("No behavior tree");
        } else {
            addLine("Behaviors:");
            if (!snapshot.treeName.empty()) {
                addLine("Behavior tree: " + snapshot.treeName);
            }

            constexpr auto npos        = std::numeric_limits<std::size_t>::max();
            std::size_t    activeIndex = npos;
            for (std::size_t i = 0; i < snapshot.entries.size(); ++i) {
                if (snapshot.entries[i].active) {
                    activeIndex = i;
                    break;
                }
            }

            auto const visibleCount = std::min(snapshot.entries.size(), kMaxDisplayedBehaviors);
            for (std::size_t i = 0; i < visibleCount; ++i) {
                addLine(formatEntry(snapshot.entries[i]), snapshot.entries[i].active);
            }
            if (snapshot.entries.size() > visibleCount) {
                addLine(fmt::format("... and {} more", snapshot.entries.size() - visibleCount));
                // Don't hide the running behavior just because it is past the display limit.
                if (activeIndex != npos && activeIndex >= visibleCount) {
                    addLine(formatEntry(snapshot.entries[activeIndex]), true);
                }
            }

            if (snapshot.entries.empty() && snapshot.activeName.empty() && snapshot.runtimeStateAvailable) {
                addLine("No behavior entries");
            } else if (!snapshot.runtimeStateAvailable) {
                addLine("Runtime behavior state unavailable");
            }
            if (!snapshot.activeName.empty() && activeIndex == npos) {
                addLine(snapshot.activeName, true);
            }
        }

        if (lines.size() > kMaxDebugTextLines) {
            lines.resize(kMaxDebugTextLines);
        }

        auto const& bounds         = target->getAABB();
        auto const  dimension      = target->getDimensionId();
        auto const  entityPosition = target->getPosition();
        auto const  attachedTo     = target->getOrCreateUniqueID();
        auto const  centerX        = (bounds.min.x + bounds.max.x) * 0.5f;
        auto const  centerY        = (bounds.min.y + bounds.max.y) * 0.5f;
        auto const  centerZ        = (bounds.min.z + bounds.max.z) * 0.5f;

        // The shapes are attached to the entity, so they follow it on their own. Only send when the text
        // or box changed (or every few seconds, to recover if the client lost them).
        std::array<float, 6> const
                   boundsKey{bounds.min.x, bounds.min.y, bounds.min.z, bounds.max.x, bounds.max.y, bounds.max.z};
        auto const sendTime = std::chrono::steady_clock::now();
        if (mLastSent.generation == generation && mLastSent.lines == lines && mLastSent.bounds == boundsKey
            && sendTime - mLastSent.sentAt < kShapeResendInterval) {
            return;
        }

        std::vector<DebugLine> sentLines = lines; // copy for the cache; `lines` is moved from below

        std::vector<PrimitiveShapeDataPayload> shapes;
        shapes.reserve(lines.size() + mActiveTextShapeCount + 1);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            auto const height = bounds.max.y + 0.2f + static_cast<float>(lines.size() - i - 1) * 0.24f;
            auto const color =
                lines[i].highlighted ? mce::Color{0.2f, 1.0f, 0.2f, 1.0f} : mce::Color{1.0f, 1.0f, 1.0f, 1.0f};
            shapes.push_back(makeTextShape(
                kShapeIdBase + i,
                Vec3{centerX - entityPosition.x, height - entityPosition.y, centerZ - entityPosition.z},
                std::move(lines[i].text),
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

        if (!sendShapes(std::move(shapes), localPlayerId)) {
            return; // local player not available yet; try again on the next refresh
        }
        mActiveTextShapeCount = sentLines.size();
        mLastSent             = SentState{generation, std::move(sentLines), boundsKey, sendTime};
    });
}

void EntityDebugOverlay::queueShapeRemoval(std::uint64_t generation) {
    if (!ll::service::getServerInstance()) {
        return;
    }

    // Deliberately not gated on mAlive: disable() relies on this to remove the shapes from the screen.
    ll::thread::ServerThreadExecutor::getDefault().execute([this, generation] {
        if (mSelectionGeneration.load() != generation || mSelectedRuntimeId.load() != 0) {
            return;
        }
        sendShapes(makeRemovalShapes(), mLocalPlayerUniqueId.load());
        mActiveTextShapeCount = 0;
        mLastSent             = {};
    });
}

} // namespace creator_tools
