//
// Our own PhysicsWorld. See SimplePhysics.h.
//

#include <algorithm>
#include <cmath>
#include "physics/Collision.h"
#include "physics/SimplePhysics.h"
#include "portals/PortalTransform.h"

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

glm::vec3 apply(const glm::mat4& m, const glm::vec3& p) {
    return glm::vec3(m * glm::vec4(p, 1.0f));
}

// A transform taken through a rigid portal transform
Transform through(const glm::mat4& m, const Transform& t) {
    return {apply(m, t.position), glm::normalize(glm::quat_cast(glm::mat3(m)) * t.rotation)};
}

bool sameOpenings(const std::vector<portals::PortalOpening>& a, const std::vector<portals::PortalOpening>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i].end != b[i].end || a[i].halfSize != b[i].halfSize || a[i].ellipse != b[i].ellipse) {
            return false;
        }
    }
    return true;
}

const portals::PortalPiece wholePiece;  // where a shape with no pieces is: whole, where it is

}  // namespace

std::unique_ptr<PhysicsWorld> makeSimplePhysicsWorld() {
    return std::make_unique<SimplePhysicsWorld>();
}

// ---------------------------------------------------------------------------------------------
// Bodies

void SimplePhysicsWorld::place(Body& body, const Transform& transform, const glm::vec3& anchor) const {
    body.transform = transform;
    body.solid = {transform.position + transform.rotation * body.offset, transform.rotation};
    body.anchor = anchor;
    boxBounds(body.solid, body.halfExtents, body.min, body.max);
    piecesOf({body.solid.position, glm::mat3_cast(body.solid.rotation), body.halfExtents}, anchor, openings,
             body.pieces);
}

glm::vec3 SimplePhysicsWorld::anchorAfter(const glm::vec3& anchor, const glm::vec3& position) const {
    glm::mat4 crossing;
    return portals::crossOpenings(openings, anchor, position, 1, crossing) > 0 ? anchor : position;
}

void SimplePhysicsWorld::setPortals(const std::vector<portals::PortalOpening>& newOpenings, int maxDepth) {
    maxDepth = std::max(maxDepth, 1);
    if (sameOpenings(openings, newOpenings) && maxDepth == portalDepth) {
        return;
    }
    openings = newOpenings;
    portalDepth = maxDepth;
    grownSlack = -1.0f;
    for (Body& body : bodies) {
        if (body.alive) {
            place(body, body.transform, body.anchor);
        }
    }
}

const std::vector<portals::PortalOpening>& SimplePhysicsWorld::openingsFor(const CharacterSettings& settings) const {
    if (settings.portalSlack <= 0.0f) {
        return openings;
    }
    if (grownSlack != settings.portalSlack) {
        grownOpenings = openings;
        for (portals::PortalOpening& opening : grownOpenings) {
            opening.halfSize += glm::vec2(settings.portalSlack);
        }
        grownSlack = settings.portalSlack;
    }
    return grownOpenings;
}

void SimplePhysicsWorld::piecesOf(const portals::OrientedBox& box, const glm::vec3& anchor,
                                  const std::vector<portals::PortalOpening>& within,
                                  std::vector<portals::PortalPiece>& pieces) const {
    pieces.clear();
    // (Only near an opening: within their bounding spheres)
    bool near = false;
    float reach = glm::length(box.half);
    for (size_t i = 0; i < within.size() && !near; i++) {
        near = glm::distance(box.center, glm::vec3(within[i].end[3])) <= reach + glm::length(within[i].halfSize);
    }
    if (!near) {
        return;
    }
    portals::portalPieces(box, anchor, within, portalDepth, maxPieces, pieces);
    if (pieces.size() == 1 && pieces[0].cutCount == 0) {
        pieces.clear();
    }
}

