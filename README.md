build on mac:
```
cmake -S . -B build
cmake --build build -j4
./build/rune_arena
```

Windows (Visual Studio 2022):
```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
.\build\Release\rune_arena.exe
```

Set to private network on windows 11
Settings -> Network & Internet -> Wi-Fi -> <current_net> -> Private network


Linux / Ubuntu
```
sudo apt update
sudo apt install -y build-essential cmake pkg-config libasound2-dev libx11-dev libxrandr-dev libxi-dev libgl1-mesa-dev libglu1-mesa-dev libxcursor-dev libxinerama-dev libwayland-dev libxkbcommon-dev
cmake -S . -B build
cmake --build build -j4
./build/rune_arena
```

tree modular export
```
ASEPRITE_DATA_DIR=/Applications/Aseprite.app/Contents/Resources/data /Applications/Aseprite.app/Contents/MacOS/aseprite -b --script-param "source_file=assets/128x128_tree_modular/128x128_tree_modular.aseprite" --script-param "output_dir=assets/128x128_tree_modular/exports" --script-param "source_name=128x128_tree_modular" --script assets/aseprite_export_layers.lua
```

Wizzard 128x128 modular export
```
ASEPRITE_DATA_DIR=/Applications/Aseprite.app/Contents/Resources/data /Applications/Aseprite.app/Contents/MacOS/aseprite -b --script-param "source_file=assets/128x128_modular/wizzard_128x128.aseprite" --script-param "output_dir=assets/128x128_modular/exports" --script-param "source_name=wizzard_128x128" --script assets/aseprite_export_layers.lua
```


Map export (twinlane)
```
ASEPRITE_DATA_DIR=/Applications/Aseprite.app/Contents/Resources/data /Applications/Aseprite.app/Contents/MacOS/aseprite -b --script-param "source_file=assets/maps/twinlane/twinlane.aseprite" --script-param "output_dir=assets/maps/twinlane/exports" --script-param "source_name=twinlane" --script assets/aseprite_export_layers.lua
```




ad852f - v 255
ad852f - v 205

