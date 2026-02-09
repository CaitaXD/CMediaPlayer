#include "app.h"
#include "miniaudio.h"
#include "music_queue.c"
#include "vendor/Cflat/src/CflatCore.h"
#include "vendor/Cflat/src/CflatMath.h"
#include "vendor/Cflat/src/CflatSlice.h"
#include "vendor/Cflat/src/CflatString.h"
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"
#include "raylib.h"
#include "vendor/raygui-4.0/styles/dark/style_dark.h"

cflat_export void *lib_new(Arena *arena, int argc, char **argv) {
    (void)argc;
    (void)argv;

    if(setlocale(LC_ALL, "zh_CN.UTF-8") == NULL){
        perror("setlocale failed");
    }
    
    AppState *app = arena_push(arena, sizeof *app, .clear = true);
    app->master_volume = 1;
    app->music_volume = 1;
    app->capture_volume = 1;
    app->arena = arena;
    app->data_converter_memory = arena_push(arena, KiB(1), .clear=true);
    app->music_queue = music_queue_new(app->arena);
    app->music_queue->playing = true;
    app->capture_low_pass_filter_alpha = 0.75;
    app->capture_high_pass_filter_alpha = 0.75;

    app->note_step = exp2(1.0/12.0); 
    app->notes[0] = 8.176;
    for (usize i = 1; i < ARRAY_SIZE(app->notes); i += 1) {
        app->notes[i] = app->notes[i-1] * app->note_step;
    }

    return app;
}

cflat_export void lib_delete(void *self) {
    AppState *app = self;
    ma_device_uninit(&app->device);
    ma_context_uninit(&app->context);
    arena_clear(app->arena);
}

