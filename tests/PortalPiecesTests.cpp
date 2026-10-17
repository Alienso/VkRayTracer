//
// Portal pieces (portals/PortalPieces.h): a shape near portal openings as its real piece and ghosts, each cut to its
// part. No scene: the library knows none.
//

#include <catch_amalgamated.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "portals/PortalPieces.h"
#include "portals/PortalTransform.h"

using namespace portals;

namespace {

// An end at position, its front (+Z) turned by yaw degrees about the vertical (90: facing +X, -90: facing -X)
glm::mat4 endAt(const glm::vec3& position, float yawDegrees) {
    return glm::rotate(glm::translate(glm::mat4(1.0f), position), glm::radians(yawDegrees), glm::vec3(0, 1, 0));
}

OrientedBox boxAt(const glm::vec3& center, const glm::vec3& half) {
    return OrientedBox{center, glm::mat3(1.0f), half};
}

glm::vec3 apply(const glm::mat4& m, const glm::vec3& p) {
    return glm::vec3(m * glm::vec4(p, 1.0f));
}

bool sameMatrix(const glm::mat4& a, const glm::mat4& b) {
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            if (std::abs(a[c][r] - b[c][r]) > 1e-5f) {
                return false;
            }
        }
    }
    return true;
}

const glm::vec2 doorSize(0.5f, 1.0f);  // half sizes: 1 wide, 2 high

// Three pairs in a row along X, each facing outward: -| |--| |--| |- (A at 1, 3, 5 facing -X; B at 2, 4, 6 facing +X)
std::vector<PortalOpening> chain() {
    std::vector<PortalOpening> openings;
    for (int k = 0; k < 3; k++) {
        addPortalPair(openings, endAt({1.0f + 2.0f * k, 1, 0}, -90.0f), endAt({2.0f + 2.0f * k, 1, 0}, 90.0f),
                      doorSize, false);
    }
    return openings;
}

// Two ends facing each other across a 2 m gap: |-> <-| (A at x = 0 facing +X, B at x = 2 facing -X)
std::vector<PortalOpening> facing() {
    std::vector<PortalOpening> openings;
    addPortalPair(openings, endAt({0, 1, 0}, 90.0f), endAt({2, 1, 0}, -90.0f), doorSize, false);
    return openings;
}

// A thin stick along X from x0 to x1, at y = 1
OrientedBox stick(float x0, float x1) {
    return boxAt({0.5f * (x0 + x1), 1, 0}, {0.5f * (x1 - x0), 0.1f, 0.1f});
}

}  // namespace

TEST_CASE("Portal pieces: a box half through a pair is the real piece and a ghost at the other end", "[Portals]") {
    std::vector<PortalOpening> openings;
    glm::mat4 a = endAt({0, 1, 0}, 0.0f);
    glm::mat4 b = endAt({10, 1, 0}, 0.0f);
    addPortalPair(openings, a, b, doorSize, false);
    OrientedBox box = boxAt({0, 1, 0.1f}, {0.3f, 0.3f, 0.4f});  // 0.3 of it behind A's plane
    std::vector<PortalPiece> pieces;
    portalPieces(box, box.center, openings, 8, 64, pieces);
    REQUIRE(pieces.size() == 2);
    const PortalPiece& real = pieces[0];
    CHECK(sameMatrix(real.transform, glm::mat4(1.0f)));
    REQUIRE(real.cutCount == 1);
    CHECK_FALSE(real.cuts[0].keepNear);
    CHECK(real.cuts[0].side == 1.0f);
    CHECK_FALSE(pieceKeeps(real, {0, 1, -0.2f}));  // the part behind A: gone through
    CHECK(pieceKeeps(real, {0, 1, 0.3f}));
    const PortalPiece& ghost = pieces[1];
    CHECK(sameMatrix(ghost.transform, portalToPartner(a, b)));
    CHECK(ghost.depth == 1);
    CHECK(ghost.cameOutOf == 1);
    REQUIRE(ghost.cutCount == 1);
    CHECK(ghost.cuts[0].keepNear);
    CHECK(apply(ghost.transform, {0, 1, -0.2f}) == glm::vec3(10, 1, 0.2f));
    CHECK(pieceKeeps(ghost, {10, 1, 0.2f}));       // what went in, out in front of B
    CHECK_FALSE(pieceKeeps(ghost, {10, 1, -0.3f}));  // what didn't, behind B: not shown
}

