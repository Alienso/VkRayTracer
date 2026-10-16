//
// The scene's entities: the tree, adding, duplicating and removing them, and the render list. See Scene.h.
//

#define GLM_ENABLE_EXPERIMENTAL  // glm/gtx: Euler angles
#include "scene/Scene.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <unordered_set>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include "assets/BuiltInModels.h"
#include "core/Configuration.h"
#include "renderer/RenderSettingsFile.h"

glm::mat4 Transform::matrix() const {
    // Read right to left: scale, rotate, then move into place
    return glm::scale(glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation), scale);
}

glm::quat eulerToQuat(const glm::vec3& degrees) {
    return glm::angleAxis(glm::radians(degrees.y), glm::vec3(0.0f, 1.0f, 0.0f)) *
           glm::angleAxis(glm::radians(degrees.x), glm::vec3(1.0f, 0.0f, 0.0f)) *
           glm::angleAxis(glm::radians(degrees.z), glm::vec3(0.0f, 0.0f, 1.0f));
}

glm::vec3 quatToEuler(const glm::quat& rotation) {
    float yaw, pitch, roll;
    glm::extractEulerAngleYXZ(glm::mat4_cast(rotation), yaw, pitch, roll);  // the inverse of Ry * Rx * Rz
    return glm::degrees(glm::vec3(pitch, yaw, roll));
}

// A transform with that matrix: the columns' lengths are the scale (one negated if it mirrors), the rest the rotation.
// A matrix with shear (a rotated non-uniform scale above) has no exact transform: this one is close.
static Transform toTransform(const glm::mat4& m) {
    Transform t;
    t.position = glm::vec3(m[3]);
    t.scale = glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
    if (glm::determinant(glm::mat3(m)) < 0.0f) {
        t.scale.x = -t.scale.x;
    }
    glm::mat3 rotation = glm::mat3(glm::vec3(m[0]) / t.scale.x, glm::vec3(m[1]) / t.scale.y,
                                   glm::vec3(m[2]) / t.scale.z);
    t.rotation = glm::normalize(glm::quat_cast(rotation));
    return t;
}

// A portal end's placement without scale (Portal.h: the size is the pair's). Columns that are already unit length are
// left exactly as they are.
static glm::mat4 withoutScale(glm::mat4 m) {
    for (int c = 0; c < 3; c++) {
        glm::vec3 axis(m[c]);
        float lengthSquared = glm::dot(axis, axis);
        if (std::abs(lengthSquared - 1.0f) > 1e-6f && lengthSquared > 0.0f) {
            m[c] = glm::vec4(axis / std::sqrt(lengthSquared), 0.0f);
        }
    }
    return m;
}

Scene::Scene(AssetStore& assets, RenderSettings& settings) : assets(assets), settings(settings) {}

void Scene::createDefault() {
    sceneCamera.position = {0.0f, 1.6f, 4.0f};
    sceneCamera.fov = Configuration::fov;
    sceneCamera.lookAt({0.0f, 0.9f, 0.0f});

    ModelComponent floor;
    floor.model = assets.isLoaded(assets.floor()) ? assets.floor() : assets.addFloor();  // made once, then kept
    assets.acquire(floor.model);
    modelTable[create("Floor", 0).id] = floor;
}

void Scene::clear() {
    releaseAll();
    pendingMap = EnvironmentHandle();
    mapError.clear();
    pendingModelEdits.clear();
    entityList.clear();
    entityIndex.clear();
    rootList.clear();
    modelTable.clear();
    portalTable.clear();
    entitiesMade = 0;
    portalPairsMade = 0;
    sceneCamera = Camera();
    cameraTeleport = glm::mat4(1.0f);
    sceneEnvironment = SceneEnvironment();
    resetRenderSettings(settings, SettingsScope::Scene);
    assets.resetEdits();
    layoutRevision++;
    createDefault();
}

void Scene::releaseAll() {
    for (const auto& [id, component] : modelTable) {
        if (component.model.valid()) {
            assets.release(component.model);
        }
    }
    for (const auto& [id, component] : portalTable) {
        if (component.endA) {
            assets.release(rimModel);
        }
    }
    if (sceneEnvironment.map.valid()) {
        assets.release(sceneEnvironment.map);
    }
    if (pendingMap.valid()) {
        assets.release(pendingMap);
    }
}

