#define CFLAT_IMPLEMENTATION
#include "vendor/Cflat/src/CflatBit.h"
#include "vendor/Cflat/src/CflatCore.h"
#include "vendor/Cflat/src/CflatArena.h"
#include <assert.h>
#include <wchar.h>
#include "vendor/Cflat/src/Cflat.h"
#include "vendor/Cflat/src/CflatRingBuffer.h"
#include "vendor/Cflat/src/CflatFft.h"

#include <raylib.h>

#if defined(OS_WINDOWS)           
    #include <minwindef.h>
    #include <winnt.h>
	#define NOGDI            
	#define NOUSER            
    typedef struct tagMSG *LPMSG;
    VOID Sleep(DWORD dwMilliseconds);
#endif

#include <miniaudio.h>
#include <stdint.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <complex.h>
#include <math.h>
#include <complex.h>
#include <string.h>
#include <stdio.h>
#include <float.h>
#include "miniaudio.h"
#include <assert.h>
#include <locale.h>
#include <minwindef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include "widgets.c"
#include "music_queue.c"
#include "audio_effects.c"

#define CAPTURE_SAMPLE_RATE 44100
#define CAPTURE_CHANNELS 2
#define FFT_SIZE (KiB(2))
#define TRACE_ERROR(message, ...) TraceLog(LOG_ERROR, message "[%s:%d] %s\n",  ##__VA_ARGS__, __FILE__, __LINE__, __func__)

typedef struct slice_f32 { CFLAT_SLICE_FIELDS(f32); } SliceF32;
typedef struct slice_c32 { CFLAT_SLICE_FIELDS(c32); } SliceC32;

typedef struct w_frequency_spectrum {
    BOUNDS;
    Color background_color;
    f32 lower_note, upper_note;
    f32 sample_rate;
    usize fft_size, channels;
    void *waves;
} WFrequencySpectrum;

typedef struct w_playlist {
    BOUNDS;
    bool dirty;
} WPlaylist;

typedef struct w_playlist_buttons {
    BOUNDS;
} WPlaylistButtons;

typedef struct app_state {
    BOUNDS;
    Arena *arena;
    
    ma_context context;
    
    ma_device_config device_config;
    ma_device device;

    ma_decoder decoder;
    
    MusicQueue *music_queue;
    
    ma_data_converter_config data_converter_config;
    ma_data_converter data_converter;
    void *data_converter_memory;
    
    CflatRingBuffer *input_buffer;
    CflatRingBuffer *output_buffer;
    
    usize fft_size;
    usize hop_size;

    c32 (smooth_waves     )[FFT_SIZE][CAPTURE_CHANNELS];
    c32 (fft_out          )[FFT_SIZE][CAPTURE_CHANNELS];
    c32 (fft_in           )[FFT_SIZE][CAPTURE_CHANNELS];
    f32 (last_input_phases)[FFT_SIZE][CAPTURE_CHANNELS];
    f32 (hanning_window   )[FFT_SIZE];

    f64 step;
    f64 notes[12*12];

    WFrequencySpectrum w_frequency_spectrum;
    WPlaylist w_playlist;
    WPlaylistButtons w_playlist_buttons;

    _Atomic(i32) atomic_latch;

    f32 master_volume;
    f32 playback_volume;
    f32 capture_volume;

    Color background_color;
    bool playlist_wrap;
    bool playlist_shuffle;

    f32 capture_low_pass_filter_alpha;
    f32 capture_high_pass_filter_alpha;
    f32 capture_preamp_gain;
} AppState;

ma_result app_play_current_music(AppState *app);
ma_result app_stop_playing(AppState *app);
ma_result app_set_pause_music(AppState *app, bool pause);
ma_result app_play_music_at_index(AppState *app, usize index);

void draw_frequencies(WFrequencySpectrum *widget);
void draw_playlist(WPlaylist *widget);
bool draw_playlist_buttons(WPlaylistButtons *widget);

ma_result on_context_uninit(ma_context *ctx) {
    AppState *app = ctx->pUserData;
    atomic_store(&app->atomic_latch, 0);
    return MA_SUCCESS;
}

#define PTR_ADD_BYTES(X, N) ((byte*)(X) + (N))

