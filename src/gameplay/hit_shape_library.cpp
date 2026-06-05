#include "gameplay/hit_shape_library.h"

#include <cmath>
#include <fstream>
#include <limits>

#include <nlohmann/json.hpp>
#include <raymath.h>

#include "collision/collision_world.h"

namespace {

std::vector<Vector2> RectangleToPoints(const Rectangle& rect) {
    return {
        {rect.x, rect.y},
        {rect.x + rect.width, rect.y},
        {rect.x + rect.width, rect.y + rect.height},
        {rect.x, rect.y + rect.height},
    };
}

Vector2 RotatePoint(Vector2 point, float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    return {point.x * c - point.y * s, point.x * s + point.y * c};
}

bool ProjectPolygon(const std::vector<Vector2>& polygon, Vector2 axis, float* out_min, float* out_max) {
    if (polygon.empty() || out_min == nullptr || out_max == nullptr) {
        return false;
    }
    axis = Vector2Normalize(axis);
    float min_value = Vector2DotProduct(polygon.front(), axis);
    float max_value = min_value;
    for (size_t i = 1; i < polygon.size(); ++i) {
        const float value = Vector2DotProduct(polygon[i], axis);
        min_value = std::min(min_value, value);
        max_value = std::max(max_value, value);
    }
    *out_min = min_value;
    *out_max = max_value;
    return true;
}

}  // namespace

bool HitShapeLibrary::LoadFromFile(const std::string& path) {
    loaded_ = false;
    last_error_.clear();
    shapes_.clear();

    std::ifstream file(path);
    if (!file.is_open()) {
        last_error_ = "failed to open hit shapes";
        return false;
    }

    nlohmann::json json;
    try {
        file >> json;
    } catch (const std::exception& ex) {
        last_error_ = std::string("invalid JSON: ") + ex.what();
        return false;
    }

    const auto shapes_it = json.find("shapes");
    if (shapes_it == json.end() || !shapes_it->is_object()) {
        last_error_ = "missing shapes object";
        return false;
    }

    for (auto it = shapes_it->begin(); it != shapes_it->end(); ++it) {
        const auto& value = it.value();
        HitShapeDefinition shape;
        shape.id = it.key();
        const std::string type = value.value("type", "circle");
        if (type == "circle") {
            shape.type = HitShapeType::Circle;
        } else if (type == "aabb") {
            shape.type = HitShapeType::Aabb;
        } else if (type == "convex_polygon") {
            shape.type = HitShapeType::ConvexPolygon;
        } else {
            last_error_ = "unknown hit shape type for '" + shape.id + "'";
            return false;
        }

        if (const auto center_it = value.find("center"); center_it != value.end() && center_it->is_object()) {
            shape.center.x = center_it->value("x", 0.0f);
            shape.center.y = center_it->value("y", 0.0f);
        }
        shape.radius = value.value("radius", 0.0f);
        if (const auto size_it = value.find("size"); size_it != value.end() && size_it->is_object()) {
            shape.size.x = size_it->value("x", 0.0f);
            shape.size.y = size_it->value("y", 0.0f);
        }
        if (const auto points_it = value.find("points"); points_it != value.end() && points_it->is_array()) {
            for (const auto& point_json : *points_it) {
                if (!point_json.is_object()) {
                    continue;
                }
                shape.points.push_back({point_json.value("x", 0.0f), point_json.value("y", 0.0f)});
            }
        }
        shapes_.emplace(shape.id, std::move(shape));
    }

    loaded_ = true;
    return true;
}

const HitShapeDefinition* HitShapeLibrary::FindById(const std::string& id) const {
    auto it = shapes_.find(id);
    return it == shapes_.end() ? nullptr : &it->second;
}