void Scene::takeFrom(Scene& other) {
    entityList = std::move(other.entityList);
    entityIndex = std::move(other.entityIndex);
    rootList = std::move(other.rootList);
    modelTable = std::move(other.modelTable);
    portalTable = std::move(other.portalTable);
    entitiesMade = other.entitiesMade;
    portalPairsMade = other.portalPairsMade;
    rimModel = other.rimModel;
    sceneCamera = other.sceneCamera;
    cameraTeleport = glm::mat4(1.0f);
    sceneEnvironment = other.sceneEnvironment;
    pendingMap = other.pendingMap;
    mapError = other.mapError;
    pendingModelEdits = std::move(other.pendingModelEdits);
    messages.insert(messages.end(), other.messages.begin(), other.messages.end());
    layoutRevision++;  // (its own counter: the render list compares with this scene's)
    other.entityList.clear();
    other.entityIndex.clear();
    other.rootList.clear();
    other.modelTable.clear();
    other.portalTable.clear();
    other.sceneEnvironment.map = EnvironmentHandle();
    other.pendingMap = EnvironmentHandle();
    other.pendingModelEdits.clear();
    other.messages.clear();
}

std::vector<EntityId> Scene::treeOrder() const {
    std::vector<EntityId> order;
    order.reserve(entityList.size());
    auto visit = [&](auto&& self, EntityId id) -> void {
        order.push_back(id);
        for (EntityId child : find(id)->children) {
            self(self, child);
        }
    };
    for (EntityId root : rootList) {
        visit(visit, root);
    }
    return order;
}

Entity& Scene::create(const std::string& name, EntityId parent) {
    return createWithId(++entitiesMade, name, parent);
}

Entity& Scene::createWithId(EntityId id, const std::string& name, EntityId parent) {
    Entity entity;
    entity.id = id;
    entity.name = name;
    entity.parent = parent;
    siblings(parent).push_back(entity.id);  // (before the list grows: siblings may point into it)
    layoutRevision++;
    entityIndex[entity.id] = entityList.size();
    entityList.push_back(std::move(entity));
    return entityList.back();
}

std::vector<EntityId>& Scene::siblings(EntityId parent) {
    return parent == 0 ? rootList : find(parent)->children;
}

std::vector<EntityId> Scene::subtree(EntityId id) const {
    std::vector<EntityId> ids = {id};
    for (size_t i = 0; i < ids.size(); i++) {  // grows while going through it
        const Entity* entity = find(ids[i]);
        ids.insert(ids.end(), entity->children.begin(), entity->children.end());
    }
    return ids;
}

void Scene::reindex() {
    entityIndex.clear();
    for (size_t i = 0; i < entityList.size(); i++) {
        entityIndex[entityList[i].id] = i;
    }
}

EntityId Scene::addObject(const std::string& fileName, bool originalSize) {
    ModelComponent component;
    component.model = assets.requestModel(fileName);  // (before anything changes: it may throw, see AssetStore.h)
    component.originalSize = originalSize && !isBuiltInModel(fileName);
    if (assets.isLoaded(component.model)) {
        for (const MeshInstance& part : assets.model(component.model).parts) {
            component.hiddenParts.push_back(part.hiddenByDefault);
        }
    } else {
        component.loading = true;  // its parts come when it arrives (update)
        component.loadingEntry = modelEntryText(fileName, false, component.originalSize);
    }
    std::string name = std::filesystem::path(fileName).stem().string();

    // Line objects up: 0, +1, -1, +2, -2, ... spacings from the center, so they don't overlap.
    // Built-in rooms and original-size scenes go to the center instead, around whatever already stands there.
    glm::vec3 position(0.0f);
    if (component.originalSize) {
        // A whole scene brings its own ground; the built-in floor would cut through it.
        for (const auto& [id, other] : modelTable) {
            if (other.model == assets.floor()) {
                find(id)->visible = false;
            }
        }
    } else if (!isBuiltInModel(fileName)) {
        int existing = static_cast<int>(std::count_if(modelTable.begin(), modelTable.end(), [&](const auto& entry) {
            return entry.second.model != assets.floor() && !isBuiltInModel(find(entry.first)->name);
        }));
        int slot = (existing + 1) / 2 * (existing % 2 == 1 ? 1 : -1);
        position.x = slot * objectSpacing;
    }

    Entity& entity = create(name, 0);
    entity.local.position = position;
    assets.acquire(component.model);
    modelTable[entity.id] = std::move(component);
    return entity.id;
}

EntityId Scene::addEmpty(const std::string& name, EntityId parent) {
    return create(name, parent).id;
}

