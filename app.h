#define CFLAT_IMPLEMENTATION
#include "vendor/Cflat/src/CflatMath.h"
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
#define CHANNELS 2
#define FFT_SIZE (KiB(1))
#define HOP_SIZE ((FFT_SIZE) / 8)
#define FFT_BINS (FFT_SIZE/2)
#define HOPS_PER_WINDOW ((f64)HOP_SIZE/(f64)FFT_SIZE)
#define TRACE_ERROR(message, ...) TraceLog(LOG_ERROR, message "[%s:%d] %s\n",  ##__VA_ARGS__, __FILE__, __LINE__, __func__)

typedef struct slice_f32 { CFLAT_SLICE_FIELDS(f32); } SliceF32;
typedef struct slice_c32 { CFLAT_SLICE_FIELDS(c32); } SliceC32;

typedef struct phase_vocoder {
    CflatRingBuffer *input_buffer;
    CflatRingBuffer *output_buffer;    
    f32 analysis_window       [FFT_SIZE    ];
    f32 synthesis_window      [FFT_SIZE    ];
    c32 fft_output            [FFT_SIZE    ][CHANNELS];
    c32 fft_input             [FFT_SIZE    ][CHANNELS];
    f32 last_input_phases     [FFT_SIZE    ][CHANNELS];
    f32 last_output_phases    [FFT_SIZE    ][CHANNELS];
    c32 smooth_waves          [FFT_SIZE    ][CHANNELS];
    f32 analysis_magnitudes   [FFT_BINS + 1][CHANNELS];
    f32 analysis_frequencies  [FFT_BINS + 1][CHANNELS];
    f32 synthesis_magnitudes  [FFT_BINS + 1][CHANNELS];
    f32 synthesis_frequencies [FFT_BINS + 1][CHANNELS];
    ma_format format;
    ma_uint32 channels;
} PhaseVocoder;

typedef struct w_frequency_spectrum {
    BOUNDS;
    Color background_color;
    f32 lower_note, upper_note;
    f32 sample_rate;
    usize fft_size, channels;
} WFrequencySpectrum;

typedef struct w_playlist {
    BOUNDS;
    bool dirty;
} WPlaylist;

typedef struct w_playlist_buttons {
    BOUNDS;
} WPlaylistButtons;

