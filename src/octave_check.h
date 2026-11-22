/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DM32_OCTAVE_CHECK_H
#define DM32_OCTAVE_CHECK_H

/* Included in fixed_codec.c, sharing its integer square-root helper. */
enum { octave_comparison_radius = 64, octave_score_one = 1024 };

static int octave_period_correlation(const int32_t filtered[320], unsigned lag)
{
    const unsigned begin = 160 - octave_comparison_radius - lag / 2;
    int32_t sum_x = 0, sum_y = 0, sum_xx = 0, sum_yy = 0, sum_xy = 0;
    for (unsigned i = begin; i < begin + 2 * octave_comparison_radius; i += 2) {
        const int32_t x = filtered[i];
        const int32_t y = filtered[i + lag];
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_yy += y * y;
        sum_xy += x * y;
    }
    const uint32_t x_energy = (uint32_t)(sum_xx - sum_x * sum_x / octave_comparison_radius);
    const uint32_t y_energy = (uint32_t)(sum_yy - sum_y * sum_y / octave_comparison_radius);
    if (!x_energy || !y_energy)
        return 0;

    /* Scale both energies together so the product fits the native square root. */
    const int exponent = 16 - __builtin_clz(x_energy | y_energy);
    const unsigned shift = exponent > 0 ? (unsigned)exponent : 0;
    const uint32_t norm = root((x_energy >> shift) * (y_energy >> shift));
    if (!norm)
        return 0;
    const int32_t cross = sum_xy - sum_x * sum_y / octave_comparison_radius;
    return ((cross >> shift) * octave_score_one) / (int32_t)norm;
}

static int octave_best_period(const int32_t filtered[320], unsigned period, unsigned *lag)
{
    const unsigned first = period * 9 / (10 * 256);
    const unsigned last = (period * 11 + 10 * 256 - 1) / (10 * 256);
    int score = INT_MIN;
    for (unsigned candidate = first; candidate <= last; ++candidate) {
        const int correlation = octave_period_correlation(filtered, candidate);
        if (correlation > score) {
            score = correlation;
            *lag = candidate;
        }
    }
    return score;
}

static unsigned octave_lower_period(const int16_t speech[320], unsigned initial,
                                    int32_t filtered[320])
{
    for (unsigned i = 1; i < 319; ++i) {
        /* A 64-point comparison with 10-bit samples bounds the sums and
           the mean-removal products, without wide arithmetic in the loop. */
        const int32_t sum = (int32_t)speech[i - 1] + 2 * (int32_t)speech[i] + speech[i + 1];
        filtered[i] = sum >> 8;
    }
    unsigned lag = 0;
    const int short_fit = octave_best_period(filtered, initial, &lag);
    const int long_fit = octave_best_period(filtered, initial * 2, &lag);
    /* Same evidence thresholds as the host trial, rounded on the Q10 scale. */
    if (long_fit < 799 || long_fit - short_fit < 195)
        return 0;
    return lag * 256;
}

#endif
