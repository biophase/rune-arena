#pragma once

#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/map_object.h"

struct LootSpawn {
    std::string object_id;
    int count = 1;
};

struct LootQuotaPlan {
    std::unordered_map<std::string, int> guaranteed_counts;
};

class LootTableLibrary {
  public:
    bool LoadFromFile(const std::string& path);

    bool IsLoaded() const { return loaded_; }
    const std::string& GetLastError() const { return last_error_; }

    LootQuotaPlan BuildMatchQuotaPlan(int player_count) const;
    std::vector<LootSpawn> ResolveDrops(const std::string& source_object_id, const std::vector<DropEntry>& legacy_drops,
                                        std::unordered_map<std::string, int>* quota_remaining,
                                        std::mt19937& rng) const;

  private:
    struct LootItemEntry {
        std::string object_id;
        int min_count = 1;
        int max_count = 1;
    };

    struct WeightedCountEntry {
        int count = 0;
        float weight = 1.0f;
    };

    struct LootOutcome {
        float weight = 1.0f;
        std::vector<LootItemEntry> items;
    };

    struct LootTable {
        std::string id;
        std::vector<WeightedCountEntry> draw_counts;
        std::vector<LootOutcome> outcomes;
    };

    const LootTable* FindTableForSource(const std::string& source_object_id) const;
    static int SampleIntRange(int min_count, int max_count, std::mt19937& rng);
    static int SampleDrawCount(const LootTable& table, std::mt19937& rng);
    static const LootOutcome* SampleOutcome(const LootTable& table, std::mt19937& rng);
    static void AppendOutcomeSpawns(const LootOutcome& outcome, std::vector<LootSpawn>* out_spawns, std::mt19937& rng);
    static const LootOutcome* SelectQuotaOutcome(const LootTable& table, std::unordered_map<std::string, int>* quota_remaining);
    static void ConsumeQuotaForOutcome(const LootOutcome& outcome, std::unordered_map<std::string, int>* quota_remaining,
                                       std::mt19937& rng);

    bool loaded_ = false;
    std::string last_error_;
    std::unordered_map<std::string, LootTable> tables_;
    std::unordered_map<std::string, std::string> source_bindings_;
    std::unordered_map<std::string, float> match_supply_targets_per_player_;
};
