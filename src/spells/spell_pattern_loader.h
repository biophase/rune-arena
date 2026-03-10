#pragma once

#include <string>
#include <unordered_map>
#include <optional>
#include <vector>

#include "game/game_types.h"

struct DirectionalPattern {
    SpellDirection direction = SpellDirection::Right;
    std::vector<std::vector<std::string>> rows;
};

enum class PlacementConstraint {
    Any,
    Old,
    Latest,
};

struct RuneSymbolInfo {
    RuneType rune_type = RuneType::Fire;
    PlacementConstraint placement_constraint = PlacementConstraint::Any;
};

struct SpellPatternDefinition {
    std::string spell_name;
    std::vector<DirectionalPattern> directional_patterns;
};

class SpellPatternLoader {
  public:
    bool LoadFromFile(const std::string& path);

    const std::vector<SpellPatternDefinition>& GetPatterns() const;
    bool SymbolMatchesRune(const std::string& symbol, RuneType rune_type) const;
    std::optional<RuneSymbolInfo> GetSymbolInfo(const std::string& symbol) const;

  private:
    SpellDirection ParseDirection(const std::string& text) const;

    std::unordered_map<std::string, RuneSymbolInfo> rune_symbol_lookup_;
    std::vector<SpellPatternDefinition> patterns_;
};
