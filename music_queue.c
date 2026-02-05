#ifndef MUSIC_QUEUE_H
#define MUSIC_QUEUE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "vendor/Cflat/src/Cflat.h"
#include "raylib.h"
#include "vendor/Cflat/src/CflatSlice.h"
#include "vendor/Cflat/src/CflatString.h"

#define INDEX_WRAP(A, M) ((A) % (M) + (M)) % (M)

#define DEFAULT_UNIQUE_SEQUENCE ((LinearCongruentialGenerator) {                            \
    .state = 42,                                                                            \
    .increment = 1013904223ULL | 1,                                                         \
    .multiplier = (6364136223846793005ULL & ~3ULL) | 1,                                     \
    .inv_multiplier = calculate_inverse_multiplier((6364136223846793005ULL & ~3ULL) | 1)    \
})

typedef struct music_file {
    StringView file_path;
    u64 cur_position;
    u64 end_position;
} MusicFile;

typedef struct music_files {
    CFLAT_SLICE_FIELDS(MusicFile);
} MusicFiles;

typedef struct linear_congruential_generator {
    u64 state;
    u64 multiplier; 
    u64 inv_multiplier; 
    u64 increment;
} LinearCongruentialGenerator;

LinearCongruentialGenerator distinct_sequence_rng(u64 seed, u64 increment);
u64 calculate_inverse_multiplier(u64 a);
u64 lcg_rand_next_u64(LinearCongruentialGenerator *lcg, u64 min_inclusive, u64 max_exclusive);
u64 lcg_rand_prev_u64(LinearCongruentialGenerator *lcg, u64 min_inclusive, u64 max_exclusive);

typedef struct music_queue {
    usize count;
    usize cursor;
    bool playing;
    MusicFiles playlist;
    LinearCongruentialGenerator lcg;
    usize rand_count;
} MusicQueue;


MusicQueue *music_queue_new(Arena *arena);

typedef struct music_queue_next_opt {
    bool reapeat: 1;
    bool random: 1;
} MusicQueueNextOpt;

MusicFile *music_queue_next_opt(MusicQueue *queue, MusicQueueNextOpt opt);
#define music_queue_next(queue, ...) music_queue_next_opt(queue, (MusicQueueNextOpt) { __VA_ARGS__ })

MusicFile *music_queue_prev_opt(MusicQueue *queue, MusicQueueNextOpt opt);
#define music_queue_prev(queue, ...) music_queue_prev_opt(queue, (MusicQueueNextOpt) { __VA_ARGS__ })

MusicFile *music_queue_peek(MusicQueue *queue);

void music_queue_push(Arena *a, MusicQueue *queue, MusicFile music);

MusicQueue *music_queue_new(Arena *arena) {
    MusicQueue *queue = arena_push(arena, sizeof *queue, .clear = true);
    queue->cursor = 0;
    queue->count = 0;
    queue->lcg = DEFAULT_UNIQUE_SEQUENCE;
    return queue;
}

void music_queue_push(Arena *a, MusicQueue *queue, MusicFile music) {
    slice_append(a, queue->playlist, music);
    queue->count += 1;
}

void music_queue_remove_at(MusicQueue *queue, usize index) {
    cflat_bounds_check(index, slice_length(queue->playlist));
    memmove(&slice_data(queue->playlist)[index], &slice_data(queue->playlist)[index + 1], (slice_length(queue->playlist) - index - 1)*sizeof(MusicFile));
    
    slice_length(queue->playlist) -= 1;
    queue->count -= 1;

    if (queue->cursor >= slice_length(queue->playlist)) {
        queue->cursor = queue->count - 1;
    }
}

MusicFile *music_queue_peek(MusicQueue *queue) {
    if (queue->cursor >= slice_length(queue->playlist)) return NULL;
    return &slice_data(queue->playlist)[queue->cursor];
}

