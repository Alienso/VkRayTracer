//
// cameraPath: the game flies the camera along the entity's children. See game/Behaviour.h for what a type is made of.
//

#include <algorithm>
#include <cmath>
#include <memory>
#include "game/Behaviour.h"

namespace {

// The point a distance along the path (around and around when it loops; held at the end when it doesn't)
glm::vec3 pointAlong(const std::vector<glm::vec3>& points, bool loop, double distance) {
    size_t segments = loop ? points.size() : points.size() - 1;
    double length = 0.0;
    for (size_t s = 0; s < segments; s++) {
        length += glm::distance(points[s], points[(s + 1) % points.size()]);
    }
    if (length <= 0.0) {
        return points[0];
    }
    distance = loop ? std::fmod(distance, length) : std::min(distance, length);
    for (size_t s = 0; s < segments; s++) {
        const glm::vec3& a = points[s];
        const glm::vec3& b = points[(s + 1) % points.size()];
        double piece = glm::distance(a, b);
        if (distance <= piece || s + 1 == segments) {
            return piece > 0.0 ? glm::mix(a, b, static_cast<float>(std::min(distance / piece, 1.0))) : b;
        }
        distance -= piece;
    }
    return points.back();
}

struct CameraPath {
    static constexpr const char* type = "cameraPath";
    static constexpr const char* help =
        "The game drives the camera: along the entity's children (in their order, where they are at Play) at speed\n"
        "units per second, loop: back to the first; looking ahead along the path or at the entity lookAt (an id).\n"
        "One per game.";
    static constexpr const char* defaults = R"({"speed": 2, "loop": true, "lookAt": 0})";
    static constexpr bool removedWithEntity = false;  // (the points were taken at Play)
    static constexpr bool onSpawnedCopies = true;     // (read for the warnings, not used)

    EntityId entity, lookAt;
    std::vector<glm::vec3> points;  // world positions at Play
    float speed;
    bool loop;

    static std::optional<CameraPath> read(const ReadContext& context, GameProperties& properties) {
        float speed = properties.number("speed", 2.0f);
        bool loop = properties.flag("loop", true);
        EntityId lookAt = context.existing(properties, "lookAt", false);
        std::vector<glm::vec3> points;
        for (EntityId child : context.entity.children) {
            points.push_back(worldPosition(context.scene, child));
        }
        if (context.world.controlsCamera) {
            properties.warn("the camera is already driven (a camera path or a player); ignored");
        } else if (points.size() < 2) {
            properties.warn("needs at least 2 children (the points of the path, in their order); ignored");
        } else if (!context.spawned) {
            context.world.controlsCamera = true;
            return CameraPath{context.entity.id, lookAt, points, speed, loop};
        }
        return std::nullopt;
    }

    void update(const GameStep& step, GameWorld&) const {
        Scene& scene = step.scene;
        Camera& camera = scene.camera();
        double distance = static_cast<double>(speed) * step.time;
        camera.position = pointAlong(points, loop, distance);
        glm::vec3 look = lookAt != 0 && scene.find(lookAt) != nullptr
                         ? worldPosition(scene, lookAt)
                         : pointAlong(points, loop, distance + 1.0);  // a unit ahead
        if (glm::distance(look, camera.position) > 1e-4f) {
            camera.lookAt(look);
        }
    }

    // The path's entity moved while paused: the path (its points taken at Play) goes with it. A point moved on its
    // own isn't followed.
    void moved(const EntityMove& move, GameWorld&) {
        for (glm::vec3& point : points) {
            point = glm::vec3(move.worldChange * glm::vec4(point, 1.0f));
        }
    }

    void removed(GameWorld& world) const { world.controlsCamera = false; }
};

}  // namespace

std::unique_ptr<BehaviourListBase> makeCameraPathList() {
    return std::make_unique<BehaviourList<CameraPath>>();
}
