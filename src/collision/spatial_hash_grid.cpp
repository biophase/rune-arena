#include "collision/spatial_hash_grid.h"

#include <algorithm>

SpatialHashGrid::SpatialHashGrid(float cell_size) : cell_size_(cell_size) {}

void SpatialHashGrid::Clear() { buckets_.clear(); }

void SpatialHashGrid::Insert(int entity_id, const Vector2& center, float radius) {
    const int min_x = static_cast<int>((center.x - radius) / cell_size_);
    const int max_x = static_cast<int>((center.x + radius) / cell_size_);
    const int min_y = static_cast<int>((center.y - radius) / cell_size_);
    const int max_y = static_cast<int>((center.y + radius) / cell_size_);

    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            buckets_[MakeKey(x, y)].push_back(entity_id);
        }
    }
}

std::vector<int> SpatialHashGrid::Query(const Vector2& center, float radius) const {
    const int min_x = static_cast<int>((center.x - radius) / cell_size_);
    const int max_x = static_cast<int>((center.x + radius) / cell_size_);
    const int min_y = static_cast<int>((center.y - radius) / cell_size_);
    const int max_y = static_cast<int>((center.y + radius) / cell_size_);

    std::vector<int> results;
    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const auto it = buckets_.find(MakeKey(x, y));
            if (it == buckets_.end()) {
                continue;
            }
            results.insert(results.end(), it->second.begin(), it->second.end());
        }
    }

    std::sort(results.begin(), results.end());
    results.erase(std::unique(results.begin(), results.end()), results.end());
    return results;
}

long long SpatialHashGrid::MakeKey(int cell_x, int cell_y) {
    return (static_cast<long long>(cell_x) << 32) ^ static_cast<unsigned int>(cell_y);
}
