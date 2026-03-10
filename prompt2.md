issues:
- We still see blinking when going left (animation). Im wondering if the issue isn't that the mirrored doesnt know which grid cell to pick because the mirror messes up sprite tile grid coords. If that is indeed the case I suggest also thinking about mirroring the grid coords horizontally. This should be possible using the new "num_columns" : 4, property in the `sprite_sheet.json`. Could this work? if so please plan a fix
- only host sees grid overlay when in rune creation mode. The overlay should be visible to the player currently using it. Multiple players should be able to use an overlay independently.
- joined users have lower fps (possible solutions: interpolate states, or send more state updates). The issue is - the host sees fluid gameplay. But other users see less frames per second. Is there a way to mitigate that? Propose a solution
- units shouldnt be allowed to exit the map. Currently exiting the map is trivial


gameplay:
- water shouldnt block projectiles only units going over them
- health bars (4x32 pixels size before applying the zoom) offset above the player with the font displaying current health made smaller as to fit in the bar and in black font. The bar uses the colors: 189, 217, 65 (RGB) for remaining health and 200, 204, 182 (RGB) from missing health. Colors are available for editing in constants. Also the font displaying current health seems a little jittery and doesn't flow smoothly with the player. What could be the issue?




Long term
- ui for runes, other player info
- dash ability
- melee cooldown
- melee animation