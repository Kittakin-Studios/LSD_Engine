#include "lsd/physics/physics.h"

#include "lsd/foundation/allocator.h"
#include "lsd/foundation/log.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Physics/Body/BodyFilter.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace lsd::physics {

namespace {

// Object layers: things that never move, everything else, and debris (moving, but hitting only the
// static level: Unreal's Debris preset, so rubble neither blocks the player nor piles up physics work).
constexpr JPH::ObjectLayer kStaticLayer = 0;
constexpr JPH::ObjectLayer kMovingLayer = 1;
constexpr JPH::ObjectLayer kDebrisLayer = 2;
const JPH::BroadPhaseLayer kStaticBroadPhase(0);
const JPH::BroadPhaseLayer kMovingBroadPhase(1);

class ObjectPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (a == kDebrisLayer || b == kDebrisLayer) return a == kStaticLayer || b == kStaticLayer;
        return a == kMovingLayer || b == kMovingLayer;
    }
};

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override { return layer == kStaticLayer ? kStaticBroadPhase : kMovingBroadPhase; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override { return layer == kStaticBroadPhase ? "Static" : "Moving"; }
#endif
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad) const override {
        if (layer == kDebrisLayer) return broad == kStaticBroadPhase;
        return layer == kMovingLayer || broad == kMovingBroadPhase;
    }
};

void trace(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    LSD_LOG_INFO("physics", "%s", buffer);
}

// Jolt's memory, counted on its own heap (Window > Memory, memreport: "Physics (Jolt)").
HeapAllocator& jolt_heap() {
    static HeapAllocator& heap = *new HeapAllocator("Physics (Jolt)");  // never freed: Jolt keeps blocks until exit
    return heap;
}

// Jolt's global setup (allocator hooks, type registry), once per process.
void init_jolt() {
    static std::once_flag once;
    std::call_once(once, [] {
        JPH::Allocate = [](size_t size) { return jolt_heap().allocate(size, 16); };
        JPH::Reallocate = [](void* block, size_t, size_t size) { return jolt_heap().reallocate(block, size, 16); };
        JPH::Free = [](void* block) { jolt_heap().deallocate(block); };
        JPH::AlignedAllocate = [](size_t size, size_t alignment) { return jolt_heap().allocate(size, alignment < 16 ? 16 : alignment); };
        JPH::AlignedFree = [](void* block) { jolt_heap().deallocate(block); };
        JPH::Trace = trace;
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
}

JPH::Vec3 to_jolt(Vec3 v) { return {v.x, v.y, v.z}; }

// Splits a rotation + translation matrix (columns normalised to drop any leftover scale).
void to_jolt(const Mat4& m, JPH::RVec3& position, JPH::Quat& rotation) {
    position = JPH::RVec3(m.at(0, 3), m.at(1, 3), m.at(2, 3));
    JPH::Vec3 axes[3];
    for (int c = 0; c < 3; ++c) axes[c] = JPH::Vec3(m.at(0, c), m.at(1, c), m.at(2, c)).NormalizedOr(JPH::Vec3::sAxisX());
    const JPH::Mat44 rotation_matrix(JPH::Vec4(axes[0], 0), JPH::Vec4(axes[1], 0), JPH::Vec4(axes[2], 0), JPH::Vec4(0, 0, 0, 1));
    rotation = rotation_matrix.GetQuaternion().Normalized();
}

// Jolt capsules run along Y; ours stand along Z.
JPH::RefConst<JPH::Shape> z_capsule(f32 half_height, f32 radius, JPH::Vec3 offset) {
    JPH::RotatedTranslatedShapeSettings settings(offset, JPH::Quat::sRotation(JPH::Vec3::sAxisX(), 0.5f * JPH::JPH_PI), new JPH::CapsuleShape(half_height, radius));
    return settings.Create().Get();
}

// Collects new contacts from Jolt's job threads (bodies) and from the character update (main thread).
class Contacts final : public JPH::ContactListener, public JPH::CharacterContactListener {
public:
    void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold&, JPH::ContactSettings&) override {
        add(static_cast<u32>(a.GetUserData()), static_cast<u32>(b.GetUserData()));
    }
    using JPH::ContactListener::OnContactValidate;  // the other overloads stay as Jolt has them
    using JPH::CharacterContactListener::OnContactValidate;
    // One-way platforms: the character only meets them from above (its feet at or over their top).
    bool OnContactValidate(const JPH::CharacterVirtual* character, const JPH::CharacterContact& contact) override {
        if (!system || !one_way_top || one_way_top->empty() || contact.mBodyB.IsInvalid()) return true;
        JPH::BodyLockRead lock(system->GetBodyLockInterfaceNoLock(), contact.mBodyB);
        if (!lock.Succeeded()) return true;
        const u64 index = lock.GetBody().GetUserData();
        if (index >= one_way_top->size()) return true;
        const f32 top = (*one_way_top)[static_cast<usize>(index)];
        if (top != top) return true;  // NaN: an ordinary body
        return static_cast<f32>(character->GetPosition().GetZ()) >= top - 0.05f;
    }
    void OnContactAdded(const JPH::CharacterVirtual*, const JPH::CharacterContact& contact, JPH::CharacterContactSettings&) override {
        if (contact.mBodyB.IsInvalid() || !system) return;
        JPH::BodyLockRead lock(system->GetBodyLockInterfaceNoLock(), contact.mBodyB);
        if (lock.Succeeded()) add(static_cast<u32>(lock.GetBody().GetUserData()), kCharacterBody);
    }
    void add(u32 a, u32 b) {
        std::lock_guard<std::mutex> guard(mutex);
        if (pending.size() < 4096) pending.push_back({a, b});
    }
    JPH::PhysicsSystem* system = nullptr;
    const std::vector<f32>* one_way_top = nullptr;  // per body: a one-way platform's top height, NaN for others
    std::mutex mutex;
    std::vector<Contact> pending;
};

// BodyDesc::group: bodies of one non-zero group never collide with each other (a ragdoll's parts).
class SameGroupFilter final : public JPH::GroupFilter {
public:
    bool CanCollide(const JPH::CollisionGroup& a, const JPH::CollisionGroup& b) const override {
        return a.GetGroupID() == JPH::CollisionGroup::cInvalidGroup || a.GetGroupID() != b.GetGroupID();
    }
};

} // namespace