EntityId Scene::addPortalPair() {
    // 3 units ahead of the camera (level), the ends 1.5 to either side, turned to face it
    glm::vec3 forward = sceneCamera.forward();
    forward.y = 0.0f;
    forward = glm::length(forward) > 1e-4f ? glm::normalize(forward) : glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 center = sceneCamera.position + forward * 3.0f;
    float yaw = glm::degrees(std::atan2(-forward.x, -forward.z)) + 0.0f;  // +Z towards the camera (+ 0: no -0)

    rimModel = assets.requestModel(portalRimModelName());  // built in: made at once
    assets.acquire(rimModel);

    Entity& group = create("Portal " + std::to_string(++portalPairsMade), 0);
    group.local.position = center;
    group.local.rotation = eulerToQuat(glm::vec3(0.0f, yaw, 0.0f));
    EntityId groupId = group.id;  // (group moves when the list grows)
    EntityId a = create("End A", groupId).id;
    EntityId b = create("End B", groupId).id;
    find(a)->local.position = glm::vec3(-1.5f, 0.0f, 0.0f);
    find(b)->local.position = glm::vec3(1.5f, 0.0f, 0.0f);

    PortalComponent portal;
    portal.other = b;
    portal.endA = true;
    portalTable[a] = portal;
    portal.other = a;
    portal.endA = false;
    portalTable[b] = portal;
    return groupId;
}

EntityId Scene::duplicate(EntityId id) {
    std::vector<EntityId> ids = subtree(id);
    std::unordered_map<EntityId, EntityId> copyOf;
    for (EntityId original : ids) {
        Entity source = *find(original);  // a copy: creating moves the list
        Entity& copy = create(source.name, original == id ? source.parent : copyOf.at(source.parent));
        copy.local = source.local;
        copy.visible = source.visible;
        copy.uniformScale = source.uniformScale;
        copyOf[original] = copy.id;
    }

    // The copy goes right after the original among its siblings, a spacing along X from it
    EntityId top = copyOf[id];
    std::vector<EntityId>& list = siblings(find(id)->parent);
    list.erase(std::find(list.begin(), list.end(), top));
    list.insert(std::find(list.begin(), list.end(), id) + 1, top);
    find(top)->local.position.x += objectSpacing;

    for (EntityId original : ids) {
        if (auto it = modelTable.find(original); it != modelTable.end()) {
            ModelComponent component = it->second;
            if (component.model.valid()) {
                assets.acquire(component.model);  // one more entity showing it: no loading
            }
            modelTable[copyOf[original]] = std::move(component);
        }
        if (auto it = portalTable.find(original); it != portalTable.end() && copyOf.count(it->second.other)) {
            PortalComponent component = it->second;
            component.other = copyOf[component.other];
            if (component.endA) {
                assets.acquire(rimModel);  // one more pair
            }
            portalTable[copyOf[original]] = component;
        }
    }
    return top;
}

void Scene::remove(EntityId id) {
    std::vector<EntityId> ids = subtree(id);
    std::unordered_set<EntityId> removed(ids.begin(), ids.end());
    // A portal end takes its other end along (a pair isn't left with one end)
    for (size_t i = 0; i < ids.size(); i++) {  // grows while going through it
        const PortalComponent* portal = this->portal(ids[i]);
        if (portal != nullptr && !removed.count(portal->other)) {
            for (EntityId other : subtree(portal->other)) {
                if (removed.insert(other).second) {
                    ids.push_back(other);
                }
            }
        }
    }

    for (EntityId e : ids) {
        const Entity* entity = find(e);
        if (!removed.count(entity->parent)) {  // the top of a removed subtree: out of its parent's list
            std::vector<EntityId>& list = siblings(entity->parent);
            list.erase(std::find(list.begin(), list.end(), e));
        }
        if (auto it = modelTable.find(e); it != modelTable.end()) {
            if (it->second.model.valid()) {
                assets.release(it->second.model);  // the last entity showing it: kept unused (AssetStore.h)
            }
            modelTable.erase(it);
        }
        if (auto it = portalTable.find(e); it != portalTable.end()) {
            if (it->second.endA) {
                assets.release(rimModel);  // the last pair: the rim model is kept unused
            }
            portalTable.erase(it);
        }
    }
    entityList.erase(std::remove_if(entityList.begin(), entityList.end(),
                                    [&](const Entity& entity) { return removed.count(entity.id) > 0; }),
                     entityList.end());
    reindex();
    layoutRevision++;
}

