#include "spells/spell_pattern_loader.h"

#include <cctype>
#include <fstream>

#include <nlohmann/json.hpp>

namespace {

bool ParsePatternGrid(const nlohmann::json& node, std::vector<std::vector<std::string>>& out_grid) {
    if (!node.is_array() || node.empty()) {
        return false;
    }

    std::vector<std::vector<std::string>> parsed;
    int expected_cols = -1;
    for (const auto& row_json : node) {
        if (!row_json.is_array() || row_json.empty()) {
            return false;
        }

        std::vector<std::string> row;
        row.reserve(row_json.size());
        for (const auto& cell_json : row_json) {
            if (cell_json.is_null()) {
                row.push_back("");
                continue;
            }
            if (!cell_json.is_string()) {
                return false;
            }
            std::string cell = cell_json.get<std::string>();
            if (cell == "_") {
                cell.clear();
            }
            row.push_back(std::move(cell));
        }

        if (expected_cols < 0) {
            expected_cols = static_cast<int>(row.size());
        } else if (static_cast<int>(row.size()) != expected_cols) {
            return false;
        }

        parsed.push_back(std::move(row));
    }

    if (parsed.empty() || expected_cols <= 0) {
        return false;
    }

    out_grid = std::move(parsed);
    return true;
}

void CollectPatternVariants(const nlohmann::json& node,
                            std::vector<std::vector<std::vector<std::string>>>& out_variants) {
    std::vector<std::vector<std::string>> grid;
    if (ParsePatternGrid(node, grid)) {
        out_variants.push_back(std::move(grid));
        return;
    }

    if (!node.is_array()) {
        return;
    }

    for (const auto& child : node) {
        CollectPatternVariants(child, out_variants);
    }
}

}  // namespace

bool SpellPatternLoader::LoadFromFile(const std::string& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return false;
    }

    nlohmann::json json;
    input >> json;

    rune_symbol_lookup_.clear();
    patterns_.clear();

    const auto abbreviations_it = json.find("abbreviations");
    if (abbreviations_it != json.end() && abbreviations_it->is_object()) {
        for (auto it = abbreviations_it->begin(); it != abbreviations_it->end(); ++it) {
            RuneType rune_type = RuneType::Fire;
            if (it.key().find("water") != std::string::npos) {
                rune_type = RuneType::Water;
            } else if (it.key().find("catal") != std::string::npos) {
                rune_type = RuneType::Catalyst;
            }
            auto add_symbol = [&](const std::string& symbol_text, PlacementConstraint explicit_constraint) {
                PlacementConstraint constraint = explicit_constraint;
                if (constraint == PlacementConstraint::Any && !symbol_text.empty()) {
                    const unsigned char c = static_cast<unsigned char>(symbol_text.front());
                    constraint = std::isupper(c) ? PlacementConstraint::Latest : PlacementConstraint::Old;
                }
                rune_symbol_lookup_[symbol_text] = RuneSymbolInfo{rune_type, constraint};
            };

            if (it.value().is_array()) {
                for (const auto& symbol : it.value()) {
                    add_symbol(symbol.get<std::string>(), PlacementConstraint::Any);
                }
            } else if (it.value().is_object()) {
                for (auto symbol_it = it.value().begin(); symbol_it != it.value().end(); ++symbol_it) {
                    PlacementConstraint constraint = PlacementConstraint::Any;
                    if (symbol_it.key() == "latest") {
                        constraint = PlacementConstraint::Latest;
                    } else if (symbol_it.key() == "old") {
                        constraint = PlacementConstraint::Old;
                    }

                    if (symbol_it.value().is_string()) {
                        add_symbol(symbol_it.value().get<std::string>(), constraint);
                    } else if (symbol_it.value().is_array()) {
                        for (const auto& symbol : symbol_it.value()) {
                            add_symbol(symbol.get<std::string>(), constraint);
                        }
                    }
                }
            }
        }
    }

    const auto patterns_it = json.find("spell_patterns");
    if (patterns_it == json.end() || !patterns_it->is_object()) {
        return true;
    }

    for (auto it = patterns_it->begin(); it != patterns_it->end(); ++it) {
        SpellPatternDefinition definition;
        definition.spell_name = it.key();
        const auto order_relevant_it = it.value().find("order_relevant");
        const auto order_relevent_it = it.value().find("order_relevent");
        if (order_relevant_it != it.value().end() && order_relevant_it->is_boolean()) {
            definition.order_relevant = order_relevant_it->get<bool>();
        } else if (order_relevent_it != it.value().end() && order_relevent_it->is_boolean()) {
            definition.order_relevant = order_relevent_it->get<bool>();
        }

        const auto directions_it = it.value().find("directions");
        if (directions_it == it.value().end() || !directions_it->is_object()) {
            continue;
        }

        for (auto dir_it = directions_it->begin(); dir_it != directions_it->end(); ++dir_it) {
            DirectionalPattern directional_pattern;
            directional_pattern.direction = ParseDirection(dir_it.key());

            CollectPatternVariants(dir_it.value(), directional_pattern.variants);
            if (!directional_pattern.variants.empty()) {
                definition.directional_patterns.push_back(directional_pattern);
            }
        }

        if (!definition.directional_patterns.empty()) {
            patterns_.push_back(definition);
        }
    }

    return true;
}

const std::vector<SpellPatternDefinition>& SpellPatternLoader::GetPatterns() const { return patterns_; }

bool SpellPatternLoader::SymbolMatchesRune(const std::string& symbol, RuneType rune_type) const {
    const auto info = GetSymbolInfo(symbol);
    return info.has_value() && info->rune_type == rune_type;
}

std::optional<RuneSymbolInfo> SpellPatternLoader::GetSymbolInfo(const std::string& symbol) const {
    const auto it = rune_symbol_lookup_.find(symbol);
    if (it == rune_symbol_lookup_.end()) {
        return std::nullopt;
    }
    return it->second;
}

SpellDirection SpellPatternLoader::ParseDirection(const std::string& text) const {
    if (text == "left") {
        return SpellDirection::Left;
    }
    if (text == "right") {
        return SpellDirection::Right;
    }
    if (text == "top") {
        return SpellDirection::Top;
    }
    if (text == "bot") {
        return SpellDirection::Bottom;
    }
    if (text == "horizontal") {
        return SpellDirection::Horizontal;
    }
    if (text == "vertical") {
        return SpellDirection::Vertical;
    }
    return SpellDirection::Right;
}
