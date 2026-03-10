Below is the full Markdown as plain text. It intentionally does NOT use triple backticks so it should copy cleanly into your editor.

Rune Arena — Codex Starter Pack

This file contains:

a shorter Codex-optimized prompt

a small architecture diagram

a set of starter class / file stubs Codex should create first

1. Codex-optimized prompt

Create a small LAN-playable C++ prototype game called Rune Arena.

Core game idea

red team vs blue team

players are wizards

players move continuously in world space

the world also contains a grid. This is the map. By default we have two tiles: "tile_grass", and "tile_water". They are read from maps. Also note that at startup the team spawn points are randomly placed on a tile marked "team_spawn_point" (see tiles_mapping.json). The map is selected together with the game mode from the lobby room

runes can only be placed on that grid and on allowable tiles. Allowable tiles are hard-coded (e.g. in "constants.h) and should be ["tile_grass"] for starters.

when predefined rune patterns are completed, they trigger spells

first prototype only needs a couple of predefined patterns, which are found in assets/spell_patterns.json.

initial game mode:

Most Kills
team with most kills after 120 seconds wins

Main technical requirements

language: C++17 or newer

build system: CMake

target platforms:

macOS

Windows 10

use dependencies from ./external

Libraries

raylib

raygui

nlohmann/json

PlatformFolders

ENet

Networking

Use host-authoritative LAN multiplayer.

one player hosts

host is the single source of truth

clients send inputs

host simulates gameplay

host sends snapshots back

Implement:

host game on port 7777

join by LAN IP

simple LAN discovery using UDP broadcast

UI

Use raygui with dark style by default.

Implement these screens:

Main Menu

player name input

Join Game

Host Game

Lobby

joined players list

host LAN IP display

mode display: Most Kills

round time display: 120

host-only Start Match

Match

render players

render rune grid

show HP

show scores

show timer

Config / settings

Store config in the user config folder using PlatformFolders.

Save JSON config to

<user-config-dir>/RuneArena/config.json

At minimum save:

player_name

window_width

window_height

fullscreen

Gameplay architecture

Use:

fixed timestep

semi-implicit Euler integration

simple explicit state structs

small typed event queue

Integration:

vel += acc * dt
pos += vel * dt

State objects

Create these core state structs:

UserSettings

MatchState

Player

Rune

Projectile

GameState

GameState should contain at least:

MatchState match
vector<Player> players
vector<Rune> runes
vector<Projectile> projectiles

Do not use a hidden singleton global state. Pass state explicitly to systems.

Event system

Create a small typed event system.

Events are facts that happened, not condition lambdas.

Event types:

RunePlacedEvent

RunePatternCompletedEvent

SpellCastEvent

PlayerHitEvent

PlayerDiedEvent

MatchEndedEvent

Create an EventQueue.

Systems update state directly, then emit events.

Modes

Create abstract base class BaseMode with methods:

GetUiName()
Update(GameState&, EventQueue&, float dt)
IsFinished(const GameState&)
GetWinningTeam(const GameState&)

Implement first mode:

MostKillsMode

Collision

Use:

circles for moving entities

AABBs for static world obstacles

uniform spatial grid for broad phase

Support:

circle vs circle
circle vs AABB
AABB vs AABB if needed

Collision response:

1 detect overlap
2 compute penetration / normal
3 separate colliders
4 remove velocity into collision normal

First playable scope

main menu

config save/load

LAN host

LAN join

LAN discovery

lobby

one map

red vs blue teams

continuous movement

rune placement on grid

one rune pattern that creates a fireball

projectile collision and damage

Most Kills mode

timer

post-match screen

Code generation expectations

Generate a compiling prototype with a clean folder structure and readable code.

Prefer

explicit structs/classes

simple systems

straightforward code

Avoid

overengineering

unnecessary abstractions

giant monolithic files

2. Architecture diagram

GameApp
│
├── ConfigManager
├── NetworkManager
├── BaseMode
└── GameState

GameState contains

MatchState
Players
Runes
Projectiles

Systems update GameState:

Input System
Movement System
Collision System
Rune System
Combat System
Mode System

All systems can emit events into EventQueue.

EventQueue then feeds:

Score updates
Visual effects
UI
Networking replication

Update order

1 Poll networking
2 Collect input
3 Update movement
4 Resolve collisions
5 Update projectiles
6 Resolve combat
7 Update rune logic
8 Update mode
9 Process events
10 Render

3. Starter class / file stubs

Create the following files first (though expanding this would probably be neessesary):

/src/main.cpp
/src/core/game_app.h
/src/core/game_app.cpp
/src/core/constants.h

/src/config/user_settings.h
/src/config/config_manager.h
/src/config/config_manager.cpp

/src/game/match_state.h
/src/game/player.h
/src/game/rune.h
/src/game/projectile.h
/src/game/game_state.h

/src/events/event_types.h
/src/events/event_queue.h
/src/events/event_queue.cpp

/src/modes/base_mode.h
/src/modes/most_kills_mode.h
/src/modes/most_kills_mode.cpp

/src/net/network_manager.h
/src/net/network_manager.cpp
/src/net/lan_discovery.h
/src/net/lan_discovery.cpp

/src/collision/spatial_hash_grid.h
/src/collision/spatial_hash_grid.cpp
/src/collision/collision_world.h
/src/collision/collision_world.cpp

/src/ui/ui_main_menu.h
/src/ui/ui_main_menu.cpp
/src/ui/ui_lobby.h
/src/ui/ui_lobby.cpp
/src/ui/ui_match.h
/src/ui/ui_match.cpp

Recommended minimal contents

constants.h

namespace Constants
{
WindowWidth = 1280
WindowHeight = 720

DefaultPort = 7777  

FixedDt = 1.0 / 60.0  

PlayerRadius = 12.0  
PlayerAcceleration = 900.0  
PlayerFriction = 8.0  

ProjectileRadius = 4.0  
ProjectileSpeed = 420.0  

MatchDurationSeconds = 120  

RuneGridWidth = 24  
RuneGridHeight = 16  
RuneCellSize = 32  

SpatialCellSize = 64  

}

user_settings.h

struct UserSettings
{
string player_name = "Player"
int window_width = 1280
int window_height = 720
bool fullscreen = false
}

match_state.h

struct MatchState
{
int round_time_seconds = 120
float time_remaining = 120

bool match_running = false  
bool match_finished = false  

int red_team_kills = 0  
int blue_team_kills = 0  

}

player.h

struct Player
{
int id = -1
string name
int team = 0

Vector2 pos  
Vector2 vel  
Vector2 aim_dir  

float radius = 12  

int hp = 100  
int kills = 0  
bool alive = true  

}

rune.h

struct Rune
{
int owner_player_id = -1
int grid_x = 0
int grid_y = 0
int rune_type = 0
bool active = true
}

projectile.h

struct Projectile
{
int owner_player_id = -1
Vector2 pos
Vector2 vel
float radius = 4
bool alive = true
}

game_state.h

struct GameState
{
MatchState match
vector<Player> players
vector<Rune> runes
vector<Projectile> projectiles
}

event_types.h

RunePlacedEvent
RunePatternCompletedEvent
SpellCastEvent
PlayerHitEvent
PlayerDiedEvent
MatchEndedEvent

event_queue.h

class EventQueue
{
Push(event)
GetEvents()
Clear()
}

base_mode.h

class BaseMode
{
virtual string GetUiName()
virtual Update(GameState&, EventQueue&, float dt)
virtual bool IsFinished(GameState&)
virtual int GetWinningTeam(GameState&)
}

most_kills_mode.h

class MostKillsMode : public BaseMode

network_manager.h

class NetworkManager
{
StartHost(port)
StartClient()
ConnectToHost(ip, port)
Poll()
IsHost()
}

game_app.h

enum AppScreen

MainMenu
Lobby
InMatch
PostMatch

class GameApp
{
Initialize()
Run()
Shutdown()

Update()  
Render()  

UpdateMainMenu()  
UpdateLobby()  
UpdateMatch()  
UpdatePostMatch()  

}

Recommended Codex instruction ordering

1 create folder structure
2 create headers / source files
3 wire CMake
4 implement config loading
5 implement main menu + lobby UI
6 implement host/join networking skeleton
7 implement match state + movement
8 implement rune placement
9 implement rune pattern → fireball
10 implement collision and damage
11 implement MostKills mode
12 make everything compile and run

Final instruction to Codex

When generating code:

prefer small readable files

prefer explicit data structs over generic systems

keep networking simple and host-authoritative

keep rendering placeholder/simple

implement only the minimum needed for a playable prototype

do not invent extra engine layers unless clearly needed

Additional gameplay / asset requirements

Asset metadata

Use both the PNG sprite sheet and its JSON metadata file.

Load sprite sheets from PNG files.

Load animation metadata from the corresponding JSON file.

The JSON metadata defines:

texture

cell_width

cell_height

animation names

facings

frame coordinates

fps

The runtime animation system should use the JSON metadata instead of hardcoded frame coordinates.

Support facings side, top, bot, and default.

For side-facing wizard sprites, use runtime mirroring:

facing right = draw side normally

facing left = draw side mirrored

The sprite metadata file already exists in assets and should be used directly.

Player facing and sprite selection

The player always faces the mouse cursor.

Compute the facing direction from the vector from player position to mouse world position.

Quantize the facing direction into one of:

top

left

bot

right

For animation selection:

use the idle animation when the player is not moving

use the walking animation when the player is moving

The idle animation chosen should be the idle animation whose facing is closest to the current facing direction.

Since the metadata only contains side, top, and bot:

right uses side without mirroring

left uses side with mirroring

top uses top

bot uses bot

Player melee attack

Add a melee attack bound to left mouse click when not in rune placing mode.

The melee attack should use the facing direction of the player.

On attack, create a temporary circular hitbox in front of the player in the direction they are facing.

This hitbox should exist only briefly.

The hitbox should damage enemy players it overlaps.

The slash animation should play during the attack.

Add the needed gameplay state for melee attack timing / cooldown / active hit window.

Add an event type for melee hits if needed, or reuse PlayerHitEvent if appropriate.

Rune placing state

Add a rune placing state to the player.

Each rune type is bound to a key:

1 = fire rune

2 = water rune

Pressing one of these keys activates rune placing mode for that rune type.

While in rune placing mode:

left mouse click places the selected rune instead of performing melee attack

the world grid is shown in a radius of 3 cells around the mouse

Render the placement preview as follows:

draw the grid_overlay sprite over each valid cell in the display radius

draw it at 50% opacity

snap the currently selected rune preview to the closest cell centroid under the mouse

draw the rune preview semi-transparently

Two runes cannot occupy the same cell.

If a cell is already occupied by a rune, placement on that cell should be rejected.

Add the required player/input state to support:

normal mode

rune placing mode

currently selected rune type

Spell pattern file

There is a JSON file at /assets/spell_patterns.json.

Codex should load and use this file instead of hardcoding rune patterns.

The file defines:

rune abbreviations

spell pattern definitions

valid directions per pattern

When a rune is placed, the existing rune placed event should be emitted:

use RunePlacedEvent

The rune placement event should trigger spell pattern checking logic.

The spell pattern checking system should:

inspect the placed rune position

examine nearby runes as needed

iterate over all spell patterns from /assets/spell_patterns.json

test whether the new rune completes any pattern

determine the matched spell name and direction

If a pattern matches:

cast the spell from the location of the last placed rune

emit RunePatternCompletedEvent

emit SpellCastEvent if useful

Runes from different teams and different players are allowed to combine into valid spell patterns.

Do not restrict pattern matching by team or owner.

Spell system

Add a spell system if one does not already exist.

Create an abstract BaseSpell class.

BaseSpell should take at least:

cast position

caster player reference or caster player id

Suggested methods:

GetName()

Cast(GameState&, EventQueue&)

Concrete spell implementations should inherit from BaseSpell.

FireBall / FireBolt implementation

Implement the first spell defined by the spell pattern file:

fire_bolt

This spell should create a projectile.

The projectile should:

spawn at the cast location

travel in the direction defined by the matched pattern in spell_patterns.json

use an appropriate projectile speed constant

collide with players and world obstacles

apply damage on hit

The spell direction must come from the matched pattern direction:

right

left

top

bot

Use a simple mapping from pattern direction to projectile velocity vector.

Additional state / classes to add

Add or extend the needed classes and state to support the above:

player facing direction

player action state

rune placing mode

selected rune type

melee attack timing

sprite animation state

spell system

spell pattern loader

sprite metadata loader

Suggested additional types

enum class FacingDirection { Left, Right, Top, Bottom };

enum class PlayerActionState { Idle, Walking, Slashing, RunePlacing };

enum class RuneType { Fire, Water };

Suggested additional files

/src/assets/sprite_metadata.h

/src/assets/sprite_metadata.cpp

/src/spells/base_spell.h

/src/spells/fire_bolt_spell.h

/src/spells/fire_bolt_spell.cpp

/src/spells/spell_pattern_loader.h

/src/spells/spell_pattern_loader.cpp

Rendering and input behavior summary

Mouse determines player facing.

Left click:

if not in rune placing mode → melee slash

if in rune placing mode → place selected rune

Key 1 selects fire rune placing mode.

Key 2 selects water rune placing mode.

While rune placing mode is active:

show local placement grid around mouse

show snapped rune preview

prevent overlapping rune placement

Implementation guidance

Keep the first implementation simple and explicit.

It is acceptable to check spell patterns only near the most recently placed rune.

Use readable code rather than a fully generic data-driven spell engine.

The first goal is a working prototype using:

sprite metadata JSON

spell pattern JSON

facing-based sprite selection

melee attack

rune placing mode

fire bolt spell casting

You should also update these earlier lines in your prompt:

Replace

one rune pattern that creates a fireball

with

spell patterns are loaded from /assets/spell_patterns.json, and the first fully implemented spell is fire_bolt

Replace

rune placement on grid

with

rune placement on grid with explicit rune placing mode, preview overlay, snapped placement, and occupied-cell rejection

Replace

render rune grid

with

render rune placement overlay only while rune placing mode is active, using the grid_overlay sprite from the sprite metadata JSON

Also implement a camera system which is centered on the controlled wizzard by each player