struct PhysicsWorld::Impl {
    JPH::Ref<JPH::GroupFilter> group_filter = new SameGroupFilter();
    BroadPhaseLayers broad_phase_layers;
    ObjectVsBroadPhaseFilter object_vs_broad_phase;
    ObjectPairFilter object_pairs;
    JPH::TempAllocatorImpl temp{16 * 1024 * 1024};
    JPH::JobSystemThreadPool jobs{JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                                  static_cast<int>(std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1)};
    JPH::PhysicsSystem system;
    std::vector<JPH::BodyID> bodies;
    std::vector<f32> one_way_top;  // per body (see Contacts::OnContactValidate)
    JPH::Ref<JPH::CharacterVirtual> character;
    Contacts contacts;
    struct Joint {
        JPH::Ref<JPH::Constraint> constraint;
        u32 a, b;
        JointType type = JointType::Hinge;
        f32 break_force = 0.0f;
    };
    std::vector<u32> broken;  // joints snapped since take_broken_joints
    std::vector<Joint> joints;
    struct Vehicle {
        JPH::Ref<JPH::VehicleConstraint> constraint;
        u32 body;
        f32 engine_torque;
        bool added = true;  // in the system (taken out while its body is kinematic)
    };
    std::vector<Vehicle> vehicles;
    void remove_vehicle(Vehicle& v) {
        if (!v.constraint) return;
        if (v.added) {
            system.RemoveStepListener(v.constraint);
            system.RemoveConstraint(v.constraint);
        }
        v.constraint = nullptr;
    }
    f32 step_height = 0.4f;
    f32 character_height = 1.8f;
    f32 character_radius = 0.35f;
};

PhysicsWorld::PhysicsWorld() {
    init_jolt();
    impl_ = new Impl();
    impl_->system.Init(16384, 0, 16384, 8192, impl_->broad_phase_layers, impl_->object_vs_broad_phase, impl_->object_pairs);
    impl_->contacts.system = &impl_->system;
    impl_->system.SetContactListener(&impl_->contacts);
    set_gravity(9.81f);
}

PhysicsWorld::~PhysicsWorld() {
    for (auto& v : impl_->vehicles) impl_->remove_vehicle(v);
    for (const auto& joint : impl_->joints) {
        if (joint.constraint) impl_->system.RemoveConstraint(joint.constraint);
    }
    JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    for (const JPH::BodyID id : impl_->bodies) {
        if (id.IsInvalid()) continue;  // removed
        bodies.RemoveBody(id);
        bodies.DestroyBody(id);
    }
    delete impl_;
}

