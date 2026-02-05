#include <stdbool.h>
#include <stdlib.h>
#define NOB_STRIP_PREFIX
#define NOB_IMPLEMENTATION
#include "nob.h"

#include <stdio.h>

#define PROGRAM "app"
#define RUNNER "runner"
#define USAGE "build <compiler: gcc, clang, tcc...> [run: builds and then run]"

#define RAYLIB "vendor/raylib/raylib-5.5_win64_mingw-w64"
#define RAYGUI "vendor/raygui-4.0"
#define MINIAUDIO "vendor/miniaudio"
#define STDC "-std=c2x"

const char *stdc_from_flag = NULL;

Cmd *cmd = &(Cmd){0};

bool add_to_libpath(Cmd *cmd, const char *var) {
    #if _WIN32
    putenv(nob_temp_sprintf("PATH=%s;%s/%s", getenv("PATH"), nob_get_current_dir_temp(), var));
    #endif
    return true;
}

void append_compiler_flags(Cmd *cmd, int c_argc, char **c_argv) {
    for (int i = 0; i < c_argc; i++) {
        const char *c_flag = nob_temp_sprintf("-%s", c_argv[i] + 2);
        if (strstr(c_flag, "std=") != NULL) {
            stdc_from_flag = c_flag;
        }
        cmd_append(cmd, c_flag);
    }
}

void cmd_link_raylib(Cmd *cmd) {
    
    cmd_append(cmd, RAYLIB "/lib/libraylibdll.a");
    cmd_append(cmd, "-I" RAYLIB "/include");
    cmd_append(cmd, "-L" RAYLIB "/lib");
    cmd_append(cmd, "-lraylib");
    #if _WIN32
    cmd_append(cmd, "-lwinmm");
    cmd_append(cmd, "-lgdi32");
    #endif
    cmd_append(cmd, "-lm");
}

void cmd_link_raygui(Cmd *cmd) {
    cmd_append(cmd, "-I" RAYGUI "/src");
}

bool build_lib(const char *compiler, int c_argc, char **c_argv) {

    if (strcmp(compiler, "cl") == 0) {
        nob_log(ERROR, "Imagine using MSVC lmao!\n");
        return false;
    }

    cmd_append(cmd, compiler);
    cmd_append(cmd, PROGRAM ".c");
    
    append_compiler_flags(cmd, c_argc, c_argv);
    
    if (!stdc_from_flag) cmd_append(cmd, STDC);
    
    #if _WIN32
    if (strcmp(compiler, "gcc") != 0) {
        cmd_append(cmd, "-fsanitize=address");
        cmd_append(cmd, "-fsanitize=undefined");
    }
    #else
    cmd_append(cmd, "-fsanitize=address");
    cmd_append(cmd, "-fsanitize=undefined");
    #endif

    cmd_append(cmd, "-ggdb");
    cmd_append(cmd, "-Wall");
    cmd_append(cmd, "-Wextra");
    cmd_append(cmd, "-I" MINIAUDIO);
    cmd_append(cmd, MINIAUDIO "/miniaudio.o");

    cmd_link_raylib(cmd);
    cmd_link_raygui(cmd);

    cmd_append(cmd, "-shared");
    cmd_append(cmd, "-o");
    #if _WIN32
    cmd_append(cmd, "bin/" PROGRAM ".dll");
    #else
    cmd_append(cmd, "bin/" PROGRAM ".so");
    #endif
    return cmd_run(cmd);
}

bool build_runner(const char *compiler, int c_argc, char **c_argv) {
    if (strcmp(compiler, "cl") == 0) {
        nob_log(ERROR, "Imagine using MSVC lmao!\n");
        return false;
    }

    cmd_append(cmd, compiler);
    cmd_append(cmd, RUNNER ".c");
    cmd_append(cmd, "-DHOT_RELOAD_ENABLED");
    
    append_compiler_flags(cmd, c_argc, c_argv);
    
    if (!stdc_from_flag) cmd_append(cmd, STDC);
    
    #if _WIN32
    if (strcmp(compiler, "gcc") != 0) {
        cmd_append(cmd, "-fsanitize=address");
        cmd_append(cmd, "-fsanitize=undefined");
    }
    #else
    cmd_append(cmd, "-fsanitize=address");
    cmd_append(cmd, "-fsanitize=undefined");
    #endif
    
    cmd_append(cmd, "-ggdb");
    cmd_append(cmd, "-Wall");
    cmd_append(cmd, "-Wextra");

    cmd_link_raylib(cmd);

    cmd_append(cmd, "-o");
    #if _WIN32
    cmd_append(cmd, "bin/" PROGRAM ".exe");
    #else
    cmd_append(cmd, "bin/" PROGRAM);
    #endif
    return cmd_run(cmd);
}

bool run(int run_argc, char **run_argv) {
    cmd_append(cmd, "bin/" PROGRAM ".exe");
    if (run_argc > 0) {
        cmd_append(cmd, run_argv[0]);
    }
    return cmd_run(cmd);
}

int main(int argc, char **argv) {

    NOB_GO_REBUILD_URSELF(argc, argv);
    
    if (!add_to_libpath(cmd, RAYLIB "/lib")) {
        printf("Failed to add vendor to path\n");
        return 1;
    }

    int iota = 1;

    if (argc == iota) {
        printf("Usage: %s\n", USAGE);
    }

    const char *compiler = argv[iota++];
    
    unsigned int c_argc = 0; 

    for (; iota + c_argc < argc && argv[iota + c_argc][0] == '-' && argv[iota + c_argc][1] == 'c'; c_argc += 1);

    if (!build_lib(compiler, c_argc, argv + iota)) {
        printf("Failed to build lib\n");
        return 1;
    }

    iota += c_argc;

    if (iota >= argc) return 0;

    if (strcmp(argv[iota++], "run") == 0) {

        int run_argc = 0;
        char **run_argv = NULL;
        
        if (!build_runner(compiler, 0, NULL)) {
            printf("Failed to build runner\n");
            return 1;
        }

        if (iota < argc && strcmp(argv[iota++], "--") == 0) {
            run_argc = argc - iota;
            run_argv = argv + iota;
        }

        for (int i = 0; i < run_argc; i++) {
            printf("%s\n", run_argv[i]);
        }

        if (!run(run_argc, run_argv)) {
            printf("Failed to run\n");
            return 1;
        }
    }
    else {
        printf("Usage: %s\n", USAGE);
    }
    
    return 0;
}