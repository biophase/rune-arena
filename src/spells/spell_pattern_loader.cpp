#include "spells/spell_pattern_loader.h"

#include <cctype>
#include <fstream>

#include <nlohmann/json.hpp>

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

        const auto directions_it = it.value().find("directions");
        if (directions_it == it.value().end() || !directions_it->is_object()) {
            continue;
        }

        for (auto dir_it = directions_it->begin(); dir_it != directions_it->end(); ++dir_it) {
            DirectionalPattern directional_pattern;
            directional_pattern.direction = ParseDirection(dir_it.key());

            if (dir_it.value().is_array()) {
                for (const auto& row_json : dir_it.value()) {
                    std::vector<std::string> row;
                    if (!row_json.is_array()) {
                        continue;
                    }
                    for (const auto& cell : row_json) {
                        row.push_back(cell.get<std::string>());
                    }
                    directional_pattern.rows.push_back(row);
                }
            }

            if (!directional_pattern.rows.empty()) {
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
    if (text == "top") {
        return SpellDirection::Top;
    }
    if (text == "bot") {
        return SpellDirection::Bottom;
    }
    return SpellDirection::Right;
}