u32 PhysicsWorld::add_body(const BodyDesc& desc) {
    JPH::RefConst<JPH::Shape> shape;
    switch (desc.shape) {
        case Shape::Mesh:
            if (desc.motion != Motion::Dynamic && desc.points && desc.indices && desc.index_count >= 3) {
                JPH::VertexList vertices;
                vertices.reserve(desc.point_count);
                for (u32 i = 0; i < desc.point_count; ++i) vertices.push_back(JPH::Float3(desc.points[i].x, desc.points[i].y, desc.points[i].z));
                JPH::IndexedTriangleList triangles;
                triangles.reserve(desc.index_count / 3);
                for (u32 i = 0; i + 2 < desc.index_count; i += 3) triangles.push_back(JPH::IndexedTriangle(desc.indices[i], desc.indices[i + 1], desc.indices[i + 2]));
                const JPH::ShapeSettings::ShapeResult result = JPH::MeshShapeSettings(vertices, triangles).Create();
                if (result.IsValid()) shape = result.Get();
                else LSD_LOG_WARN("physics", "Mesh collision failed (%s): using a convex hull", result.GetError().c_str());
            }
            if (shape) break;
            [[fallthrough]];  // moving bodies cannot use triangles: their convex hull instead
        case Shape::Convex:
            if (desc.points && desc.point_count >= 4) {
                JPH::Array<JPH::Vec3> points;
                points.reserve(desc.point_count);
                for (u32 i = 0; i < desc.point_count; ++i) points.push_back(to_jolt(desc.points[i]));
                const JPH::ShapeSettings::ShapeResult result = JPH::ConvexHullShapeSettings(points).Create();
                if (result.IsValid()) shape = result.Get();
                else LSD_LOG_WARN("physics", "Convex collision failed (%s): using a box", result.GetError().c_str());
            }
            if (shape) break;
            [[fallthrough]];
        case Shape::Box: {
            // Flat boxes (planes) still need some thickness; the convex radius must fit inside.
            const JPH::Vec3 half = JPH::Vec3::sMax(to_jolt(desc.half_extents), JPH::Vec3::sReplicate(0.01f));
            shape = new JPH::BoxShape(half, std::fmin(JPH::cDefaultConvexRadius, half.ReduceMin() * 0.5f));
            break;
        }
        case Shape::Sphere: shape = new JPH::SphereShape(std::fmax(desc.half_extents.x, 0.01f)); break;
        case Shape::Capsule: shape = z_capsule(std::fmax(desc.half_extents.z, 0.01f), std::fmax(desc.half_extents.x, 0.01f), JPH::Vec3::sZero()); break;
    }
    const Vec3 com = desc.centre_of_mass_offset;
    if (desc.motion == Motion::Dynamic && (com.x != 0.0f || com.y != 0.0f || com.z != 0.0f)) {
        const JPH::ShapeSettings::ShapeResult result = JPH::OffsetCenterOfMassShapeSettings(to_jolt(com), shape).Create();
        if (result.IsValid()) shape = result.Get();
    }
    JPH::RVec3 position;
    JPH::Quat rotation;
    to_jolt(desc.transform, position, rotation);
    const JPH::EMotionType motion = desc.motion == Motion::Static ? JPH::EMotionType::Static
                                  : desc.motion == Motion::Dynamic ? JPH::EMotionType::Dynamic
                                                                   : JPH::EMotionType::Kinematic;
    const JPH::ObjectLayer layer = desc.motion == Motion::Static ? kStaticLayer : desc.debris ? kDebrisLayer : kMovingLayer;
    JPH::BodyCreationSettings settings(shape, position, rotation, motion, layer);
    settings.mFriction = desc.friction;
    settings.mUserData = impl_->bodies.size();  // our index, for contacts and ray hits
    settings.mRestitution = desc.restitution;
    if (desc.group) settings.mCollisionGroup = JPH::CollisionGroup(impl_->group_filter, desc.group, 0);
    if (desc.plane_2d) settings.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationZ | JPH::EAllowedDOFs::RotationY;
    if (desc.motion == Motion::Dynamic) {
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = std::fmax(desc.mass, 0.01f);
    }
    const JPH::BodyID id = impl_->system.GetBodyInterface().CreateAndAddBody(
        settings, desc.motion == Motion::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
    if (id.IsInvalid()) LSD_LOG_ERROR("physics", "Out of bodies");
    impl_->bodies.push_back(id);
    f32 top = std::numeric_limits<f32>::quiet_NaN();
    if (desc.one_way && !id.IsInvalid()) {
        top = static_cast<f32>(impl_->system.GetBodyInterface().GetTransformedShape(id).GetWorldSpaceBounds().mMax.GetZ());
        impl_->contacts.one_way_top = &impl_->one_way_top;
    }
    impl_->one_way_top.push_back(top);
    return static_cast<u32>(impl_->bodies.size() - 1);
}

u32 PhysicsWorld::body_count() const { return static_cast<u32>(impl_->bodies.size()); }

void PhysicsWorld::move_kinematic(u32 body, const Mat4& transform, f32 dt) {
    if (body >= impl_->bodies.size() || impl_->bodies[body].IsInvalid() || dt <= 0.0f) return;
    JPH::RVec3 position;
    JPH::Quat rotation;
    to_jolt(transform, position, rotation);
    impl_->system.GetBodyInterface().MoveKinematic(impl_->bodies[body], position, rotation, dt);
}

void PhysicsWorld::remove_body(u32 body) {
    if (body >= impl_->bodies.size() || impl_->bodies[body].IsInvalid()) return;
    for (auto& joint : impl_->joints) {  // Jolt joints point at their bodies: they go first
        if (joint.constraint && (joint.a == body || joint.b == body)) {
            impl_->system.RemoveConstraint(joint.constraint);
            joint.constraint = nullptr;
        }
    }
    for (auto& v : impl_->vehicles) {
        if (v.body == body) impl_->remove_vehicle(v);
    }
    JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    bodies.RemoveBody(impl_->bodies[body]);
    bodies.DestroyBody(impl_->bodies[body]);
    impl_->bodies[body] = JPH::BodyID();
}

bool PhysicsWorld::raycast(Vec3 origin, Vec3 direction, f32 max_distance, RayHit& hit, u32 ignore_body) const {
    const f32 length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    if (length <= 0.0f || max_distance <= 0.0f) return false;
    const JPH::Vec3 span = to_jolt(direction) * (max_distance / length);
    const JPH::RRayCast ray(JPH::RVec3(origin.x, origin.y, origin.z), span);
    JPH::RayCastResult result;
    const bool ignoring = ignore_body < impl_->bodies.size() && !impl_->bodies[ignore_body].IsInvalid();
    const JPH::IgnoreSingleBodyFilter skip(ignoring ? impl_->bodies[ignore_body] : JPH::BodyID());
    const JPH::BodyFilter everything;
    if (!impl_->system.GetNarrowPhaseQuery().CastRay(ray, result, {}, {}, ignoring ? static_cast<const JPH::BodyFilter&>(skip) : everything)) return false;
    const JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
    JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), result.mBodyID);
    if (!lock.Succeeded()) return false;
    const JPH::Vec3 normal = lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point);
    hit.body = static_cast<u32>(lock.GetBody().GetUserData());
    hit.point = {static_cast<f32>(point.GetX()), static_cast<f32>(point.GetY()), static_cast<f32>(point.GetZ())};
    hit.normal = {normal.GetX(), normal.GetY(), normal.GetZ()};
    hit.distance = result.mFraction * max_distance;
    return true;
}

