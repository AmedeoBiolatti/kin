// Linked into every kin game on Android. SDL's Java activity starts the app
// by calling SDL_main in libmain.so; this calls the game's ordinary main()
// after telling SDL the entry point is set up (SDL_main.h does that on other
// platforms, so games need not include it). C, as C++ may not call main.
#define SDL_MAIN_HANDLED // declarations only: main stays main
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

int main(int argc, char* argv[]);

__attribute__((visibility("default"))) int SDL_main(int argc, char* argv[]) {
    SDL_SetMainReady();
    return main(argc, argv);
}
