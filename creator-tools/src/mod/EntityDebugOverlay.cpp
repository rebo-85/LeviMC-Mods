#include "mod/EntityDebugOverlay.h"
#include "mod/CreatorTools.h"
#include "mod/GameplayState.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ServerThreadExecutor.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/CaretMeasureData.h"
#include "mc/client/gui/TextMeasureData.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/input/MouseAction.h"
#include "mc/deps/input/RectangleArea.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/entity/components/BehaviorComponent.h"
#include "mc/legacy/ActorRuntimeID.h"
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
#include <array>
#include <cmath>
#include <fmt/format.h>
#include <glm/matrix.hpp>
#include <utility>

#include <Windows.h>

namespace creator_tools {

namespace {

constexpr auto        kBehaviorRefreshInterval = std::chrono::milliseconds{250};
constexpr std::size_t kMaxDisplayedBehaviors   = 8;

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

bool projectToScreen(
    MinecraftUIRenderContext& context,
    mce::Camera const&        camera,
    Vec3 const&               cameraPosition,
    Vec3 const&               position,
    float&                    screenX,
    float&                    screenY,
    std::string&              failure,
    glm::vec4&                cameraSpace,
    glm::vec4&                clip,
    glm::vec2&                ndc
) {
    auto const offset =
        glm::vec3{position.x, position.y, position.z} - glm::vec3{cameraPosition.x, cameraPosition.y, cameraPosition.z};
    auto const depth = glm::dot(offset, camera.mForward.get());
    if (depth <= 0.001f) {
        failure = "behind-camera";
        return false;
    }

    auto const horizontal               = glm::dot(offset, camera.mRight.get());
    auto const vertical                 = glm::dot(offset, camera.mUp.get());
    cameraSpace                         = glm::vec4{horizontal, vertical, -depth, 1.0f};
    constexpr float kFallbackFovDegrees = 70.0f;
    auto const      configuredFov       = static_cast<float>(camera.mFov);
    auto const      fovRadians     = configuredFov > 0.001f
                                       ? (configuredFov > 3.14159265f ? glm::radians(configuredFov) : configuredFov)
                                       : glm::radians(kFallbackFovDegrees);
    auto const      tanHalfFov     = std::tan(fovRadians * 0.5f);
    auto const      bounds         = context.getFullClippingRectangle();
    auto const      viewportWidth  = static_cast<float>(bounds._x1 - bounds._x0);
    auto const      viewportHeight = static_cast<float>(bounds._y1 - bounds._y0);
    if (!std::isfinite(tanHalfFov) || tanHalfFov <= 0.001f || viewportHeight <= 0.0f || viewportWidth <= 0.0f) {
        failure = "invalid-projection-parameters";
        return false;
    }

    auto const aspect = viewportWidth / viewportHeight;
    ndc               = {horizontal / (depth * tanHalfFov * aspect), vertical / (depth * tanHalfFov)};
    clip              = glm::vec4{ndc.x, ndc.y, 0.0f, 1.0f};
    if (std::abs(ndc.x) > 1.0f || std::abs(ndc.y) > 1.0f) {
        failure = "outside-viewport";
        return false;
    }

    screenX = bounds._x0 + (ndc.x + 1.0f) * 0.5f * (bounds._x1 - bounds._x0);
    screenY = bounds._y0 + (1.0f - ndc.y) * 0.5f * (bounds._y1 - bounds._y0);
    failure.clear();
    return true;
}

bool projectToScreen(
    MinecraftUIRenderContext& context,
    mce::Camera const&        camera,
    Vec3 const&               cameraPosition,
    Vec3 const&               position,
    float&                    screenX,
    float&                    screenY
) {
    std::string failure;
    glm::vec4   cameraSpace{};
    glm::vec4   clip{};
    glm::vec2   ndc{};
    return projectToScreen(
        context,
        camera,
        cameraPosition,
        position,
        screenX,
        screenY,
        failure,
        cameraSpace,
        clip,
        ndc
    );
}

void drawClippedLine(
    MinecraftUIRenderContext& context,
    RectangleArea const&      bounds,
    glm::vec2                 start,
    glm::vec2                 end,
    mce::Color const&         color
) {
    auto const delta  = end - start;
    auto const length = glm::length(delta);
    if (length <= 0.001f) {
        return;
    }

    auto const minX = static_cast<float>(bounds._x0);
    auto const maxX = static_cast<float>(bounds._x1);
    auto const minY = static_cast<float>(bounds._y0);
    auto const maxY = static_cast<float>(bounds._y1);
    float      t0   = 0.0f;
    float      t1   = 1.0f;
    auto const clip = [&t0, &t1](float p, float q) {
        if (std::abs(p) <= 0.001f) {
            return q >= 0.0f;
        }
        auto const ratio = q / p;
        if (p < 0.0f) {
            t0 = std::max(t0, ratio);
        } else {
            t1 = std::min(t1, ratio);
        }
        return t0 <= t1;
    };
    if (!clip(-delta.x, start.x - minX) || !clip(delta.x, maxX - start.x) || !clip(-delta.y, start.y - minY)
        || !clip(delta.y, maxY - start.y)) {
        return;
    }

    auto const clippedStart  = start + delta * t0;
    auto const clippedEnd    = start + delta * t1;
    auto const clippedDelta  = clippedEnd - clippedStart;
    auto const clippedLength = glm::length(clippedDelta);
    if (clippedLength <= 0.001f) {
        return;
    }
    auto const steps = std::clamp(static_cast<int>(std::ceil(clippedLength / 3.0f)), 1, 128);
    for (int i = 0; i <= steps; ++i) {
        auto const point = clippedStart + clippedDelta * (static_cast<float>(i) / static_cast<float>(steps));
        context
            .fillRectangle(RectangleArea{point.x - 1.5f, point.x + 1.5f, point.y - 1.5f, point.y + 1.5f}, color, 1.0f);
    }
}

void drawCollisionBox(
    MinecraftUIRenderContext& context,
    mce::Camera const&        camera,
    Vec3 const&               cameraPosition,
    AABB const&               box
) {
    std::array<Vec3, 8> corners{
        Vec3{box.min.x, box.min.y, box.min.z},
        Vec3{box.max.x, box.min.y, box.min.z},
        Vec3{box.min.x, box.max.y, box.min.z},
        Vec3{box.max.x, box.max.y, box.min.z},
        Vec3{box.min.x, box.min.y, box.max.z},
        Vec3{box.max.x, box.min.y, box.max.z},
        Vec3{box.min.x, box.max.y, box.max.z},
        Vec3{box.max.x, box.max.y, box.max.z}
    };
    std::array<glm::vec2, 8> projected{};
    std::array<bool, 8>      visible{};
    for (std::size_t i = 0; i < corners.size(); ++i) {
        float x = 0.0f;
        float y = 0.0f;
        if (projectToScreen(context, camera, cameraPosition, corners[i], x, y)) {
            projected[i] = {x, y};
            visible[i]   = true;
        }
    }

    constexpr std::array<std::array<std::size_t, 2>, 12> edges{
        {{{0, 1}},
         {{0, 2}},
         {{0, 4}},
         {{1, 3}},
         {{1, 5}},
         {{2, 3}},
         {{2, 6}},
         {{3, 7}},
         {{4, 5}},
         {{4, 6}},
         {{5, 7}},
         {{6, 7}}}
    };
    auto const bounds = context.getFullClippingRectangle();
    auto const color  = mce::Color{0.2f, 0.9f, 1.0f, 1.0f};
    for (auto const& edge : edges) {
        if (visible[edge[0]] && visible[edge[1]]) {
            drawClippedLine(context, bounds, projected[edge[0]], projected[edge[1]], color);
        }
    }
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
            mSelectionGeneration.fetch_add(1);
            {
                std::scoped_lock lock(mBehaviorRefreshMutex);
                mNextBehaviorRefresh = {};
            }
            {
                std::scoped_lock lock(mBehaviorSnapshotMutex);
                mBehaviorSnapshot = {};
            }
            CreatorTools::getInstance().getSelf().getLogger().info(
                "Entity debug label {} for runtime ID {}",
                next == 0 ? "hidden" : "enabled",
                runtimeId
            );
        }
    );

    mRenderListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::BeforeUIRenderEvent>(
        [this](ll::event::BeforeUIRenderEvent& event) {
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
                logger.info("Entity debug renderer active for runtime ID {}", runtimeId);
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

            auto const  frameAlpha           = client->getFrameAlpha();
            auto const& box                  = target->getAABB();
            auto const  targetRenderPosition = target->getInterpolatedPosition(frameAlpha);
            auto const  targetOffset         = targetRenderPosition - target->getPosition();
            AABB const  renderBox{box.min + targetOffset, box.max + targetOffset};
            Vec3        labelPosition{
                (renderBox.min.x + renderBox.max.x) * 0.5f,
                renderBox.max.y + 0.2f,
                (renderBox.min.z + renderBox.max.z) * 0.5f
            };
            auto* localPlayer = client->getLocalPlayer();
            if (!localPlayer) {
                logger.warn("Entity debug: local player is unavailable for projection");
                return;
            }
            auto const cameraPosition = localPlayer->getInterpolatedPosition(frameAlpha)
                                      + (localPlayer->getEyePos() - localPlayer->getPosition());

            float       screenX = 0.0f;
            float       screenY = 0.0f;
            std::string projectionFailure;
            glm::vec4   cameraSpace{};
            glm::vec4   clip{};
            glm::vec2   ndc{};
            auto&       context = event.uiRenderContext();
            auto const& camera  = client->getCamera();
            drawCollisionBox(context, camera, cameraPosition, renderBox);
            auto const projected = projectToScreen(
                context,
                camera,
                cameraPosition,
                labelPosition,
                screenX,
                screenY,
                projectionFailure,
                cameraSpace,
                clip,
                ndc
            );
            auto const now = std::chrono::steady_clock::now();
            if (now >= mNextProjectionDiagnostic) {
                auto const bounds = context.getFullClippingRectangle();
                logger.info(
                    "Entity debug projection id={} result={} anchor={} cameraPos=({:.2f},{:.2f},{:.2f}) "
                    "forward=({:.2f},{:.2f},{:.2f}) right=({:.2f},{:.2f},{:.2f}) up=({:.2f},{:.2f},{:.2f}) "
                    "cameraSpace=({:.2f},{:.2f},{:.2f},{:.2f}) fov={:.3f} fovFallback={} aspect={:.3f} "
                    "ndc=({:.3f},{:.3f}) uiBounds=({:.1f},{:.1f})-({:.1f},{:.1f}) screen=({:.1f},{:.1f})",
                    runtimeId,
                    projected ? "ok" : projectionFailure,
                    labelPosition.toString(),
                    cameraPosition.x,
                    cameraPosition.y,
                    cameraPosition.z,
                    camera.mForward->x,
                    camera.mForward->y,
                    camera.mForward->z,
                    camera.mRight->x,
                    camera.mRight->y,
                    camera.mRight->z,
                    camera.mUp->x,
                    camera.mUp->y,
                    camera.mUp->z,
                    cameraSpace.x,
                    cameraSpace.y,
                    cameraSpace.z,
                    cameraSpace.w,
                    static_cast<float>(camera.mFov),
                    static_cast<float>(camera.mFov) <= 0.001f,
                    static_cast<float>(camera.mAspectRatio),
                    ndc.x,
                    ndc.y,
                    static_cast<float>(bounds._x0),
                    static_cast<float>(bounds._y0),
                    static_cast<float>(bounds._x1),
                    static_cast<float>(bounds._y1),
                    screenX,
                    screenY
                );
                mNextProjectionDiagnostic = now + std::chrono::seconds{1};
            }
            if (!projected) {
                if (mLastProjectionWarningGeneration != generation) {
                    logger.warn("Entity debug: could not project entity {} ({})", runtimeId, projectionFailure);
                    mLastProjectionWarningGeneration = generation;
                }
                return;
            }

            std::vector<std::pair<std::string, bool>> lines;
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

            auto const      bounds     = context.getFullClippingRectangle();
            constexpr float lineHeight = 10.0f;
            float           y = std::max(bounds._y0, screenY - lineHeight * static_cast<float>(lines.size()) - 4.0f);
            TextMeasureData const  textData{1.0f, 0.0f, true, false, false, ui::TextAlignment::Center};
            CaretMeasureData const caretData{0, false};

            for (auto& [text, active] : lines) {
                RectangleArea const area{screenX - 160.0f, screenX + 160.0f, y, y + lineHeight};
                auto const color = active ? mce::Color{0.2f, 1.0f, 0.2f, 1.0f} : mce::Color{1.0f, 1.0f, 1.0f, 1.0f};
                context
                    .drawDebugText(area, std::move(text), color, 1.0f, ui::TextAlignment::Center, textData, caretData);
                y += lineHeight;
            }
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
    mSelectedRuntimeId.store(0);
    mSelectionGeneration.fetch_add(1);
    {
        std::scoped_lock lock(mBehaviorRefreshMutex);
        mNextBehaviorRefresh = {};
    }
    std::scoped_lock lock(mBehaviorSnapshotMutex);
    mBehaviorSnapshot = {};
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

} // namespace creator_tools