cflat_export void lib_enable(void *self) {
    SetWindowState(FLAG_WINDOW_RESIZABLE);
    GuiLoadStyleDark();
    GuiSetStyle(LISTVIEW, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
    SetTargetFPS(69);
    ma_result result;
    AppState *app = self;
    
    result = ma_context_init(NULL, 0, NULL, &app->context);
    
    app->context.callbacks.onContextUninit = on_context_uninit;
    app->context.pUserData = app;

    if (result != MA_SUCCESS) {
        TRACE_ERROR("Couldn't initialize main context\n");
        return;
    }
    
    app->background_color = (Color) {0x18, 0x18, 0x18, 255};
    app->device_config = ma_device_config_init(ma_device_type_duplex);
    app->device_config.capture.pDeviceID = NULL;
    app->device_config.playback.pDeviceID = NULL;
    app->device_config.capture.format = ma_format_f32;
    app->device_config.playback.format = ma_format_f32;
    app->device_config.playback.channels = CHANNELS;
    app->device_config.capture.channels = CHANNELS;
    app->device_config.sampleRate = CAPTURE_SAMPLE_RATE;
    app->device_config.dataCallback = data_callback;
    app->device_config.pUserData = app;
    app->bounds = (Rectangle) { .x = 0, .y = 0, .width = GetScreenWidth(), .height = GetScreenHeight() };
    
    static f32 s_hanning_window[FFT_SIZE];
    hanning_window_f32(FFT_SIZE, s_hanning_window, WINDOW_CREATE);

    app->music_queue_vocoder.format   = app->device_config.playback.format;
    app->music_queue_vocoder.channels = app->device_config.playback.channels;
    
    mem_copy(app->music_queue_vocoder.analysis_window,  s_hanning_window, sizeof(s_hanning_window));
    mem_copy(app->music_queue_vocoder.synthesis_window, s_hanning_window, sizeof(s_hanning_window));
    
    app->capture_vocoder.format    = app->device_config.capture.format;
    app->capture_vocoder.channels  = app->device_config.capture.channels;

    mem_copy(app->capture_vocoder.analysis_window,   s_hanning_window, sizeof(s_hanning_window));
    mem_copy(app->capture_vocoder.synthesis_window,  s_hanning_window, sizeof(s_hanning_window));

    result = ma_device_init(&app->context, &app->device_config, &app->device);
    
    if (result != MA_SUCCESS) {
        TRACE_ERROR("Couldn't initialize main context\n");
        return;
    }
    
    if (app->music_queue->playing) {
        result = ma_device_start(&app->device);
    }

    if((usize)app->music_queue->cursor < (usize)slice_length(app->music_queue->playlist)) {

        MusicFile *music_file = &slice_data(app->music_queue->playlist)[app->music_queue->cursor];

        result = ma_data_converter_init_preallocated(&app->data_converter_config, app->data_converter_memory, &app->data_converter);
    
        if (result != MA_SUCCESS) {
            TRACE_ERROR("Couldn't initialize data converter, reason: %s\n", ma_result_description(result));
            return;
        }

        result = ma_decoder_init_file(slice_data(music_file->file_path), NULL, &app->decoder);

        if (result != MA_SUCCESS) {
            TRACE_ERROR("Could not load file: %s reason: %s\n", slice_data(music_file->file_path), ma_result_description(result));
            return;
        }

        ma_decoder_seek_to_pcm_frame(&app->decoder, music_file->cur_position);
    }

    const usize capture_frame_size = ma_get_bytes_per_frame(app->device.capture.format, app->device.capture.channels);

    app->capture_vocoder.input_buffer = ring_buffer_new_opt(capture_frame_size, app->arena, KiB(16), (RingBufferNewOpt) {
        .align = 64,
        .clear = true,
    });

    app->capture_vocoder.output_buffer = ring_buffer_new_opt(capture_frame_size, app->arena, KiB(16), (RingBufferNewOpt) {
        .align = 64,
        .clear = true,
    });

    const usize playback_frame_size = ma_get_bytes_per_frame(app->device.playback.format, app->device.playback.channels);
    app->music_queue_vocoder.input_buffer = ring_buffer_new_opt(playback_frame_size, app->arena, KiB(16), (RingBufferNewOpt) {
        .align = 64,
        .clear = true,
    });

    app->music_queue_vocoder.output_buffer = ring_buffer_new_opt(playback_frame_size, app->arena, KiB(16), (RingBufferNewOpt) {
        .align = 64,
        .clear = true,
    });
    
    // Widget Bounds

    app->w_frequency_spectrum = (WFrequencySpectrum) {
        .bounds = child_rect(app->bounds, .width = .75, .height = .75, .position.x = .25, .position.y = 0),
        .background_color = app->background_color,
        .fft_size = FFT_SIZE,
        .channels = CHANNELS,
        .sample_rate = CAPTURE_SAMPLE_RATE,
        .lower_note = app->notes[NOTE_INDEX(C, -1)],
        .upper_note = app->notes[NOTE_INDEX(C, 10)],
    };

    app->w_playlist = (WPlaylist) {
        .bounds = next_rect(app->w_frequency_spectrum.bounds, 
            .width = .33, .height = 1,
            .direction = LEFT,
        ),
        .dirty = true,
    };
    
    app->w_playlist_buttons = (WPlaylistButtons) {
        .bounds = next_rect(app->w_frequency_spectrum.bounds, .direction = DOWN, .direction.y = DOWN.y, .width = 1, .height = .25),
    };

}

cflat_export void lib_update(void *self) {
    
    AppState *app = self;

    if (IsWindowResized()) {
        app->bounds = (Rectangle) { .x = 0, .y = 0, .width = GetScreenWidth(), .height = GetScreenHeight() };
        app->w_frequency_spectrum.bounds = child_rect(app->bounds, .width = 1, .height = .75, .position.x = .25, .position.y = 0);
        printf("New size: %f %f\n", app->bounds.width, app->bounds.height);
    }

    if (IsFileDropped()) {
        // TODO: Check if path is directory and load all files in it
        FilePathList paths = LoadDroppedFiles();
        for (usize i = 0; i < paths.count; i += 1) {
            char *path = paths.paths[i];
            
            char* supported_extensions[] = {".mp3", ".ogg", ".wav", ".flac" };
            
            bool is_valid_file = false;
            for (usize i = 0; i < ARRAY_SIZE(supported_extensions); i += 1) {
                is_valid_file = sv_find_index(sv_from_cstr(path), supported_extensions[i]) != -1;
                if (is_valid_file) break;
            }
            if (!is_valid_file) continue;

            MusicFile music_file = { .file_path = sv_clone(app->arena, path) };
            
            music_queue_push(app->arena, app->music_queue, music_file);
            if (i == 0 && app->music_queue->count == 1) {
                app->music_queue->cursor = app->music_queue->count - 1;
                app_play_current_music(app);
            }
        }
        UnloadDroppedFiles(paths);
    }

    BeginDrawing();
    ClearBackground(app->background_color);
    
    const f32 dt = GetFrameTime();

    char fps_text[64];
    sprintf(fps_text, "FPS: %0.2f", ceilf(1.0/dt));
    SetWindowTitle(fps_text);
    
    Arena *arena = app->arena;
    
    c32 (*capture_waves)[FFT_SIZE][CHANNELS] = &app->capture_vocoder.fft_output;
    c32 (*music_waves  )[FFT_SIZE][CHANNELS] = &app->music_queue_vocoder.fft_output;
    c32 (*display_waves)[FFT_SIZE][CHANNELS] = &app->display_waves;

    TempArena eventloop_arena = arena_temp_begin(arena);
    for (usize frame = 0; frame < FFT_SIZE; frame++)
    for (usize channel = 0; channel < CHANNELS; channel += 1) {
        const f32 decay_constant = 25;
        c32 music_wave    = (*music_waves)[frame][channel];
        c32 capture_wave  = (*capture_waves)[frame][channel];
        c32 display_wave  = (*display_waves)[frame][channel];
        display_wave      = sexpdecay_c32(music_wave + capture_wave, display_wave, decay_constant, dt);
        app->display_waves[frame][channel] = display_wave;
    }

    draw_frequencies(&app->w_frequency_spectrum);
    if(draw_playlist_buttons(&app->w_playlist_buttons))
    {
        app->w_playlist.dirty = true;
    }
    draw_playlist(&app->w_playlist);
    
    arena_temp_end(eventloop_arena);
    
    EndDrawing();
}

cflat_export void lib_disable(void *self) {
    AppState *app = self;
    
    ma_device_stop(&app->device);
    ma_device_uninit(&app->device);
    ma_context_uninit(&app->context);
    
    if ((usize)app->music_queue->cursor < (usize)slice_length(app->music_queue->playlist)) {
        ma_decoder_uninit(&app->decoder);
        ma_data_converter_uninit(&app->data_converter, NULL);
    }

    while (atomic_load(&app->atomic_latch)) ;
}

void draw_playlist(WPlaylist *widget) {
    AppState *app = container_of(widget, AppState, w_playlist);
    Rectangle playlist_bounds = widget->bounds;
    DrawRectangleLinesEx(playlist_bounds, 1, BLUE);
    static int playlist_active_index = 0;
    static int playlist_scroll_index = 0;
    static int playlist_focus_index = 0;

    if (widget->dirty) {
        widget->dirty = false;
        playlist_active_index = (int)app->music_queue->cursor;
        int playlist_visible_count = (int)playlist_bounds.height/(GuiGetStyle(LISTVIEW, LIST_ITEMS_HEIGHT) + GuiGetStyle(LISTVIEW, LIST_ITEMS_SPACING));
        int max_scroll_index = app->music_queue->count - playlist_visible_count;
        usize items_count = (playlist_visible_count/2ULL);
        playlist_scroll_index = playlist_active_index - items_count;
        if (playlist_scroll_index < 0) playlist_scroll_index = 0;
        if (playlist_scroll_index > max_scroll_index) playlist_scroll_index = max_scroll_index;
    }

    TempArena temp;
    arena_scratch_scope(temp, 1, &app->arena) {
        CStringSlice music_names = music_queue_get_names_as_cstr(temp.arena, app->music_queue);

        (void)GuiListViewEx(playlist_bounds, slice_data(music_names), slice_length(music_names), &playlist_scroll_index, &playlist_active_index, &playlist_focus_index);       

        if (playlist_active_index != (int)app->music_queue->cursor) {
            if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                app_play_music_at_index(app, playlist_active_index);
            }
        }

        Vector2 mouse_pos = GetMousePosition();
        if (IsKeyPressed(KEY_DELETE) && CheckCollisionPointRec(mouse_pos, playlist_bounds)) {
            if((usize)playlist_active_index < app->music_queue->count) music_queue_remove_at(app->music_queue, playlist_focus_index);
            playlist_active_index = (int)app->music_queue->cursor;
        }
    }
}

