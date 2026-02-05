
#include "vendor/Cflat/src/CflatCore.h"
#include "vendor/Cflat/src/CflatMath.h"
#include "vendor/Cflat/src/CflatRingBuffer.h"
#include <math.h>

cflat_enum(WindowingOption, u8) {
    WINDOW_INPLACE         = 0,
    WINDOW_CREATE          = 1,
};


c32* carray_assign_f32c32(const usize size, c32 array[size], const f32 in[size]) {
    for (usize i = 0; i < size; i += 1) array[i] = in[i];
    return array;
}

f32* carray_sum_c32f32(const usize size, f32 array[size], const c32 in[size]) {
    for (usize i = 0; i < size; i += 1) array[i] += crealf(in[i]);
    return array;
}

f32 carray_scale_f32(const usize size, f32 array[size], f32 scale) {
    for (usize i = 0; i < size; i += 1) array[i] *= scale;
    return scale;
}

void carray_lowpass_filter_f32(usize frames, usize channels, f32 array[frames][channels], f32 alpha) {
    for (usize channel = 0; channel < channels; channel += 1) array[0][channel] = alpha * array[0][channel];
    
    for (usize i = 1; i < frames; i += 1)
    for (usize channel = 0; channel < channels; channel += 1) {
        f32 x   = array[i][channel];
        f32 ym1 = array[i-1][channel];
        array[i][channel] = ym1 + alpha * (x - ym1);
    }
}

void carray_highpass_filter_f32(usize frames, usize channels, f32 array[frames][channels], f32 alpha) {
    for (usize i = 1; i < frames; i += 1)
    for (usize channel = 0; channel < channels; channel += 1) {
        f32 x   = array[i][channel];
        f32 xm1 = array[i-1][channel];
        f32 ym1 = array[i-1][channel];
        array[i][channel] = alpha * (ym1 + x - xm1);
    }
}

void carray_window_f32(usize frames, usize channels, f32 (*restrict array)[frames][channels], f32 *restrict window) {
    for (usize i = 0; i < frames; i += 1) 
    for (usize channel = 0; channel < channels; channel += 1) 
        (*array)[i][channel] *= window[i];
}

void carray_window_c32(usize frames, usize channels, c32 (*restrict array)[frames][channels], f32 *restrict window) {
    for (usize i = 0; i < frames; i += 1) 
    for (usize channel = 0; channel < channels; channel += 1) 
        (*array)[i][channel] *= window[i];
}

void cosine_window_f32(usize len, f32 out[len], usize coeff_len, const f32 coefficients[coeff_len], WindowingOption clear)
{
    if (clear == WINDOW_CREATE) for (usize i = 0; i < len; i += 1) out[i] = 1;

    if (len == 1)
    {
        out[0] *= 1.0;
    }
    else
    {
        for (usize i = 0; i < len; ++i)
        {
            f32 reuslt = 0.0;
            for (usize j = 0; j < coeff_len; ++j)
                reuslt += coefficients[j] * cosf(i * j * 2.0f * pi / len);
            out[i] *= reuslt;
        }
    }
}

void hanning_window_f32(usize len, f32 out[len], WindowingOption clear) {
    const f32 coeff[] = { 0.5f, -0.5f };
    cosine_window_f32(len, out, ARRAY_SIZE(coeff), coeff, clear);
}

void flattop_window_f32(usize len, f32 out[len], WindowingOption clear)
{
    const f32 coeff[] = { 0.21557895, -0.41663158, 0.277263158, -0.083578947, 0.006947368 };
    cosine_window_f32(len, out, ARRAY_SIZE(coeff), coeff, clear);
}


f32 spldB_f32(c32 wave) {
    return 20.0f*log10f(cabsf(wave)/20e-6);
}

f32 dB_f32(c32 wave, f32 relative) {
    return 20.0f*log10f(cabsf(wave)/relative);
}

f32 gain_to_volume_f32(f32 gain) {
    return pow(10.0f, gain/20.0f);
}

f32 mag_f32(c32 wave) {
    return cabsf(wave);
}

f32 phase_f32(c32 wave) {
    return cargf(wave);
}

f32 wrap_phase_f32(f32 phase, f32 min, f32 max) {
    f32 range = max - min;
    return fmodf(phase + range, range) + min;
}

void copy_apply_lowpass_filter_f32(usize frames, usize channels, f32 out[frames][channels], const f32 in[frames][channels], f32 alpha) {
    for (usize channel = 0; channel < channels; channel += 1)
        out[0][channel] = alpha * in[0][channel];
    
    for (usize i = 1; i < frames; i += 1)
    for (usize channel = 0; channel < channels; channel += 1) {
        f32 x   = in[i][channel];
        f32 ym1 = out[i-1][channel];
        out[i][channel] = ym1 + alpha * (x - ym1);
    }
}

void copy_apply_highpass_filter_f32(usize frames, usize channels, f32 out[frames][channels], const f32 in[frames][channels], f32 alpha) {
    for (usize channel = 0; channel < channels; channel += 1)
        out[0][channel] = in[0][channel];
    
    for (usize i = 1; i < frames; i += 1)
    for (usize channel = 0; channel < channels; channel += 1) {
        f32 x   = in[i][channel];
        f32 xm1 = in[i-1][channel];
        f32 ym1 = out[i-1][channel];
        out[i][channel] = alpha * (ym1 + x - xm1);
    }
}

void copy_lerpbuffer_f32(usize frames, usize channels, f32 out[frames][channels], const f32 in[frames][channels], f32 t) {
    for (usize i = 0; i < frames; i += 1)
    for (usize channel = 0; channel < channels; channel += 1) {
        f32 a = out[i][channel];
        f32 b = in[i][channel];
        out[i][channel] = a + (b - a) * t;
    }
}