bool Scene::setParent(EntityId id, EntityId parent, bool keepWorld) {
    Entity* entity = find(id);
    if (entity == nullptr || (parent != 0 && find(parent) == nullptr) || isUnder(parent, id)) {
        return false;
    }
    glm::mat4 world = worldTransform(id);
    std::vector<EntityId>& from = siblings(entity->parent);
    from.erase(std::find(from.begin(), from.end(), id));
    entity->parent = parent;
    siblings(parent).push_back(id);
    layoutRevision++;  // the tree order changed
    glm::mat4 parentWorld = parent == 0 ? glm::mat4(1.0f) : worldTransform(parent);
    // Flattened (a scale of 0 in it or above it) there is no transform that keeps it in place: inverting the parent,
    // or splitting a flat matrix into scale and rotation, gives NaNs (and a scene file that can't be read back:
    // JSON has no NaN). It keeps its local transform then.
    auto invertible = [](const glm::mat4& m) { return std::abs(glm::determinant(glm::mat3(m))) > 1e-12f; };
    if (keepWorld && invertible(parentWorld) && invertible(world)) {
        entity->local = toTransform(parent == 0 ? world : glm::inverse(parentWorld) * world);
    }
    return true;
}

Entity* Scene::find(EntityId id) {
    auto it = entityIndex.find(id);
    return it == entityIndex.end() ? nullptr : &entityList[it->second];
}

const Entity* Scene::find(EntityId id) const {
    auto it = entityIndex.find(id);
    return it == entityIndex.end() ? nullptr : &entityList[it->second];
}

bool Scene::isUnder(EntityId id, EntityId ancestor) const {
    for (EntityId e = id; e != 0; e = find(e)->parent) {
        if (e == ancestor) {
            return true;
        }
    }
    return false;
}

glm::mat4 Scene::worldTransform(EntityId id) const {
    const Entity* entity = find(id);
    glm::mat4 world = entity->local.matrix();
    for (EntityId p = entity->parent; p != 0; p = find(p)->parent) {
        world = find(p)->local.matrix() * world;
    }
    return world;
}

bool Scene::visibleInWorld(EntityId id) const {
    for (EntityId e = id; e != 0; e = find(e)->parent) {
        if (!find(e)->visible) {
            return false;
        }
    }
    return true;
}

ModelComponent* Scene::model(EntityId id) {
    auto it = modelTable.find(id);
    return it == modelTable.end() ? nullptr : &it->second;
}

const ModelComponent* Scene::model(EntityId id) const {
    auto it = modelTable.find(id);
    return it == modelTable.end() ? nullptr : &it->second;
}

PortalComponent* Scene::portal(EntityId id) {
    auto it = portalTable.find(id);
    return it == portalTable.end() ? nullptr : &it->second;
}

const PortalComponent* Scene::portal(EntityId id) const {
    auto it = portalTable.find(id);
    return it == portalTable.end() ? nullptr : &it->second;
}

void Scene::setPartHidden(EntityId id, size_t part, bool hidden) {
    ModelComponent* component = model(id);
    if (component == nullptr) {
        return;
    }
    if (component->hiddenParts.size() <= part) {
        component->hiddenParts.resize(part + 1, false);
    }
    if (component->hiddenParts[part] != hidden) {
        component->hiddenParts[part] = hidden;
        layoutRevision++;  // the entity's range of the render list changes size
    }
}

void Scene::syncPortal(EntityId end) {
    const PortalComponent* source = portal(end);
    PortalComponent* other = source != nullptr ? portal(source->other) : nullptr;
    if (other != nullptr) {
        other->shape = source->shape;
        other->size = source->size;
        other->rim = source->rim;
        other->rimWidth = source->rimWidth;
        other->rimDepth = source->rimDepth;
    }
}

std::vector<Scene::Placed> Scene::placeAll() const {
    std::vector<Placed> placed;
    placed.reserve(entityList.size());
    auto visit = [&](auto&& self, EntityId id, const glm::mat4* parentWorld, bool parentVisible) -> void {
        const Entity& entity = entityList[entityIndex.at(id)];
        glm::mat4 world = parentWorld != nullptr ? *parentWorld * entity.local.matrix() : entity.local.matrix();
        bool visible = parentVisible && entity.visible;
        placed.push_back({&entity, world, visible});
        for (EntityId child : entity.children) {
            self(self, child, &world, visible);
        }
    };
    for (EntityId root : rootList) {
        visit(visit, root, nullptr, true);
    }
    return placed;
}

// A bar of a rim: the unit cube stretched to length x width x depth, its length along `along` (in the opening's
// plane, local to the end), centered at `center`
static glm::mat4 rimBar(const glm::mat4& end, glm::vec2 center, glm::vec2 along, float length, float width, float depth) {
    glm::vec2 across = glm::vec2(-along.y, along.x);
    glm::mat4 local(1.0f);
    local[0] = glm::vec4(along * length, 0.0f, 0.0f);
    local[1] = glm::vec4(across * width, 0.0f, 0.0f);
    local[2] = glm::vec4(0.0f, 0.0f, depth, 0.0f);
    local[3] = glm::vec4(center, 0.0f, 1.0f);
    return end * local;
}