bool draw_playlist_buttons(WPlaylistButtons *widget) {
    AppState *app = container_of(widget, AppState, w_playlist_buttons);
    
    DrawRectangleLinesEx(widget->bounds,1, RED);

    Rectangle pause_button = child_rect(
        widget->bounds,
        .width = .1, .height = .2, 
        .position.x = .5, .position.y = 0.01,
    );

    MusicFile *music_file = NULL;
    f32 seconds_played = 0;
    bool interacted = false;
        
    if ((usize)app->music_queue->cursor < (usize)slice_length(app->music_queue->playlist)) {
        music_file = &slice_data(app->music_queue->playlist)[app->music_queue->cursor];
        seconds_played = (f32)music_file->cur_position/app->data_converter.sampleRateIn;
    }

    if (GuiButton(pause_button, app->music_queue->playing ? "#132#" : "#131#")) {
        app_set_pause_music(app, app->music_queue->playing);
    }

    Rectangle prev_button = next_rect(pause_button, .direction = LEFT);
        
    if (music_file != NULL) {
        seconds_played = music_file->cur_position/app->device.sampleRate;
    }

    if (seconds_played > 1 && GuiButton(prev_button, seconds_played > 1 ? "#211#" : "#129#")) {
        interacted = true;
        app_set_pause_music(app, false);
        app_play_current_music(app);
    }
    else if (GuiButton(prev_button, seconds_played > 1 ? "#211#" : "#129#")) {
        if (music_queue_prev(app->music_queue, .reapeat = app->playlist_wrap, .random = app->playlist_shuffle)) {
            interacted = true;
            app_set_pause_music(app, false);
            app_play_current_music(app);
        }
    }
        
    Rectangle next_button = next_rect(pause_button, .direction = RIGHT);
    if (GuiButton(next_button, "#134#")) {
        if (music_queue_next(app->music_queue, .reapeat = app->playlist_wrap, .random = app->playlist_shuffle)) {
            interacted = true;
            app_set_pause_music(app, false);
            app_play_current_music(app);
        }
    }
    
    Rectangle wrap_button = next_rect(next_button, .direction = RIGHT);
    if (GuiButton(wrap_button, "Wrap")) {
        app->playlist_wrap = !app->playlist_wrap;
    }

    if (app->playlist_wrap) {
      GuiDrawRectangle(wrap_button, 2, GREEN, GetColor(GuiGetStyle(DEFAULT, BORDER_WIDTH)));
    }

    Rectangle shuffle_button = next_rect(prev_button, .direction = LEFT);
    if (GuiButton(shuffle_button, "Shuffle")) {
        app->playlist_shuffle = !app->playlist_shuffle;
    }

    Rectangle capture_volume_slider = next_rect(
        shuffle_button,
        .height = .5,
        .width = 2,
        .direction.x = RIGHT.x * 1.05,
        .direction.y = DOWN.y * 1.05,
    );
    
    Vector2 mouse_pos = GetMousePosition();
    bool right_click = IsMouseButtonPressed(MOUSE_RIGHT_BUTTON);

    GuiSlider(capture_volume_slider, "Capture Volume", NULL, &app->capture_volume, 0, 1);
    if (right_click && CheckCollisionPointRec(mouse_pos, capture_volume_slider)) {
        app->capture_volume = 0;
    }

    Rectangle capture_gain = next_rect(
        capture_volume_slider,
        .direction = DOWN,
    );

    Rectangle capture_high_pass_filter = next_rect(
        capture_gain,
        .direction = DOWN,
    );
    
    Rectangle capture_low_pass_filter = next_rect(
        capture_high_pass_filter,
        .direction = DOWN,
    );

    Rectangle pitch_shift_slider = next_rect(
        capture_low_pass_filter,
        .direction = DOWN,
    );

    Rectangle dec_shift_button = next_rect(
        pitch_shift_slider,
        .width = .25f,
        .direction = LEFT,
    );

    Rectangle inc_shift_button = next_rect(
        pitch_shift_slider,
        .width = .25f,
        .direction = RIGHT,
    );

    Rectangle pitch_shift_label = next_rect(
        pitch_shift_slider,
        .direction = DOWN,
    );

    GuiSlider(capture_gain, TextFormat("Gain: %.2f", app->capture_preamp_gain), NULL, &app->capture_preamp_gain, -20, 20);
    if (right_click && CheckCollisionPointRec(mouse_pos, capture_gain)) {
        app->capture_preamp_gain = 0;
    }
    
    GuiSlider(capture_low_pass_filter, TextFormat("Low Pass Filter: %.2f", app->capture_low_pass_filter_alpha), NULL, &app->capture_low_pass_filter_alpha, 0, 1);
    if (right_click && CheckCollisionPointRec(mouse_pos, capture_low_pass_filter)) {
        app->capture_low_pass_filter_alpha = 0;
    }
    
    GuiSlider(capture_high_pass_filter, TextFormat("High Pass Filter: %.2f", app->capture_high_pass_filter_alpha), NULL, &app->capture_high_pass_filter_alpha, 0, 1);
    if (right_click && CheckCollisionPointRec(mouse_pos, capture_high_pass_filter)) {
        app->capture_high_pass_filter_alpha = 0;
    }
    
    GuiSlider(pitch_shift_slider, "", NULL, &app->pitch_shift_semitones, -12, 12);
    if (right_click && CheckCollisionPointRec(mouse_pos, pitch_shift_slider)) {
        app->pitch_shift_semitones = 0;
    }

    DrawText(TextFormat("Transpose: %.2f", app->pitch_shift_semitones), pitch_shift_label.x, pitch_shift_label.y, 10, GetColor(GuiGetStyle(TEXT, TEXT_COLOR_NORMAL)));
    if (GuiButton(dec_shift_button, "-")) {
        app->pitch_shift_semitones -= 1;
    }
    if (GuiButton(inc_shift_button, "+")) {
        app->pitch_shift_semitones += 1;
    }
    
    if (app->playlist_shuffle) {
      GuiDrawRectangle(shuffle_button, 2, GREEN, GetColor(GuiGetStyle(DEFAULT, BORDER_WIDTH)));
    }
    
    Rectangle master_volume_slider = next_rect(
        shuffle_button,
        .width = 2, .height = .33334,
        .direction.x = LEFT.x * 1.125,
        .direction.y = DOWN.y * 0.5,
    );

    GuiSlider(master_volume_slider, "Master", NULL, &app->master_volume, 0, 1);

    Rectangle playback_volume_slider = next_rect(
        master_volume_slider,
        .direction = DOWN,
    );

    GuiSlider(playback_volume_slider, "Playback", NULL, &app->music_volume, 0, 1);


    Rectangle seconds_played_bounds = next_rect(
        playback_volume_slider,
        .direction = DOWN,
    );
        
    if (music_file == NULL) {
        GuiLabel(seconds_played_bounds, "0.00");
    }
    else {
        char buffer[16];
        sprintf(buffer, "%.2zu:%.2zu", (usize)seconds_played/60, (usize)seconds_played%60);
        GuiLabel(seconds_played_bounds, buffer);
    }

    bool up = IsKeyDown(KEY_RIGHT);
    bool down = IsKeyDown(KEY_LEFT);

    if (CheckCollisionPointRec(mouse_pos, capture_gain)) {
        f32 step = up ? 1 : down ? -1 : 0;
        app->capture_preamp_gain = clamp_f32(app->capture_preamp_gain + step, -20, 20);
    }
    if (CheckCollisionPointRec(mouse_pos, capture_low_pass_filter)) {
        f32 step = up ? 0.01 : down ? -0.01 : 0;
        app->capture_low_pass_filter_alpha = clamp_f32(app->capture_low_pass_filter_alpha + step, 0, 1);
    }
    if (CheckCollisionPointRec(mouse_pos, capture_high_pass_filter)) {
        f32 step = up ? 0.01 : down ? -0.01 : 0;
        app->capture_high_pass_filter_alpha = clamp_f32(app->capture_high_pass_filter_alpha + step, 0, 1);
    }

    return interacted;
}

