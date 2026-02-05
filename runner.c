#include <stdio.h>
#include <synchapi.h>
#define RUNNER_IMPLEMENTATION
#include "runner.h"
#include <raylib.h>

#define LIB_NAME "./bin/app"
#define LIB_PATH LIB(LIB_NAME)

int main(int argc, char** argv) {
    OsHandle app_handle = {0};
    if (!runner_reload_lib(LIB_PATH, &app_handle)) {
        return -1;
    }

    InitWindow(1280, 720, "Runner");

    Arena *arena = arena_new();
    void *app = lib_new(arena, argc, argv);
    lib_enable(app);
    
    if (app == NULL) {
        printf("Failed to create app instance\n");
        return -1;
    }


    while (!WindowShouldClose()) {

        if (IsKeyPressed(KEY_R)) {
            lib_disable(app);
            
            if (!runner_reload_lib(LIB_PATH, &app_handle)) {
                printf("Failed to reload lib\n");
                break;
            }

            lib_enable(app);
        }

        lib_update(app);
    }

    lib_delete(app);
}