// A rectangle's rim: four bars around it (top and bottom covering the corners). An ellipse's: short straight
// bars along its edge, each touching it in its middle (so lying outside it: the tangent of a convex shape stays
// outside), a little longer than their share of the edge so neighbors overlap on the outside.
std::vector<std::pair<uint32_t, glm::mat4>> Scene::buildPortalRims(const std::vector<PlacedPair>& pairs) const {
    std::vector<std::pair<uint32_t, glm::mat4>> bars;
    if (pairs.empty()) {
        return bars;
    }
    const SceneModel& model = assets.model(rimModel);
    for (const PlacedPair& pair : pairs) {
        const PortalComponent& portal = *pair.portal;
        if (!portal.rim || portal.rimWidth <= 0.0f) {
            continue;
        }
        float w = portal.size.x * 0.5f, h = portal.size.y * 0.5f, r = portal.rimWidth, d = std::max(portal.rimDepth, 1e-4f);
        for (int e = 0; e < 2; e++) {
            uint32_t mesh = model.parts[e].meshIndex;
            const glm::mat4& end = pair.ends[e];
            if (portal.shape == PortalShape::Rectangle) {
                bars.push_back({mesh, rimBar(end, {0.0f, h + r * 0.5f}, {1.0f, 0.0f}, 2.0f * (w + r), r, d)});
                bars.push_back({mesh, rimBar(end, {0.0f, -h - r * 0.5f}, {1.0f, 0.0f}, 2.0f * (w + r), r, d)});
                bars.push_back({mesh, rimBar(end, {-w - r * 0.5f, 0.0f}, {0.0f, 1.0f}, 2.0f * h, r, d)});
                bars.push_back({mesh, rimBar(end, {w + r * 0.5f, 0.0f}, {0.0f, 1.0f}, 2.0f * h, r, d)});
            } else {
                const int segments = 48;
                float step = glm::two_pi<float>() / segments;
                for (int k = 0; k < segments; k++) {
                    float angle = (k + 0.5f) * step;
                    glm::vec2 point = {w * std::cos(angle), h * std::sin(angle)};
                    glm::vec2 outward = glm::normalize(glm::vec2(std::cos(angle) / w, std::sin(angle) / h));
                    glm::vec2 along = {-outward.y, outward.x};
                    float edge = glm::length(glm::vec2(-w * std::sin(angle), h * std::cos(angle))) * step;
                    float length = edge * (1.0f + r / std::min(w, h)) * 1.1f;
                    bars.push_back({mesh, rimBar(end, point + outward * (r * 0.5f), along, length, r, d)});
                }
            }
        }
    }
    return bars;
}

template <typename Visit>
void Scene::forEachVisiblePart(const PlacedModel& placed, Visit&& visit) const {
    const ModelComponent& component = *placed.component;
    const SceneModel& model = assets.model(component.model);
    glm::mat4 placedModel = placed.world * (component.originalSize ? glm::mat4(1.0f) : model.fit);
    for (size_t p = 0; p < model.parts.size(); p++) {
        if (p < component.hiddenParts.size() && component.hiddenParts[p]) {
            continue;
        }
        const MeshInstance& part = model.parts[p];
        visit(part.meshIndex, placedModel * part.transform);
    }
}

// Where each point was last frame, for the motion vectors: last frame's world transform * inverse(this frame's) *
// this frame's position. Exactly identity when it didn't move (the product can be off by rounding, which would count
// as motion).
static glm::mat4 motionFrom(const glm::mat4* last, const glm::mat4& world) {
    return last != nullptr && *last != world ? *last * glm::inverse(world) : glm::mat4(1.0f);
}

