#define CFLAT_IMPLEMENTATION
#include "vendor/Cflat/src/CflatRingBuffer.h"
#include <complex.h>
#include <math.h>
#include <stdio.h>

#include "vendor/Cflat/src/Cflat.h"
#include "vendor/Cflat/src/CflatMath.h"
#include "vendor/Cflat/src/CflatCore.h"
#include "vendor/Cflat/src/CflatFft.h"
#include "vendor/Cflat/src/CflatLinear.h"

#define FFT_SIZE (KiB(1))
#define CHANNELS 2
#define RUNS 1000
#define SAMPLE_RATE 44100
#define MOCK_AUDIO_BUFFER_SIZE (240)

#include <time.h>
#include "phase_vocoder.c"
#include "audio_effects.c"

int cflat_verify_fft(usize n) {
    cflat_assert(cflat_is_pow2(n));

    _Alignas(32) c32 original[FFT_SIZE];
    _Alignas(32) c32 in_out  [FFT_SIZE];
    _Alignas(32) c32 scratch [FFT_SIZE];

    for (usize i = 0; i < n; i++) {
        f32 re = ((f32)rand() / (f32)RAND_MAX) * 2.0f - 1.0f;
        f32 im = ((f32)rand() / (f32)RAND_MAX) * 2.0f - 1.0f;
        original[i] = re + I * im;
        in_out[i] = original[i];
    }

    cflat_twidle_precompute();
    cflat_fft_c32(n, scratch, in_out);   // scratch = FFT(in_out)
    cflat_ifft_c32(n, in_out, scratch);  // in_out = IFFT(scratch)

    c64 sum_sq_error = 0.0;
    c64 sum_sq_mag = 0.0;
    f64 sum_linear_error = 0.0;

    for (usize i = 0; i < n; i++) {
        c64 diff = (c64)in_out[i] - (c64)original[i];
        
        f64 err_re = creal(diff);
        f64 err_im = cimag(diff);
        sum_sq_error += (err_re * err_re) + (err_im * err_im);

        f64 orig_re = creal(original[i]);
        f64 orig_im = cimag(original[i]);
        sum_sq_mag += (orig_re * orig_re) + (orig_im * orig_im);

        sum_linear_error += cabs(diff);
    }

    f64 rms_error = sqrt(sum_sq_error / n);
    f64 relative_error = sqrt(sum_sq_error / sum_sq_mag);
    f64 linear_error = sum_linear_error / n;

    printf("--- FFT Verification (N=%zu) ---\n", n);
    printf("RMS Error:      %.2e\n", rms_error);
    printf("Relative Error: %.2e\n", relative_error);
    printf("Linear Error:   %.2e\n", linear_error);

    if (relative_error <= 1e-3) {
        printf("Verdict: PASS\n");
        return 0;
    } else {
        printf("Verdict: FAIL (High precision loss)\n");
        return -1;
    }
}

static double get_time_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (ts.tv_sec * 1000.0) + (ts.tv_nsec / 1000000.0);
}

int main(int argc, char **argv) {

    if (cflat_verify_fft(FFT_SIZE) != 0) return -1;

    _Alignas(64) static f32 input_buffer[MOCK_AUDIO_BUFFER_SIZE][CHANNELS];
    _Alignas(64) static f32 output_buffer[MOCK_AUDIO_BUFFER_SIZE][CHANNELS];
    _Alignas(64) static PhaseVocoderf32 vocoder = { .pitch_ratio = 1.0, .time_stretch = 1.0 };

    f32 deadline_ms = (f32)FFT_SIZE / (f32)SAMPLE_RATE * 1000.0f;
    f64 total_time = 0;
    f64 max_time = 0;
    u32 deadlines_missed = 0;

    cflat_twidle_precompute();

    for (usize b = 0; b < RUNS; b++) {
        for (usize ch = 0; ch < CHANNELS; ch++) {
            for (usize i = 0; i < MOCK_AUDIO_BUFFER_SIZE; i++) {
                input_buffer[i][ch] = sinf(2.0f * pi * 440.0f * (f32)i / SAMPLE_RATE);
            }
        }

        double start = get_time_ms();
        {
            vocoder_poll_f32(&vocoder, MOCK_AUDIO_BUFFER_SIZE, CHANNELS, &output_buffer, &input_buffer);
            interleaved_lowpass_filter_f32(MOCK_AUDIO_BUFFER_SIZE, CHANNELS, output_buffer, (LowPassFilter[CHANNELS]){{ .alpha = 0.75 }, { .alpha = 0.75 }});
            interleaved_highpass_filter_f32(MOCK_AUDIO_BUFFER_SIZE, CHANNELS, output_buffer, (HighPassFilter[CHANNELS]){{ .alpha = 0.75 }, { .alpha = 0.75 }});
            carray_scale_f32(MOCK_AUDIO_BUFFER_SIZE*CHANNELS, (void*)&output_buffer, 0.5);
        }
        double end = get_time_ms();
        double elapsed = end - start;

        total_time += elapsed;
        if (elapsed > max_time) max_time = elapsed;
        if (elapsed > deadline_ms) deadlines_missed++;
    }

    printf("--- Real-Time Audio Benchmark ---\n");
    printf("Config: %d channels, %d FFT size, %d Hz\n", CHANNELS, FFT_SIZE, SAMPLE_RATE);
    printf("Buffer Deadline:  %.3f ms\n", deadline_ms);
    printf("Average Time:     %.3f ms\n", total_time / RUNS);
    printf("Worst-Case Time:  %.3f ms\n", max_time);
    printf("Deadlines Missed: %u / %d\n", deadlines_missed, RUNS);
    printf("CPU Load:         %.2f%%\n", (total_time / RUNS) / deadline_ms * 100.0);

    return 0;
}