SimplePhysicsWorld::Capsule SimplePhysicsWorld::capsuleAt(const glm::vec3& a, const glm::vec3& b, float radius,
                                                          const glm::vec3& anchor,
                                                          const std::vector<portals::PortalOpening>& within) const {
    Capsule capsule{a, b, radius, glm::min(a, b) - glm::vec3(radius), glm::max(a, b) + glm::vec3(radius), {}};
    if (!openings.empty()) {
        glm::vec3 axis = b - a;
        float length = glm::length(axis);
        portals::OrientedBox box;
        box.center = 0.5f * (a + b);
        if (length > 1e-6f) {  // (its own axes: the segment along Y)
            glm::vec3 y = axis / length;
            glm::vec3 x = std::abs(y.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
            glm::vec3 z = glm::normalize(glm::cross(x, y));
            box.axes = glm::mat3(glm::cross(y, z), y, z);
        }
        box.half = glm::vec3(radius, 0.5f * length + radius, radius);
        piecesOf(box, anchor, within, capsule.pieces);
    }
    return capsule;
}

SimplePhysicsWorld::Capsule SimplePhysicsWorld::characterAt(const CharacterSettings& settings,
                                                            const glm::vec3& feet) const {
    glm::vec3 a, b;
    segment(settings, feet, a, b);
    return capsuleAt(a, b, settings.shape.radius, feet + glm::vec3(0.0f, settings.anchorHeight, 0.0f),
                     openingsFor(settings));
}

std::optional<Contact> SimplePhysicsWorld::contact(const Capsule& capsule, const Body& body) const {
    if (capsule.pieces.empty() && body.pieces.empty()) {
        if (!boundsMeet(capsule.min, capsule.max, body.min, body.max)) {
            return std::nullopt;
        }
        return capsuleBoxContact(capsule.a, capsule.b, capsule.radius, body.solid, body.halfExtents);
    }
    // Each piece of the capsule against each of the body's, where both are kept
    std::optional<Contact> deepest;
    size_t capsulePieces = std::max<size_t>(capsule.pieces.size(), 1);
    size_t bodyPieces = std::max<size_t>(body.pieces.size(), 1);
    for (size_t i = 0; i < capsulePieces; i++) {
        const portals::PortalPiece& mine = capsule.pieces.empty() ? wholePiece : capsule.pieces[i];
        glm::vec3 a = i == 0 ? capsule.a : apply(mine.transform, capsule.a);
        glm::vec3 b = i == 0 ? capsule.b : apply(mine.transform, capsule.b);
        glm::vec3 min = glm::min(a, b) - glm::vec3(capsule.radius);
        glm::vec3 max = glm::max(a, b) + glm::vec3(capsule.radius);
        for (size_t j = 0; j < bodyPieces; j++) {
            const portals::PortalPiece& its = body.pieces.empty() ? wholePiece : body.pieces[j];
            Transform solid = body.solid;
            glm::vec3 boxMin = body.min;
            glm::vec3 boxMax = body.max;
            if (j > 0) {
                solid = through(its.transform, body.solid);
                boxBounds(solid, body.halfExtents, boxMin, boxMax);
            }
            if (!boundsMeet(min, max, boxMin, boxMax)) {
                continue;
            }
            std::optional<Contact> found = capsuleBoxContact(a, b, capsule.radius, solid, body.halfExtents);
            if (!found) {
                continue;
            }
            glm::vec3 middle = found->point - found->normal * (0.5f * found->depth);
            if (!portals::pieceKeeps(mine, middle) || !portals::pieceKeeps(its, middle)) {
                continue;
            }
            if (i > 0) {  // back to where the capsule really is
                glm::mat4 back = glm::inverse(mine.transform);
                found->normal = glm::normalize(glm::mat3(back) * found->normal);
                found->point = apply(back, found->point);
            }
            if (!deepest || found->depth > deepest->depth) {
                deepest = found;
            }
        }
    }
    return deepest;
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
    place(body, transform, transform.position);
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
        place(*body, transform, anchorAfter(body->anchor, transform.position));
    }
}

void SimplePhysicsWorld::setEnabled(BodyId id, bool enabled) {
    if (Body* body = findBody(id)) {
        body->enabled = enabled;
    }
}

void SimplePhysicsWorld::setTag(BodyId id, uint64_t tag) {
    if (Body* body = findBody(id)) {
        body->tag = tag;
    }
}

void SimplePhysicsWorld::remove(BodyId id) {
    if (Body* body = findBody(id)) {
        body->alive = false;
        freeBodies.push_back(id.value - 1);
        liveBodies--;
    }
}

