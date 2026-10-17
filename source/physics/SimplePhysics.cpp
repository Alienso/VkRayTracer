//
// Our own PhysicsWorld. See SimplePhysics.h.
//

#include <algorithm>
#include <cmath>
#include "physics/Collision.h"
#include "physics/SimplePhysics.h"

namespace physics {

namespace {

glm::vec3 horizontal(const glm::vec3& v) {
    return glm::vec3(v.x, 0.0f, v.z);
}

// The character's capsule segment (feet: its lowest point)
void segment(const CharacterSettings& settings, const glm::vec3& feet, glm::vec3& a, glm::vec3& b) {
    a = feet + glm::vec3(0.0f, settings.shape.radius, 0.0f);
    b = feet + glm::vec3(0.0f, settings.shape.radius + 2.0f * settings.shape.halfHeight, 0.0f);
}

bool boundsMeet(const glm::vec3& minA, const glm::vec3& maxA, const glm::vec3& minB, const glm::vec3& maxB) {
    return !glm::any(glm::lessThan(maxA, minB)) && !glm::any(glm::greaterThan(minA, maxB));
}

Transform compose(const Transform& parent, const Transform& child) {
    return {parent.position + parent.rotation * child.position, glm::normalize(parent.rotation * child.rotation)};
}

// Part way from a to b: the position in a line, the turn the short way
Transform between(const Transform& a, const Transform& b, float t) {
    return {glm::mix(a.position, b.position, t), glm::normalize(glm::slerp(a.rotation, b.rotation, t))};
}

float angleBetween(const glm::quat& a, const glm::quat& b) {
    return 2.0f * std::acos(std::min(1.0f, std::abs(glm::dot(a, b))));
}

}  // namespace

std::unique_ptr<PhysicsWorld> makeSimplePhysicsWorld() {
    return std::make_unique<SimplePhysicsWorld>();
}

// ---------------------------------------------------------------------------------------------
// Bodies

void SimplePhysicsWorld::place(Body& body, const Transform& transform) {
    body.transform = transform;
    body.solid = {transform.position + transform.rotation * body.offset, transform.rotation};
    boxBounds(body.solid, body.halfExtents, body.min, body.max);
}

BodyId SimplePhysicsWorld::addBox(const BoxShape& shape, const Transform& transform) {
    Body body;
    body.halfExtents = glm::abs(shape.halfExtents);
    body.offset = glm::vec3(0.0f);
    for (int axis = 0; axis < 3; axis++) {
        if (body.halfExtents[axis] < minHalfThickness) {
            if (axis == 1) {  // (the top stays where it was)
                body.offset.y = body.halfExtents.y - minHalfThickness;
            }
            body.halfExtents[axis] = minHalfThickness;
        }
    }
    body.alive = true;
    place(body, transform);
    uint32_t index;
    if (!freeBodies.empty()) {
        index = freeBodies.back();
        freeBodies.pop_back();
        bodies[index] = body;
    } else {
        index = static_cast<uint32_t>(bodies.size());
        bodies.push_back(body);
    }
    liveBodies++;
    return BodyId{index + 1};
}

SimplePhysicsWorld::Body* SimplePhysicsWorld::findBody(BodyId id) {
    if (!id.valid() || id.value > bodies.size() || !bodies[id.value - 1].alive) {
        return nullptr;
    }
    return &bodies[id.value - 1];
}

void SimplePhysicsWorld::setTransform(BodyId id, const Transform& transform) {
    if (Body* body = findBody(id)) {
        place(*body, transform);
    }
}

void SimplePhysicsWorld::setEnabled(BodyId id, bool enabled) {
    if (Body* body = findBody(id)) {
        body->enabled = enabled;
    }
}

void SimplePhysicsWorld::remove(BodyId id) {
    if (Body* body = findBody(id)) {
        body->alive = false;
        freeBodies.push_back(id.value - 1);
        liveBodies--;
    }
}

bool SimplePhysicsWorld::blockedAt(const std::vector<Mover>& movers, const Transform& pivot, const MoveSkips& skips,
                                   glm::vec3& normal) {
    for (const Mover& mover : movers) {
        place(*mover.body, compose(pivot, mover.relative));
    }
    float deepest = contactSkin;
    bool blocked = false;
    for (const Mover& mover : movers) {
        if (!mover.body->enabled) {
            continue;
        }
        for (size_t i = 0; i < bodies.size(); i++) {
            const Body& other = bodies[i];
            if (skips.bodies[i] || !other.alive || !other.enabled ||
                !boundsMeet(mover.body->min, mover.body->max, other.min, other.max)) {
                continue;
            }
            std::optional<Contact> contact =
                boxBoxContact(mover.body->solid, mover.body->halfExtents, other.solid, other.halfExtents);
            if (contact && contact->depth > deepest) {
                deepest = contact->depth;
                normal = contact->normal;
                blocked = true;
            }
        }
    }
    if (blocked) {
        return true;
    }
    for (size_t i = 0; i < characters.size(); i++) {
        if (characters[i].alive && !skips.characters[i] && squeezed(characters[i], movers)) {
            normal = glm::vec3(0.0f);
            return true;
        }
    }
    return false;
}

float SimplePhysicsWorld::sweepGroup(const std::vector<Mover>& movers, const Transform& a, const Transform& b,
                                     const MoveSkips& skips, bool& blocked, glm::vec3& normal) {
    // Tried at points no further apart than the thinnest mover's half size: none passes through a body between them
    float reach = 0.0f;
    float thinnest = INFINITY;
    for (const Mover& mover : movers) {
        const glm::vec3& half = mover.body->halfExtents;
        reach = std::max(reach, glm::length(mover.relative.position) + glm::length(half));
        thinnest = std::min(thinnest, std::min(half.x, std::min(half.y, half.z)));
    }
    float travel = glm::length(b.position - a.position) + angleBetween(a.rotation, b.rotation) * reach;
    int points = std::clamp(static_cast<int>(std::ceil(travel / thinnest)), 1, 64);
    float free = 0.0f;
    for (int k = 1; k <= points; k++) {
        float t = static_cast<float>(k) / static_cast<float>(points);
        glm::vec3 found;
        if (!blockedAt(movers, between(a, b, t), skips, found)) {
            free = t;
            continue;
        }
        // Between the last free point and this blocked one
        normal = found;
        float low = free;
        float high = t;
        for (int i = 0; i < 8; i++) {
            float middle = 0.5f * (low + high);
            if (blockedAt(movers, between(a, b, middle), skips, found)) {
                high = middle;
                normal = found;
            } else {
                low = middle;
            }
        }
        blocked = true;
        return low;
    }
    blocked = false;
    return 1.0f;
}

GroupMove SimplePhysicsWorld::moveBodies(const std::vector<BodyId>& group, const Transform& from,
                                         const Transform& to) {
    GroupMove result{to, false};
    MoveSkips skips{std::vector<char>(bodies.size(), 0), std::vector<char>(characters.size(), 0)};
    std::vector<Mover> movers;
    glm::quat toGroup = glm::conjugate(from.rotation);
    for (BodyId id : group) {
        if (Body* body = findBody(id)) {
            skips.bodies[id.value - 1] = 1;
            movers.push_back({body, {toGroup * (body->transform.position - from.position),
                                     glm::normalize(toGroup * body->transform.rotation)}});
        }
    }
    if (movers.empty()) {
        return result;
    }

    // What they are in already (as they are now) doesn't block them
    for (const Mover& mover : movers) {
        if (!mover.body->enabled) {
            continue;
        }
        for (size_t i = 0; i < bodies.size(); i++) {
            const Body& other = bodies[i];
            if (skips.bodies[i] || !other.alive || !other.enabled ||
                !boundsMeet(mover.body->min, mover.body->max, other.min, other.max)) {
                continue;
            }
            std::optional<Contact> contact =
                boxBoxContact(mover.body->solid, mover.body->halfExtents, other.solid, other.halfExtents);
            if (contact && contact->depth > contactSkin) {
                skips.bodies[i] = 1;
            }
        }
    }
    for (size_t i = 0; i < characters.size(); i++) {
        if (characters[i].alive && squeezed(characters[i], movers)) {
            skips.characters[i] = 1;
        }
    }

    bool blocked = false;
    glm::vec3 normal(0.0f);
    float t = sweepGroup(movers, from, to, skips, blocked, normal);
    if (blocked) {
        result.blocked = true;
        Transform reached = between(from, to, t);
        // Slides: what is left of the straight move, without its part into what stopped it (a squeezed character
        // gives no normal: it only stops)
        for (int slide = 0; slide < 2 && normal != glm::vec3(0.0f); slide++) {
            glm::vec3 left = to.position - reached.position;
            float into = glm::dot(left, normal);
            if (into < 0.0f) {
                left -= into * normal;
            }
            if (glm::length(left) < 1e-6f) {
                break;
            }
            Transform target{reached.position + left, reached.rotation};
            bool again = false;
            glm::vec3 next(0.0f);
            float s = sweepGroup(movers, reached, target, skips, again, next);
            reached = again ? between(reached, target, s) : target;
            if (!again) {
                break;
            }
            normal = next;
        }
        result.reached = reached;
    }
    for (const Mover& mover : movers) {
        place(*mover.body, compose(result.reached, mover.relative));
    }
    return result;
}

// ---------------------------------------------------------------------------------------------
// Characters

CharacterId SimplePhysicsWorld::addCharacter(const CharacterSettings& settings, const glm::vec3& feet) {
    Character character{settings, CharacterState{feet}, true};
    uint32_t index;
    if (!freeCharacters.empty()) {
        index = freeCharacters.back();
        freeCharacters.pop_back();
        characters[index] = character;
    } else {
        index = static_cast<uint32_t>(characters.size());
        characters.push_back(character);
    }
    return CharacterId{index + 1};
}

SimplePhysicsWorld::Character* SimplePhysicsWorld::findCharacter(CharacterId id) {
    if (!id.valid() || id.value > characters.size() || !characters[id.value - 1].alive) {
        return nullptr;
    }
    return &characters[id.value - 1];
}

const SimplePhysicsWorld::Character* SimplePhysicsWorld::findCharacter(CharacterId id) const {
    return const_cast<SimplePhysicsWorld*>(this)->findCharacter(id);
}

void SimplePhysicsWorld::removeCharacter(CharacterId id) {
    if (Character* character = findCharacter(id)) {
        character->alive = false;
        freeCharacters.push_back(id.value - 1);
    }
}

CharacterState SimplePhysicsWorld::character(CharacterId id) const {
    const Character* character = findCharacter(id);
    return character != nullptr ? character->state : CharacterState();
}

void SimplePhysicsWorld::teleport(CharacterId id, const glm::vec3& feet, const glm::vec3& velocity) {
    if (Character* character = findCharacter(id)) {
        character->state = CharacterState{feet, velocity, false};
    }
}

void SimplePhysicsWorld::resolve(const CharacterSettings& settings, Moving& moving, int rounds) const {
    float radius = settings.shape.radius;
    float minGroundY = std::cos(glm::radians(settings.maxSlopeDegrees)) - 1e-4f;
    for (int round = 0; round < rounds; round++) {
        glm::vec3 a, b;
        segment(settings, moving.feet, a, b);
        glm::vec3 min = moving.feet - glm::vec3(radius, 0.0f, radius);
        glm::vec3 max = moving.feet + glm::vec3(radius, 2.0f * (settings.shape.halfHeight + radius), radius);
        Contact deepest;
        bool found = false;
        for (const Body& body : bodies) {
            if (!body.alive || !body.enabled || glm::any(glm::lessThan(body.max, min)) ||
                glm::any(glm::greaterThan(body.min, max))) {
                continue;
            }
            std::optional<Contact> contact = capsuleBoxContact(a, b, radius, body.solid, body.halfExtents);
            if (contact && (!found || contact->depth > deepest.depth)) {
                deepest = *contact;
                found = true;
            }
        }
        if (!found) {
            return;
        }
        glm::vec3 n = deepest.normal;
        if (n.y >= minGroundY) {
            // Ground: straight up, so standing on a slope doesn't slide down it
            moving.feet.y += deepest.depth / n.y;
            moving.ground = true;
            moving.groundNormal = n;
            moving.velocity.y = std::max(moving.velocity.y, 0.0f);
            continue;
        }
        glm::vec3 flat = horizontal(n);
        float flatLength = glm::length(flat);
        if (n.y > 0.0f && flatLength > 1e-4f) {
            // Too steep to stand on: only sideways, so it can't be climbed (and is slid down, falling)
            n = flat / flatLength;
            moving.feet += n * (deepest.depth / flatLength);
        } else {
            moving.feet += n * deepest.depth;  // a wall, a ceiling
        }
        float into = glm::dot(moving.velocity, n);
        if (into < 0.0f) {
            moving.velocity -= into * n;
        }
        moving.wall = moving.wall || flatLength > 0.1f;
    }
}

float SimplePhysicsWorld::deepestOverlap(const CharacterSettings& settings, const glm::vec3& feet) const {
    float radius = settings.shape.radius;
    glm::vec3 a, b;
    segment(settings, feet, a, b);
    glm::vec3 min = feet - glm::vec3(radius, 0.0f, radius);
    glm::vec3 max = feet + glm::vec3(radius, 2.0f * (settings.shape.halfHeight + radius), radius);
    float deepest = 0.0f;
    for (const Body& body : bodies) {
        if (body.alive && body.enabled && boundsMeet(body.min, body.max, min, max)) {
            if (std::optional<Contact> contact = capsuleBoxContact(a, b, radius, body.solid, body.halfExtents)) {
                deepest = std::max(deepest, contact->depth);
            }
        }
    }
    return deepest;
}

bool SimplePhysicsWorld::squeezed(const Character& character, const std::vector<Mover>& movers) const {
    const CharacterSettings& settings = character.settings;
    glm::vec3 a, b;
    segment(settings, character.state.feet, a, b);
    bool touches = false;
    for (const Mover& mover : movers) {
        if (mover.body->enabled) {
            std::optional<Contact> contact =
                capsuleBoxContact(a, b, settings.shape.radius, mover.body->solid, mover.body->halfExtents);
            touches = touches || (contact && contact->depth > contactSkin);
        }
    }
    if (!touches) {
        return false;
    }
    Moving trial{character.state.feet, glm::vec3(0.0f)};
    resolve(settings, trial, 8);
    return deepestOverlap(settings, trial.feet) > squeezeTolerance;
}

SimplePhysicsWorld::Moving SimplePhysicsWorld::sweep(const CharacterSettings& settings, Moving moving,
                                                     const glm::vec3& delta) const {
    float maxStep = 0.5f * settings.shape.radius;
    int steps = std::max(1, static_cast<int>(std::ceil(glm::length(delta) / maxStep)));
    for (int i = 0; i < steps; i++) {
        moving.feet += delta / static_cast<float>(steps);
        moving.ground = false;  // (what the last substep touched)
        resolve(settings, moving);
    }
    return moving;
}

CharacterState SimplePhysicsWorld::moveCharacter(CharacterId id, const glm::vec3& wantedVelocity, float jumpSpeed,
                                                 const glm::vec3& gravity, float seconds) {
    Character* character = findCharacter(id);
    if (character == nullptr) {
        return CharacterState();
    }
    const CharacterSettings& settings = character->settings;
    const CharacterState& start = character->state;

    // The velocity: walking at once on the ground, steering towards it in the air; a jump; gravity
    glm::vec3 velocity = start.velocity;
    glm::vec3 wanted = horizontal(wantedVelocity);
    if (start.onGround) {
        velocity = glm::vec3(wanted.x, velocity.y, wanted.z);
    } else {
        glm::vec3 change = wanted - horizontal(velocity);
        float most = settings.airAcceleration * seconds;
        if (glm::length(change) > most) {
            change *= most / glm::length(change);
        }
        velocity += change;
    }
    bool jumped = start.onGround && jumpSpeed > 0.0f;
    if (jumped) {
        velocity.y = jumpSpeed;
    }
    velocity += gravity * seconds;

    // The move, a substep at a time; a substep stopped by a wall while walking is tried again over a step
    glm::vec3 delta = velocity * seconds;
    float maxStep = 0.5f * settings.shape.radius;
    int steps = std::max(1, static_cast<int>(std::ceil(glm::length(delta) / maxStep)));
    glm::vec3 part = delta / static_cast<float>(steps);
    glm::vec3 partFlat = horizontal(part);
    bool walking = start.onGround && !jumped && glm::length(partFlat) > 1e-6f;
    Moving moving{start.feet, velocity};
    for (int i = 0; i < steps; i++) {
        Moving plain = moving;
        plain.wall = false;
        plain = sweep(settings, plain, part);
        if (walking && plain.wall) {
            // Raised, then a radius further than the substep (the capsule's round bottom has to get past the
            // ledge's edge to stand on it), then down: landing higher than it was, it steps up. It keeps only the
            // substep's own advance, at the height it landed on.
            glm::vec3 direction = glm::normalize(partFlat);
            Moving up = sweep(settings, moving, glm::vec3(0.0f, settings.stepHeight, 0.0f));
            float risen = up.feet.y - moving.feet.y;
            Moving across = sweep(settings, up, partFlat + direction * settings.shape.radius);
            Moving down = sweep(settings, across, glm::vec3(0.0f, -risen - std::max(-part.y, 0.0f), 0.0f));
            float stepped = glm::dot(horizontal(down.feet - moving.feet), direction);
            float blocked = glm::dot(horizontal(plain.feet - moving.feet), direction);
            if (down.ground && down.feet.y > moving.feet.y + 1e-4f && stepped > blocked + 1e-5f) {
                Moving raised = moving;
                raised.feet = glm::vec3(moving.feet.x + partFlat.x, down.feet.y, moving.feet.z + partFlat.z);
                raised.velocity = glm::vec3(velocity.x, 0.0f, velocity.z);
                raised.wall = false;
                resolve(settings, raised);
                raised.ground = true;
                raised.groundNormal = down.groundNormal;
                plain = raised;
            }
        }
        moving = plain;
    }
    // Walking down a slope or a low ledge: kept on the ground when it is within a step straight below (a probe that
    // slid off an edge sideways isn't kept: it falls instead)
    if (start.onGround && !jumped && !moving.ground) {
        Moving probe = sweep(settings, moving, glm::vec3(0.0f, -settings.stepHeight, 0.0f));
        if (probe.ground && glm::length(horizontal(probe.feet - moving.feet)) < 1e-3f) {
            moving = probe;
        }
    }

    character->state = CharacterState{moving.feet, moving.velocity, moving.ground, moving.groundNormal};
    return character->state;
}

bool SimplePhysicsWorld::overlapsCapsule(const CapsuleShape& shape, const Transform& transform) const {
    glm::vec3 axis = transform.rotation * glm::vec3(0.0f, shape.halfHeight, 0.0f);
    glm::vec3 a = transform.position - axis;
    glm::vec3 b = transform.position + axis;
    glm::vec3 min = glm::min(a, b) - glm::vec3(shape.radius);
    glm::vec3 max = glm::max(a, b) + glm::vec3(shape.radius);
    for (const Body& body : bodies) {
        if (!body.alive || !body.enabled || glm::any(glm::lessThan(body.max, min)) ||
            glm::any(glm::greaterThan(body.min, max))) {
            continue;
        }
        if (capsuleBoxContact(a, b, shape.radius, body.solid, body.halfExtents)) {
            return true;
        }
    }
    return false;
}

void SimplePhysicsWorld::clear() {
    bodies.clear();
    freeBodies.clear();
    liveBodies = 0;
    characters.clear();
    freeCharacters.clear();
}

}  // namespace physics