namespace {

// The query shape in Jolt's terms; capsules turned from Jolt's Y axis to our Z (as in add_body).
JPH::ShapeRefC query_shape(const QueryShape& s) {
    switch (s.kind) {
        case QueryShape::Box: {
            const JPH::Vec3 half(std::fmax(s.half_extents.x, 0.01f), std::fmax(s.half_extents.y, 0.01f), std::fmax(s.half_extents.z, 0.01f));
            return new JPH::BoxShape(half, std::fmin(0.05f, half.ReduceMin() * 0.5f));
        }
        case QueryShape::Capsule: {
            JPH::RotatedTranslatedShapeSettings settings(JPH::Vec3::sZero(), JPH::Quat::sRotation(JPH::Vec3::sAxisX(), 0.5f * JPH::JPH_PI),
                                                         new JPH::CapsuleShape(std::fmax(s.half_height, 0.001f), std::fmax(s.radius, 0.01f)));
            return settings.Create().Get();
        }
        case QueryShape::Sphere: break;
    }
    return new JPH::SphereShape(std::fmax(s.radius, 0.01f));
}

} // namespace

bool PhysicsWorld::sweep(const QueryShape& shape, Vec3 from, Vec3 to, RayHit& hit) const {
    const JPH::Vec3 span = to_jolt(to - from);
    if (span.LengthSq() <= 0.0f) return false;
    const JPH::ShapeRefC jolt_shape = query_shape(shape);
    const JPH::RShapeCast cast(jolt_shape, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(JPH::RVec3(from.x, from.y, from.z)), span);
    JPH::ShapeCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;  // start-inside hits count, like Unreal
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    impl_->system.GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector);
    if (!collector.HadHit()) return false;
    const JPH::ShapeCastResult& result = collector.mHit;
    JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), result.mBodyID2);
    if (!lock.Succeeded()) return false;
    const JPH::Vec3 normal = -result.mPenetrationAxis.NormalizedOr(JPH::Vec3::sAxisZ());  // from the body towards the shape
    hit.body = static_cast<u32>(lock.GetBody().GetUserData());
    hit.point = {result.mContactPointOn2.GetX(), result.mContactPointOn2.GetY(), result.mContactPointOn2.GetZ()};
    hit.normal = {normal.GetX(), normal.GetY(), normal.GetZ()};
    hit.distance = result.mFraction * span.Length();
    return true;
}

u32 PhysicsWorld::overlap(const QueryShape& shape, Vec3 at, u32* bodies, u32 capacity) const {
    const JPH::ShapeRefC jolt_shape = query_shape(shape);
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    impl_->system.GetNarrowPhaseQuery().CollideShape(jolt_shape, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(JPH::RVec3(at.x, at.y, at.z)), settings,
                                                    JPH::RVec3::sZero(), collector);
    u32 n = 0;
    for (const JPH::CollideShapeResult& result : collector.mHits) {
        JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), result.mBodyID2);
        if (!lock.Succeeded()) continue;
        const u32 body = static_cast<u32>(lock.GetBody().GetUserData());
        bool seen = false;  // a body touching with several sub-shapes is listed once
        for (u32 k = 0; k < n; ++k) seen = seen || bodies[k] == body;
        if (!seen && n < capacity) bodies[n++] = body;
    }
    return n;
}

u32 PhysicsWorld::take_contacts(Contact* out, u32 capacity) {
    std::lock_guard<std::mutex> guard(impl_->contacts.mutex);
    const u32 n = static_cast<u32>(impl_->contacts.pending.size() < capacity ? impl_->contacts.pending.size() : capacity);
    for (u32 i = 0; i < n; ++i) out[i] = impl_->contacts.pending[i];
    impl_->contacts.pending.clear();
    return n;
}

