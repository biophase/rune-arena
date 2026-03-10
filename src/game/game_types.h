#pragma once

enum class FacingDirection {
    Left,
    Right,
    Top,
    Bottom,
};

enum class PlayerActionState {
    Idle,
    Walking,
    Slashing,
    RunePlacing,
};

enum class RuneType {
    Fire = 0,
    Water = 1,
};

enum class TileType {
    Grass,
    Water,
    SpawnPoint,
    Unknown,
};

enum class SpellDirection {
    Left,
    Right,
    Top,
    Bottom,
};

struct GridCoord {
    int x = 0;
    int y = 0;

    bool operator==(const GridCoord& other) const { return x == other.x && y == other.y; }
};