void Scene::remakeInstances(RenderList& list, const std::vector<PlacedModel>& models,
                            std::vector<std::pair<uint32_t, glm::mat4>>& rims) const {
    // Last frame's entities, found by id (entities come and go; a new one has no motion). A child that moves with its
    // parent has its own world transform here, so it gets its motion too.
    std::unordered_map<EntityId, glm::mat4> previous;
    for (const RenderList::BuiltEntity& built : list.builtEntities) {
        previous.emplace(built.id, built.world);
    }
    list.instances.clear();
    list.motions.clear();
    list.builtEntities.clear();
    list.revision++;
    list.motionChanged = true;
    for (const PlacedModel& placed : models) {
        RenderList::BuiltEntity built{placed.id, placed.component->model, placed.visible,
                                      placed.component->originalSize, placed.world,
                                      {static_cast<uint32_t>(list.instances.size()), 0}, 0};
        if (placed.visible) {
            auto last = previous.find(placed.id);
            built.motion = static_cast<uint32_t>(list.motions.size());
            list.motions.push_back(motionFrom(last != previous.end() ? &last->second : nullptr, placed.world));
            forEachVisiblePart(placed, [&](uint32_t mesh, const glm::mat4& world) {
                list.instances.push_back({mesh, built.motion, world});
            });
            built.range.count = static_cast<uint32_t>(list.instances.size()) - built.range.first;
        }
        list.builtEntities.push_back(built);
    }
    // The portal rims: still (no motion), one shared identity motion
    list.firstRim = static_cast<uint32_t>(list.instances.size());
    if (!rims.empty()) {
        uint32_t motionIndex = static_cast<uint32_t>(list.motions.size());
        list.motions.push_back(glm::mat4(1.0f));
        for (const auto& [mesh, world] : rims) {
            list.instances.push_back({mesh, motionIndex, world});
        }
    }
    list.builtRims = std::move(rims);
    list.builtLayout = layoutRevision;
    list.builtAssets = assets.revision();
}

bool Scene::updateInstances(RenderList& list, const std::vector<PlacedModel>& models,
                            std::vector<std::pair<uint32_t, glm::mat4>>& rims) const {
    if (layoutRevision != list.builtLayout || assets.revision() != list.builtAssets ||
        models.size() != list.builtEntities.size() || rims.size() != list.builtRims.size()) {
        return false;
    }
    for (size_t i = 0; i < models.size(); i++) {
        const RenderList::BuiltEntity& built = list.builtEntities[i];
        if (built.id != models[i].id || built.model != models[i].component->model ||
            built.visible != models[i].visible) {
            return false;
        }
    }
    for (size_t i = 0; i < rims.size(); i++) {
        if (rims[i].first != list.builtRims[i].first) {
            return false;
        }
    }

    for (size_t i = 0; i < models.size(); i++) {
        const PlacedModel& placed = models[i];
        RenderList::BuiltEntity& built = list.builtEntities[i];
        bool moved = placed.world != built.world;
        if (built.visible) {
            glm::mat4 motion = motionFrom(&built.world, placed.world);
            if (list.motions[built.motion] != motion) {
                list.motions[built.motion] = motion;
                list.motionChanged = true;
            }
            if (moved || placed.component->originalSize != built.originalSize) {
                uint32_t next = built.range.first;
                uint32_t end = built.range.first + built.range.count;
                forEachVisiblePart(placed, [&](uint32_t, const glm::mat4& world) {
                    if (next < end) {
                        list.instances[next].world = world;
                    }
                    next++;
                });
                if (next != end) {  // its parts were shown or hidden without setPartHidden: remade instead
                    return false;
                }
                if (built.range.count > 0) {
                    list.changed.push_back(built.range);
                }
            }
        }
    }
    // (Only now: a remake instead would still need last frame's)
    for (size_t i = 0; i < models.size(); i++) {
        list.builtEntities[i].world = models[i].world;
        list.builtEntities[i].originalSize = models[i].component->originalSize;
    }
    if (rims != list.builtRims) {
        for (size_t i = 0; i < rims.size(); i++) {
            list.instances[list.firstRim + i].world = rims[i].second;
        }
        list.changed.push_back({list.firstRim, static_cast<uint32_t>(rims.size())});
        list.builtRims = std::move(rims);
    }
    return true;
}

std::vector<Scene::PlacedPair> Scene::placePairs(const std::vector<Placed>& placed) const {
    std::vector<PlacedPair> pairs;
    if (portalTable.empty()) {
        return pairs;
    }
    std::unordered_map<EntityId, size_t> placedIndex;
    placedIndex.reserve(placed.size());
    for (size_t i = 0; i < placed.size(); i++) {
        placedIndex[placed[i].entity->id] = i;
    }
    for (const Placed& p : placed) {
        const PortalComponent* portal = this->portal(p.entity->id);
        if (portal != nullptr && portal->endA && p.visible) {
            auto other = placedIndex.find(portal->other);
            if (other != placedIndex.end() && placed[other->second].visible) {
                pairs.push_back({{withoutScale(p.world), withoutScale(placed[other->second].world)}, portal});
            }
        }
    }
    return pairs;
}