u32 PhysicsWorld::add_joint(const JointDesc& desc) {
    auto id_of = [&](u32 body) { return body < impl_->bodies.size() ? impl_->bodies[body] : JPH::BodyID(); };  // invalid = the world
    const JPH::BodyID a = id_of(desc.body_a), b = id_of(desc.body_b);
    const JPH::RVec3 pivot(desc.pivot.x, desc.pivot.y, desc.pivot.z);
    const JPH::Vec3 axis = to_jolt(desc.axis).NormalizedOr(JPH::Vec3::sAxisZ());
    const JPH::Vec3 normal = axis.GetNormalizedPerpendicular();
    JPH::Ref<JPH::TwoBodyConstraintSettings> settings;
    switch (desc.type) {
        case JointType::Fixed: {
            auto* s = new JPH::FixedConstraintSettings();
            s->mAutoDetectPoint = true;
            settings = s;
            break;
        }
        case JointType::Ball: {
            auto* s = new JPH::PointConstraintSettings();
            s->mPoint1 = s->mPoint2 = pivot;
            settings = s;
            break;
        }
        case JointType::Hinge: {
            auto* s = new JPH::HingeConstraintSettings();
            s->mPoint1 = s->mPoint2 = pivot;
            s->mHingeAxis1 = s->mHingeAxis2 = axis;
            s->mNormalAxis1 = s->mNormalAxis2 = normal;
            if (desc.limited) {
                s->mLimitsMin = JPH::DegreesToRadians(std::fmin(desc.limit_min, 0.0f));
                s->mLimitsMax = JPH::DegreesToRadians(std::fmax(desc.limit_max, 0.0f));
                if (desc.spring_frequency > 0.0f) s->mLimitsSpringSettings = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, desc.spring_frequency, desc.spring_damping);
            }
            s->mMotorSettings = JPH::MotorSettings(10.0f, 1.0f, 0.0f, desc.motor_strength);
            settings = s;
            break;
        }
        case JointType::Cone: {
            auto* s = new JPH::SwingTwistConstraintSettings();
            s->mPosition1 = s->mPosition2 = pivot;
            s->mTwistAxis1 = s->mTwistAxis2 = axis;
            s->mPlaneAxis1 = s->mPlaneAxis2 = normal;
            const f32 cone = JPH::DegreesToRadians(std::clamp(desc.cone_angle, 0.0f, 179.0f));
            s->mNormalHalfConeAngle = s->mPlaneHalfConeAngle = cone;
            s->mTwistMinAngle = desc.limited ? JPH::DegreesToRadians(std::clamp(desc.limit_min, -179.0f, 0.0f)) : -JPH::JPH_PI;
            s->mTwistMaxAngle = desc.limited ? JPH::DegreesToRadians(std::clamp(desc.limit_max, 0.0f, 179.0f)) : JPH::JPH_PI;
            settings = s;
            break;
        }
        case JointType::Slider: {
            auto* s = new JPH::SliderConstraintSettings();
            s->mPoint1 = s->mPoint2 = pivot;
            s->mSliderAxis1 = s->mSliderAxis2 = axis;
            s->mNormalAxis1 = s->mNormalAxis2 = normal;
            if (desc.limited) {
                s->mLimitsMin = std::fmin(desc.limit_min, 0.0f);
                s->mLimitsMax = std::fmax(desc.limit_max, 0.0f);
                if (desc.spring_frequency > 0.0f) s->mLimitsSpringSettings = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, desc.spring_frequency, desc.spring_damping);
            }
            s->mMotorSettings = JPH::MotorSettings(10.0f, 1.0f, desc.motor_strength, 0.0f);
            settings = s;
            break;
        }
    }
    JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    JPH::Ref<JPH::Constraint> constraint = a.IsInvalid() && b.IsInvalid() ? nullptr : bodies.CreateConstraint(settings, b, a);  // Jolt measures body 2 against body 1: limits and motors describe body_a
    if (constraint) {
        if (desc.motor_speed != 0.0f && desc.type == JointType::Hinge) {
            auto* hinge = static_cast<JPH::HingeConstraint*>(constraint.GetPtr());
            hinge->SetMotorState(JPH::EMotorState::Velocity);
            hinge->SetTargetAngularVelocity(JPH::DegreesToRadians(desc.motor_speed));
        } else if (desc.motor_speed != 0.0f && desc.type == JointType::Slider) {
            auto* slider = static_cast<JPH::SliderConstraint*>(constraint.GetPtr());
            slider->SetMotorState(JPH::EMotorState::Velocity);
            slider->SetTargetVelocity(desc.motor_speed);
        }
        impl_->system.AddConstraint(constraint);
        bodies.ActivateConstraint(static_cast<JPH::TwoBodyConstraint*>(constraint.GetPtr()));
    } else {
        LSD_LOG_WARN("physics", "A joint needs at least one body");
    }
    impl_->joints.push_back({constraint, desc.body_a, desc.body_b, desc.type, desc.break_force});
    return static_cast<u32>(impl_->joints.size() - 1);
}

u32 PhysicsWorld::joint_count() const { return static_cast<u32>(impl_->joints.size()); }

u32 PhysicsWorld::take_broken_joints(u32* out, u32 capacity) {
    const u32 n = static_cast<u32>(impl_->broken.size() < capacity ? impl_->broken.size() : capacity);
    for (u32 i = 0; i < n; ++i) out[i] = impl_->broken[i];
    impl_->broken.clear();
    return n;
}

Mat4 PhysicsWorld::body_transform(u32 body) const {
    if (body >= impl_->bodies.size() || impl_->bodies[body].IsInvalid()) return Mat4{};
    const JPH::RMat44 m = impl_->system.GetBodyInterface().GetWorldTransform(impl_->bodies[body]);
    Mat4 out;
    for (int c = 0; c < 4; ++c) {
        const JPH::Vec4 column = c < 3 ? m.GetColumn4(static_cast<JPH::uint>(c)) : JPH::Vec4(JPH::Vec3(m.GetTranslation()), 1.0f);
        for (int r = 0; r < 4; ++r) out.at(r, c) = column[static_cast<JPH::uint>(r)];
    }
    return out;
}

