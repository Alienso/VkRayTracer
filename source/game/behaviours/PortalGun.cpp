//
// portalGun: the player places a portal pair by shooting. See game/Behaviour.h for what a type is
// made of.
//
// Left click shoots end A, right click end B: a ray from the camera along its view (PhysicsWorld::rayCast: the
// colliders, through the portals' openings) puts the end on the collider face it hits (portals::placePortalEnd: on a
// wall hanging down from the hit when there is room below), as a child of the collider's entity, so it moves with
// it; a second shot moves it. An end not placed yet is hidden, so the pair is off (no view, no collision, nothing
// carried through) until both are: a pair is on while both ends are visible (Scene::placePairs). Removed with what it
// was on (and so its other end too), a new pair is made.
//

#include <cmath>
#include <memory>
#include "game/Behaviour.h"
#include "game/Colliders.h"
#include "portals/PortalPlacement.h"

namespace {

struct PortalGun {
    static constexpr const char* type = "portalGun";
    static constexpr const char* help =
        "The player's portal gun: left click puts the pair's first end where the view's center hits a collider,\n"
        "right click the second (again: moves it there). On a wall an end hangs down from where it hit (so a shot\n"
        "at eye height can be walked into), resting on the floor when there is too little room. An end goes under\n"
        "the entity it was put on and moves with it. The pair works once both are placed. width, height: the\n"
        "openings' size (ellipse: oval ones); range: how far it reaches. pair: a portal pair in the scene to use\n"
        "(its group or one of its ends; its rims and size as set there; hidden at Play); 0: a new pair is made at\n"
        "Play. One per game.";
    static constexpr const char* defaults =
        R"({"width": 1.2, "height": 2.2, "ellipse": false, "range": 50, "pair": 0})";
    static constexpr bool removedWithEntity = true;
    static constexpr bool onSpawnedCopies = false;  // (one gun)

    EntityId entity;
    glm::vec2 size;
    bool ellipse;
    float range;
    EntityId pair;              // the group given, 0: made at the first step
    EntityId ends[2] = {0, 0};  // A (left click), B (right click)
    bool ready = false;

    static std::optional<PortalGun> read(const ReadContext& context, GameProperties& properties) {
        PortalGun gun{context.entity.id,
                      glm::vec2(std::max(properties.number("width", 1.2f), 0.05f),
                                std::max(properties.number("height", 2.2f), 0.05f)),
                      properties.flag("ellipse", false),
                      std::max(properties.number("range", 50.0f), 0.0f),
                      context.existing(properties, "pair", false)};
        if (context.world.portalGun) {
            properties.warn("there is a portal gun already; ignored");
            return std::nullopt;
        }
        if (gun.pair != 0 && findEnds(context.scene, gun.pair, gun.ends) == false) {
            properties.warn("\"pair\": entity " + std::to_string(gun.pair) + " isn't a portal pair (its group or an "
                            "end); a new pair is made");
            gun.pair = 0;
        }
        context.world.portalGun = true;
        return gun;
    }

    // A pair's ends (A first): of one of them, or the group's two children that are portal ends
    static bool findEnds(const Scene& scene, EntityId pairOrEnd, EntityId ends[2]) {
        ends[0] = ends[1] = 0;
        if (const PortalComponent* portal = scene.portal(pairOrEnd)) {
            ends[portal->endA ? 0 : 1] = pairOrEnd;
            ends[portal->endA ? 1 : 0] = static_cast<EntityId>(portal->other);
        } else {
            for (EntityId child : scene.find(pairOrEnd)->children) {
                if (const PortalComponent* end = scene.portal(child)) {
                    ends[end->endA ? 0 : 1] = child;
                }
            }
        }
        return ends[0] != 0 && ends[1] != 0 && scene.find(ends[0]) != nullptr && scene.find(ends[1]) != nullptr &&
               scene.portal(ends[0])->other == ends[1];
    }

    // The pair, made now when none was given; both ends hidden until shot
    void setUp(Scene& scene) {
        if (pair == 0 || scene.find(pair) == nullptr || !findEnds(scene, pair, ends)) {
            pair = scene.addPortalPair();
            Entity& group = *scene.find(pair);
            group.name = "Portal gun's pair";
            group.local = Transform();  // (the ends are placed in the world)
            findEnds(scene, pair, ends);
            PortalComponent& portal = *scene.portal(ends[0]);
            portal.size = size;
            portal.shape = ellipse ? PortalShape::Ellipse : PortalShape::Rectangle;
            scene.syncPortal(ends[0]);
        }
        for (EntityId end : ends) {
            scene.find(end)->visible = false;
        }
        ready = true;
    }