// Whether the line from a to b crosses an end's opening (end: its placement without scale), and where along it
// (0..1). The same test as the shader's portalOpeningHit, for a line instead of a ray.
static bool crossesOpening(const glm::mat4& end, const PortalComponent& portal, const glm::vec3& a, const glm::vec3& b,
                           float& along) {
    glm::vec3 center(end[3]);
    glm::vec3 normal(end[2]);
    float da = glm::dot(a - center, normal);
    float db = glm::dot(b - center, normal);
    if ((da >= 0.0f) == (db >= 0.0f)) {
        return false;  // on the same side
    }
    along = da / (da - db);
    glm::vec3 offset = a + (b - a) * along - center;
    glm::vec2 p(glm::dot(offset, glm::vec3(end[0])) / (portal.size.x * 0.5f),
                glm::dot(offset, glm::vec3(end[1])) / (portal.size.y * 0.5f));
    return portal.shape == PortalShape::Ellipse ? glm::dot(p, p) <= 1.0f : std::max(std::abs(p.x), std::abs(p.y)) <= 1.0f;
}

void Scene::moveCameraThroughPortals(const glm::vec3& from) {
    if (portalTable.empty() || from == sceneCamera.position) {
        return;
    }
    std::vector<PlacedPair> pairs = placePairs(placeAll());
    glm::vec3 a = from;
    int cameOutOf = -1;  // the end it just came out of (2 * pair + end): the line starts on its plane
    // As many times as the shader lets a ray through by default ("Max portal crossings"): a step that long is rare
    for (int crossing = 0; crossing < 8; crossing++) {
        int nearest = -1;
        float nearestAlong = 2.0f;
        for (size_t i = 0; i < pairs.size() * 2; i++) {
            float along;
            if (static_cast<int>(i) != cameOutOf &&
                crossesOpening(pairs[i / 2].ends[i % 2], *pairs[i / 2].portal, a, sceneCamera.position, along) &&
                along < nearestAlong) {
                nearest = static_cast<int>(i);
                nearestAlong = along;
            }
        }
        if (nearest < 0) {
            return;
        }
        const PlacedPair& pair = pairs[nearest / 2];
        glm::mat4 toPartner = portalToPartner(pair.ends[nearest % 2], pair.ends[1 - nearest % 2]);
        glm::vec3 crossedAt = a + (sceneCamera.position - a) * nearestAlong;
        a = glm::vec3(toPartner * glm::vec4(crossedAt, 1.0f));
        sceneCamera.position = glm::vec3(toPartner * glm::vec4(sceneCamera.position, 1.0f));
        glm::vec3 forward = glm::normalize(glm::mat3(toPartner) * sceneCamera.forward());
        sceneCamera.pitch = glm::clamp(glm::degrees(std::asin(glm::clamp(forward.y, -1.0f, 1.0f))), -89.0f, 89.0f);
        sceneCamera.yaw = glm::degrees(std::atan2(forward.x, -forward.z));
        cameraTeleport = toPartner * cameraTeleport;
        cameOutOf = nearest ^ 1;
    }
}

void Scene::buildRenderList(RenderList& list) {
    std::vector<Placed> placed = placeAll();

    // The model entities, in tree order (also the instances' order)
    std::vector<PlacedModel> models;
    for (const Placed& p : placed) {
        EntityId id = p.entity->id;
        if (const ModelComponent* component = model(id); component != nullptr && assets.isLoaded(component->model)) {
            models.push_back({id, component, p.visible, p.world});
        }
    }
    std::vector<PlacedPair> pairs = placePairs(placed);
    std::vector<std::pair<uint32_t, glm::mat4>> rims = buildPortalRims(pairs);
    list.camera = sceneCamera;
    list.cameraTeleport = cameraTeleport;
    cameraTeleport = glm::mat4(1.0f);
    list.environment = sceneEnvironment;

    // The instances: only the moved entities' ranges, unless the layout changed
    list.builds++;
    list.changed.clear();
    list.motionChanged = false;
    std::vector<RenderList::BuiltEntity> before;
    std::vector<std::pair<uint32_t, glm::mat4>> rimsBefore;
    if (checkUpdates) {
        before = list.builtEntities;
        rimsBefore = rims;
    }
    if (!updateInstances(list, models, rims)) {
        list.changed.clear();
        remakeInstances(list, models, rims);
    } else if (checkUpdates) {
        RenderList scratch;
        scratch.builtEntities = std::move(before);
        remakeInstances(scratch, models, rimsBefore);
        size_t wrong = 0;
        for (size_t i = 0; i < list.instances.size() && i < scratch.instances.size(); i++) {
            const RenderInstance& x = list.instances[i];
            const RenderInstance& y = scratch.instances[i];
            wrong += x.mesh != y.mesh || x.motion != y.motion || x.world != y.world ? 1 : 0;
        }
        if (wrong > 0 || list.instances.size() != scratch.instances.size() || list.motions != scratch.motions) {
            checkFailures++;
            std::cerr << "Scene update check: " << wrong << " of " << list.instances.size() << " instances ("
                      << scratch.instances.size() << " remade) and " << (list.motions != scratch.motions ? "the" : "no")
                      << " motions differ from a remake" << std::endl;
        }
    }

    list.lights.clear();
    list.portals.clear();
    for (const PlacedPair& pair : pairs) {
        for (const glm::mat4& end : pair.ends) {
            list.portals.push_back({end, pair.portal->size * 0.5f, pair.portal->shape});
        }
    }
    for (const PlacedModel& placedModel : models) {
        if (!placedModel.visible) {
            continue;
        }
        const SceneModel& model = assets.model(placedModel.component->model);
        glm::mat4 world = placedModel.world * (placedModel.component->originalSize ? glm::mat4(1.0f) : model.fit);
        // The file's distances (falloff, range) are scaled with the object. The file's own node scales don't
        // count (glTF: they don't affect lights).
        float scale = std::cbrt(std::abs(glm::determinant(glm::mat3(world))));
        for (const PunctualLight& light : model.lights) {
            if (light.enabled) {
                list.lights.push_back({light.type, light.color, light.intensity, light.range, light.innerConeAngle,
                                       light.outerConeAngle, world * light.transform, scale});
            }
        }
    }
}