void apply_filters(ma_device* dvc, void* buffer, const ma_uint32 frames_count) {
    AppState *app = dvc->pUserData;
    carray_scale_f32(frames_count*dvc->capture.channels, buffer, app->capture_volume*app->master_volume + gain_to_volume_f32(app->capture_preamp_gain));
    carray_lowpass_filter_f32 (frames_count, dvc->capture.channels, buffer, app->capture_low_pass_filter_alpha);
    carray_highpass_filter_f32(frames_count, dvc->capture.channels, buffer, app->capture_high_pass_filter_alpha);
}

void apply_block_processing(ma_device* dvc) {
    AppState *app = dvc->pUserData;
    CflatRingBuffer *input_buffer = app->input_buffer;
    CflatRingBuffer *output_buffer = app->output_buffer;

    const usize frame_size = ma_get_bytes_per_frame(dvc->capture.format, dvc->capture.channels);
    const usize sample_size = ma_get_bytes_per_sample(dvc->capture.format);
    const usize channels = dvc->capture.channels;

    for (usize i = 0; i < app->fft_size; i += 1) {
        usize ring_index = (input_buffer->write + i - app->fft_size + input_buffer->length) % input_buffer->length;
        for (usize channel = 0; channel < channels; channel += 1) {
            app->fft_in[i][channel] = *(f32*)PTR_ADD_BYTES(input_buffer->data, (ring_index*frame_size + channel*sample_size)) * app->hanning_window[i];
        }
    }

    fast_fourier_transform_c32(app->fft_size, dvc->capture.channels, app->fft_in, app->fft_out);
    
    const usize bins_count = app->fft_size/2;
    for (usize k = 0; k < bins_count; k += 1)
    for (usize channel = 0; channel < dvc->capture.channels; channel += 1) {
        f32 mag = mag_f32(app->fft_out[k][channel]);
        f32 phase = phase_f32(app->fft_out[k][channel]);
        
        f32 phse_diff = phase - app->last_input_phases[k][channel];
        
        f32 bin_center_freq = 2.0f * pi* (f32)k/ (f32)app->fft_size;
        phse_diff = wrap_phase_f32(phse_diff - bin_center_freq * app->hop_size, -pi, pi);

        f32 bin_deviation = phse_diff * (f32)app->fft_size / (f32)app->hop_size / 2.0f * pi;
        f32 estimated_phase = phase + bin_deviation;

        app->last_input_phases[k][channel] = phase;

        (void)mag;
        (void)phase;
        (void)estimated_phase;
    }
    
    inverse_fast_fourier_transform_c32(app->fft_size, dvc->capture.channels, app->fft_out, app->fft_in);
    carray_window_c32(app->fft_size, dvc->capture.channels, &app->fft_in, app->hanning_window);
    
    for (usize i = 0; i < app->fft_size; i += 1)
    {
        usize ring_index = (output_buffer->write + i) % output_buffer->length;
        carray_sum_c32f32(channels, (void*)(output_buffer->data + ring_index*frame_size), app->fft_in[i]);
    }
}

void data_callback(ma_device* dvc, void* out_buffer, const void* in_buffer, const ma_uint32 frames_count) {
    cflat_assert(dvc->capture.format == ma_format_f32);
    cflat_assert(dvc->playback.format == ma_format_f32);
    cflat_assert(dvc->playback.channels == dvc->capture.channels);

    AppState *app = dvc->pUserData;
    const usize frame_size = ma_get_bytes_per_frame(dvc->playback.format, dvc->playback.channels);
    
    static usize s_hop_counter = 0;
    
    //f32 hops_per_window = (f32)hop_size / (f32)app->fft_size;

    for (usize i = 0; i < frames_count; i += 1) {
        const void *in = PTR_ADD_BYTES(in_buffer, i*frame_size);
        void *out = PTR_ADD_BYTES(out_buffer, i*frame_size);
        
        (ring_buffer_overwrite)(frame_size, app->input_buffer, in);
        (ring_buffer_read)(frame_size, app->output_buffer, out, (RingBufferReadOpt) { .clear = true });
        //carray_scale_f32(channels, PTR_ADD_BYTES(out_buffer, i*frame_size), hops_per_window);

        if (++s_hop_counter >= app->hop_size) {
            s_hop_counter = 0;
            apply_block_processing(dvc);
            app->output_buffer->write = (app->output_buffer->write + app->hop_size) % app->output_buffer->length;
        }
    }

    apply_filters(dvc, out_buffer, frames_count);
}