std::optional<Contact> SimplePhysicsWorld::boxContact(const Body& a, const Body& b, bool own) const {
    if (a.pieces.empty() && b.pieces.empty()) {
        if (own || !boundsMeet(a.min, a.max, b.min, b.max)) {
            return std::nullopt;
        }
        return boxBoxContact(a.solid, a.halfExtents, b.solid, b.halfExtents);
    }
    // Each piece of a against each of b's, judged at the contact's points: the deepest one both keep
    std::optional<Contact> deepest;
    size_t piecesA = std::max<size_t>(a.pieces.size(), 1);
    size_t piecesB = std::max<size_t>(b.pieces.size(), 1);
    std::vector<glm::vec3> points;
    std::vector<float> depths;
    for (size_t i = 0; i < piecesA; i++) {
        const portals::PortalPiece& pieceA = a.pieces.empty() ? wholePiece : a.pieces[i];
        Transform solidA = a.solid;
        glm::vec3 minA = a.min;
        glm::vec3 maxA = a.max;
        if (i > 0) {
            solidA = through(pieceA.transform, a.solid);
            boxBounds(solidA, a.halfExtents, minA, maxA);
        }
        for (size_t j = 0; j < piecesB; j++) {
            const portals::PortalPiece& pieceB = b.pieces.empty() ? wholePiece : b.pieces[j];
            if (own && pieceA.transform == pieceB.transform) {
                continue;  // (rigid together)
            }
            Transform solidB = b.solid;
            glm::vec3 minB = b.min;
            glm::vec3 maxB = b.max;
            if (j > 0) {
                solidB = through(pieceB.transform, b.solid);
                boxBounds(solidB, b.halfExtents, minB, maxB);
            }
            if (!boundsMeet(minA, maxA, minB, maxB)) {
                continue;
            }
            std::optional<Contact> found =
                boxBoxContact(solidA, a.halfExtents, solidB, b.halfExtents, &points, &depths);
            if (!found) {
                continue;
            }
            if (pieceA.cutCount > 0 || pieceB.cutCount > 0) {
                int kept = -1;
                for (size_t k = 0; k < points.size(); k++) {
                    glm::vec3 middle = points[k] - found->normal * (0.5f * depths[k]);
                    if ((kept < 0 || depths[k] > depths[kept]) && portals::pieceKeeps(pieceA, middle) &&
                        portals::pieceKeeps(pieceB, middle)) {
                        kept = static_cast<int>(k);
                    }
                }
                if (kept < 0) {
                    continue;
                }
                found->point = points[kept];
                found->depth = std::min(found->depth, depths[kept]);
            }
            if (i > 0) {  // back to where a really is
                glm::mat4 back = glm::inverse(pieceA.transform);
                found->normal = glm::normalize(glm::mat3(back) * found->normal);
                found->point = apply(back, found->point);
            }
            if (!deepest || found->depth > deepest->depth) {
                deepest = found;
            }
        }
    }
    return deepest;
}

std::optional<Contact> SimplePhysicsWorld::ownContact(const std::vector<Mover>& movers) const {
    bool cut = false;
    for (const Mover& mover : movers) {
        cut = cut || !mover.body->pieces.empty();
    }
    if (!cut) {
        return std::nullopt;  // (all whole: rigid together)
    }
    std::optional<Contact> deepest;
    for (size_t m = 0; m < movers.size(); m++) {
        for (size_t n = m; n < movers.size(); n++) {
            if (!movers[m].body->enabled || !movers[n].body->enabled) {
                continue;
            }
            std::optional<Contact> found = boxContact(*movers[m].body, *movers[n].body, true);
            if (found && (!deepest || found->depth > deepest->depth)) {
                deepest = found;
            }
        }
    }
    return deepest;
}