u32 PhysicsWorld::add_vehicle(const VehicleDesc& desc) {
    if (desc.body >= impl_->bodies.size() || impl_->bodies[desc.body].IsInvalid() || desc.wheel_count == 0) {
        LSD_LOG_WARN("physics", "A vehicle needs a dynamic body and at least one wheel");
        impl_->vehicles.push_back({nullptr, desc.body, desc.engine_torque});
        return static_cast<u32>(impl_->vehicles.size() - 1);
    }
    JPH::VehicleConstraintSettings settings;
    settings.mUp = JPH::Vec3::sAxisZ();
    settings.mForward = JPH::Vec3::sAxisX();
    settings.mMaxPitchRollAngle = JPH::DegreesToRadians(desc.max_pitch_roll_degrees);
    auto* controller = new JPH::WheeledVehicleControllerSettings();
    controller->mEngine.mMaxTorque = desc.engine_torque;
    controller->mEngine.mMaxRPM = desc.max_rpm;
    const u32 wheels = desc.wheel_count < kMaxWheels ? desc.wheel_count : kMaxWheels;
    std::vector<u32> driven;
    for (u32 i = 0; i < wheels; ++i) {
        const WheelDesc& w = desc.wheels[i];
        auto* wheel = new JPH::WheelSettingsWV();
        wheel->mPosition = to_jolt(w.position);
        wheel->mSuspensionDirection = -JPH::Vec3::sAxisZ();
        wheel->mSteeringAxis = JPH::Vec3::sAxisZ();
        wheel->mWheelUp = JPH::Vec3::sAxisZ();
        wheel->mWheelForward = JPH::Vec3::sAxisX();
        wheel->mSuspensionMinLength = 0.0f;
        wheel->mSuspensionMaxLength = std::fmax(w.suspension_length, 0.01f);
        wheel->mSuspensionSpring.mFrequency = std::fmax(w.suspension_frequency, 0.1f);
        wheel->mSuspensionSpring.mDamping = std::clamp(w.suspension_damping, 0.0f, 1.0f);
        wheel->mRadius = std::fmax(w.radius, 0.05f);
        wheel->mWidth = std::fmax(w.width, 0.02f);
        wheel->mMaxSteerAngle = JPH::DegreesToRadians(w.max_steer_degrees);
        wheel->mMaxBrakeTorque = desc.brake_torque;
        wheel->mMaxHandBrakeTorque = w.handbrake ? desc.handbrake_torque : 0.0f;
        const f32 grip = std::fmax(w.friction, 0.0f);
        for (JPH::LinearCurve::Point& p : wheel->mLongitudinalFriction.mPoints) p.mY *= grip;
        for (JPH::LinearCurve::Point& p : wheel->mLateralFriction.mPoints) p.mY *= grip;
        settings.mWheels.push_back(wheel);
        if (w.driven) driven.push_back(i);
    }
    // Driven wheels in left / right pairs (by position along the car), each pair a differential
    // sharing the engine's torque; a wheel without a partner gets one of its own.
    std::sort(driven.begin(), driven.end(), [&](u32 a, u32 b) { return desc.wheels[a].position.x < desc.wheels[b].position.x; });
    for (usize i = 0; i < driven.size();) {
        JPH::VehicleDifferentialSettings diff;
        const bool pair = i + 1 < driven.size() && std::fabs(desc.wheels[driven[i]].position.x - desc.wheels[driven[i + 1]].position.x) < 0.05f;
        const u32 a = driven[i], b = pair ? driven[i + 1] : driven[i];
        const bool a_left = desc.wheels[a].position.y >= desc.wheels[b].position.y;
        diff.mLeftWheel = static_cast<int>(a_left ? a : b);
        diff.mRightWheel = pair ? static_cast<int>(a_left ? b : a) : -1;
        controller->mDifferentials.push_back(diff);
        i += pair ? 2 : 1;
    }
    if (desc.gear_count) controller->mTransmission.mGearRatios.assign(desc.gear_ratios, desc.gear_ratios + std::min(desc.gear_count, 8u));
    if (desc.reverse_ratio > 0.0f) {
        controller->mTransmission.mReverseGearRatios.clear();
        controller->mTransmission.mReverseGearRatios.push_back(-desc.reverse_ratio);
    }
    for (JPH::VehicleDifferentialSettings& d : controller->mDifferentials) d.mEngineTorqueRatio = 1.0f / static_cast<f32>(controller->mDifferentials.size());
    settings.mController = controller;
    JPH::Ref<JPH::VehicleConstraint> constraint;
    {
        JPH::BodyLockWrite lock(impl_->system.GetBodyLockInterface(), impl_->bodies[desc.body]);
        if (lock.Succeeded()) constraint = new JPH::VehicleConstraint(lock.GetBody(), settings);
    }
    if (!constraint) {
        impl_->vehicles.push_back({nullptr, desc.body, desc.engine_torque});
        return static_cast<u32>(impl_->vehicles.size() - 1);
    }
    constraint->SetVehicleCollisionTester(new JPH::VehicleCollisionTesterCastCylinder(kMovingLayer));  // hits what the body hits
    impl_->system.AddConstraint(constraint);
    impl_->system.AddStepListener(constraint);
    impl_->vehicles.push_back({constraint, desc.body, desc.engine_torque});
    return static_cast<u32>(impl_->vehicles.size() - 1);
}

void PhysicsWorld::drive_vehicle(u32 vehicle, f32 forward, f32 right, f32 brake, f32 handbrake, f32 torque_scale) {
    if (vehicle >= impl_->vehicles.size() || !impl_->vehicles[vehicle].constraint || !impl_->vehicles[vehicle].added) return;
    Impl::Vehicle& v = impl_->vehicles[vehicle];
    auto* controller = static_cast<JPH::WheeledVehicleController*>(v.constraint->GetController());
    controller->GetEngine().mMaxTorque = v.engine_torque * std::fmax(torque_scale, 0.0f);
    controller->SetDriverInput(std::clamp(forward, -1.0f, 1.0f), std::clamp(right, -1.0f, 1.0f), std::clamp(brake, 0.0f, 1.0f), std::clamp(handbrake, 0.0f, 1.0f));
    if (forward != 0.0f || right != 0.0f || brake != 0.0f || handbrake != 0.0f) impl_->system.GetBodyInterface().ActivateBody(impl_->bodies[v.body]);  // a parked car sleeps
}

