#include "raylib.h"
#include "vendor/Cflat/src/Cflat.h"
#include "raygui.h"

#define SIZE                            \
    union {                             \
        Vector2 size;                   \
        struct { f32 width, height; };  \
    }

typedef struct next_rect_opt {
    Vector2 direction;
    SIZE;
} NextBoundsOpt;

typedef struct child_rect_opt {
    SIZE;
    Vector2 position;
} ChildBoundsOpt;

#define UP    ((Vector2) { .x =  0, .y = -1 })
#define DOWN  ((Vector2) { .x =  0, .y =  1 })
#define LEFT  ((Vector2) { .x = -1, .y =  0 })
#define RIGHT ((Vector2) { .x =  1, .y =  0 })

Rectangle next_rect_opt(Rectangle current, NextBoundsOpt opt);
Rectangle child_rect_opt(Rectangle current, ChildBoundsOpt opt);
Vector2 rect_center(Rectangle rect);
Vector2 rect_position(Rectangle rect);

#define next_rect(current, ...) next_rect_opt(current, CFLAT_OPT(NextBoundsOpt, .width = 1, .height= 1,  __VA_ARGS__))
#define child_rect(current, ...) child_rect_opt(current, CFLAT_OPT(ChildBoundsOpt, .width = 1, .height= 1, __VA_ARGS__))

#define BOUNDS                              \
union {                                     \
    Rectangle bounds;                       \
    struct { f32 x, y, width, height; };    \
    struct { Vector2 position, size; };     \
}

Rectangle next_rect_opt(Rectangle current, NextBoundsOpt opt) {
    f32 x = opt.direction.x > 0 ? current.x + current.width  *opt.direction.x : current.x + opt.width*current.width  *opt.direction.x;
    f32 y = opt.direction.y > 0 ? current.y + current.height *opt.direction.y : current.y + opt.height*current.height*opt.direction.y;
    Rectangle next = {
        .x = x,
        .y = y,
        .width  = opt.width*current.width,
        .height = opt.height*current.height,
    };
    return next;
}

Rectangle child_rect_opt(Rectangle current, ChildBoundsOpt opt) {
    Rectangle result = {
        .x = current.x + opt.position.x*(current.width  - opt.width),
        .y = current.y + opt.position.y*(current.height - opt.height),
        .width  = opt.width  * current.width,
        .height = opt.height * current.height,
    };
    return result;
}


Vector2 rect_center(Rectangle rect) {
    return (Vector2) { .x = rect.x + rect.width/2, .y = rect.y + rect.height/2 };
}

Vector2 rect_position(Rectangle rect) {
    return (Vector2) { .x = rect.x, .y = rect.y };
}