TEST_CASE("Portal pieces: going into the back of an end comes out of the back of the other", "[Portals]") {
    std::vector<PortalOpening> openings;
    addPortalPair(openings, endAt({0, 1, 0}, 0.0f), endAt({10, 1, 0}, 0.0f), doorSize, false);
    OrientedBox box = boxAt({0, 1, -0.1f}, {0.3f, 0.3f, 0.4f});  // from behind A, 0.3 of it in front
    std::vector<PortalPiece> pieces;
    portalPieces(box, box.center, openings, 8, 64, pieces);
    REQUIRE(pieces.size() == 2);
    CHECK(pieces[0].cuts[0].side == -1.0f);
    CHECK_FALSE(pieceKeeps(pieces[0], {0, 1, 0.2f}));
    CHECK(pieceKeeps(pieces[1], {10, 1, -0.2f}));
    CHECK_FALSE(pieceKeeps(pieces[1], {10, 1, 0.3f}));
}

TEST_CASE("Portal pieces: a box not going into an opening is one piece, uncut", "[Portals]") {
    std::vector<PortalOpening> openings;
    addPortalPair(openings, endAt({0, 1, 0}, 0.0f), endAt({10, 1, 0}, 0.0f), doorSize, false);
    std::vector<PortalPiece> pieces;
    for (const OrientedBox& box : {boxAt({0, 1, 3}, glm::vec3(0.3f)),             // far off
                                   boxAt({3, 1, 0.1f}, {0.3f, 0.3f, 0.4f}),      // through the plane, beside it
                                   boxAt({0, 1, 0.45f}, {0.3f, 0.3f, 0.4f}),     // just in front, not beyond
                                   boxAt({0.5f, 1, 0.1f}, {0.3f, 0.3f, 0.4f}),   // half in the wall beside it
                                   boxAt({0, 1, 0}, glm::vec3(5.0f))}) {         // a room around the opening
        portalPieces(box, box.center, openings, 8, 64, pieces);
        REQUIRE(pieces.size() == 1);
        CHECK(pieces[0].cutCount == 0);
    }
}

TEST_CASE("Portal pieces: what a cut keeps", "[Portals]") {
    PortalPiece piece;
    piece.cutCount = 1;
    piece.cuts[0] = PortalCut{glm::mat4(1.0f), doorSize, false, false, 1.0f};  // an entrance: its front is near
    CHECK_FALSE(pieceKeeps(piece, {0, 0, -1}));  // behind, in the tube: gone through
    CHECK(pieceKeeps(piece, {2, 0, -1}));        // behind, beside the opening
    CHECK(pieceKeeps(piece, {0, 0, 1}));         // in front
    CHECK(pieceKeeps(piece, {0, 0, 0}));         // on the plane
    CHECK_FALSE(pieceKeeps(piece, {0.45f, 0.9f, -1}));
    piece.cuts[0].ellipse = true;
    CHECK(pieceKeeps(piece, {0.45f, 0.9f, -1}));  // in the rectangle's corner, outside the ellipse
    piece.cuts[0].keepNear = true;                // an exit: only its front
    CHECK_FALSE(pieceKeeps(piece, {2, 0, -1}));
    CHECK(pieceKeeps(piece, {2, 0, 1}));
    CHECK(pieceKeeps(piece, {0, 0, 0}));
}

TEST_CASE("Portal pieces: a stick through a chain of pairs is a piece per gap", "[Portals]") {
    std::vector<PortalOpening> openings = chain();
    std::vector<PortalPiece> pieces;
    portalPieces(stick(0.5f, 4.5f), {0.5f, 1, 0}, openings, 8, 64, pieces);  // its origin at its back end
    REQUIRE(pieces.size() == 4);
    // The real one up to A1, then out of B1 into A2, out of B2 into A3, out of B3
    CHECK(pieces[0].cutCount == 1);
    CHECK(pieces[1].cutCount == 2);
    CHECK(pieces[2].cutCount == 2);
    CHECK(pieces[3].cutCount == 1);
    for (int k = 1; k <= 3; k++) {
        CHECK(pieces[k].depth == k);
        CHECK(apply(pieces[k].transform, {4.5f, 1, 0}).x == Catch::Approx(4.5f + k).margin(1e-5));
    }
    // Each shows its gap's part only
    CHECK(pieceKeeps(pieces[0], {0.8f, 1, 0}));
    CHECK_FALSE(pieceKeeps(pieces[0], {1.5f, 1, 0}));
    CHECK(pieceKeeps(pieces[1], {2.5f, 1, 0}));
    CHECK_FALSE(pieceKeeps(pieces[1], {1.5f, 1, 0}));
    CHECK_FALSE(pieceKeeps(pieces[1], {3.5f, 1, 0}));
    CHECK(pieceKeeps(pieces[3], {7.0f, 1, 0}));
}

