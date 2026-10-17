//
// player: the camera as a body that walks, runs, jumps and falls among the colliders (PHYSICS_PLAN.md). See
// game/Behaviour.h for what a type is made of.
//

#include <algorithm>
#include <cmath>
#include <memory>
#include "game/Behaviour.h"

namespace {

struct Player {
    static constexpr const char* type = "player";
    static constexpr const char* help =
        "The player: a body (radius, height) starting at the entity, the camera at its eyes (eyeHeight). The mouse\n"
        "turns the view (sensitivity: degrees per pixel), W/A/S/D walk at speed (Shift: runSpeed), Space jumps\n"
        "(jumpSpeed, gravity); slopes up to maxSlope degrees and ledges up to stepHeight are walked up. Falling below\n"
        "respawnBelow (y) puts it back at the start. The entity follows it. One per game.";
    static constexpr const char* defaults =
        R"({"radius": 0.3, "height": 1.8, "eyeHeight": 1.65, "speed": 4, "runSpeed": 7, "jumpSpeed": 5,)"
        R"( "gravity": 9.81, "maxSlope": 45, "stepHeight": 0.3, "respawnBelow": -50, "sensitivity": 0.1})";
    static constexpr bool removedWithEntity = true;
    static constexpr bool onSpawnedCopies = false;  // (one player)

    EntityId entity;
    physics::CharacterId body;
    float eyeHeight, speed, runSpeed, jumpSpeed, gravity, respawnBelow, sensitivity;
    glm::vec3 start;  // the feet at Play
    float yaw, pitch; // the view, degrees (as Camera's)

    static std::optional<Player> read(const ReadContext& context, GameProperties& properties) {
        float radius = std::max(properties.number("radius", 0.3f), 0.01f);
        float height = properties.number("height", 1.8f);
        physics::CharacterSettings settings;
        settings.shape = {radius, std::max(0.5f * height - radius, 0.0f)};
        settings.maxSlopeDegrees = properties.number("maxSlope", 45.0f);
        settings.stepHeight = std::max(properties.number("stepHeight", 0.3f), 0.0f);
        Player player{context.entity.id,
                      physics::CharacterId(),
                      properties.number("eyeHeight", 1.65f),
                      properties.number("speed", 4.0f),
                      properties.number("runSpeed", 7.0f),
                      properties.number("jumpSpeed", 5.0f),
                      properties.number("gravity", 9.81f),
                      properties.number("respawnBelow", -50.0f),
                      properties.number("sensitivity", 0.1f),
                      worldPosition(context.scene, context.entity.id),
                      context.scene.camera().yaw,
                      context.scene.camera().pitch};
        if (context.world.controlsCamera) {
            properties.warn("the camera is already driven (a camera path or a player); ignored");
            return std::nullopt;
        }
        if (context.world.physics == nullptr) {
            return std::nullopt;
        }
        player.body = context.world.physics->addCharacter(settings, player.start);
        context.world.controlsCamera = true;
        context.world.mouseLook = true;
        return player;
    }

    void update(const GameStep& step, GameWorld& world) {
        Scene& scene = step.scene;
        if (world.physics == nullptr || scene.find(entity) == nullptr) {
            return;
        }
        const InputState& input = step.input;
        if (input.mouse) {
            yaw += input.mouseDelta.x * sensitivity;
            pitch = std::clamp(pitch - input.mouseDelta.y * sensitivity, -89.0f, 89.0f);
        }
        // Walking on the ground plane, relative to the view (as Camera::forward / right)
        float yawRadians = glm::radians(yaw);
        glm::vec3 forward(std::sin(yawRadians), 0.0f, -std::cos(yawRadians));
        glm::vec3 right(std::cos(yawRadians), 0.0f, std::sin(yawRadians));
        glm::vec3 wanted(0.0f);
        float jump = 0.0f;
        if (input.keyboard) {
            wanted = forward * static_cast<float>(input.isDown(Key::W) - input.isDown(Key::S)) +
                     right * static_cast<float>(input.isDown(Key::D) - input.isDown(Key::A));
            if (glm::length(wanted) > 0.0f) {
                wanted = glm::normalize(wanted) * (input.isDown(Key::LeftShift) ? runSpeed : speed);
            }
            jump = input.wasPressed(Key::Space) ? jumpSpeed : 0.0f;
        }
        physics::CharacterState state = world.physics->moveCharacter(body, wanted, jump, glm::vec3(0.0f, -gravity, 0.0f),
                                                                     static_cast<float>(step.seconds));
        if (state.feet.y < respawnBelow) {
            world.physics->teleport(body, start, glm::vec3(0.0f));
            state.feet = start;
            step.snap(entity);
            step.snapCamera();
        }
        Camera& camera = scene.camera();
        camera.position = state.feet + glm::vec3(0.0f, eyeHeight, 0.0f);
        camera.yaw = yaw;
        camera.pitch = pitch;
        follow(scene, state.feet);
    }

    // The entity at the feet, turned to the view (a model under it shows the player)
    void follow(Scene& scene, const glm::vec3& feet) const {
        Entity& e = *scene.find(entity);
        glm::quat turn = glm::angleAxis(glm::radians(-yaw), glm::vec3(0.0f, 1.0f, 0.0f));  // -Z to forward
        if (e.parent == 0) {
            e.local.position = feet;
            e.local.rotation = turn;
            return;
        }
        glm::mat4 parent = scene.worldTransform(e.parent);
        e.local.position = glm::vec3(glm::inverse(parent) * glm::vec4(feet, 1.0f));
        glm::mat3 parentTurn(glm::normalize(glm::vec3(parent[0])), glm::normalize(glm::vec3(parent[1])),
                             glm::normalize(glm::vec3(parent[2])));
        e.local.rotation = glm::normalize(glm::inverse(glm::quat_cast(parentTurn)) * turn);
    }

    // Put somewhere else by hand while paused: the body goes there (still; the respawn point stays)
    void moved(const EntityMove& move, GameWorld& world) const {
        if (world.physics != nullptr) {
            glm::vec3 feet = world.physics->character(body).feet;
            world.physics->teleport(body, glm::vec3(move.worldChange * glm::vec4(feet, 1.0f)), glm::vec3(0.0f));
        }
    }

    void removed(GameWorld& world) const {
        if (world.physics != nullptr) {
            world.physics->removeCharacter(body);
        }
        world.controlsCamera = false;
        world.mouseLook = false;
    }
};

}  // namespace

std::unique_ptr<BehaviourListBase> makePlayerList() {
    return std::make_unique<BehaviourList<Player>>();
}
