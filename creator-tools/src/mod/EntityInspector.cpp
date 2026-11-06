#include "mod/EntityInspector.h"
#include "mod/Config.h"
#include "mod/CreatorTools.h"
#include "mod/DebugShapes.h"
#include "mod/GameplayState.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ServerThreadExecutor.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/renderer/actor/ActorRenderDispatcher.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/minecraft_renderer/objects/NameTagRenderObject.h"
#include "mc/deps/input/MouseAction.h"
#include "mc/entity/components/BehaviorComponent.h"
#include "mc/entity/components/GoalSelectorComponent.h"
#include "mc/entity/components/PrioritizedGoal.h"
#include "mc/entity/components_json_legacy/NavigationComponent.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/network/packet/BoxDataPayload.h"
#include "mc/network/packet/PrimitiveShapeDataPayload.h"
#include "mc/network/packet/PrimitiveShapesPacket.h"
#include "mc/network/packet/TextDataPayload.h"
#include "mc/scripting/modules/minecraft/primitiveshapes/ScriptPrimitiveShapeType.h"
#include "mc/util/NamedMolangScript.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinition.h"
#include "mc/world/actor/ai/goal/BaseGoal.h"
#include "mc/world/actor/ai/goal/GoalId.h"
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
#include "mc/world/actor/state/PropertyComponent.h"
#include "mc/world/actor/state/PropertyContainer.h"
#include "mc/world/actor/state/PropertyGroup.h"
#include "mc/world/actor/state/PropertyMetadata.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/pathfinder/Path.h"
#include "mc/world/phys/AABB.h"
#include "mc/world/phys/HitResult.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fmt/format.h>
#include <optional>
#include <string_view>
#include <typeinfo>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <Windows.h>