TEST_CASE("Portal pieces: a stick between ends facing each other comes out again and again", "[Portals]") {
    std::vector<PortalOpening> openings = facing();
    std::vector<PortalPiece> pieces;
    // 2.5 gaps long, from x = 0.5: three pieces in the gap
    portalPieces(stick(0.5f, 5.5f), {0.5f, 1, 0}, openings, 8, 64, pieces);
    REQUIRE(pieces.size() == 3);
    for (int k = 1; k <= 2; k++) {
        CHECK(apply(pieces[k].transform, {0.5f, 1, 0}).x == Catch::Approx(0.5f - 2.0f * k).margin(1e-5));
        CHECK(pieces[k].cameOutOf == 0);  // out of A each time
    }
    CHECK(pieces[0].cutCount == 1);
    CHECK(pieces[1].cutCount == 2);  // A's near side kept, into B again
    CHECK(pieces[2].cutCount == 1);  // its end is in the gap
    CHECK(pieceKeeps(pieces[2], {1.0f, 1, 0}));
    CHECK_FALSE(pieceKeeps(pieces[2], {-0.5f, 1, 0}));

    // 20 gaps long: as far as the depth goes
    portalPieces(stick(0.5f, 40.5f), {0.5f, 1, 0}, openings, 8, 64, pieces);
    REQUIRE(pieces.size() == 9);
    CHECK(pieces.back().depth == 8);
    CHECK(pieces.back().cutCount == 2);  // still cut at B, no ghost of its own
    for (const PortalPiece& piece : pieces) {
        CHECK(piece.cutCount <= 2);  // (the cuts taken along are dropped where they no longer cut)
    }

    // And as far as the total allows
    portalPieces(stick(0.5f, 40.5f), {0.5f, 1, 0}, openings, 8, 4, pieces);
    REQUIRE(pieces.size() == 4);
    CHECK(pieces.back().cutCount == 2);
}

TEST_CASE("Portal pieces: a box going into two openings at once branches, its corner shown once; the total caps it",
          "[Portals]") {
    // Two pairs facing across the same square: X (A at x = 0 facing +X, B at x = 2 facing -X) and Z (A at z = 0
    // facing +Z, B at z = 2 facing -Z), each 3 by 3; a box at the square's corner goes out through both B ends
    std::vector<PortalOpening> openings;
    addPortalPair(openings, endAt({0, 1, 1}, 90.0f), endAt({2, 1, 1}, -90.0f), glm::vec2(1.5f), false);
    addPortalPair(openings, endAt({1, 1, 0}, 0.0f), endAt({1, 1, 2}, 180.0f), glm::vec2(1.5f), false);
    OrientedBox box = boxAt({1.75f, 1, 1.75f}, {0.5f, 0.3f, 0.5f});
    std::vector<PortalPiece> pieces;
    portalPieces(box, box.center, openings, 8, 64, pieces);
    // The real one; out of A_X; out of A_Z; and the corner through both, out of A_X and then out of A_Z
    REQUIRE(pieces.size() == 4);
    CHECK(pieces[0].cutCount == 2);
    CHECK(pieces[1].cameOutOf == 0);
    CHECK(pieces[2].cameOutOf == 2);
    CHECK(pieces[3].cameOutOf == 2);
    CHECK(pieces[3].depth == 2);
    glm::vec3 intoX(2.2f, 1, 1.5f);  // beyond B_X, in its tube: shown out of A_X only
    CHECK_FALSE(pieceKeeps(pieces[0], intoX));
    CHECK(pieceKeeps(pieces[1], apply(pieces[1].transform, intoX)));
    CHECK_FALSE(pieceKeeps(pieces[2], apply(pieces[2].transform, intoX)));
    // The corner beyond both: shown once, by the ghost of the ghost
    glm::vec3 corner(2.2f, 1, 2.2f);
    CHECK_FALSE(pieceKeeps(pieces[0], corner));
    CHECK_FALSE(pieceKeeps(pieces[1], apply(pieces[1].transform, corner)));
    CHECK_FALSE(pieceKeeps(pieces[2], apply(pieces[2].transform, corner)));
    CHECK(pieceKeeps(pieces[3], apply(pieces[3].transform, corner)));

    portalPieces(box, box.center, openings, 8, 3, pieces);
    CHECK(pieces.size() == 3);
}
