
#include "vendor/Cflat/src/CflatCore.h"
#include "vendor/Cflat/src/CflatMath.h"
#include "vendor/Cflat/src/CflatRingBuffer.h"
#include <math.h>

cflat_enum(WindowingOption, u8) {
    WINDOW_INPLACE         = 0,
    WINDOW_CREATE          = 1,
};


c32* carray_copy_f32_to_c32(const usize size, c32 array[size], const f32 in[size]) {
    for (usize i = 0; i < size; i += 1) array[i] = in[i];
    return array;
}

f32* carray_sum_c32f32(const usize size, f32 array[size], const c32 in[size]) {
    for (usize i = 0; i < size; i += 1) array[i] += crealf(in[i]);
    return array;
}

void carray_scale_f32(const usize size, f32 array[size], f32 scale) {
    for (usize i = 0; i < size; i += 1) array[i] *= scale;
}

void carray_add_f32(const usize size, f32 (*restrict out)[size], const f32 (*restrict in)[size]) {
    for (usize i = 0; i < size; i += 1) (*out)[i] += (*in)[i];
}

void carray_add_scaled_f32(const usize size, f32 (*restrict out)[size], const f32 (*restrict in)[size], f32 scale) {
    for (usize i = 0; i < size; i += 1) (*out)[i] += (*in)[i] * scale;
}

void carray_lowpass_filter_f32(usize frames, usize channels, f32 out[frames][channels], f32 alpha, f32 prev_out[channels]) {
    for (usize channel = 0; channel < channels; channel += 1) out[0][channel] = alpha * out[0][channel];
    
    for (usize i = 0; i < frames; i += 1)
    for (usize c = 0; c < channels; c += 1) {
        f32 x   = out[i][c];
        f32 ym1 = prev_out[c];
        prev_out[c] = out[i][c] = ym1 + alpha * (x - ym1);
    }
}

void carray_highpass_filter_f32(usize frames, usize channels, f32 out[frames][channels], f32 alpha, f32 prev_in[channels], f32 prev_out[channels]) {

    for (usize i = 0; i < frames; i += 1)
    for (usize c = 0; c < channels; c += 1) {
        f32 x   = out[i][c];
        f32 xm1 = prev_in[c];
        f32 ym1 = prev_out[c];
        f32 y = alpha * (ym1 + x - xm1);
        prev_in[c] = x;
        prev_out[c] = out[i][c] = y;
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

c32 freq_f32(f32 mag, f32 phase) {
    return mag*cexpf(I*phase);
}

#define TWENTY_OVER_LN_10 (8.6858896380650365530225783783321016458879401160733313222890756633) // 20/ln(10)
#define LN_10_OVER_TWENTY (0.1151292546497022842008995727342182103800550744314386488016663950) // ln(10)/20
#define SPL_OFFSET        (93.979400086720376095725222105510139464636202370757829173791450777) // 20*log10(1/20e-6)
#define LN_2_OVER_12      (0.0577622650466621091181026767881813806729583445300212711767233341) // ln(2)/12
#define AUDIO_EPSILON     (1e-12f)                                                             

f32 linear_to_dB_f32(f32 linear) {
    linear = fmaxf(linear, AUDIO_EPSILON);
    return logf(linear)*TWENTY_OVER_LN_10;                  // 20*log10(linear)
}

f32 linear_to_spldB_f32(f32 linear) {
    linear = fmaxf(linear, AUDIO_EPSILON);
    return logf(linear)*TWENTY_OVER_LN_10 + SPL_OFFSET;     // 20*log10(linear/20e-6)
}

f32 dB_to_linear_f32(f32 dB) {
    return expf(dB*LN_10_OVER_TWENTY);                      // pow(10, dB/20)
}

f32 wave_spldB_f32(c32 wave) {
    f32 linear = fmaxf(cabsf(wave), AUDIO_EPSILON);
    return logf(linear)*TWENTY_OVER_LN_10 + SPL_OFFSET;     // 20*log10(||wave||/20e-6)
}

f32 wave_dB_f32(c32 wave, f32 relative) {
    f32 linear = fmaxf(cabsf(wave), AUDIO_EPSILON);
    relative = fmaxf(relative, AUDIO_EPSILON);
    return logf(linear/relative)*TWENTY_OVER_LN_10;         // 20*log10(||wave||/relative)
}

f32 pitch_ratio_from_semitones_f32(f32 semitones) {
    return expf(semitones * LN_2_OVER_12);                  // pow(2, semitones/12)
}

f32 wave_mag_f32(c32 wave) {
    return cabsf(wave);
}

f32 wave_phase_f32(c32 wave) {
    return cargf(wave);
}

f32 round_nearest_int_f32(f32 x) {
    return floorf(x + 0.5f);
}

f32 wrap_2pi_f32(f32 x) {
    f32 angle = fmodf(x, 2.0f * pi);
    if (angle < 0) angle += 2.0f * pi;
    return angle;
}

f32 wrap_f32(f32 x, f32 low, f32 high) {
    if (low <= x && x < high) return x;

    const f32 range = high - low;
    const f32 inv_range = 1.0f/ range;
    const f32 num_wraps = floorf((x - low) * inv_range);
    return x - range * num_wraps;
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