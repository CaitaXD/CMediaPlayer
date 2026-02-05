

#if !defined(RUNNER_H)
#define RUNNER_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define CFLAT_IMPLEMENTATION
#include "vendor/Cflat/src/CflatLib.h"

#if defined(OS_WINDOWS)
#   define LIB(NAME) (NAME ".dll")
#elif defined(OS_UNIX)
#   define LIB(NAME) ("lib" NAME ".so")
#endif

#if defined (HOT_RELOAD_ENABLED)
#define FUNCTION_DECLARATION(RETURN, NAME, ...) typedef RETURN (*NAME)(__VA_ARGS__)
#else
#define FUNCTION_DECLARATION(RETURN, NAME, ...) typedef RETURN (NAME)(__VA_ARGS__)
#endif

FUNCTION_DECLARATION(void*, lib_new_t,     CflatArena *arena, int argc, char **argv);
FUNCTION_DECLARATION(void,  lib_delete_t,  void *self);
FUNCTION_DECLARATION(void,  lib_update_t,  void *self);
FUNCTION_DECLARATION(void,  lib_enable_t,  void *self);
FUNCTION_DECLARATION(void,  lib_disable_t, void *self);

#define FUNCTIONS_LIST      \
    X(lib_new)              \
    X(lib_delete)           \
    X(lib_update)           \
    X(lib_enable)           \
    X(lib_disable)

#if defined (ADDITIONAL_FUNCTIONS)
ADDITIONAL_FUNCTIONS
#endif

#if defined (HOT_RELOAD_ENABLED)
#   define X(NAME) NAME##_t NAME = 0;
#else
#   define X(NAME) NAME##_t NAME;
#endif

FUNCTIONS_LIST

#undef X

bool runner_reload_lib(const char *lib, CflatOsHandle *lib_handle);

#if defined (RUNNER_IMPLEMENTATION)

bool runner_reload_lib(const char *lib, CflatOsHandle *lib_handle) {
    
    if (CflatOsHandleIsValid(*lib_handle)) cflat_lib_close(*lib_handle);
    *lib_handle = cflat_lib_open(lib);
    
    if (!CflatOsHandleIsValid(*lib_handle)) {
        fprintf(stderr, "Failed to load app: %s\n", lib);
        return false;
    }

    #define X(NAME)                                                                     \
    NAME = (NAME##_t)cflat_load_symbol(*lib_handle, #NAME);                             \
    if (NAME == NULL) {                                                                 \
        fprintf(stderr, "Failed to load symbol: %s from %s\n", #NAME, lib);             \
        return false;                                                                   \
    }

    FUNCTIONS_LIST

    #if defined (ADDITIONAL_FUNCTIONS)
    ADDITIONAL_FUNCTIONS
    #endif

    #undef X
    return true;
}

#endif //RUNNER_IMPLEMENTATION

#endif //RUNNER_H