    void update(const GameStep& step, GameWorld& world) {
        Scene& scene = step.scene;
        if (world.physics == nullptr || scene.find(entity) == nullptr) {
            return;
        }
        if (ready && (scene.find(ends[0]) == nullptr || scene.find(ends[1]) == nullptr)) {
            pair = 0;  // (removed with what it was put on: a new pair)
            ready = false;
        }
        if (!ready) {
            setUp(scene);
        }
        if (!step.input.mouse) {
            return;
        }
        for (int button : {MouseButton::Left, MouseButton::Right}) {
            if (step.input.mousePressed[button]) {
                shoot(step, *world.physics, button == MouseButton::Left ? 0 : 1);
            }
        }
    }

    // The ray from the view's center; where it hits a collider that takes portals, the end goes
    void shoot(const GameStep& step, const physics::PhysicsWorld& physics, int which) const {
        Scene& scene = step.scene;
        EntityId end = ends[which];
        EntityId other = ends[1 - which];
        if (scene.find(end) == nullptr || scene.find(other) == nullptr) {
            return;
        }
        const Camera& camera = scene.camera();
        std::optional<physics::RayHit> hit = physics.rayCast(camera.position, camera.forward(), range);
        if (!hit || !colliderPortalable(scene, static_cast<EntityId>(hit->tag))) {
            return;
        }
        portals::PortalSurface surface{hit->faceCenter, hit->normal, {hit->faceAxes[0], hit->faceAxes[1]},
                                       hit->faceHalf};
        glm::mat4 otherPlaced = scene.worldTransform(other);
        bool otherShown = scene.visibleInWorld(other);
        glm::vec2 halfSize = scene.portal(end)->size * 0.5f;
        portals::PlacementOptions options;
        options.freeDistance = [&](const glm::vec3& from, const glm::vec3& direction, float maxDistance) {
            std::optional<physics::RayHit> below = physics.rayCast(from, direction, maxDistance);
            return below ? below->distance : maxDistance;
        };
        for (const Entity& entity : scene.entities()) {  // (other pairs' ends: not covered)
            const PortalComponent* portal = scene.portal(entity.id);
            if (portal != nullptr && entity.id != end && entity.id != other && scene.visibleInWorld(entity.id)) {
                glm::mat4 placedEnd = scene.worldTransform(entity.id);
                for (int axis = 0; axis < 3; axis++) {
                    placedEnd[axis] = glm::vec4(glm::normalize(glm::vec3(placedEnd[axis])), 0.0f);  // (no scale)
                }
                options.keepClearOf.emplace_back(placedEnd, portal->size * 0.5f);
            }
        }
        auto place = [&]() {
            return portals::placePortalEnd(surface, hit->point, hit->direction, halfSize,
                                           otherShown ? &otherPlaced : nullptr, options);
        };
        std::optional<glm::mat4> placed = place();
        if (!placed) {
            return;
        }
        // It becomes a child of what it was put on (it moves with it); its local transform takes that one's scale
        // back out, so it stays the size it is. Turned at an angle to a parent scaled unevenly it would be sheared:
        // then its top goes along the face's nearest axis instead.
        EntityId parent = static_cast<EntityId>(hit->tag);
        if (scene.find(parent) == nullptr || !scene.setParent(end, parent, false)) {
            parent = scene.find(end)->parent;  // (kept where it is in the tree)
        }
        glm::mat4 parentWorld = parent != 0 ? scene.worldTransform(parent) : glm::mat4(1.0f);
        glm::mat4 local = glm::inverse(parentWorld) * *placed;
        if (!rigidColumns(local)) {
            options.topAlongFaceAxis = true;
            placed = place();
            if (!placed) {
                return;
            }
            local = glm::inverse(parentWorld) * *placed;
        }
        Entity& e = *scene.find(end);
        glm::vec3 scale(glm::length(glm::vec3(local[0])), glm::length(glm::vec3(local[1])),
                        glm::length(glm::vec3(local[2])));
        glm::mat3 turn(glm::vec3(local[0]) / scale.x, glm::vec3(local[1]) / scale.y, glm::vec3(local[2]) / scale.z);
        e.local.position = glm::vec3(local[3]);
        e.local.rotation = glm::normalize(glm::quat_cast(turn));
        e.local.scale = scale;
        e.visible = true;
        step.snap(end);
    }

    // Whether a matrix's three columns are at right angles (it is a turn and a scale: no shear)
    static bool rigidColumns(const glm::mat4& m) {
        for (int i = 0; i < 3; i++) {
            glm::vec3 a = glm::normalize(glm::vec3(m[i]));
            glm::vec3 b = glm::normalize(glm::vec3(m[(i + 1) % 3]));
            if (std::abs(glm::dot(a, b)) > 1e-3f) {
                return false;
            }
        }
        return true;
    }

    void removed(GameWorld& world) const {
        world.portalGun = false;
    }
};

}  // namespace

std::unique_ptr<BehaviourListBase> makePortalGunList() {
    return std::make_unique<BehaviourList<PortalGun>>();
}
