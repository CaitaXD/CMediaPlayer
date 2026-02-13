#include "vendor/Cflat/src/CflatCore.h"
#include "vendor/Cflat/src/CflatFft.h"
#include "vendor/Cflat/src/CflatRingBuffer.h"
#include "audio_effects.c"

#if !defined(FFT_SIZE)
#   define FFT_SIZE (KiB(1))
#endif

#if !defined(FFT_BINS)
#   define FFT_BINS (FFT_SIZE/2)
#endif

#if !defined(HOP_SIZE)
#   define HOP_SIZE ((FFT_SIZE) / 8)
#endif

#if !defined(HOPS_PER_WINDOW)
#   define HOPS_PER_WINDOW ((f64)HOP_SIZE/(f64)FFT_SIZE)
#endif

#if !defined(CHANNELS)
#   define CHANNELS 2
#endif

#define PTR_ADD_BYTES(X, N) ((byte*)(X) + (N))

typedef struct frame_f32 {
    f32 samples[CHANNELS];
} Framef32;

typedef struct ring_buffer_32Kib_f32 {
    isize read;
    isize write;
    usize length;
    Framef32 data[KiB(16)];
} RingBuffer32Kibf32;

typedef struct phase_vocoder {
    cflat_alignas(64) RingBuffer32Kibf32 input_buffer;
    cflat_alignas(64) RingBuffer32Kibf32 output_buffer;    
    cflat_alignas(64) f32 analysis_window       [FFT_SIZE];
    cflat_alignas(64) f32 synthesis_window      [FFT_SIZE];
    cflat_alignas(64) c32 fft_output            [CHANNELS][FFT_SIZE    ];
    cflat_alignas(64) c32 fft_input             [CHANNELS][FFT_SIZE    ];
    cflat_alignas(64) f32 last_input_phases     [CHANNELS][FFT_SIZE    ];
    cflat_alignas(64) f32 last_output_phases    [CHANNELS][FFT_SIZE    ];
    cflat_alignas(64) f32 analysis_magnitudes   [CHANNELS][FFT_BINS + 1];
    cflat_alignas(64) f32 analysis_frequencies  [CHANNELS][FFT_BINS + 1];
    cflat_alignas(64) f32 synthesis_magnitudes  [CHANNELS][FFT_BINS + 1];
    cflat_alignas(64) f32 synthesis_frequencies [CHANNELS][FFT_BINS + 1];
    usize hop;
    f32 pitch_ratio;
    f32 time_stretch;
} PhaseVocoderf32;

void phase_vocoder_analyse(PhaseVocoderf32 *vocoder) {
    RingBuffer32Kibf32 *input_buffer = &vocoder->input_buffer;
    const usize channels = CHANNELS;
    for (usize channel = 0; channel < channels; channel += 1) {
        for (usize k = 0; k < FFT_SIZE; k += 1) {
            usize ring_index = (input_buffer->write + k - FFT_SIZE + input_buffer->length) % input_buffer->length;
            f32 in = input_buffer->data[ring_index].samples[channel];
            vocoder->fft_input[channel][k] =  in*vocoder->analysis_window[k]*HOPS_PER_WINDOW;
        }
    }

    for (usize ch = 0; ch < channels; ch += 1) cflat_fft_c32(FFT_SIZE, vocoder->fft_output[ch], vocoder->fft_input[ch]);
    
    for (usize k = 0; k < FFT_BINS; k += 1)
    for (usize channel = 0; channel < channels; channel += 1) {
        c32 wave = vocoder->fft_output[channel][k];
        f32 amplitude = wave_mag_f32(wave);
        f32 phase = wave_phase_f32(wave);
        f32 phase_delta = phase - vocoder->last_input_phases[channel][k];
        f32 bin_center_freq = 2.0f * pi* (f32)k/ (f32)FFT_SIZE;
        phase_delta = wrap_f32(phase_delta - bin_center_freq*HOP_SIZE, -pi, pi);
        f32 deviation = phase_delta * (f32)FFT_SIZE/(f32)HOP_SIZE / (2.0f * pi);
        vocoder->analysis_frequencies[channel][k] = (f32)k + deviation;
        vocoder->analysis_magnitudes[channel][k] = amplitude;
        vocoder->last_input_phases[channel][k] = phase;
    }
}

