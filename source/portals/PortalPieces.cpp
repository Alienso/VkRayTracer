//
// Pieces of a shape near portal openings. See PortalPieces.h.
//
// A piece's region is worked with as a convex set of points: the box's corners placed by its transform, cut to the
// near side of its exit cuts. (Its entrance cuts make it non-convex: they only count where noted.) A convex region's
// cross-section with a plane is spanned by where the segments between points on opposite sides meet the plane: the
// region's edges are among those segments, and every such point is inside the region.
//

#include <algorithm>
#include <cmath>
#include "portals/PortalPieces.h"
#include "portals/PortalTransform.h"

namespace portals {

namespace {

constexpr float keepTolerance = 1e-5f;

glm::vec3 apply(const glm::mat4& m, const glm::vec3& p) {
    return glm::vec3(m * glm::vec4(p, 1.0f));
}

bool insideOutline(const glm::vec2& p, const glm::vec2& half, bool ellipse, float grow) {
    if (ellipse) {
        glm::vec2 q = p / (half + grow);
        return glm::dot(q, q) <= 1.0f;
    }
    return std::abs(p.x) <= half.x + grow && std::abs(p.y) <= half.y + grow;
}

float cross2(const glm::vec2& o, const glm::vec2& a, const glm::vec2& b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

// The convex hull, counter-clockwise (monotone chain)
std::vector<glm::vec2> hull(std::vector<glm::vec2> points) {
    std::sort(points.begin(), points.end(), [](const glm::vec2& a, const glm::vec2& b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    });
    points.erase(std::unique(points.begin(), points.end()), points.end());
    if (points.size() < 3) {
        return points;
    }
    std::vector<glm::vec2> result(2 * points.size());
    size_t k = 0;
    for (size_t i = 0; i < points.size(); i++) {
        while (k >= 2 && cross2(result[k - 2], result[k - 1], points[i]) <= 0.0f) {
            k--;
        }
        result[k++] = points[i];
    }
    for (size_t i = points.size() - 1, lower = k + 1; i-- > 0;) {
        while (k >= lower && cross2(result[k - 2], result[k - 1], points[i]) <= 0.0f) {
            k--;
        }
        result[k++] = points[i];
    }
    result.resize(k - 1);
    return result;
}

// A convex polygon cut to the rectangle [-half, half] (Sutherland-Hodgman)
std::vector<glm::vec2> clipToRectangle(std::vector<glm::vec2> polygon, const glm::vec2& half) {
    for (int axis = 0; axis < 2; axis++) {
        for (float sign : {1.0f, -1.0f}) {
            if (polygon.empty()) {
                return polygon;
            }
            // Kept: sign * p[axis] <= half[axis]
            std::vector<glm::vec2> clipped;
            for (size_t i = 0; i < polygon.size(); i++) {
                const glm::vec2& p = polygon[i];
                const glm::vec2& q = polygon[(i + 1) % polygon.size()];
                float dp = sign * p[axis] - half[axis];
                float dq = sign * q[axis] - half[axis];
                if (dp <= 0.0f) {
                    clipped.push_back(p);
                }
                if ((dp < 0.0f && dq > 0.0f) || (dp > 0.0f && dq < 0.0f)) {
                    clipped.push_back(p + (q - p) * (dp / (dp - dq)));
                }
            }
            polygon = std::move(clipped);
        }
    }
    return polygon;
}

// Whether a convex polygon (counter-clockwise, or fewer than 3 points) meets an ellipse: in the space where the
// ellipse is the unit circle, the origin is inside the polygon or an edge comes within 1 of it
bool meetsEllipse(const std::vector<glm::vec2>& polygon, const glm::vec2& half) {
    std::vector<glm::vec2> q;
    for (const glm::vec2& p : polygon) {
        q.push_back(p / half);
    }
    if (q.size() >= 3) {
        bool inside = true;
        for (size_t i = 0; i < q.size() && inside; i++) {
            inside = cross2(q[i], q[(i + 1) % q.size()], glm::vec2(0.0f)) >= 0.0f;
        }
        if (inside) {
            return true;
        }
    }
    for (size_t i = 0; i < q.size(); i++) {
        const glm::vec2& a = q[i];
        const glm::vec2& b = q[(i + 1) % q.size()];
        glm::vec2 ab = b - a;
        float t = glm::dot(ab, ab) > 0.0f ? glm::clamp(-glm::dot(a, ab) / glm::dot(ab, ab), 0.0f, 1.0f) : 0.0f;
        if (glm::length(a + ab * t) <= 1.0f) {
            return true;
        }
    }
    return false;
}

// A convex polygon in an opening's plane cut to its outline (grown by grow); empty: they don't meet. (An ellipse's
// part is its bounding rectangle's: a little more than the ellipse's.)
std::vector<glm::vec2> withinOutline(const std::vector<glm::vec2>& polygon, const glm::vec2& half, bool ellipse,
                                     float grow) {
    if (polygon.empty() || (ellipse && !meetsEllipse(polygon, half + grow))) {
        return {};
    }
    return clipToRectangle(polygon, half + grow);
}

// The points of a convex region cut to a plane's side (in the plane's space: side * z >= 0 kept)
std::vector<glm::vec3> cutToSide(const std::vector<glm::vec3>& points, const glm::mat4& worldToEnd, float side) {
    std::vector<float> z;
    for (const glm::vec3& p : points) {
        z.push_back(side * apply(worldToEnd, p).z);
    }
    std::vector<glm::vec3> kept;
    for (size_t i = 0; i < points.size(); i++) {
        if (z[i] >= -keepTolerance) {
            kept.push_back(points[i]);
        }
        for (size_t j = i + 1; j < points.size(); j++) {
            if ((z[i] > 0.0f && z[j] < 0.0f) || (z[i] < 0.0f && z[j] > 0.0f)) {
                kept.push_back(points[i] + (points[j] - points[i]) * (z[i] / (z[i] - z[j])));
            }
        }
    }
    return kept;
}

// A convex region's cross-section with a plane, in the plane's XY, as a convex polygon
std::vector<glm::vec2> crossSection(const std::vector<glm::vec3>& points, const glm::mat4& worldToEnd) {
    std::vector<glm::vec3> local;
    for (const glm::vec3& p : points) {
        local.push_back(apply(worldToEnd, p));
    }
    std::vector<glm::vec2> section;
    for (size_t i = 0; i < local.size(); i++) {
        const glm::vec3& a = local[i];
        if (std::abs(a.z) <= keepTolerance) {
            section.emplace_back(a.x, a.y);
        }
        for (size_t j = i + 1; j < local.size(); j++) {
            const glm::vec3& b = local[j];
            if ((a.z > keepTolerance && b.z < -keepTolerance) || (a.z < -keepTolerance && b.z > keepTolerance)) {
                glm::vec3 c = a + (b - a) * (a.z / (a.z - b.z));
                section.emplace_back(c.x, c.y);
            }
        }
    }
    return hull(section);
}

// Whether some point is beyond a cut's plane (side * z < 0)
bool anyBeyond(const std::vector<glm::vec3>& points, const PortalCut& cut) {
    for (const glm::vec3& p : points) {
        if (cut.side * apply(cut.worldToEnd, p).z < -keepTolerance) {
            return true;
        }
    }
    return false;
}

// Whether all the points are in an entrance cut's removed region (beyond the plane, in the tube; its boundary counts)
bool allRemovedBy(const std::vector<glm::vec3>& points, const PortalCut& cut) {
    for (const glm::vec3& p : points) {
        glm::vec3 q = apply(cut.worldToEnd, p);
        if (cut.side * q.z > keepTolerance || !insideOutline(glm::vec2(q), cut.halfSize, cut.ellipse, keepTolerance)) {
            return false;
        }
    }
    return true;
}

// Whether a cut removes anything of a convex region
bool cutsInto(const std::vector<glm::vec3>& region, const PortalCut& cut) {
    if (!anyBeyond(region, cut)) {
        return false;
    }
    if (cut.keepNear) {
        return true;
    }
    std::vector<glm::vec2> beyond;  // the part beyond, seen along the tube
    for (const glm::vec3& p : cutToSide(region, cut.worldToEnd, -cut.side)) {
        beyond.emplace_back(apply(cut.worldToEnd, p));
    }
    return !withinOutline(hull(beyond), cut.halfSize, cut.ellipse, 0.0f).empty();
}

// A piece's region: the box's corners placed by its transform, cut to the near side of its exit cuts
std::vector<glm::vec3> regionOf(const std::vector<glm::vec3>& corners, const glm::mat4& transform,
                                const PortalCut* cuts, int cutCount) {
    std::vector<glm::vec3> points;
    for (const glm::vec3& corner : corners) {
        points.push_back(apply(transform, corner));
    }
    for (int i = 0; i < cutCount; i++) {
        if (cuts[i].keepNear) {
            points = cutToSide(points, cuts[i].worldToEnd, cuts[i].side);
        }
    }
    return points;
}

// An opening a piece straddles: its entrance cut and where the piece crosses it (the cross-section within the
// outline, in the world)
struct Straddle {
    int opening;
    PortalCut cut;
    std::vector<glm::vec3> section;
};

}  // namespace

void addPortalPair(std::vector<PortalOpening>& openings, const glm::mat4& endA, const glm::mat4& endB,
                   const glm::vec2& halfSize, bool ellipse) {
    openings.push_back({endA, halfSize, ellipse});
    openings.push_back({endB, halfSize, ellipse});
}

void portalPieces(const OrientedBox& box, const glm::vec3& anchor, const std::vector<PortalOpening>& openings,
                  int maxDepth, int maxPieces, std::vector<PortalPiece>& pieces) {
    pieces.clear();
    pieces.emplace_back();
    std::vector<glm::vec3> corners;
    for (int corner = 0; corner < 8; corner++) {
        glm::vec3 p = box.center;
        for (int axis = 0; axis < 3; axis++) {
            p += box.axes[axis] * (((corner >> axis) & 1) ? box.half[axis] : -box.half[axis]);
        }
        corners.push_back(p);
    }

    for (size_t k = 0; k < pieces.size(); k++) {
        PortalPiece piece = pieces[k];  // (a copy: ghosts are added to the list below)
        std::vector<glm::vec3> region = regionOf(corners, piece.transform, piece.cuts, piece.cutCount);
        glm::vec3 near = piece.cameOutOf >= 0 ? glm::vec3(openings[piece.cameOutOf].end[3]) : anchor;

        // The openings it goes into: partly beyond the plane (from its near side), crossing it within the outline
        std::vector<Straddle> straddles;
        for (size_t i = 0; i < openings.size(); i++) {
            if (static_cast<int>(i) == piece.cameOutOf) {
                continue;
            }
            const PortalOpening& opening = openings[i];
            glm::mat4 worldToEnd = glm::inverse(opening.end);
            float side = apply(worldToEnd, near).z >= 0.0f ? 1.0f : -1.0f;
            PortalCut cut{worldToEnd, opening.halfSize, opening.ellipse, false, side};
            if (!anyBeyond(region, cut)) {
                continue;
            }
            std::vector<glm::vec2> section = crossSection(region, worldToEnd);
            bool inside = !section.empty();
            for (size_t p = 0; p < section.size() && inside; p++) {
                inside = insideOutline(section[p], opening.halfSize, opening.ellipse, straddleMargin);
            }
            if (!inside) {
                continue;  // (in the wall around it, or a whole room around the opening)
            }
            Straddle straddle{static_cast<int>(i), cut, {}};
            for (const glm::vec2& p : section) {
                straddle.section.push_back(apply(opening.end, glm::vec3(p, 0.0f)));
            }
            straddles.push_back(std::move(straddle));
        }
        // One whose crossing is all inside another's tube (or a tube the piece is already cut by) is never reached:
        // the piece went into the other one first (an end behind an end)
        std::vector<int> entered;
        for (size_t c = 0; c < straddles.size(); c++) {
            bool hidden = false;
            for (size_t d = 0; d < straddles.size() && !hidden; d++) {
                hidden = d != c && allRemovedBy(straddles[c].section, straddles[d].cut);
            }
            for (int i = 0; i < piece.cutCount && !hidden; i++) {
                hidden = !piece.cuts[i].keepNear && allRemovedBy(straddles[c].section, piece.cuts[i]);
            }
            if (!hidden && pieces[k].cutCount < PortalPiece::maxCuts) {
                pieces[k].cuts[pieces[k].cutCount++] = straddles[c].cut;
                entered.push_back(static_cast<int>(c));
            }
        }

        // A ghost through each, the exit's near side kept. It takes along the parent's cuts but for the openings
        // entered here from this one on: where the piece goes into two at once (a corner through both), the first
        // takes the shared part (and its ghost goes on into the second one from there); each is shown once.
        const PortalPiece parent = pieces[k];  // (a copy: the list grows)
        int ownCuts = piece.cutCount;          // (the parent's cuts from here on are the openings entered here)
        for (size_t j = 0; j < entered.size(); j++) {
            if (parent.depth >= maxDepth || static_cast<int>(pieces.size()) >= maxPieces) {
                break;
            }
            int c = entered[j];
            const Straddle& straddle = straddles[c];
            int exit = straddle.opening ^ 1;
            const PortalOpening& partner = openings[exit];
            glm::mat4 through = portalToPartner(openings[straddle.opening].end, partner.end);
            glm::mat4 back = glm::inverse(through);
            std::vector<PortalCut> cuts = {
                PortalCut{glm::inverse(partner.end), partner.halfSize, partner.ellipse, true, straddle.cut.side}};
            for (int i = 0; i < ownCuts + static_cast<int>(j); i++) {
                PortalCut moved = parent.cuts[i];
                moved.worldToEnd = parent.cuts[i].worldToEnd * back;
                cuts.push_back(moved);
            }
            PortalPiece ghost;
            ghost.transform = through * parent.transform;
            ghost.depth = parent.depth + 1;
            ghost.cameOutOf = exit;
            // Only the cuts that still cut into it (most taken along are on the side it left). An exit cut is tested
            // against the region the others leave.
            std::vector<glm::vec3> ghostRegion = regionOf(corners, ghost.transform, cuts.data(),
                                                          static_cast<int>(cuts.size()));
            for (size_t i = 0; i < cuts.size() && ghost.cutCount < PortalPiece::maxCuts; i++) {
                bool keep = i == 0;
                if (!keep && cuts[i].keepNear) {
                    std::vector<PortalCut> others = cuts;
                    others.erase(others.begin() + static_cast<long>(i));
                    keep = cutsInto(regionOf(corners, ghost.transform, others.data(), static_cast<int>(others.size())),
                                    cuts[i]);
                } else if (!keep) {
                    keep = cutsInto(ghostRegion, cuts[i]);
                }
                if (keep) {
                    ghost.cuts[ghost.cutCount++] = cuts[i];
                }
            }
            pieces.push_back(ghost);
        }
    }
}

bool pieceKeeps(const PortalPiece& piece, const glm::vec3& worldPoint) {
    for (int i = 0; i < piece.cutCount; i++) {
        const PortalCut& cut = piece.cuts[i];
        glm::vec3 q = apply(cut.worldToEnd, worldPoint);
        if (cut.side * q.z < -keepTolerance &&
            (cut.keepNear || insideOutline(glm::vec2(q), cut.halfSize, cut.ellipse, 0.0f))) {
            return false;
        }
    }
    return true;
}

int crossOpenings(const std::vector<PortalOpening>& openings, const glm::vec3& from, const glm::vec3& to,
                  int maxCrossings, glm::mat4& total) {
    total = glm::mat4(1.0f);
    glm::vec3 a = from;
    glm::vec3 b = to;
    int cameOutOf = -1;  // (the line starts on its plane)
    for (int crossing = 0; crossing < maxCrossings; crossing++) {
        int nearest = -1;
        float nearestAlong = 2.0f;
        for (size_t i = 0; i < openings.size(); i++) {
            const PortalOpening& opening = openings[i];
            glm::vec3 center(opening.end[3]);
            glm::vec3 normal(opening.end[2]);
            float da = glm::dot(a - center, normal);
            float db = glm::dot(b - center, normal);
            if (static_cast<int>(i) == cameOutOf || (da >= 0.0f) == (db >= 0.0f)) {
                continue;
            }
            float along = da / (da - db);
            glm::vec3 offset = a + (b - a) * along - center;
            glm::vec2 p(glm::dot(offset, glm::vec3(opening.end[0])), glm::dot(offset, glm::vec3(opening.end[1])));
            if (insideOutline(p, opening.halfSize, opening.ellipse, 0.0f) && along < nearestAlong) {
                nearest = static_cast<int>(i);
                nearestAlong = along;
            }
        }
        if (nearest < 0) {
            return crossing;
        }
        glm::mat4 toPartner = portalToPartner(openings[nearest].end, openings[nearest ^ 1].end);
        a = apply(toPartner, a + (b - a) * nearestAlong);
        b = apply(toPartner, b);
        total = toPartner * total;
        cameOutOf = nearest ^ 1;
    }
    return maxCrossings;
}

}  // namespace portals