Mat4 PhysicsWorld::wheel_transform(u32 vehicle, u32 wheel) const {
    if (vehicle >= impl_->vehicles.size() || !impl_->vehicles[vehicle].constraint || wheel >= impl_->vehicles[vehicle].constraint->GetWheels().size()) return Mat4{};
    // The wheel model's right is -Y and up +Z: our x forward, y left, z up.
    const JPH::RMat44 m = impl_->vehicles[vehicle].constraint->GetWheelWorldTransform(wheel, -JPH::Vec3::sAxisY(), JPH::Vec3::sAxisZ());
    Mat4 out;
    for (int c = 0; c < 4; ++c) {
        const JPH::Vec4 column = c < 3 ? m.GetColumn4(static_cast<JPH::uint>(c)) : JPH::Vec4(JPH::Vec3(m.GetTranslation()), 1.0f);
        for (int r = 0; r < 4; ++r) out.at(r, c) = column[static_cast<JPH::uint>(r)];
    }
    return out;
}

VehicleState PhysicsWorld::vehicle_state(u32 vehicle) const {
    VehicleState s;
    if (vehicle >= impl_->vehicles.size() || !impl_->vehicles[vehicle].constraint) return s;
    const Impl::Vehicle& v = impl_->vehicles[vehicle];
    const JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    const JPH::Vec3 forward = bodies.GetRotation(impl_->bodies[v.body]) * JPH::Vec3::sAxisX();
    s.speed = bodies.GetLinearVelocity(impl_->bodies[v.body]).Dot(forward);
    const auto* controller = static_cast<const JPH::WheeledVehicleController*>(v.constraint->GetController());
    s.rpm = controller->GetEngine().GetCurrentRPM();
    s.gear = controller->GetTransmission().GetCurrentGear();
    for (const JPH::Wheel* w : v.constraint->GetWheels()) s.wheels_on_ground += w->HasContact() ? 1u : 0u;
    return s;
}

void PhysicsWorld::set_gravity(f32 g) { impl_->system.SetGravity(JPH::Vec3(0, 0, -g)); }

void PhysicsWorld::step(f32 dt) {
    if (dt <= 0.0f) return;
    impl_->system.Update(dt, 1, &impl_->temp, &impl_->jobs);
    // Breakable joints: the impulse holding the pivot together this step, over dt, is the force.
    for (u32 i = 0; i < impl_->joints.size(); ++i) {
        Impl::Joint& j = impl_->joints[i];
        if (!j.constraint || j.break_force <= 0.0f) continue;
        JPH::Constraint* c = j.constraint.GetPtr();
        f32 impulse = 0.0f;
        switch (j.type) {
            case JointType::Fixed: impulse = static_cast<JPH::FixedConstraint*>(c)->GetTotalLambdaPosition().Length(); break;
            case JointType::Ball: impulse = static_cast<JPH::PointConstraint*>(c)->GetTotalLambdaPosition().Length(); break;
            case JointType::Hinge: impulse = static_cast<JPH::HingeConstraint*>(c)->GetTotalLambdaPosition().Length(); break;
            case JointType::Slider: impulse = static_cast<JPH::SliderConstraint*>(c)->GetTotalLambdaPosition().Length(); break;
            case JointType::Cone: impulse = static_cast<JPH::SwingTwistConstraint*>(c)->GetTotalLambdaPosition().Length(); break;
        }
        if (impulse / dt <= j.break_force) continue;
        impl_->system.RemoveConstraint(j.constraint);
        j.constraint = nullptr;
        impl_->broken.push_back(i);
    }
}

void PhysicsWorld::create_character(const CharacterDesc& desc) {
    const f32 half_height = std::fmax(desc.height * 0.5f - desc.radius, 0.05f);
    JPH::CharacterVirtualSettings settings;
    settings.mUp = JPH::Vec3::sAxisZ();
    settings.mMaxSlopeAngle = JPH::DegreesToRadians(desc.max_slope_degrees);
    settings.mShape = z_capsule(half_height, desc.radius, JPH::Vec3(0, 0, half_height + desc.radius));  // origin at the feet
    settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisZ(), -desc.radius);  // ground contacts are below the lower sphere's centre
    impl_->character = new JPH::CharacterVirtual(&settings, JPH::RVec3(desc.feet.x, desc.feet.y, desc.feet.z), JPH::Quat::sIdentity(), 0, &impl_->system);
    impl_->character->SetListener(&impl_->contacts);
    impl_->step_height = desc.step_height;
    impl_->character_radius = desc.radius;
    impl_->character_height = desc.height;
}

void PhysicsWorld::set_character_limits(f32 radius, f32 max_slope_degrees, f32 step_height) {
    JPH::CharacterVirtual* c = impl_->character.GetPtr();
    if (!c) return;
    c->SetMaxSlopeAngle(JPH::DegreesToRadians(max_slope_degrees));
    impl_->step_height = step_height;
    if (radius != impl_->character_radius && radius > 0.01f) {
        const f32 old = impl_->character_radius;
        impl_->character_radius = radius;
        if (!set_character_height(impl_->character_height)) impl_->character_radius = old;  // would not fit here: keeps the old one
    }
}

void PhysicsWorld::set_body_material(u32 body, f32 friction, f32 restitution) {
    if (body >= impl_->bodies.size() || impl_->bodies[body].IsInvalid()) return;
    JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    bodies.SetFriction(impl_->bodies[body], friction);
    bodies.SetRestitution(impl_->bodies[body], restitution);
}

