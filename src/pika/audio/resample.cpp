#include "resample.h"

#include <algorithm>
#include <cmath>

namespace pika::audio {

namespace {

/*
 * The prototype low pass, committed rather than generated at build time like
 * the sound clips: 256 numbers are not worth a codegen step, and a table that
 * cannot silently change between builds is worth something in a signal path
 * whose failure mode is inaudible.
 *
 *   scipy.signal.firwin(256, 3700, window=('kaiser', 6.6), fs=32000)
 *
 * Where those three numbers come from. The target is 70dB of stopband: the
 * ADC is 12 bit, so about 72dB of full scale SNR, and alias rejection below
 * the converter's own floor buys nothing measurable. Kaiser's order rule
 * N = (A - 8) / (2.285 * dw) at N = 256 gives a transition of about 540Hz;
 * centring that on the 4kHz fold point puts the cutoff at 3700Hz, and MELPe's
 * band stops at 3.4kHz so nothing useful is inside it. Beta follows from A:
 * 0.5842 * (70 - 21)^0.4 + 0.07886 * (70 - 21) = 6.6.
 *
 * Measured: flat to 3.4kHz, -69dB at 4kHz, -77dB worst case above 4.3kHz,
 * -107dB at the backlight's 10kHz. Coefficients sum to 1, so the filter is
 * unity gain and the decimator needs no scaling; the interpolator multiplies
 * by four because each phase carries a quarter of the sum.
 */
constexpr float fir_h[fir_taps] = {
        -0.000021397f, -0.000018154f, -0.000002046f, +0.000021115f, +0.000039000f, +0.000038821f, +0.000015366f,
        -0.000023665f, -0.000059044f, -0.000068888f, -0.000041242f, +0.000016467f, +0.000077511f, +0.000107456f,
        +0.000082691f, +0.000006338f, -0.000088319f, -0.000151241f, -0.000141169f, -0.000050714f, +0.000083517f,
        +0.000194092f, +0.000215600f, +0.000121709f, -0.000053861f, -0.000226808f, -0.000301460f, -0.000222211f,
        -0.000010198f, +0.000237365f, +0.000390088f, +0.000351604f, +0.000117139f, -0.000211650f, -0.000468367f,
        -0.000504490f, -0.000272695f, +0.000134728f, +0.000518930f, +0.000669667f, +0.000478030f, +0.000007379f,
        -0.000521011f, -0.000829561f, -0.000727956f, -0.000225512f, +0.000451962f, +0.000960300f, +0.001009431f,
        +0.000524953f, -0.000289442f, -0.001032557f, -0.001300570f, -0.000902998f, +0.000014142f, +0.001013230f,
        +0.001570386f, +0.001346796f, +0.000387137f, -0.000867938f, -0.001779420f, -0.001831708f, -0.000918302f,
        +0.000564210f, +0.001881342f, +0.002320454f, +0.001570990f, -0.000075160f, -0.001825488f, -0.002763197f,
        -0.002322096f, -0.000616657f, +0.001560184f, +0.003098637f, +0.003132034f, +0.001515525f, -0.001036583f,
        -0.003256029f, -0.003943919f, -0.002609341f, +0.000212625f, +0.003157846f, +0.004683622f, +0.003867154f,
        +0.000943382f, -0.002722573f, -0.005260440f, -0.005237635f, -0.002450171f, +0.001866800f, +0.005567701f,
        +0.006648387f, +0.004312635f, -0.000505246f, -0.005481963f, -0.008005638f, -0.006523682f, -0.001453646f,
        +0.004858150f, +0.009193053f, +0.009070450f, +0.004121171f, -0.003515014f, -0.010066796f, -0.011948306f,
        -0.007661352f, +0.001197625f, +0.010439674f, +0.015191315f, +0.012371263f, +0.002519801f, -0.010033954f,
        -0.018945751f, -0.018897002f, -0.008513078f, +0.008331400f, +0.023687661f, +0.028942224f, +0.019102129f,
        -0.003982080f, -0.031117898f, -0.048583154f, -0.043391035f, -0.008969299f, +0.051149568f, +0.123364459f,
        +0.188044142f, +0.226188215f, +0.226188215f, +0.188044142f, +0.123364459f, +0.051149568f, -0.008969299f,
        -0.043391035f, -0.048583154f, -0.031117898f, -0.003982080f, +0.019102129f, +0.028942224f, +0.023687661f,
        +0.008331400f, -0.008513078f, -0.018897002f, -0.018945751f, -0.010033954f, +0.002519801f, +0.012371263f,
        +0.015191315f, +0.010439674f, +0.001197625f, -0.007661352f, -0.011948306f, -0.010066796f, -0.003515014f,
        +0.004121171f, +0.009070450f, +0.009193053f, +0.004858150f, -0.001453646f, -0.006523682f, -0.008005638f,
        -0.005481963f, -0.000505246f, +0.004312635f, +0.006648387f, +0.005567701f, +0.001866800f, -0.002450171f,
        -0.005237635f, -0.005260440f, -0.002722573f, +0.000943382f, +0.003867154f, +0.004683622f, +0.003157846f,
        +0.000212625f, -0.002609341f, -0.003943919f, -0.003256029f, -0.001036583f, +0.001515525f, +0.003132034f,
        +0.003098637f, +0.001560184f, -0.000616657f, -0.002322096f, -0.002763197f, -0.001825488f, -0.000075160f,
        +0.001570990f, +0.002320454f, +0.001881342f, +0.000564210f, -0.000918302f, -0.001831708f, -0.001779420f,
        -0.000867938f, +0.000387137f, +0.001346796f, +0.001570386f, +0.001013230f, +0.000014142f, -0.000902998f,
        -0.001300570f, -0.001032557f, -0.000289442f, +0.000524953f, +0.001009431f, +0.000960300f, +0.000451962f,
        -0.000225512f, -0.000727956f, -0.000829561f, -0.000521011f, +0.000007379f, +0.000478030f, +0.000669667f,
        +0.000518930f, +0.000134728f, -0.000272695f, -0.000504490f, -0.000468367f, -0.000211650f, +0.000117139f,
        +0.000351604f, +0.000390088f, +0.000237365f, -0.000010198f, -0.000222211f, -0.000301460f, -0.000226808f,
        -0.000053861f, +0.000121709f, +0.000215600f, +0.000194092f, +0.000083517f, -0.000050714f, -0.000141169f,
        -0.000151241f, -0.000088319f, +0.000006338f, +0.000082691f, +0.000107456f, +0.000077511f, +0.000016467f,
        -0.000041242f, -0.000068888f, -0.000059044f, -0.000023665f, +0.000015366f, +0.000038821f, +0.000039000f,
        +0.000021115f, -0.000002046f, -0.000018154f, -0.000021397f,
};

/* One round and clamp, in one place, because it is the only spot where the
   float pipeline meets the int16 the rest of the audio path deals in. */
inline int16_t to_sample(float v) { return (int16_t) std::lround(std::clamp(v, -32768.0f, 32767.0f)); }

}// namespace

void Decimator4::reset() { std::fill(std::begin(work_), std::end(work_), 0.0f); }

size_t Decimator4::process(std::span<const int16_t> in, std::span<int16_t> out) {
    const size_t n = in.size();

    if (n > max_in || (n % 4U) != 0U || out.size() < n / 4U) {
        return 0U;
    }

    /* work_ is [ the previous fir_taps-1 samples | this block ]. */
    constexpr size_t hist = fir_taps - 1U;

    for (size_t i = 0; i < n; i++) { work_[hist + i] = (float) in[i]; }

    const size_t outs = n / 4U;

    for (size_t j = 0; j < outs; j++) {
        const float *w = &work_[4U * j];
        float acc = 0.0f;

        for (size_t k = 0; k < fir_taps; k++) { acc += fir_h[k] * w[k]; }
        out[j] = to_sample(acc);
    }

    /* Slide the tail down. The window start advances by 4 per output and so
       by exactly n per block, which is why the two ends stay in step. */
    std::copy(&work_[n], &work_[n + hist], &work_[0]);
    return outs;
}

void Interpolator4::reset() { std::fill(std::begin(work_), std::end(work_), 0.0f); }

size_t Interpolator4::process(std::span<const int16_t> in, std::span<int16_t> out) {
    const size_t n = in.size();

    if (n > max_in || out.size() < 4U * n) {
        return 0U;
    }

    constexpr size_t hist = phase_taps - 1U;

    for (size_t i = 0; i < n; i++) { work_[hist + i] = (float) in[i]; }

    for (size_t i = 0; i < n; i++) {
        const float *w = &work_[i]; /* w[hist] is the newest sample. */

        for (size_t p = 0; p < 4U; p++) {
            float acc = 0.0f;

            for (size_t k = 0; k < phase_taps; k++) { acc += fir_h[4U * k + p] * w[hist - k]; }
            out[4U * i + p] = to_sample(4.0f * acc);
        }
    }

    std::copy(&work_[n], &work_[n + hist], &work_[0]);
    return 4U * n;
}

}// namespace pika::audio