void draw_frequencies(WFrequencySpectrum *widget) {
    AppState *app = container_of(widget, AppState, w_frequency_spectrum);

    DrawRectangleRec(widget->bounds, widget->background_color);

    const f32 x = widget->bounds.x;
    const f32 y = widget->bounds.y;
    const f32 width = widget->bounds.width;
    const f32 height = widget->bounds.height;
    const f32 sample_rate = widget->sample_rate;

    const usize channels = widget->channels;
    const usize fft_size = widget->fft_size;

    const f32 lower_note = widget->lower_note;
    const f32 upper_note = widget->upper_note;

    c32 (*waves)[fft_size][channels] = &app->display_waves;

    usize fft_resolution_in_notes = 0;
    for (usize bin = 1; bin <= fft_size/2; bin += 1) {
        f32 freq = (f32)bin/fft_size*sample_rate;
        if (freq > upper_note) break;
        if (freq < lower_note) continue;
        f32 next_freq = freq * app->note_step;
        for (f32 hz = freq; hz < next_freq; hz = (f32)bin/fft_size*sample_rate) {
            if (bin >= fft_size/2) break;
            bin += 1;
        }
        fft_resolution_in_notes += 1;
    }

    f32 bin_width = (width/fft_resolution_in_notes);
    
    for (usize channel = 0; channel < channels; channel++) {

        usize channel_offset = channel*(height/channels);
        usize index = 0;
        for (usize bin = 1; bin <= fft_size/2; bin += 1) {
            
            f32 freq = (f32)bin/fft_size*sample_rate;
            if (freq > upper_note) break;
            if (freq < lower_note) continue;
            
            f32 next_freq = freq * app->note_step;
            
            f32 db = wave_spldB_f32((*waves)[bin][channel]);
            
            for (f32 hz = freq; hz < next_freq; hz = (f32)bin/fft_size*sample_rate) {
                if (bin >= fft_size/2) break;
                f32 db2 = wave_spldB_f32((*waves)[bin++][channel]);
                db = fmaxf(db2, db);
            }

            f32 t = clamp_f32(ilerp_f32(10, 130, db), 0, 1);
            f32 bin_height = (height/channels) * t;
            f32 bin_y      = height + y - bin_height - channel_offset;
            f32 bin_x      = x + (index++)*bin_width;
            
            for (usize i = bin_y; i < bin_y + bin_height; i += 1) {
                f32 t2 = ilerp_f32(bin_y, bin_y + bin_height, i);
                
                Color base = ColorLerp(YELLOW, RED, 1-t);
                Color tip = ColorLerp(MAGENTA, RED, .5f);
                Color cell_color = ColorLerp(base, tip, (1-t2));
                DrawRectangle(bin_x, i, bin_width*fmaxf(t, .5f), 1, cell_color);
            }
        }
    }

    for (usize channel = 0; channel < channels; channel++) {
        Rectangle channel_rect = child_rect(widget->bounds, .width = .1, .height = .1, .position.x = 0.01, .position.y = (.1 + channel)/channels);
        GuiDrawText((channel == 0) ? "L" : (channel == 1) ? "R" : "", channel_rect, 64, WHITE);
    }
}

