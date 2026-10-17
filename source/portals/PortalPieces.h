//
// Pieces of a shape near portal openings (PORTAL_PHYSICS_PLAN.md): an object part way through a portal is in both
// places at once, for what is seen and for what collides. One module finds the pieces for the renderer and for the
// physics, so what is seen is what collides. Plain glm: nothing of the scene, the assets, the physics or the game.
//
// A piece is the shape placed by a transform (the identity for the real one) and cut by openings:
// - "remove beyond, in the tube" (an entrance): of an opening the shape goes into, the part on the far side of the
//   opening's plane and inside its outline extruded across the plane is removed. What is beyond the plane beside the
//   opening stays (the wall around a portal still blocks; a part sticking out beside the opening is still there).
// - "keep the near side" (an exit): of the end a ghost came out of, only what is on the side it came out on is kept.
// The real shape gets an entrance cut for each opening it straddles, and a ghost through each one: the shape placed
// by the pair's transform (portalToPartner), cut to the exit's side. A ghost that straddles further openings (not
// the one it came out of) gets entrance cuts and ghosts of its own, up to maxDepth crossings and maxPieces in all.
//
// Which side is the near one (what goes in from where): for the real shape, the side of its anchor (the entity's
// origin: the point the game carries through portals, so the real piece is always the one on the origin's side); for
// a ghost, the side of the end it came out of. Portals work both ways: going into the back of an end comes out of the
// back of the other.
//
// A shape bigger than the opening: the cuts are of the tube, so the part beside the opening stays at the entrance;
// the ghost keeps everything on the exit's side, so a part that never went through (sticking out sideways into the
// wall) also shows at the exit (LIMITATIONS.md "What portals carry").
//

#ifndef VULKANDEMO_PORTALPIECES_H
#define VULKANDEMO_PORTALPIECES_H

#include <vector>
#include <glm/glm.hpp>

namespace portals {

// A shape's bounds placed in the world: a box with its own axes (unit length, the columns) and half sizes
struct OrientedBox {
    glm::vec3 center = glm::vec3(0.0f);
    glm::mat3 axes = glm::mat3(1.0f);
    glm::vec3 half = glm::vec3(0.5f);
};

// One end of a pair (the ends' convention: scene/Portal.h; end: its world placement without scale). Openings come in
// pairs: 2k and 2k + 1 are the two ends of pair k (addPortalPair).
struct PortalOpening {
    glm::mat4 end = glm::mat4(1.0f);
    glm::vec2 halfSize = glm::vec2(0.5f, 1.0f);
    bool ellipse = false;
};

// A cut of a piece, in the end's own space (worldToEnd: the end's placement inverted): the opening's plane is its
// z = 0, the near side the one where side * z > 0
struct PortalCut {
    glm::mat4 worldToEnd = glm::mat4(1.0f);
    glm::vec2 halfSize = glm::vec2(0.5f, 1.0f);
    bool ellipse = false;
    bool keepNear = false;  // true: an exit, only the near side kept; false: an entrance, beyond in the tube removed
    float side = 1.0f;      // 1: the near side is the end's front (+Z), -1: its back
};

struct PortalPiece {
    static constexpr int maxCuts = 4;
    glm::mat4 transform = glm::mat4(1.0f);  // takes the real shape's world points to where this piece is
    int cutCount = 0;
    PortalCut cuts[maxCuts];
    int depth = 0;       // the crossings it took (0: the real one)
    int cameOutOf = -1;  // the opening it came out of (-1: the real one)
};

// A shape goes into an opening when part of it is beyond the opening's plane (from its near side) and where it
// crosses the plane lies all within the outline grown by this. A shape crossing the plane partly beside the opening
// doesn't go in: it is in the wall around it (it stays whole, as without portals), or it is something big around
// the opening (a room whose wall the portal is in: cutting it would hollow the wall behind the opening, and show
// the whole room again at the other end).
constexpr float straddleMargin = 0.01f;

void addPortalPair(std::vector<PortalOpening>& openings, const glm::mat4& endA, const glm::mat4& endB,
                   const glm::vec2& halfSize, bool ellipse);

// The pieces of a shape whose bounds are box and whose real side is anchor's: pieces[0] is the real one (the identity
// transform), then its ghosts, breadth first. A piece gets at most PortalPiece::maxCuts cuts (further openings it
// straddles are left out); past maxDepth or maxPieces a straddled opening still cuts, but makes no ghost.
void portalPieces(const OrientedBox& box, const glm::vec3& anchor, const std::vector<PortalOpening>& openings,
                  int maxDepth, int maxPieces, std::vector<PortalPiece>& pieces);

// Whether a point of the world is in the piece (its cuts' regions; points on a cut's plane are kept)
bool pieceKeeps(const PortalPiece& piece, const glm::vec3& worldPoint);

}  // namespace portals

#endif //VULKANDEMO_PORTALPIECES_H
