#include "core/game_app.h"

#include <string>

int main(int argc, char** argv) {
    bool force_windowed = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--windowed") {
            force_windowed = true;
        }
    }

    GameApp app(force_windowed);
    if (!app.Initialize()) {
        return 1;
    }

    app.Run();
    app.Shutdown();
    return 0;
}
