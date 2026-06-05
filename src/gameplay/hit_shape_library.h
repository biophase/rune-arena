#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <raylib.h>

enum class HitShapeType {
    Circle,
    Aabb,
    ConvexPolygon,
};

struct HitShapeDefinition {
    std::string id;
    HitShapeType type = HitShapeType::Circle;
    Vector2 center = {0.0f, 0.0f};
    float radius = 0.0f;
    Vector2 size = {0.0f, 0.0f};
    std::vector<Vector2> points;
};

class HitShapeLibrary {
  public:
    bool LoadFromFile(const std::string& path);

    bool IsLoaded() const { return loaded_; }
    const std::string& GetLastError() const { return last_error_; }
    const HitShapeDefinition* FindById(const std::string& id) const;

    static bool OverlapsAabb(const HitShapeDefinition& shape, Vector2 origin, float rotation_radians,
                             const Rectangle& target_aabb, Vector2* out_normal = nullptr,
                             float* out_penetration = nullptr);
    static std::vector<Vector2> BuildWorldPoints(const HitShapeDefinition& shape, Vector2 origin, float rotation_radians);

  private:
    static bool PolygonVsPolygon(const std::vector<Vector2>& a, const std::vector<Vector2>& b, Vector2* out_normal,
                                 float* out_penetration);

    bool loaded_ = false;
    std::string last_error_;
    std::unordered_map<std::string, HitShapeDefinition> shapes_;
};