void phase_vocoder_synthesize(PhaseVocoderf32 *vocoder) {
    
    RingBuffer32Kibf32 *output_buffer = &vocoder->output_buffer;
    const usize channels    = CHANNELS;

    for (usize k = 0; k < FFT_BINS; k += 1) 
    for (usize channel = 0; channel < channels; channel += 1) {
        f32 amplitude = vocoder->synthesis_magnitudes[channel][k];
        f32 deviation = vocoder->synthesis_frequencies[channel][k] - k;
        f32 phase_delta = deviation * (2.0f * pi) * HOPS_PER_WINDOW;
        f32 bin_center_freq = 2.0f * pi * (f32)k/(f32)FFT_SIZE;
        phase_delta += bin_center_freq * HOP_SIZE;
        f32 out_phase = wrap_f32(vocoder->last_output_phases[channel][k] + phase_delta, -pi, pi);
        c32 out_freq = freq_f32(amplitude, out_phase);
        vocoder->fft_output[channel][k] = out_freq;
        if (k > 0 && k < FFT_BINS) {
            vocoder->fft_output[channel][FFT_SIZE - k] = conjf(out_freq);
        }
        vocoder->last_output_phases[channel][k] = out_phase;
    }
    
    for (usize ch = 0; ch < channels; ch += 1) cflat_ifft_c32(FFT_SIZE, vocoder->fft_input[ch], vocoder->fft_output[ch]);
    
    for (usize channel = 0; channel < channels; channel += 1) 
    for (usize k = 0; k < FFT_SIZE; k += 1) {
        usize ring_index = (output_buffer->write + k) % output_buffer->length;
        output_buffer->data[ring_index].samples[channel] += vocoder->fft_input[channel][k] * vocoder->synthesis_window[k];
    }
    
}

void phase_vocoder_reset_synthesis(PhaseVocoderf32 *vocoder) {
    const usize channels = CHANNELS;
    for (usize channel = 0; channel < channels; channel += 1) 
    for (usize k = 0; k < FFT_BINS; k += 1) {
        vocoder->synthesis_magnitudes[channel][k] = vocoder->synthesis_frequencies[channel][k] = 0;
    }
}

void phase_vocoder_pitch_shift(PhaseVocoderf32 *vocoder) {
    const usize channels = CHANNELS;
    const f32 ratio      = vocoder->pitch_ratio;
    for (usize channel = 0; channel < channels; channel += 1)
    for (usize k = 0; k < FFT_BINS; k += 1) {
        const usize new_bin = round_nearest_int_f32(k * ratio);
        if (new_bin > FFT_BINS) continue;
        vocoder->synthesis_magnitudes[channel][new_bin] += vocoder->analysis_magnitudes[channel][k];
        vocoder->synthesis_frequencies[channel][new_bin] = vocoder->analysis_frequencies[channel][k] * ratio;
    }
}

void phase_vocoder_procces_f32(PhaseVocoderf32 *vocoder) {    
    RingBuffer32Kibf32 *output_buffer = &vocoder->output_buffer;
    if (++vocoder->hop >= HOP_SIZE) {
        vocoder->hop = 0;
        phase_vocoder_analyse(vocoder);
        phase_vocoder_reset_synthesis(vocoder);
        phase_vocoder_pitch_shift(vocoder);
        phase_vocoder_synthesize(vocoder);
        output_buffer->write = (output_buffer->write + HOP_SIZE) % output_buffer->length;
    }
}

void vocoder_poll_f32(PhaseVocoderf32 *vocoder, const usize frames_count, const usize channels, f32 (*restrict out_buffer)[frames_count][channels], const f32 (*restrict in_buffer)[frames_count][channels]) {
    RingBuffer32Kibf32 *output_buffer = &vocoder->output_buffer;
    RingBuffer32Kibf32 *input_buffer  = &vocoder->input_buffer;
    output_buffer->length = ARRAY_SIZE(output_buffer->data);
    input_buffer->length = ARRAY_SIZE(input_buffer->data);

    for (usize k = 0; k < frames_count; k += 1) {
        Framef32 frame_in = input_buffer->data[input_buffer->write];
        ring_buffer_overwrite((void*)input_buffer, sizeof(frame_in), &(*in_buffer)[k]);
        Framef32 frame_out;
        ring_buffer_read((void*)output_buffer, sizeof(frame_out), frame_out.samples, .clear = true);
        carray_add_f32(ARRAY_SIZE(frame_out.samples), &(*out_buffer)[k], &frame_out.samples);
        phase_vocoder_procces_f32(vocoder);
    }
}