bool SimplePhysicsWorld::blockedAt(const std::vector<Mover>& movers, const Transform& pivot, const MoveSkips& skips,
                                   glm::vec3& normal) {
    for (const Mover& mover : movers) {
        place(*mover.body, compose(pivot, mover.relative), mover.anchor);
    }
    float deepest = contactSkin;
    bool blocked = false;
    for (const Mover& mover : movers) {
        if (!mover.body->enabled) {
            continue;
        }
        for (size_t i = 0; i < bodies.size(); i++) {
            const Body& other = bodies[i];
            if (skips.bodies[i] || !other.alive || !other.enabled) {
                continue;
            }
            std::optional<Contact> touching = boxContact(*mover.body, other, false);
            if (touching && touching->depth > deepest) {
                deepest = touching->depth;
                normal = touching->normal;
                blocked = true;
            }
        }
    }
    if (blocked) {
        return true;
    }
    if (!skips.own) {
        // Into its own other pieces (out of a portal into itself): it only stops
        std::optional<Contact> own = ownContact(movers);
        if (own && own->depth > contactSkin) {
            normal = glm::vec3(0.0f);
            return true;
        }
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
            movers.push_back({body,
                              {toGroup * (body->transform.position - from.position),
                               glm::normalize(toGroup * body->transform.rotation)},
                              body->anchor});
        }
    }
    if (movers.empty()) {
        return result;
    }

    // What they are in already (as they are now) doesn't block them
    for (const Mover& mover : movers) {
        place(*mover.body, mover.body->transform, mover.anchor);  // (their pieces from where they start)
    }
    for (const Mover& mover : movers) {
        if (!mover.body->enabled) {
            continue;
        }
        for (size_t i = 0; i < bodies.size(); i++) {
            const Body& other = bodies[i];
            if (skips.bodies[i] || !other.alive || !other.enabled) {
                continue;
            }
            std::optional<Contact> touching = boxContact(*mover.body, other, false);
            if (touching && touching->depth > contactSkin) {
                skips.bodies[i] = 1;
            }
        }
    }
    std::optional<Contact> own = ownContact(movers);
    skips.own = own && own->depth > contactSkin;
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
        Transform placed = compose(result.reached, mover.relative);
        place(*mover.body, placed, anchorAfter(mover.anchor, placed.position));
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
    float minGroundY = std::cos(glm::radians(settings.maxSlopeDegrees)) - 1e-4f;
    for (int round = 0; round < rounds; round++) {
        Capsule capsule = characterAt(settings, moving.feet);
        Contact deepest;
        bool found = false;
        for (const Body& body : bodies) {
            if (!body.alive || !body.enabled) {
                continue;
            }
            std::optional<Contact> touching = contact(capsule, body);
            if (touching && (!found || touching->depth > deepest.depth)) {
                deepest = *touching;
                found = true;
            }
        }
        if (!found) {
            return;
        }
        glm::vec3 pushedFrom = moving.feet;
        glm::vec3 n = deepest.normal;
        if (n.y >= minGroundY) {
            // Ground: straight up, so standing on a slope doesn't slide down it
            moving.feet.y += deepest.depth / n.y;
            moving.ground = true;
            moving.groundNormal = n;
            moving.velocity.y = std::max(moving.velocity.y, 0.0f);
            carry(settings, moving, pushedFrom);
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
        carry(settings, moving, pushedFrom);
    }
}

void SimplePhysicsWorld::carry(const CharacterSettings& settings, Moving& moving, const glm::vec3& from) const {
    if (openings.empty()) {
        return;
    }
    glm::vec3 up(0.0f, settings.anchorHeight, 0.0f);
    glm::mat4 crossing;
    if (portals::crossOpenings(openingsFor(settings), from + up, moving.feet + up, portalDepth, crossing) == 0) {
        return;
    }
    // The anchor goes through; the capsule stays upright under it
    moving.feet = apply(crossing, moving.feet + up) - up;
    glm::mat3 turn(crossing);
    moving.velocity = turn * moving.velocity;
    moving.groundNormal = glm::normalize(turn * moving.groundNormal);
    moving.crossings++;
    moving.crossing = crossing * moving.crossing;
}

float SimplePhysicsWorld::deepestOverlap(const CharacterSettings& settings, const glm::vec3& feet) const {
    Capsule capsule = characterAt(settings, feet);
    float deepest = 0.0f;
    for (const Body& body : bodies) {
        if (body.alive && body.enabled) {
            if (std::optional<Contact> touching = contact(capsule, body)) {
                deepest = std::max(deepest, touching->depth);
            }
        }
    }
    return deepest;
}