void play_file_data_callback(ma_device* dvc, void* out, const void* in, const ma_uint32 frames_count) {
    (void)in;
    AppState *app = dvc->pUserData;

    if (app->music_queue->cursor >= slice_length(app->music_queue->playlist)) return;
    if (app->music_queue->playing == false) return;

    MusicFile *music_file = &slice_data(app->music_queue->playlist)[app->music_queue->cursor];
    ma_decoder* decoder = &app->decoder;
    ma_data_converter *data_converter = &app->data_converter;
    ma_uint64 required_input_frames;
    ma_result result;
    
    const usize output_frame_size = ma_get_bytes_per_frame(decoder->outputFormat, decoder->outputChannels);
    result = ma_data_converter_get_required_input_frame_count(data_converter, frames_count, &required_input_frames);

    TempArena callback_arena;
    arena_scratch_scope(callback_arena, app->arena) {
        Arena *arena = callback_arena.arena;
        
        void* decoder_frames = arena_push(arena, required_input_frames*output_frame_size, .clear = true);
        ma_uint64 frames_read;
        
        while ((result = ma_decoder_read_pcm_frames(decoder, decoder_frames, required_input_frames, &frames_read)) == MA_SUCCESS && frames_read < required_input_frames) ;
        
        if (result == MA_AT_END) {
            if (music_queue_next(app->music_queue, .reapeat = app->playlist_wrap, .random = app->playlist_shuffle)) {
                app_play_current_music(app);
            }
            scope_exit;
        }

        music_file->cur_position += frames_read;

        const usize playback_frame_size = ma_get_bytes_per_frame(dvc->playback.format, dvc->playback.channels);

        f32 *converted_frames = arena_push(arena, frames_count*playback_frame_size, .clear = true);

        ma_uint64 frame_count_in = required_input_frames, frame_count_out = frames_count;
        
        result = ma_data_converter_process_pcm_frames(data_converter, decoder_frames, &frame_count_in, converted_frames, &frame_count_out);
        result = ma_mix_pcm_frames_f32(out, converted_frames, frames_count, dvc->capture.channels, app->playback_volume * app->master_volume);

        for (usize i = 0; i < max(frame_count_out, frames_count); i += 1) {
            (ring_buffer_overwrite)(playback_frame_size, app->output_buffer, (byte*)out + i*playback_frame_size);
        }
    }
}

void cflat_sleep_msec(u32 milliseconds) {
    #if defined(OS_WINDOWS)
        Sleep(milliseconds);
    #else
        sleep(milliseconds/1000);
    #endif
}
#define NOTE_INDEX(NOTE, OCTAVE) ((INDEX_WRAP(NOTE, 12)) + ((OCTAVE) + (1)) * (12))

cflat_enum(Note, u8) {
    C = 0,
    D = 2,
    E = 4,
    F = 5,
    G = 7,
    A = 9,
    B = 11,

    C_SHARP = C + 1,
    D_SHARP = D + 1,
    E_SHARP = E + 1,
    F_SHARP = F + 1,
    G_SHARP = G + 1,
    A_SHARP = A + 1,
    B_SHARP = C,

    C_FLAT = B,
    D_FLAT = D - 1,
    E_FLAT = E - 1,
    F_FLAT = F - 1,
    G_FLAT = G - 1,
    A_FLAT = A - 1,
    B_FLAT = B - 1,
};

#define POW1(X) ((X))
#define POW2(X) ((X)*(X))
#define POW3(X) ((X)*(X)*(X))
#define POW4(X) ((X)*(X)*(X)*(X))
#define POW5(X) ((X)*(X)*(X)*(X)*(X))
#define POW6(X) ((X)*(X)*(X)*(X)*(X)*(X))
#define POW7(X) ((X)*(X)*(X)*(X)*(X)*(X)*(X))
#define POW8(X) ((X)*(X)*(X)*(X)*(X)*(X)*(X)*(X))
#define POW9(X) ((X)*(X)*(X)*(X)*(X)*(X)*(X)*(X)*(X))
#define POW10(X) ((X)*(X)*(X)*(X)*(X)*(X)*(X)*(X)*(X)*(X))
#define POW(X, N) CONCAT(POW, N)(X)