typedef struct app_state {
    /* Memory */
    Arena *arena;
    /* UI */
    BOUNDS;
    Color background_color;
    WFrequencySpectrum w_frequency_spectrum;
    WPlaylist w_playlist;
    WPlaylistButtons w_playlist_buttons;
    c32 display_waves[FFT_SIZE][CHANNELS];
    /* Syncronization */
    _Atomic(i32) atomic_latch;
    /* Audio API */
    ma_context context;
    ma_decoder decoder;
    ma_device device;
    ma_device_config device_config;
    ma_data_converter_config data_converter_config;
    void *data_converter_memory;
    ma_data_converter data_converter;
    /* Audio Effects */
    PhaseVocoder music_queue_vocoder;
    PhaseVocoder capture_vocoder;
    /* Audio Effect Parameters */
    f32 master_volume;
    f32 music_volume;
    f32 capture_volume;
    f32 capture_low_pass_filter_alpha;
    f32 capture_high_pass_filter_alpha;
    f32 capture_preamp_gain;
    f32 pitch_shift_semitones;
    bool playlist_wrap;
    bool playlist_shuffle;
    /* Music */
    MusicQueue *music_queue;
    /* Notes */
    f64 note_step;
    f64 notes[12*12];
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

void capture_filters(ma_device* dvc, void* buffer, const ma_uint32 frames_count) {
    AppState *app = dvc->pUserData;
    f32 linear_gain = dB_to_linear_f32(app->capture_preamp_gain);
    carray_scale_f32(frames_count*dvc->capture.channels, buffer,    app->capture_volume * (1.0f + linear_gain));
    carray_lowpass_filter_f32 (frames_count, dvc->capture.channels, buffer, app->capture_low_pass_filter_alpha);
    carray_highpass_filter_f32(frames_count, dvc->capture.channels, buffer, app->capture_high_pass_filter_alpha);
}

void phase_vocoder_analyse(PhaseVocoder *vocoder) {
    const CflatRingBuffer *input_buffer = vocoder->input_buffer;

    const usize frame_size = ma_get_bytes_per_frame(vocoder->format, vocoder->channels);
    const usize sample_size = ma_get_bytes_per_sample(vocoder->format);
    const usize channels = vocoder->channels;

    for (usize i = 0; i < FFT_SIZE; i += 1) {
        usize ring_index = (input_buffer->write + i - FFT_SIZE + input_buffer->length) % input_buffer->length;
        for (usize channel = 0; channel < channels; channel += 1) {
            vocoder->fft_input[i][channel] = *(f32*)PTR_ADD_BYTES(input_buffer->data, (ring_index*frame_size + channel*sample_size)) * vocoder->analysis_window[i] * HOPS_PER_WINDOW;
        }
    }

    fast_fourier_transform_c32(FFT_SIZE, channels, vocoder->fft_input, vocoder->fft_output);
    
    for (usize k = 0; k < FFT_BINS; k += 1)
    for (usize channel = 0; channel < channels; channel += 1) {
        c32 wave = vocoder->fft_output[k][channel];
        f32 amplitude = wave_mag_f32(wave);
        f32 phase = wave_phase_f32(wave);
        f32 phase_delta = phase - vocoder->last_input_phases[k][channel];
        f32 bin_center_freq = 2.0f * pi* (f32)k/ (f32)FFT_SIZE;
        phase_delta = wrap_f32(phase_delta - bin_center_freq*HOP_SIZE, -pi, pi);
        f32 deviation = phase_delta * (f32)FFT_SIZE/(f32)HOP_SIZE / (2.0f * pi);
        vocoder->analysis_frequencies[k][channel] = (f32)k + deviation;
        vocoder->analysis_magnitudes[k][channel] = amplitude;
        vocoder->last_input_phases[k][channel] = phase;
    }
}

void phase_vocoder_synthesize(PhaseVocoder *vocoder) {
    
    const CflatRingBuffer *output_buffer = vocoder->output_buffer;
    const usize frame_size = ma_get_bytes_per_frame(vocoder->format, vocoder->channels);
    const usize sample_size = ma_get_bytes_per_sample(vocoder->format);

    for (usize k = 0; k < FFT_BINS; k += 1) 
    for (usize channel = 0; channel < vocoder->channels; channel += 1) {
        f32 amplitude = vocoder->synthesis_magnitudes[k][channel];
        f32 deviation = vocoder->synthesis_frequencies[k][channel] - k;
        f32 phase_delta = deviation * (2.0f * pi) * HOPS_PER_WINDOW;
        f32 bin_center_freq = 2.0f * pi * (f32)k/(f32)FFT_SIZE;
        phase_delta += bin_center_freq * HOP_SIZE;
        f32 out_phase = wrap_f32(vocoder->last_output_phases[k][channel] + phase_delta, -pi, pi);
        c32 out_freq = freq_f32(amplitude, out_phase);
        vocoder->fft_output[k][channel] = out_freq;
        if (k > 0 && k < FFT_BINS) {
            vocoder->fft_output[FFT_SIZE - k][channel] = conjf(out_freq);
        }
        vocoder->last_output_phases[k][channel] = out_phase;
    }
    
    inverse_fast_fourier_transform_c32(FFT_SIZE, vocoder->channels, vocoder->fft_output, vocoder->fft_input);
    
    for (usize i = 0; i < FFT_SIZE; i += 1)
    {
        usize ring_index = (output_buffer->write + i) % output_buffer->length;
        for (usize channel = 0; channel < vocoder->channels; channel += 1) {
            *(f32*)PTR_ADD_BYTES(output_buffer->data, ring_index*frame_size + channel*sample_size) += vocoder->fft_input[i][channel] * vocoder->synthesis_window[i];
        }
    }
}

void phase_vocoder_reset_synthesis(PhaseVocoder *vocoder) {
    for (usize k = 0; k < FFT_BINS; k += 1) 
    for (usize channel = 0; channel < vocoder->channels; channel += 1) {
        vocoder->synthesis_magnitudes[k][channel] = vocoder->synthesis_frequencies[k][channel] = 0;
    }
}

void phase_vocoder_pitch_shift(PhaseVocoder *vocoder, f32 semitones) {
    const f32 ratio = pitch_ratio_from_semitones_f32(semitones);
    for (usize k = 0; k < FFT_BINS; k += 1) {
        const usize new_bin = round_nearest_int_f32(k * ratio);
        if (new_bin > FFT_BINS) continue;

        for (usize channel = 0; channel < vocoder->channels; channel += 1) {
            vocoder->synthesis_magnitudes[new_bin][channel] += vocoder->analysis_magnitudes[k][channel];
            vocoder->synthesis_frequencies[new_bin][channel] = vocoder->analysis_frequencies[k][channel] * ratio;
        }
    }
}

void phase_vocoder(PhaseVocoder *vocoder) {    
    static usize s_hop_counter = 0;
    CflatRingBuffer *output_buffer = vocoder->output_buffer;
    if (++s_hop_counter >= HOP_SIZE) {
        s_hop_counter = 0;
        phase_vocoder_analyse(vocoder);
        phase_vocoder_reset_synthesis(vocoder);
        phase_vocoder_pitch_shift(vocoder, 0);
        phase_vocoder_synthesize(vocoder);
        output_buffer->write = (output_buffer->write + HOP_SIZE) % output_buffer->length;
    }
}

void capture_vocoder(ma_device* dvc) {
    AppState *app = dvc->pUserData;
    static usize s_hop_counter = 0;
    CflatRingBuffer *output_buffer = app->capture_vocoder.output_buffer;
    if (++s_hop_counter >= HOP_SIZE) {
        s_hop_counter = 0;
        phase_vocoder_analyse(&app->capture_vocoder);
        phase_vocoder_reset_synthesis(&app->capture_vocoder);
        phase_vocoder_pitch_shift(&app->capture_vocoder, app->pitch_shift_semitones);
        phase_vocoder_synthesize(&app->capture_vocoder);
        output_buffer->write = (output_buffer->write + HOP_SIZE) % output_buffer->length;
    }
}


void app_poll_music_queue_f32(AppState *app, const ma_uint32 frames_count, const usize channels, f32 frames[frames_count][channels]) {

    if (app->music_queue->cursor >= slice_length(app->music_queue->playlist)) return;
    if (app->music_queue->playing == false) return;

    MusicFile *music_file = &slice_data(app->music_queue->playlist)[app->music_queue->cursor];
    ma_decoder* decoder = &app->decoder;
    ma_data_converter *data_converter = &app->data_converter;
    ma_uint64 required_input_frames;
    ma_result result;
    ma_device *dvc = &app->device;
    
    const usize output_frame_size = ma_get_bytes_per_frame(decoder->outputFormat, decoder->outputChannels);
    result = ma_data_converter_get_required_input_frame_count(data_converter, frames_count, &required_input_frames);

    TempArena callback_arena;
    arena_scratch_scope(callback_arena, 1, &app->arena) {
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

        const usize frame_size = ma_get_bytes_per_frame(dvc->playback.format, dvc->playback.channels);

        f32 (*converted_frames)[frames_count][channels] = arena_push(arena, frames_count*frame_size, .clear = true);
        ma_uint64 frame_count_in = required_input_frames, frame_count_out = frames_count;
        result = ma_data_converter_process_pcm_frames(data_converter, decoder_frames, &frame_count_in, (void*)converted_frames, &frame_count_out);
        if (result != MA_SUCCESS) {
            TRACE_ERROR("Couldn't convert frames, reason: %s\n", ma_result_description(result));
            return;
        }
        
        carray_scale_f32(frames_count*channels, (void*)converted_frames, app->music_volume);
        for (usize i = 0; i < frame_count_out; i += 1) {
            f32 out[channels];
            ring_buffer_overwrite(app->music_queue_vocoder.input_buffer, frame_size, (*converted_frames)[i]);
            ring_buffer_read(app->music_queue_vocoder.output_buffer, frame_size, out, .clear = true);
            carray_add_f32(channels, &frames[i], &out);
            phase_vocoder(&app->music_queue_vocoder);
        }
    }
}

void app_poll_capture_device_f32(AppState *app, const ma_uint32 frames_count, const usize channels, f32 out_buffer[frames_count][channels], const f32 in_buffer[frames_count][channels]) {
    
    ma_device *dvc = &app->device;
    const usize frame_size = ma_get_bytes_per_frame(dvc->playback.format, dvc->playback.channels);

    for (usize i = 0; i < frames_count; i += 1) {
        void *top = app->capture_vocoder.input_buffer->data + app->capture_vocoder.input_buffer->write*frame_size;
        ring_buffer_overwrite(app->capture_vocoder.input_buffer, frame_size, in_buffer[i]);
        capture_filters(dvc, top, 1);
        
        f32 out[channels];
        ring_buffer_read(app->capture_vocoder.output_buffer, frame_size, out, .clear = true);
        carray_add_scaled_f32(channels, &out_buffer[i], &out, 1.0 / HOPS_PER_WINDOW);
        capture_vocoder(dvc);
    }
}

void data_callback(ma_device* dvc, void* out_buffer, const void* in_buffer, const ma_uint32 frames_count) {
    cflat_assert(dvc->capture.format == ma_format_f32);
    cflat_assert(dvc->playback.format == ma_format_f32);
    cflat_assert(dvc->playback.channels == dvc->capture.channels);
    AppState *app = dvc->pUserData;
    
    app_poll_music_queue_f32(app, frames_count, dvc->playback.channels, out_buffer);
    app_poll_capture_device_f32(app, frames_count, dvc->capture.channels, out_buffer, in_buffer);
    carray_scale_f32(frames_count * dvc->playback.channels, out_buffer, app->master_volume);
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