std::vector<Vector2> HitShapeLibrary::BuildWorldPoints(const HitShapeDefinition& shape, Vector2 origin, float rotation_radians) {
    std::vector<Vector2> world_points;
    if (shape.type == HitShapeType::ConvexPolygon) {
        world_points.reserve(shape.points.size());
        for (const Vector2& point : shape.points) {
            world_points.push_back(Vector2Add(origin, RotatePoint(point, rotation_radians)));
        }
        return world_points;
    }

    if (shape.type == HitShapeType::Aabb) {
        const Vector2 half = {shape.size.x * 0.5f, shape.size.y * 0.5f};
        const std::vector<Vector2> local_points = {
            {shape.center.x - half.x, shape.center.y - half.y},
            {shape.center.x + half.x, shape.center.y - half.y},
            {shape.center.x + half.x, shape.center.y + half.y},
            {shape.center.x - half.x, shape.center.y + half.y},
        };
        world_points.reserve(local_points.size());
        for (const Vector2& point : local_points) {
            world_points.push_back(Vector2Add(origin, RotatePoint(point, rotation_radians)));
        }
    }
    return world_points;
}

bool HitShapeLibrary::PolygonVsPolygon(const std::vector<Vector2>& a, const std::vector<Vector2>& b, Vector2* out_normal,
                                       float* out_penetration) {
    float smallest_overlap = std::numeric_limits<float>::max();
    Vector2 smallest_axis = {1.0f, 0.0f};

    auto test_axes = [&](const std::vector<Vector2>& polygon) {
        for (size_t i = 0; i < polygon.size(); ++i) {
            const Vector2 current = polygon[i];
            const Vector2 next = polygon[(i + 1) % polygon.size()];
            const Vector2 edge = Vector2Subtract(next, current);
            Vector2 axis = {-edge.y, edge.x};
            if (Vector2LengthSqr(axis) <= 0.0001f) {
                continue;
            }
            axis = Vector2Normalize(axis);

            float a_min = 0.0f;
            float a_max = 0.0f;
            float b_min = 0.0f;
            float b_max = 0.0f;
            ProjectPolygon(a, axis, &a_min, &a_max);
            ProjectPolygon(b, axis, &b_min, &b_max);

            const float overlap = std::min(a_max, b_max) - std::max(a_min, b_min);
            if (overlap <= 0.0f) {
                return false;
            }
            if (overlap < smallest_overlap) {
                smallest_overlap = overlap;
                smallest_axis = axis;
            }
        }
        return true;
    };

    if (!test_axes(a) || !test_axes(b)) {
        return false;
    }

    if (out_penetration != nullptr) {
        *out_penetration = smallest_overlap;
    }
    if (out_normal != nullptr) {
        *out_normal = smallest_axis;
    }
    return true;
}

bool HitShapeLibrary::OverlapsAabb(const HitShapeDefinition& shape, Vector2 origin, float rotation_radians,
                                   const Rectangle& target_aabb, Vector2* out_normal, float* out_penetration) {
    Vector2 normal = {0.0f, 0.0f};
    float penetration = 0.0f;

    if (shape.type == HitShapeType::Circle) {
        const Vector2 center = Vector2Add(origin, RotatePoint(shape.center, rotation_radians));
        const bool overlap = CollisionWorld::CircleVsAabb(center, shape.radius, target_aabb, normal, penetration);
        if (overlap && out_normal != nullptr) {
            *out_normal = normal;
        }
        if (overlap && out_penetration != nullptr) {
            *out_penetration = penetration;
        }
        return overlap;
    }

    const std::vector<Vector2> shape_points = BuildWorldPoints(shape, origin, rotation_radians);
    const std::vector<Vector2> target_points = RectangleToPoints(target_aabb);
    const bool overlap = PolygonVsPolygon(shape_points, target_points, &normal, &penetration);
    if (overlap && out_normal != nullptr) {
        *out_normal = normal;
    }
    if (overlap && out_penetration != nullptr) {
        *out_penetration = penetration;
    }
    return overlap;
}
