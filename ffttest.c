#include <math.h>
#define CFLAT_IMPLEMENTATION
#include <stdio.h>
#include "vendor/Cflat/src/Cflat.h"
#include "vendor/Cflat/src/CflatMath.h"
#include "vendor/Cflat/src/CflatCore.h"
#include "vendor/Cflat/src/CflatFft.h"

#define FFT_SIZE (KiB(4))
#define CHANNELS 2


f32 test_symmetry(c32 *out, usize n, usize channels) {
    f32 max_err = 0.0f;
    for (usize k = 1; k < n / 2; k++)
    for (usize channel = 0; channel < channels; channel += 1) {
        c32 a = out[k*channels + channel];
        c32 b = out[(n - k)*channels + channel];
        
        f32 err_re = fabsf(crealf(a) - crealf(b));
        f32 err_im = fabsf(cimagf(a) + cimagf(b));
        
        if (err_re > max_err) max_err = err_re;
        if (err_im > max_err) max_err = err_im;
    }
    printf("Symmetry error: %f\n", max_err);
    return max_err;
}


int check() {

    c32 a[FFT_SIZE][CHANNELS];
    c32 baseline[FFT_SIZE][CHANNELS];
    c32 fft_rec[FFT_SIZE][CHANNELS];

    for (usize i = 0; i < FFT_SIZE; i += 1)
    for (usize channel = 0; channel < CHANNELS; channel += 1) {
        f32 f = 2*pi/(f32)i/(f32)FFT_SIZE;
        a[i][channel] = f;
    }

    precompute_twiddles(FFT_SIZE);
    __fft_c32(FFT_SIZE, CHANNELS, 1, (void*)a, (void*)baseline);
    if (test_symmetry((void*)baseline, FFT_SIZE, CHANNELS) > 1e-3f) return -1;

    __fft_lut_c32(FFT_SIZE, CHANNELS, 1, (void*)a, (void*)fft_rec, (void*)tls_twiddles, 1);
    if (test_symmetry((void*)fft_rec, FFT_SIZE, CHANNELS) > 1e-3f) return -1;

    return 0;
}

int main(int argc, char **argv) {
    #if defined(CHECK)
    if (check() != 0) return -1;
    printf("Success!\n");
    return 0;
    #endif

    _Alignas(64) static c32 in[FFT_SIZE][CHANNELS];
    _Alignas(64) static c32 out0[FFT_SIZE][CHANNELS];
    
    volatile usize ffts = 100;

    for (usize i = 0; i < FFT_SIZE; i += 1)
    for (usize channel = 0; channel < CHANNELS; channel += 1) {
        f32 f = 2*pi/(f32)i/(f32)FFT_SIZE;
        in[i][channel] = f;
    }

    if (argc < 2) {
        printf("Usage: ffttest <id>\n");
        return -1;
    }

    int id = atoi(argv[1]);

    for (usize i = 0; i < ffts; i += 1) {
        if (id == 0) {
            __fft_c32(FFT_SIZE, CHANNELS, 1, (void*)in, (void*)out0);
        }
        else if (id == 1) {
            precompute_twiddles(FFT_SIZE);
            __fft_lut_c32(FFT_SIZE, CHANNELS, 1, (void*)in, (void*)out0, tls_twiddles, 1);
        }
        else if (id == 2) {
        }
        else if (id == 3) {

        }
    }

    return 0;
}