void PhysicsWorld::set_body_mass(u32 body, f32 mass) {
    if (body >= impl_->bodies.size() || impl_->bodies[body].IsInvalid() || mass <= 0.0f) return;
    JPH::BodyLockWrite lock(impl_->system.GetBodyLockInterface(), impl_->bodies[body]);
    if (!lock.Succeeded() || !lock.GetBody().IsDynamic()) return;
    lock.GetBody().GetMotionProperties()->ScaleToMass(mass);
}

bool PhysicsWorld::apply_buoyancy(u32 body, Vec3 surface, Vec3 normal, f32 buoyancy, f32 linear_drag, f32 angular_drag, f32 dt) {
    if (body >= impl_->bodies.size() || impl_->bodies[body].IsInvalid() || dt <= 0.0f) return false;
    JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    if (bodies.GetMotionType(impl_->bodies[body]) != JPH::EMotionType::Dynamic) return false;
    // Wakes a sleeping body when it is in the water (a wave may lift it).
    return bodies.ApplyBuoyancyImpulse(impl_->bodies[body], JPH::RVec3(surface.x, surface.y, surface.z), JPH::Vec3(normal.x, normal.y, normal.z), buoyancy,
                                       linear_drag, angular_drag, JPH::Vec3::sZero(), impl_->system.GetGravity(), dt);
}

void PhysicsWorld::set_body_kinematic(u32 body, bool kinematic) {
    if (body >= impl_->bodies.size() || impl_->bodies[body].IsInvalid()) return;
    JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    if (bodies.GetMotionType(impl_->bodies[body]) == JPH::EMotionType::Static) return;
    // A vehicle drives only a dynamic body: out of the system while it is kinematic, back after.
    for (Impl::Vehicle& v : impl_->vehicles) {
        if (v.body != body || !v.constraint || v.added == !kinematic) continue;
        if (kinematic) {
            impl_->system.RemoveStepListener(v.constraint);
            impl_->system.RemoveConstraint(v.constraint);
        } else {
            impl_->system.AddConstraint(v.constraint);
            impl_->system.AddStepListener(v.constraint);
        }
        v.added = !kinematic;
    }
    bodies.SetMotionType(impl_->bodies[body], kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic, JPH::EActivation::Activate);
}

bool PhysicsWorld::set_character_height(f32 height) {
    JPH::CharacterVirtual* c = impl_->character.GetPtr();
    if (!c) return false;
    impl_->character_height = height;
    const f32 radius = impl_->character_radius;
    const f32 half_height = std::fmax(height * 0.5f - radius, 0.05f);
    const JPH::RefConst<JPH::Shape> shape = z_capsule(half_height, radius, JPH::Vec3(0, 0, half_height + radius));
    // SetShape refuses (keeps the old capsule) when the new one would sink into something above.
    return c->SetShape(shape, 1.5f * impl_->system.GetPhysicsSettings().mPenetrationSlop, impl_->system.GetDefaultBroadPhaseLayerFilter(kMovingLayer),
                       impl_->system.GetDefaultLayerFilter(kMovingLayer), {}, {}, impl_->temp);
}

void PhysicsWorld::move_character(Vec3 velocity, bool jump, f32 jump_speed, f32 dt, bool swimming) {
    JPH::CharacterVirtual* c = impl_->character.GetPtr();
    if (!c || dt <= 0.0f) return;
    if (swimming) {  // water: steer in 3D, no gravity, no stairs
        c->SetLinearVelocity(JPH::Vec3(velocity.x, velocity.y, velocity.z));
        c->ExtendedUpdate(dt, JPH::Vec3::sZero(), {}, impl_->system.GetDefaultBroadPhaseLayerFilter(kMovingLayer), impl_->system.GetDefaultLayerFilter(kMovingLayer),
                          {}, {}, impl_->temp);
        return;
    }
    c->UpdateGroundVelocity();
    const JPH::Vec3 ground = c->GetGroundVelocity();
    const JPH::Vec3 vertical(0, 0, c->GetLinearVelocity().GetZ());
    const bool on_ground = c->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
    JPH::Vec3 next;
    if (on_ground && vertical.GetZ() - ground.GetZ() < 0.1f) {  // standing (not moving away from the ground)
        next = ground;
        if (jump) next += JPH::Vec3(0, 0, jump_speed);
    } else {
        next = vertical;
    }
    next += impl_->system.GetGravity() * dt;
    next += JPH::Vec3(velocity.x, velocity.y, 0);
    c->SetLinearVelocity(next);

    JPH::CharacterVirtual::ExtendedUpdateSettings update;
    update.mStickToFloorStepDown = JPH::Vec3(0, 0, -0.5f);
    update.mWalkStairsStepUp = JPH::Vec3(0, 0, impl_->step_height);
    c->ExtendedUpdate(dt, impl_->system.GetGravity(), update, impl_->system.GetDefaultBroadPhaseLayerFilter(kMovingLayer),
                      impl_->system.GetDefaultLayerFilter(kMovingLayer), {}, {}, impl_->temp);
}

void PhysicsWorld::teleport_character(Vec3 feet) {
    if (!impl_->character) return;
    impl_->character->SetPosition(JPH::RVec3(feet.x, feet.y, feet.z));
    impl_->character->SetLinearVelocity(JPH::Vec3::sZero());
}

Vec3 PhysicsWorld::character_feet() const {
    if (!impl_->character) return {};
    const JPH::RVec3 p = impl_->character->GetPosition();
    return {static_cast<f32>(p.GetX()), static_cast<f32>(p.GetY()), static_cast<f32>(p.GetZ())};
}

bool PhysicsWorld::character_on_ground() const {
    return impl_->character && impl_->character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
}

} // namespace lsd::physics