void Scene::setEnvironmentMap(const std::string& fileName) {
    EnvironmentHandle map = assets.requestEnvironment(fileName);
    mapError.clear();
    if (map == pendingMap) {
        return;  // already on its way
    }
    if (pendingMap.valid()) {
        assets.release(pendingMap);  // another one was picked meanwhile: that one isn't wanted any more
        pendingMap = EnvironmentHandle();
    }
    if (map == sceneEnvironment.map) {
        return;
    }
    assets.acquire(map);
    pendingMap = map;
    updateEnvironmentMap();  // (already loaded: at once)
}

void Scene::updateEnvironmentMap() {
    if (!pendingMap.valid() || assets.isLoading(pendingMap)) {
        return;
    }
    if (assets.isLoaded(pendingMap)) {
        if (sceneEnvironment.map.valid()) {
            assets.release(sceneEnvironment.map);  // the last user of the old map: it is unloaded
        }
        sceneEnvironment.map = pendingMap;
    } else {
        mapError = assets.environmentMap(pendingMap).name + " not loaded: " + assets.loadError(pendingMap);
        messages.push_back({SceneMessage::Level::Error, "Environment map " + mapError});
        assets.release(pendingMap);
    }
    pendingMap = EnvironmentHandle();
}

bool Scene::loading() const {
    if (pendingMap.valid()) {
        return true;
    }
    for (const auto& [id, component] : modelTable) {
        if (component.loading) {
            return true;
        }
    }
    return false;
}

void Scene::update() {
    std::vector<ModelHandle> arrived;  // reported once per model, not per entity
    for (auto& [id, component] : modelTable) {
        if (!component.loading || assets.isLoading(component.model)) {
            continue;
        }
        const std::string& entityName = find(id)->name;
        component.loading = false;
        if (assets.isLoaded(component.model)) {
            const SceneModel& model = assets.model(component.model);
            component.hiddenParts.clear();
            for (const MeshInstance& part : model.parts) {
                component.hiddenParts.push_back(part.hiddenByDefault);
            }
            for (const std::string& warning : applyLoadingEntry(component)) {
                messages.push_back({SceneMessage::Level::Warning, entityName + ": " + warning});
            }
            if (std::find(arrived.begin(), arrived.end(), component.model) == arrived.end()) {
                arrived.push_back(component.model);
                messages.push_back({SceneMessage::Level::Info, "Loaded " + model.name});
                for (const std::string& warning : applyPendingEdits(model.name)) {
                    messages.push_back({SceneMessage::Level::Warning, warning});  // (names the model)
                }
            }
            layoutRevision++;
        } else {
            // Failed: kept as a missing model, its entry saved as it was
            component.missingFile = assets.modelName(component.model);
            component.missingReason = assets.loadError(component.model);
            component.missingEntry = component.loadingEntry;
            messages.push_back({SceneMessage::Level::Error, entityName + ": model " + component.missingFile +
                                                            " not loaded: " + component.missingReason});
            const std::string& name = component.missingFile;
            pendingModelEdits.erase(std::remove_if(pendingModelEdits.begin(), pendingModelEdits.end(),
                                                   [&](const auto& edit) { return edit.first == name; }),
                                    pendingModelEdits.end());
            assets.release(component.model);
            component.model = ModelHandle();
        }
        component.loadingEntry.clear();
    }
    if (!arrived.empty()) {
        assets.materialsChanged();  // the edits applied
    }
    updateEnvironmentMap();
}
