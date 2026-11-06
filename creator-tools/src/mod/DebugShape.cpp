#include "mod/DebugShapes.h"

#include "ll/api/service/Bedrock.h"

#include "mc/network/packet/BoxDataPayload.h"
#include "mc/network/packet/LineDataPayload.h"
#include "mc/network/packet/PrimitiveShapesPacket.h"
#include "mc/network/packet/SphereDataPayload.h"
#include "mc/network/packet/TextDataPayload.h"
#include "mc/scripting/modules/minecraft/primitiveshapes/ScriptPrimitiveShapeType.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include <utility>

namespace creator_tools::debug_shapes {

namespace {
constexpr float        kMaxRenderDistance = 64.0f;
constexpr std::uint8_t kSphereSegments    = 12;
} // namespace

PrimitiveShapeDataPayload makeTextShape(
    std::uint64_t id,
    Vec3 const&   locationOffset,
    std::string   text,
    mce::Color    color,
    DimensionType dimension,
    ActorUniqueID attachedTo,
    float         scale
) {
    PrimitiveShapeDataPayload shape{};
    shape.mNetworkId         = id;
    shape.mShapeType         = ScriptModuleMinecraft::ScriptPrimitiveShapeType::Text;
    shape.mLocation          = locationOffset;
    shape.mColor             = color;
    shape.mDimensionId       = dimension;
    shape.mAttachedToId      = attachedTo;
    shape.mMaxRenderDistance = kMaxRenderDistance;
    shape.mScale             = scale;

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
    shape.mMaxRenderDistance = kMaxRenderDistance;
    shape.mExtraDataPayload  = BoxDataPayload{
        Vec3{bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y, bounds.max.z - bounds.min.z}
    };
    return shape;
}

PrimitiveShapeDataPayload
makeLineShape(std::uint64_t id, Vec3 const& start, Vec3 const& end, mce::Color color, DimensionType dimension) {
    PrimitiveShapeDataPayload shape{};
    shape.mNetworkId         = id;
    shape.mShapeType         = ScriptModuleMinecraft::ScriptPrimitiveShapeType::Line;
    shape.mLocation          = start; // not attached to anything, so this is a world position
    shape.mColor             = color;
    shape.mDimensionId       = dimension;
    shape.mMaxRenderDistance = kMaxRenderDistance;
    shape.mExtraDataPayload  = LineDataPayload{end};
    return shape;
}

PrimitiveShapeDataPayload
makeSphereShape(std::uint64_t id, Vec3 const& center, float radius, mce::Color color, DimensionType dimension) {
    PrimitiveShapeDataPayload shape{};
    shape.mNetworkId         = id;
    shape.mShapeType         = ScriptModuleMinecraft::ScriptPrimitiveShapeType::Sphere;
    shape.mLocation          = center;
    shape.mScale             = radius;
    shape.mColor             = color;
    shape.mDimensionId       = dimension;
    shape.mMaxRenderDistance = kMaxRenderDistance;

    SphereDataPayload payload{};
    payload.mNumSegments    = kSphereSegments;
    shape.mExtraDataPayload = payload;
    return shape;
}

PrimitiveShapeDataPayload makeRemovedShape(std::uint64_t id) {
    PrimitiveShapeDataPayload shape{};
    shape.mNetworkId = id;
    return shape;
}

void appendRemovedPathShapes(
    std::vector<PrimitiveShapeDataPayload>& out,
    std::size_t                             fromSegment,
    std::size_t                             toSegment,
    bool                                    target,
    bool                                    destination
) {
    for (std::size_t i = fromSegment; i < toSegment; ++i) {
        out.push_back(makeRemovedShape(kPathLineBase + i));
    }
    if (target) {
        out.push_back(makeRemovedShape(kPathTarget));
    }
    if (destination) {
        out.push_back(makeRemovedShape(kPathDestination));
    }
}

std::vector<PrimitiveShapeDataPayload> makeRemovalOfAll() {
    std::vector<PrimitiveShapeDataPayload> shapes;
    shapes.reserve(kMaxTextLines + 1 + kMaxPathSegments + 2);
    for (std::size_t i = 0; i < kMaxTextLines; ++i) {
        shapes.push_back(makeRemovedShape(kTextBase + i));
    }
    shapes.push_back(makeRemovedShape(kBox));
    appendRemovedPathShapes(shapes, 0, kMaxPathSegments, true, true);
    return shapes;
}

bool send(std::vector<PrimitiveShapeDataPayload> shapes, std::int64_t localPlayerId) {
    if (localPlayerId == 0) {
        return false;
    }
    auto level = ll::service::getLevel();
    if (!level) {
        return false;
    }

    // Look the player up in the server's player list instead of dynamic_cast-ing an Actor: Minecraft's
    // own classes have no usable RTTI, so dynamic_cast on them throws std::__non_rtti_object.
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

} // namespace creator_tools::debug_shapes