MusicFile *music_queue_next_opt(MusicQueue *queue, MusicQueueNextOpt opt) {
    usize next_index;
    MusicFile *next_music;
    u64 now = (u64)time(NULL);

    if (opt.random) {
        if (queue->rand_count >= slice_length(queue->playlist)) {
            if (!opt.reapeat) return NULL;
            queue->rand_count = 0;
            queue->lcg = distinct_sequence_rng(queue->cursor, now);
        }
        next_index = lcg_rand_next_u64(&queue->lcg, 0, slice_length(queue->playlist));
        next_music = slice_at(queue->playlist, next_index);
        queue->cursor = next_index;
        queue->rand_count += 1;
        return next_music;
    }

    queue->rand_count = 0;
    next_index = opt.reapeat ? (queue->cursor + 1) % slice_length(queue->playlist) : queue->cursor + 1;
    if (next_index >= slice_length(queue->playlist)) return NULL;
    
    next_music = &slice_data(queue->playlist)[next_index];
    queue->cursor = next_index;
    return next_music;
}

MusicFile *music_queue_prev_opt(MusicQueue *queue, MusicQueueNextOpt opt) {
    u64 now = (u64)time(NULL);
    usize next_index;
    MusicFile *music;

    if (opt.random) {
        if (queue->rand_count == 0) {
            if (!opt.reapeat) return NULL;
            queue->rand_count = slice_length(queue->playlist);
            queue->lcg = distinct_sequence_rng(queue->cursor, now);
        }
        next_index = lcg_rand_prev_u64(&queue->lcg, 0, slice_length(queue->playlist));
        music = slice_at(queue->playlist, next_index);
        queue->cursor = next_index;
        queue->rand_count -= 1;
        return music;
    }

    queue->rand_count = 0;
    if (queue->cursor == 0) {
        if (!opt.reapeat) return NULL;
        next_index = slice_length(queue->playlist) - 1;
    }
    else {
        next_index = queue->cursor - 1;
    }

    music = &slice_data(queue->playlist)[next_index];
    queue->cursor = next_index;
    return music;
}

u64 calculate_inverse_multiplier(u64 multiplier) {
    u64 x = multiplier;
    for (u64 i = 0; i < 5; i++) { 
        x = x * (2 - multiplier * x);
    }
    return x;
}

LinearCongruentialGenerator distinct_sequence_rng(u64 seed, u64 increment) {
    return (LinearCongruentialGenerator) {
        .state = seed,
        .increment = increment | 1,
        .multiplier = (6364136223846793005ULL & ~3ULL) | 1,
        .inv_multiplier = calculate_inverse_multiplier((6364136223846793005ULL & ~3ULL) | 1),
    };
}

u64 lcg_rand_next_u64(LinearCongruentialGenerator *lcg, u64 min_inclusive, u64 max_exclusive) {
    u64 mask = next_pow2(max_exclusive) - 1;
    do {
        lcg->state = (lcg->multiplier * lcg->state + lcg->increment) & mask; 
    } while (lcg->state < min_inclusive || lcg->state >= max_exclusive);
    return lcg->state;
}

uint64_t lcg_rand_prev_u64(LinearCongruentialGenerator *lcg, u64 min_inclusive, u64 max_exclusive) {
    uint64_t mask = next_pow2(max_exclusive) - 1;
    do {
        lcg->state = ((lcg->state - lcg->increment) * lcg->inv_multiplier) & mask;
    } while (lcg->state < min_inclusive || lcg->state >= max_exclusive);
    return lcg->state;
}

typedef struct cstring_slice {
    CFLAT_SLICE_FIELDS(const char*);
} CStringSlice;

CStringSlice music_queue_get_names_as_cstr(Arena *a, MusicQueue *queue) {
    CStringSlice result = cflat_slice_new(CStringSlice, a, 0, .clear = true, .capacity = slice_length(queue->playlist));

    for (usize i = 0; i < (usize)slice_length(queue->playlist); i += 1) {

        StringView path = slice_data(queue->playlist)[i].file_path;
        
        StringView last = {0};
        StringView name = path_name(path);
        while (name.data != last.data) {
            last = name;
            name = path_name(name);
        }
        
        isize index = sv_find_last_index(name, ".");
        if (index > 0) name = slice_take(name, index);

        const char *cstr = sv_clone(a, name).data;
        slice_append(a, result, cstr);
    }

    return result;
}

#endif //MUSIC_QUEUE_H