namespace creator_tools {

namespace {

constexpr auto        kBehaviorRefreshInterval = std::chrono::milliseconds{250};
constexpr auto        kShapeRefreshInterval    = std::chrono::milliseconds{100};
constexpr auto        kShapeResendInterval     = std::chrono::seconds{2}; // re-send even if unchanged, to self-heal
constexpr std::size_t kMaxCollectedBehaviors   = 64;
constexpr std::size_t kMaxDebugTextLines       = debug_shapes::kMaxTextLines;

std::atomic<std::uint64_t> gHiddenNameTagRuntimeId{0};

Config::EntityDebug const& cfg() { return CreatorTools::getInstance().getConfig().entityDebug; }

mce::Color toColor(std::array<float, 4> const& color) {
    return mce::Color{color[0], color[1], color[2], color[3]};
}

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

/// Name shown for a goal: the goal's own name if it has one, else its C++ class name.
std::string goalDisplayName(BaseGoal const& goal) {
    auto const& name = goal.mName.get();
    if (!name.empty()) {
        return name;
    }
    std::string_view type = typeid(goal).name();
    for (std::string_view prefix : {"class ", "struct "}) {
        if (type.starts_with(prefix)) {
            type.remove_prefix(prefix.size());
        }
    }
    return std::string{type};
}

/// Lists the entity's goal-selector goals (the `minecraft:behavior.*` components) as
/// "[priority] name", lowest priority number first. `mUsed` is the goal selector's own "running" flag.
void collectGoalEntries(GoalSelectorComponent const& selector, std::vector<BehaviorEntry>& entries) {
    std::vector<BehaviorEntry> goals;
    std::vector<int>           priorities;
    for (auto const& element : selector.mGoalMap.get()) {
        auto const& prioritized = element.second;
        auto const* goal        = prioritized.mGoal.get();
        if (!goal) {
            continue;
        }
        auto const priority = prioritized.mPriority;
        goals.push_back(BehaviorEntry{fmt::format("[{}] {}", priority, goalDisplayName(*goal)), 0, prioritized.mUsed});
        priorities.push_back(priority);
    }

    std::vector<std::size_t> order(goals.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return priorities[a] < priorities[b];
    });
    for (auto index : order) {
        if (entries.size() >= kMaxCollectedBehaviors) {
            break;
        }
        entries.push_back(std::move(goals[index]));
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

/// The part of an entity's path that is still ahead of it, as world positions.
struct PathPoints {
    std::vector<Vec3> points;      // [0] = the entity's feet, then each remaining node
    bool              truncated{}; // the path was longer than kMaxPathSegments, so `points.back()` isn't the end
};

/// Reads the entity's current navigation path (server thread). nullopt means it has no active path.
std::optional<PathPoints> collectPathPoints(Actor& entity, AABB const& bounds) {
    auto navigation = entity.getEntityContext().tryGetComponent<NavigationComponent>();
    if (!navigation) {
        return std::nullopt;
    }
    Path const* path = navigation->mPath.get();
    if (!path) {
        return std::nullopt;
    }

    auto const& nodes = path->mNodes.get();
    auto const  index = static_cast<std::size_t>(path->mIndex); // the node the entity is heading to
    if (index >= nodes.size()) {
        return std::nullopt;
    }

    PathPoints out;
    out.points.reserve(debug_shapes::kMaxPathSegments + 1);
    out.points.push_back(
        Vec3{
            (bounds.min.x + bounds.max.x) * 0.5f,
            bounds.min.y + cfg().pathHeightOffset,
            (bounds.min.z + bounds.max.z) * 0.5f
        }
    );
    auto const end = std::min(nodes.size(), index + debug_shapes::kMaxPathSegments);
    for (std::size_t i = index; i < end; ++i) {
        auto const& pos = nodes[i].pos.get();
        out.points.push_back(
            Vec3{
                static_cast<float>(pos.x) + 0.5f,
                static_cast<float>(pos.y) + cfg().pathHeightOffset,
                static_cast<float>(pos.z) + 0.5f
            }
        );
    }
    out.truncated = end < nodes.size();
    return out;
}

/// Current values of the entity's `minecraft:entity` properties, as (name, value) text pairs.
std::vector<std::pair<std::string, std::string>> collectEntityProperties(Actor& actor) {
    std::vector<std::pair<std::string, std::string>> out;

    auto props = actor.getEntityContext().tryGetComponent<PropertyComponent>();
    if (!props) {
        return out;
    }

    auto const& group     = *props->mPropertyGroup.get();
    auto const& container = props->mPropertyContainer.get();

    for (auto const& meta : group.mPropertyMetadata.get()) {
        auto const& name  = meta.mName.get();
        auto const  value = container.getValueVariant(name.getHash());

        std::string text = "?";
        if (value) {
            text = std::visit(
                [](auto const& v) -> std::string {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::string>) {
                        return v;
                    } else if constexpr (std::is_same_v<T, bool>) {
                        return v ? "true" : "false";
                    } else if constexpr (std::is_same_v<T, float>) {
                        return fmt::format("{:.2f}", v);
                    } else {
                        return fmt::format("{}", v);
                    }
                },
                *value
            );
        }
        out.emplace_back(name.getString(), std::move(text));
    }
    return out;
}

} // namespace

LL_AUTO_TYPE_INSTANCE_HOOK(
    HideSelectedNameTagHook,
    ll::memory::HookPriority::Normal,
    ActorRenderDispatcher,
    &ActorRenderDispatcher::extractRenderTextObjects,
    std::vector<NameTagRenderObject>,
    Font&                font,
    Tessellator&         tessellator,
    Actor&               actor,
    std::string const&   str,
    Vec3                  camTargetPos,
    float                 a,
    mce::Color            color
) {
    auto const hidden = gHiddenNameTagRuntimeId.load(std::memory_order_relaxed);
    if (cfg().hideNameTag && hidden != 0 && static_cast<std::uint64_t>(actor.getRuntimeID()) == hidden) {
        return {};
    }
    return origin(font, tessellator, actor, str, camTargetPos, a, color);
}

bool EntityInspector::enable() {
    mAlive->store(true);

    mMouseListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::MouseInputEvent>(
        [this](ll::event::MouseInputEvent& event) {
            if (event.actionButtonId() != MouseAction::ActionRight || event.buttonData() != MouseAction::DataDown) {
                return;
            }
            auto const isDown = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
            auto const& config = cfg();
            if (isDown(VK_CONTROL) != config.inspectCtrl || isDown(VK_SHIFT) != config.inspectShift
                || isDown(VK_MENU) != config.inspectAlt) {
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

            // Swallow the click so Ctrl+Shift+right-click doesn't also interact with the entity.
            event.cancel();

            auto const runtimeId = static_cast<std::uint64_t>(target->getRuntimeID());
            auto const selected  = mSelectedRuntimeId.load();
            auto const next      = selected == runtimeId ? 0 : runtimeId;
            mSelectedRuntimeId.store(next);
            gHiddenNameTagRuntimeId.store(next, std::memory_order_relaxed);
            auto const generation = mSelectionGeneration.fetch_add(1) + 1;
            resetSelectionState();
            if (next == 0) {
                queueShapeRemoval(generation);
            } else if (selected != 0 && ll::service::getServerInstance()) {
                // Switching from one mob to another: only one mob may be labelled at a time, so clear the
                // previous mob's shapes first. The next update (a later frame) then draws the new one.
                ll::thread::ServerThreadExecutor::getDefault().execute([this, alive = mAlive, generation] {
                    if (!alive->load() || mSelectionGeneration.load() != generation) {
                        return;
                    }
                    clearShapesOnServer();
                });
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

void EntityInspector::disable() {
    mMouseListener.reset();
    mRenderListener.reset();
    clearSelection(); // queues removal of the on-screen shapes
    mAlive->store(false);
}

void EntityInspector::clearSelection() {
    gHiddenNameTagRuntimeId.store(0, std::memory_order_relaxed);
    if (mSelectedRuntimeId.exchange(0) == 0) {
        return;
    }

    auto const generation = mSelectionGeneration.fetch_add(1) + 1;
    resetSelectionState();
    queueShapeRemoval(generation);
}

void EntityInspector::resetSelectionState() {
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

void EntityInspector::requestBehaviorSnapshot(std::uint64_t runtimeId, std::int64_t uniqueId) {
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

                // Classic mobs: the goal selector holds the minecraft:behavior.* goals with their priorities.
                if (auto goalSelector = target->getEntityContext().tryGetComponent<GoalSelectorComponent>()) {
                    collectGoalEntries(*goalSelector, snapshot.entries);
                    if (!snapshot.entries.empty()) {
                        snapshot.available             = true;
                        snapshot.runtimeStateAvailable = true;
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

void EntityInspector::queueShapeUpdate(std::uint64_t runtimeId, std::uint64_t generation) {
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
            clearShapesOnServer();
            return;
        }

        updateInfoShapes(target, runtimeId, generation, localPlayerId);
        if (cfg().showPath) {
            updatePathShapes(*target, generation, localPlayerId);
        }
    });
}

/// Label (health, position, behaviors) and collision box. Both are attached to the entity, so they follow it
/// on their own and only need re-sending when their content changes.
void EntityInspector::updateInfoShapes(
    Actor*        target,
    std::uint64_t runtimeId,
    std::uint64_t generation,
    std::int64_t  localPlayerId
) {
    std::vector<DebugLine> lines;
    auto                   addLine = [&lines](std::string text, bool highlighted = false) {
        lines.push_back(DebugLine{std::move(text), highlighted});
    };

    addLine(fmt::format("Health: {}/{}", target->getHealth(), target->getMaxHealth()));

    auto const position = target->getPosition();
    addLine(fmt::format("Position: {:.1f}, {:.1f}, {:.1f}", position.x, position.y, position.z));

    auto const properties = collectEntityProperties(*target);
    if (!properties.empty()) {
        addLine("Properties:");
        for (auto const& [name, value] : properties) {
            addLine(fmt::format("  {}: {}", name, value));
        }
    }

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
        addLine("No behaviors");
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

        auto const visibleCount = std::min(snapshot.entries.size(), static_cast<std::size_t>(cfg().maxBehaviors));
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
    auto const scale = cfg().labelScale;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        auto const height = bounds.max.y + 0.2f + static_cast<float>(lines.size() - i - 1) * (0.24f * scale);
        auto const color =
            lines[i].highlighted ? mce::Color{0.2f, 1.0f, 0.2f, 1.0f} : mce::Color{1.0f, 1.0f, 1.0f, 1.0f};
        shapes.push_back(
            debug_shapes::makeTextShape(
                debug_shapes::kTextBase + i,
                Vec3{centerX - entityPosition.x, height - entityPosition.y, centerZ - entityPosition.z},
                std::move(lines[i].text),
                color,
                dimension,
                attachedTo,
                scale
            )
        );
    }
    for (std::size_t i = lines.size(); i < mActiveTextShapeCount; ++i) {
        shapes.push_back(debug_shapes::makeRemovedShape(debug_shapes::kTextBase + i));
    }
    shapes.push_back(
        debug_shapes::makeBoxShape(
            debug_shapes::kBox,
            bounds,
            Vec3{centerX - entityPosition.x, centerY - entityPosition.y, centerZ - entityPosition.z},
            dimension,
            attachedTo
        )
    );

    if (!debug_shapes::send(std::move(shapes), localPlayerId)) {
        return; // local player not available yet; try again on the next refresh
    }
    mActiveTextShapeCount = sentLines.size();
    mLastSent             = SentState{generation, std::move(sentLines), boundsKey, sendTime};
}

/// The entity's current navigation path: a line along the nodes still ahead of it, a marker on the node it is
/// walking to now, and a larger marker on the final node. These are world-space shapes (not attached to the
/// entity), so they are re-sent whenever the entity moves or the path changes.
void EntityInspector::updatePathShapes(Actor& target, std::uint64_t generation, std::int64_t localPlayerId) {
    auto const now    = std::chrono::steady_clock::now();
    auto const bounds = target.getAABB();
    auto const path   = collectPathPoints(target, bounds);

    if (!path) {
        // No active path: take down whatever was drawn for the previous one.
        if (mActivePathSegmentCount != 0 || mTargetMarkerActive || mDestinationMarkerActive) {
            std::vector<PrimitiveShapeDataPayload> batch;
            debug_shapes::appendRemovedPathShapes(
                batch,
                0,
                mActivePathSegmentCount,
                mTargetMarkerActive,
                mDestinationMarkerActive
            );
            if (!debug_shapes::send(std::move(batch), localPlayerId)) {
                return; // try again on the next refresh
            }
        }
        resetPathState();
        return;
    }

    auto const&                       points = path->points;
    std::vector<std::array<float, 3>> key;
    key.reserve(points.size());
    for (auto const& point : points) {
        key.push_back({point.x, point.y, point.z});
    }
    if (mLastPathSent.generation == generation && mLastPathSent.points == key
        && mLastPathSent.truncated == path->truncated && now - mLastPathSent.sentAt < kShapeResendInterval) {
        return;
    }

    auto const dimension    = target.getDimensionId();
    auto const segmentCount = points.size() - 1;

    std::vector<PrimitiveShapeDataPayload> batch;
    batch.reserve(segmentCount + mActivePathSegmentCount + 2);
    for (std::size_t i = 0; i < segmentCount; ++i) {
        batch.push_back(
            debug_shapes::makeLineShape(
                debug_shapes::kPathLineBase + i,
                points[i],
                points[i + 1],
                toColor(cfg().pathLineColor),
                dimension
            )
        );
    }
    // The path got shorter: remove the segments that no longer exist.
    debug_shapes::appendRemovedPathShapes(batch, segmentCount, mActivePathSegmentCount, false, false);

    // points[1] is the node being walked to. When it is also the last node, only the destination marker is shown.
    bool const showTarget      = points.size() > 2;
    bool const showDestination = !path->truncated;
    if (showTarget) {
        batch.push_back(
            debug_shapes::makeSphereShape(
                debug_shapes::kPathTarget,
                points[1],
                cfg().pathTargetRadius,
                toColor(cfg().pathTargetColor),
                dimension
            )
        );
    } else if (mTargetMarkerActive) {
        batch.push_back(debug_shapes::makeRemovedShape(debug_shapes::kPathTarget));
    }
    if (showDestination) {
        batch.push_back(
            debug_shapes::makeSphereShape(
                debug_shapes::kPathDestination,
                points.back(),
                cfg().pathDestRadius,
                toColor(cfg().pathDestColor),
                dimension
            )
        );
    } else if (mDestinationMarkerActive) {
        batch.push_back(debug_shapes::makeRemovedShape(debug_shapes::kPathDestination));
    }

    if (!debug_shapes::send(std::move(batch), localPlayerId)) {
        return; // local player not available yet; try again on the next refresh
    }
    mActivePathSegmentCount  = segmentCount;
    mTargetMarkerActive      = showTarget;
    mDestinationMarkerActive = showDestination;
    mLastPathSent            = SentPathState{generation, std::move(key), path->truncated, now};
}

void EntityInspector::resetPathState() {
    mActivePathSegmentCount  = 0;
    mTargetMarkerActive      = false;
    mDestinationMarkerActive = false;
    mLastPathSent            = {};
}

/// Removes every shape this mod draws and forgets what was sent (server thread).
void EntityInspector::clearShapesOnServer() {
    debug_shapes::send(debug_shapes::makeRemovalOfAll(), mLocalPlayerUniqueId.load());
    mActiveTextShapeCount = 0;
    mLastSent             = {};
    resetPathState();
}

void EntityInspector::queueShapeRemoval(std::uint64_t generation) {
    if (!ll::service::getServerInstance()) {
        return;
    }

    // Deliberately not gated on mAlive: disable() relies on this to remove the shapes from the screen.
    ll::thread::ServerThreadExecutor::getDefault().execute([this, generation] {
        if (mSelectionGeneration.load() != generation || mSelectedRuntimeId.load() != 0) {
            return;
        }
        clearShapesOnServer();
    });
}

} // namespace creator_tools
