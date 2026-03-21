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