bool SimplePhysicsWorld::squeezed(const Character& character, const std::vector<Mover>& movers) const {
    const CharacterSettings& settings = character.settings;
    Capsule capsule = characterAt(settings, character.state.feet);
    bool touches = false;
    for (const Mover& mover : movers) {
        if (mover.body->enabled) {
            std::optional<Contact> touching = contact(capsule, *mover.body);
            touches = touches || (touching && touching->depth > contactSkin);
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
    glm::vec3 part = delta / static_cast<float>(steps);
    for (int i = 0; i < steps; i++) {
        glm::vec3 from = moving.feet;
        int crossings = moving.crossings;
        glm::mat4 crossed = moving.crossing;
        moving.feet += part;
        carry(settings, moving, from);
        moving.ground = false;  // (what the last substep touched)
        resolve(settings, moving);
        if (moving.crossings != crossings) {
            part = glm::mat3(moving.crossing * glm::inverse(crossed)) * part;  // (the rest goes on from there)
        }
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
        int crossings = moving.crossings;
        glm::mat4 crossed = moving.crossing;
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
            if (down.ground && down.feet.y > moving.feet.y + 1e-4f && stepped > blocked + 1e-5f &&
                down.crossings == moving.crossings) {
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
        if (moving.crossings != crossings) {  // through a portal: the rest of the move goes on from the other end
            glm::mat3 turn(moving.crossing * glm::inverse(crossed));
            part = turn * part;
            partFlat = horizontal(part);
            velocity = turn * velocity;
        }
    }
    // Walking down a slope or a low ledge: kept on the ground when it is within a step straight below (a probe that
    // slid off an edge sideways isn't kept: it falls instead)
    if (start.onGround && !jumped && !moving.ground) {
        Moving probe = sweep(settings, moving, glm::vec3(0.0f, -settings.stepHeight, 0.0f));
        if (probe.ground && probe.crossings == moving.crossings &&
            glm::length(horizontal(probe.feet - moving.feet)) < 1e-3f) {
            moving = probe;
        }
    }

    character->state = CharacterState{moving.feet,         moving.velocity,      moving.ground,
                                      moving.groundNormal, moving.crossings > 0, moving.crossing};
    return character->state;
}

std::optional<RayHit> SimplePhysicsWorld::rayCast(const glm::vec3& from, const glm::vec3& direction,
                                                  float maxDistance) const {
    if (glm::length(direction) < 1e-12f) {
        return std::nullopt;
    }
    glm::vec3 origin = from;
    glm::vec3 d = glm::normalize(direction);
    float left = maxDistance;
    float travelled = 0.0f;
    glm::mat4 through(1.0f);
    int cameOutOf = -1;  // (the ray starts on its plane)
    for (int crossing = 0; crossing <= portalDepth; crossing++) {
        // The nearest body ahead
        const Body* nearest = nullptr;
        float nearestDistance = left;
        int axis = 0;
        float sign = 1.0f;
        for (const Body& body : bodies) {
            float t;
            int k;
            float s;
            if (body.alive && body.enabled &&
                rayBoxHit(body.solid, body.halfExtents, origin, d, nearestDistance, t, k, s) &&
                (nearest == nullptr || t < nearestDistance)) {
                nearest = &body;
                nearestDistance = t;
                axis = k;
                sign = s;
            }
        }
        // An opening before it: on from the other end
        int opening = -1;
        float openingAlong = 2.0f;
        glm::vec3 end = origin + d * nearestDistance;
        for (size_t i = 0; i < openings.size(); i++) {
            float along;
            if (static_cast<int>(i) != cameOutOf && portals::crossesOpening(openings[i], origin, end, along) &&
                along < openingAlong) {
                opening = static_cast<int>(i);
                openingAlong = along;
            }
        }
        if (opening >= 0) {
            if (crossing == portalDepth) {
                return std::nullopt;  // (lost, as the renderer's rays past "Max portal crossings")
            }
            glm::mat4 toPartner = portalToPartner(openings[opening].end, openings[opening ^ 1].end);
            float t = openingAlong * nearestDistance;
            travelled += t;
            left -= t;
            origin = apply(toPartner, origin + d * t);
            d = glm::normalize(glm::mat3(toPartner) * d);
            through = toPartner * through;
            cameOutOf = opening ^ 1;
            continue;
        }
        if (nearest == nullptr) {
            return std::nullopt;
        }
        RayHit hit;
        hit.body = BodyId{static_cast<uint32_t>(nearest - bodies.data()) + 1};
        hit.tag = nearest->tag;
        hit.point = origin + d * nearestDistance;
        hit.distance = travelled + nearestDistance;
        hit.direction = d;
        glm::mat3 turn = glm::mat3_cast(nearest->solid.rotation);
        hit.normal = turn[axis] * sign;
        hit.faceCenter = nearest->solid.position + hit.normal * nearest->halfExtents[axis];
        hit.faceAxes[0] = turn[(axis + 1) % 3];
        hit.faceAxes[1] = turn[(axis + 2) % 3];
        hit.faceHalf = glm::vec2(nearest->halfExtents[(axis + 1) % 3], nearest->halfExtents[(axis + 2) % 3]);
        hit.throughPortals = through;
        return hit;
    }
    return std::nullopt;
}

bool SimplePhysicsWorld::overlapsCapsule(const CapsuleShape& shape, const Transform& transform) const {
    glm::vec3 axis = transform.rotation * glm::vec3(0.0f, shape.halfHeight, 0.0f);
    glm::vec3 a = transform.position - axis;
    glm::vec3 b = transform.position + axis;
    Capsule capsule = capsuleAt(a, b, shape.radius, transform.position, openings);
    for (const Body& body : bodies) {
        if (body.alive && body.enabled && contact(capsule, body)) {
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
