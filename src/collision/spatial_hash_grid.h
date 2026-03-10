#pragma once

#include <unordered_map>
#include <vector>

#include <raylib.h>

class SpatialHashGrid {
  public:
    explicit SpatialHashGrid(float cell_size = 64.0f);

    void Clear();
    void Insert(int entity_id, const Vector2& center, float radius);
    std::vector<int> Query(const Vector2& center, float radius) const;

  private:
    static long long MakeKey(int cell_x, int cell_y);

    float cell_size_ = 64.0f;
    std::unordered_map<long long, std::vector<int>> buckets_;
};
