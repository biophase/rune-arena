#include "gameplay/loot_tables.h"

#include <cmath>
#include <fstream>

#include <nlohmann/json.hpp>

bool LootTableLibrary::LoadFromFile(const std::string& path) {
    loaded_ = false;
    last_error_.clear();
    tables_.clear();
    source_bindings_.clear();
    match_supply_targets_per_player_.clear();

    std::ifstream file(path);
    if (!file.is_open()) {
        last_error_ = "failed to open loot tables";
        return false;
    }

    nlohmann::json json;
    try {
        file >> json;
    } catch (const std::exception& ex) {
        last_error_ = std::string("invalid JSON: ") + ex.what();
        return false;
    }

    if (const auto targets_it = json.find("match_supply_targets");
        targets_it != json.end() && targets_it->is_object()) {
        for (auto it = targets_it->begin(); it != targets_it->end(); ++it) {
            match_supply_targets_per_player_[it.key()] = it.value().get<float>();
        }
    }

    if (const auto tables_it = json.find("tables"); tables_it != json.end() && tables_it->is_object()) {
        for (auto it = tables_it->begin(); it != tables_it->end(); ++it) {
            LootTable table;
            table.id = it.key();
            const auto& value = it.value();
            if (const auto draw_counts_it = value.find("draw_counts");
                draw_counts_it != value.end() && draw_counts_it->is_array()) {
                for (const auto& entry_json : *draw_counts_it) {
                    if (!entry_json.is_object()) {
                        continue;
                    }
                    table.draw_counts.push_back(
                        {entry_json.value("count", 0), entry_json.value("weight", 1.0f)});
                }
            }
            if (const auto outcomes_it = value.find("outcomes"); outcomes_it != value.end() && outcomes_it->is_array()) {
                for (const auto& outcome_json : *outcomes_it) {
                    if (!outcome_json.is_object()) {
                        continue;
                    }
                    LootOutcome outcome;
                    outcome.weight = outcome_json.value("weight", 1.0f);
                    if (const auto items_it = outcome_json.find("items"); items_it != outcome_json.end() && items_it->is_array()) {
                        for (const auto& item_json : *items_it) {
                            if (!item_json.is_object()) {
                                continue;
                            }
                            LootItemEntry item;
                            item.object_id = item_json.value("object_id", "");
                            item.min_count = std::max(1, item_json.value("min", 1));
                            item.max_count = std::max(item.min_count, item_json.value("max", item.min_count));
                            if (!item.object_id.empty()) {
                                outcome.items.push_back(item);
                            }
                        }
                    }
                    if (!outcome.items.empty()) {
                        table.outcomes.push_back(std::move(outcome));
                    }
                }
            }
            tables_.emplace(table.id, std::move(table));
        }
    }

    if (const auto bindings_it = json.find("bindings"); bindings_it != json.end() && bindings_it->is_object()) {
        for (auto it = bindings_it->begin(); it != bindings_it->end(); ++it) {
            source_bindings_[it.key()] = it.value().get<std::string>();
        }
    }

    loaded_ = true;
    return true;
}

LootQuotaPlan LootTableLibrary::BuildMatchQuotaPlan(int player_count) const {
    LootQuotaPlan plan;
    for (const auto& [item_id, per_player] : match_supply_targets_per_player_) {
        const int total = static_cast<int>(std::ceil(std::max(0.0f, per_player) * static_cast<float>(std::max(0, player_count))));
        if (total > 0) {
            plan.guaranteed_counts[item_id] = total;
        }
    }
    return plan;
}

const LootTableLibrary::LootTable* LootTableLibrary::FindTableForSource(const std::string& source_object_id) const {
    const auto binding_it = source_bindings_.find(source_object_id);
    if (binding_it == source_bindings_.end()) {
        return nullptr;
    }
    const auto table_it = tables_.find(binding_it->second);
    return table_it == tables_.end() ? nullptr : &table_it->second;
}

int LootTableLibrary::SampleIntRange(int min_count, int max_count, std::mt19937& rng) {
    if (max_count <= min_count) {
        return std::max(1, min_count);
    }
    std::uniform_int_distribution<int> dist(min_count, max_count);
    return dist(rng);
}

int LootTableLibrary::SampleDrawCount(const LootTable& table, std::mt19937& rng) {
    if (table.draw_counts.empty()) {
        return 1;
    }
    float total_weight = 0.0f;
    for (const auto& entry : table.draw_counts) {
        total_weight += std::max(0.0f, entry.weight);
    }
    if (total_weight <= 0.0f) {
        return table.draw_counts.front().count;
    }
    std::uniform_real_distribution<float> dist(0.0f, total_weight);
    float cursor = dist(rng);
    for (const auto& entry : table.draw_counts) {
        cursor -= std::max(0.0f, entry.weight);
        if (cursor <= 0.0f) {
            return entry.count;
        }
    }
    return table.draw_counts.back().count;
}

