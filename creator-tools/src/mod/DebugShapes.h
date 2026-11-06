#pragma once

#include "mc/deps/core/math/Color.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/network/packet/PrimitiveShapeDataPayload.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/phys/AABB.h"

#include <cstdint>
#include <string>
#include <vector>

namespace creator_tools::debug_shapes {

// Shape IDs. Each group has its own range so groups can be updated and removed independently.
inline constexpr std::uint64_t kIdBase       = 0x43544F4F4C530000ULL;
inline constexpr std::size_t   kMaxTextLines = 48;
inline constexpr float         kTextScale    = 0.3f;
inline constexpr std::uint64_t kTextBase     = kIdBase;                 // + line index
inline constexpr std::uint64_t kBox          = kIdBase + kMaxTextLines; // collision box wireframe

inline constexpr std::size_t   kMaxPathSegments = 48;
inline constexpr std::uint64_t kPathLineBase    = kIdBase + 0x100; // + segment index
inline constexpr std::uint64_t kPathTarget      = kIdBase + 0x200; // marker on the node being walked to now
inline constexpr std::uint64_t kPathDestination = kIdBase + 0x201; // marker on the final node

/// Text attached to an entity. `locationOffset` is relative to the entity.
PrimitiveShapeDataPayload makeTextShape(
    std::uint64_t id,
    Vec3 const&   locationOffset,
    std::string   text,
    mce::Color    color,
    DimensionType dimension,
    ActorUniqueID attachedTo
);

/// Wireframe box attached to an entity. `locationOffset` is relative to the entity.
PrimitiveShapeDataPayload makeBoxShape(
    std::uint64_t id,
    AABB const&   bounds,
    Vec3 const&   locationOffset,
    DimensionType dimension,
    ActorUniqueID attachedTo
);

/// Line in world space.
PrimitiveShapeDataPayload
makeLineShape(std::uint64_t id, Vec3 const& start, Vec3 const& end, mce::Color color, DimensionType dimension);

/// Sphere in world space. `radius` is applied through the shape's scale.
PrimitiveShapeDataPayload
makeSphereShape(std::uint64_t id, Vec3 const& center, float radius, mce::Color color, DimensionType dimension);

/// A shape with only an ID set, which tells the client to remove it.
PrimitiveShapeDataPayload makeRemovedShape(std::uint64_t id);

/// Removal entries for the path line segments [fromSegment, toSegment) and, optionally, the two markers.
void appendRemovedPathShapes(
    std::vector<PrimitiveShapeDataPayload>& out,
    std::size_t                             fromSegment,
    std::size_t                             toSegment,
    bool                                    target,
    bool                                    destination
);

/// Removal entries for every shape this mod can draw.
std::vector<PrimitiveShapeDataPayload> makeRemovalOfAll();

/// Sends the shapes to the local player only. The integrated server may have LAN guests, and
/// broadcasting with sendToClients() would show them our debug shapes too.
/// Server thread only. Returns false if the local player's server-side entity can't be found.
///
/// NOTE: relies on Packet::sendTo(Player const&); if your LeviLamina version names it differently,
/// this is the one place to adjust.
bool send(std::vector<PrimitiveShapeDataPayload> shapes, std::int64_t localPlayerId);

} // namespace creator_tools::debug_shapes
