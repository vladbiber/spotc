/* Procedural rain: filtered pink-noise wash, muffled drop grains, distant thunder.
 * Mixed after the speed change so it is never slowed. */
#ifndef SPOTC_RAIN_H
#define SPOTC_RAIN_H

#include "vinyl.h"

#define RAIN_GRAINS 64

typedef struct { const char *name; float wash, patter, density, near, thunder; } RainPreset;

/* density = drop grains per second, near = close drops per second,
 * thunder = rumbles per minute */
static const RainPreset rain_presets[] = {
    { "off",     0.0f,   0.0f,   0.0f,   0.0f, 0.0f },
    { "drizzle", 0.030f, 0.020f, 180.0f, 0.6f, 0.0f },
    { "rain",    0.055f, 0.026f, 520.0f, 1.6f, 0.0f },
    { "storm",   0.080f, 0.030f, 1100.0f, 3.0f, 1.5f },
};
#define RAIN_PRESETS (int)(sizeof rain_presets / sizeof rain_presets[0])

typedef struct { float amp, decay, lp, z, pan; } Grain;

typedef struct {
    uint32_t seed;
    float pink[2][7];
    Biquad wash_hp[2], wash_lp1[2], wash_lp2[2];
    Biquad pat_hp[2], pat_lp[2], thunder_lp1, thunder_lp2;
    float swell, swell_target;
    Grain g[RAIN_GRAINS];
    int next_grain;
    float thunder_env, thunder_peak;
    int thunder_rising;
} Rain;

static void rain_init(Rain *r) {
    memset(r, 0, sizeof *r);
    r->seed = 0x9E3779B9u;
    r->swell = r->swell_target = 0.9f;
    for (int c = 0; c < 2; c++) {
        bq_set(&r->wash_hp[c], 140, 0.7f, 1);
        bq_set(&r->wash_lp1[c], 2200, 0.6f, 0);
        bq_set(&r->wash_lp2[c], 3200, 0.6f, 0);
        bq_set(&r->pat_hp[c], 260, 0.7f, 1);
        bq_set(&r->pat_lp[c], 5000, 0.6f, 0);
    }
    bq_set(&r->thunder_lp1, 110, 0.7f, 0);
    bq_set(&r->thunder_lp2, 70, 0.7f, 0);
}

static inline float rain_rnd(Rain *r) {
    uint32_t x = r->seed;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    r->seed = x;
    return (x >> 8) * (1.0f / 16777216.0f);
}

/* Paul Kellet's economy pink noise filter */
static inline float rain_pink(Rain *r, int c) {
    float w = rain_rnd(r) * 2 - 1, *b = r->pink[c];
    b[0] = 0.99886f * b[0] + w * 0.0555179f;
    b[1] = 0.99332f * b[1] + w * 0.0750759f;
    b[2] = 0.96900f * b[2] + w * 0.1538520f;
    b[3] = 0.86650f * b[3] + w * 0.3104856f;
    b[4] = 0.55000f * b[4] + w * 0.5329522f;
    b[5] = -0.7616f * b[5] - w * 0.0168980f;
    float p = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f;
    b[6] = w * 0.115926f;
    return p * 0.11f;
}

static void rain_spawn(Rain *r, float amp, float ms_lo, float ms_hi, float hz_lo, float hz_hi) {
    Grain *g = &r->g[r->next_grain];
    r->next_grain = (r->next_grain + 1) % RAIN_GRAINS;
    float ms = ms_lo + rain_rnd(r) * (ms_hi - ms_lo);
    float hz = hz_lo + rain_rnd(r) * (hz_hi - hz_lo);
    g->amp = amp;
    g->decay = expf(-1.0f / (VINYL_RATE * ms * 0.001f));
    g->lp = 1.0f - expf(-2.0f * (float)M_PI * hz / VINYL_RATE);
    g->z = 0;
    /* most drops land near the middle, a few far to the sides */
    float u = rain_rnd(r) * 2 - 1;
    g->pan = 0.5f + 0.5f * u * fabsf(u);
}

static void rain_run(Rain *r, float out[2], const RainPreset *p) {
    /* the whole shower breathes slowly, never jumps */
    if (rain_rnd(r) < 0.15f / VINYL_RATE) r->swell_target = 0.72f + rain_rnd(r) * 0.28f;
    r->swell += (r->swell_target - r->swell) * (0.35f / VINYL_RATE);

    float wl = bq_run(&r->wash_lp2[0], bq_run(&r->wash_lp1[0], bq_run(&r->wash_hp[0], rain_pink(r, 0))));
    float wr = bq_run(&r->wash_lp2[1], bq_run(&r->wash_lp1[1], bq_run(&r->wash_hp[1], rain_pink(r, 1))));
    float wash = p->wash * r->swell * 5.0f;

    float density = p->density * (0.6f + 0.4f * r->swell);
    if (rain_rnd(r) < density / VINYL_RATE) {
        float a = rain_rnd(r);
        rain_spawn(r, p->patter * (0.15f + a * a * 0.85f), 0.4f, 2.2f, 900.0f, 3600.0f);
    }
    if (rain_rnd(r) < p->near / VINYL_RATE)
        rain_spawn(r, p->patter * (1.2f + rain_rnd(r) * 1.3f), 4.0f, 11.0f, 450.0f, 1100.0f);

    float pl = 0, pr = 0;
    for (int i = 0; i < RAIN_GRAINS; i++) {
        Grain *g = &r->g[i];
        if (g->amp < 1e-5f) continue;
        g->z += ((rain_rnd(r) * 2 - 1) - g->z) * g->lp;
        float v = g->z * g->amp;
        pl += v * (1.0f - g->pan);
        pr += v * g->pan;
        g->amp *= g->decay;
    }
    pl = bq_run(&r->pat_lp[0], bq_run(&r->pat_hp[0], pl)) * 10.0f;
    pr = bq_run(&r->pat_lp[1], bq_run(&r->pat_hp[1], pr)) * 10.0f;

    float th = 0;
    if (p->thunder > 0.0f) {
        if (!r->thunder_rising && r->thunder_env < 0.002f &&
            rain_rnd(r) < p->thunder / (60.0f * VINYL_RATE)) {
            r->thunder_rising = 1;
            r->thunder_peak = 0.4f + rain_rnd(r) * 0.6f;
        }
        if (r->thunder_rising) {
            r->thunder_env += (r->thunder_peak - r->thunder_env) * (1.2f / VINYL_RATE);
            if (r->thunder_env > r->thunder_peak * 0.95f) r->thunder_rising = 0;
        } else {
            r->thunder_env *= 1.0f - 0.18f / VINYL_RATE;
        }
        if (r->thunder_env > 0.001f)
            th = bq_run(&r->thunder_lp2, bq_run(&r->thunder_lp1, rain_rnd(r) * 2 - 1)) * r->thunder_env * 0.9f;
    }

    out[0] += wl * wash + pl + th;
    out[1] += wr * wash + pr + th;
}

static inline float rain_soft_limit(float x) {
    float a = fabsf(x);
    if (a <= 0.8f) return x;
    float y = 0.8f + 0.2f * tanhf((a - 0.8f) / 0.2f);
    return x < 0 ? -y : y;
}

#endif