const LootTableLibrary::LootOutcome* LootTableLibrary::SampleOutcome(const LootTable& table, std::mt19937& rng) {
    if (table.outcomes.empty()) {
        return nullptr;
    }
    float total_weight = 0.0f;
    for (const auto& outcome : table.outcomes) {
        total_weight += std::max(0.0f, outcome.weight);
    }
    if (total_weight <= 0.0f) {
        return &table.outcomes.front();
    }
    std::uniform_real_distribution<float> dist(0.0f, total_weight);
    float cursor = dist(rng);
    for (const auto& outcome : table.outcomes) {
        cursor -= std::max(0.0f, outcome.weight);
        if (cursor <= 0.0f) {
            return &outcome;
        }
    }
    return &table.outcomes.back();
}

void LootTableLibrary::AppendOutcomeSpawns(const LootOutcome& outcome, std::vector<LootSpawn>* out_spawns, std::mt19937& rng) {
    if (out_spawns == nullptr) {
        return;
    }
    for (const auto& item : outcome.items) {
        out_spawns->push_back({item.object_id, SampleIntRange(item.min_count, item.max_count, rng)});
    }
}

const LootTableLibrary::LootOutcome* LootTableLibrary::SelectQuotaOutcome(
    const LootTable& table, std::unordered_map<std::string, int>* quota_remaining) {
    if (quota_remaining == nullptr || quota_remaining->empty()) {
        return nullptr;
    }
    const LootOutcome* best_outcome = nullptr;
    int best_score = 0;
    for (const auto& outcome : table.outcomes) {
        int score = 0;
        for (const auto& item : outcome.items) {
            auto quota_it = quota_remaining->find(item.object_id);
            if (quota_it != quota_remaining->end() && quota_it->second > 0) {
                score += quota_it->second;
            }
        }
        if (score > best_score) {
            best_score = score;
            best_outcome = &outcome;
        }
    }
    return best_outcome;
}

void LootTableLibrary::ConsumeQuotaForOutcome(const LootOutcome& outcome,
                                              std::unordered_map<std::string, int>* quota_remaining,
                                              std::mt19937& rng) {
    if (quota_remaining == nullptr) {
        return;
    }
    for (const auto& item : outcome.items) {
        auto quota_it = quota_remaining->find(item.object_id);
        if (quota_it == quota_remaining->end() || quota_it->second <= 0) {
            continue;
        }
        quota_it->second = std::max(0, quota_it->second - SampleIntRange(item.min_count, item.max_count, rng));
    }
}

std::vector<LootSpawn> LootTableLibrary::ResolveDrops(const std::string& source_object_id, const std::vector<DropEntry>& legacy_drops,
                                                      std::unordered_map<std::string, int>* quota_remaining,
                                                      std::mt19937& rng) const {
    std::vector<LootSpawn> spawns;
    if (const LootTable* table = FindTableForSource(source_object_id); table != nullptr) {
        const int draws = std::max(0, SampleDrawCount(*table, rng));
        for (int i = 0; i < draws; ++i) {
            const LootOutcome* outcome = SelectQuotaOutcome(*table, quota_remaining);
            if (outcome == nullptr) {
                outcome = SampleOutcome(*table, rng);
            }
            if (outcome == nullptr) {
                continue;
            }
            AppendOutcomeSpawns(*outcome, &spawns, rng);
            ConsumeQuotaForOutcome(*outcome, quota_remaining, rng);
        }
        return spawns;
    }

    for (const DropEntry& drop : legacy_drops) {
        bool force_drop = false;
        if (quota_remaining != nullptr) {
            auto quota_it = quota_remaining->find(drop.object_id);
            force_drop = quota_it != quota_remaining->end() && quota_it->second > 0;
        }
        std::uniform_real_distribution<float> chance_dist(0.0f, 1.0f);
        if (!force_drop && chance_dist(rng) > drop.chance) {
            continue;
        }
        const int count = SampleIntRange(std::max(1, drop.min_count), std::max(drop.min_count, drop.max_count), rng);
        spawns.push_back({drop.object_id, count});
        if (quota_remaining != nullptr) {
            auto quota_it = quota_remaining->find(drop.object_id);
            if (quota_it != quota_remaining->end()) {
                quota_it->second = std::max(0, quota_it->second - count);
            }
        }
    }
    return spawns;
}