ma_result app_stop_playing(AppState *app) {
    ma_result result = MA_SUCCESS;
    MusicQueue *queue = app->music_queue;
    if (!queue->playing) return result;
    queue->playing = false;
    
    result = ma_decoder_uninit(&app->decoder);
    if (result != MA_SUCCESS) {
        TRACE_ERROR("Couldn't uninit decoder, reason: %s\n", ma_result_description(result));
        return result;
    }

    return result;
}

ma_result app_set_pause_music(AppState *app, bool pause) {
    
    ma_result result = MA_SUCCESS;
    if (app->music_queue->playing == !pause) return result;
    app->music_queue->playing = !pause;
    
    if (pause) {
        //result = ma_device_stop(&app->device);
    }
    else {
        //result = ma_device_start(&app->device);
    }

    if (result != MA_SUCCESS) {
        TRACE_ERROR("Couldn't start main device, reason: %s\n", ma_result_description(result));
        return result;
    }
    
    return MA_SUCCESS;
}

ma_result app_play_current_music(AppState *app) {
    ma_result result = MA_SUCCESS;
    MusicQueue *queue = app->music_queue;
    //if (queue->playing) return result;
    queue->playing = true;

    MusicFile *current = music_queue_peek(queue);
    if (current == NULL) return MA_ERROR;
    
    current->cur_position = 0;
    result = ma_decoder_init_file(slice_data(current->file_path), NULL, &app->decoder);
    if (result != MA_SUCCESS) {
        TRACE_ERROR("Could not load file: %s reason: %s\n", slice_data(current->file_path), ma_result_description(result));
        return result;
    }

    app->data_converter_config = ma_data_converter_config_init(
        app->decoder.outputFormat,      app->device.playback.format, 
        app->decoder.outputChannels,    app->device.playback.channels, 
        app->decoder.outputSampleRate,  app->device.sampleRate
    );

    result = ma_data_converter_init_preallocated(&app->data_converter_config, app->data_converter_memory, &app->data_converter);
    if (result != MA_SUCCESS) {
        TRACE_ERROR("Couldn't initialize data converter, reason: %s\n", ma_result_description(result));
        return result;
    }

    result = ma_decoder_get_length_in_pcm_frames(&app->decoder, &current->end_position);
    if (result != MA_SUCCESS) {
        TRACE_ERROR("Couldn't get length of %s: %s", slice_data(current->file_path), ma_result_description(result));
        return result;
    }

    app->w_playlist.dirty = true;
    return result;
}

ma_result app_play_music_at_index(AppState *app, usize index) {
    ma_result result = MA_SUCCESS;
    MusicQueue *queue = app->music_queue;
    //if (queue->playing) return result;
    queue->playing = true;
    queue->cursor = index;
    app_play_current_music(app);
    return result;
}