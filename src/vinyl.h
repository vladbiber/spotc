/* Record emulation for spotc-fx: wow and flutter locked to the platter,
 * groove rolloff with soft saturation and mono bass, surface hiss, motor
 * rumble, random crackle and two scratches that tick once per revolution.
 * Runs per output frame, so a slowed track also slows the platter. */
#ifndef SPOTC_VINYL_H
#define SPOTC_VINYL_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define VINYL_RATE 44100.0f
#define VINYL_DELAY 256
#define VINYL_BASE 96.0f
#define VINYL_RPM (100.0 / 3.0)

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } Biquad;

typedef struct { const char *name; float crackle, warmth, wow, surface; } VinylPreset;

/* index 0 means off; spotc cycles through these with one key */
static const VinylPreset vinyl_presets[] = {
    { "off",    0.0f,  0.0f,  0.0f, 0.0f  },
    { "mint",   0.12f, 0.5f,  0.2f, 0.15f },
    { "played", 0.45f, 0.6f,  0.4f, 0.4f  },
    { "dusty",  0.8f,  0.75f, 0.6f, 0.7f  },
    { "lo-fi",  0.6f,  1.0f,  0.9f, 0.85f },
};
#define VINYL_PRESETS (int)(sizeof vinyl_presets / sizeof vinyl_presets[0])

static void bq_set(Biquad *q, float freq, float Q, int high) {
    double w = 2.0 * M_PI * freq / VINYL_RATE, cw = cos(w), alpha = sin(w) / (2.0 * Q), a0 = 1.0 + alpha;
    if (high) { q->b0 = (float)((1 + cw) / 2 / a0); q->b1 = (float)(-(1 + cw) / a0); }
    else      { q->b0 = (float)((1 - cw) / 2 / a0); q->b1 = (float)((1 - cw) / a0); }
    q->b2 = q->b0;
    q->a1 = (float)(-2 * cw / a0);
    q->a2 = (float)((1 - alpha) / a0);
}

static void bq_band(Biquad *q, float freq, float Q) {
    double w = 2.0 * M_PI * freq / VINYL_RATE, alpha = sin(w) / (2.0 * Q), a0 = 1.0 + alpha;
    q->b0 = (float)(alpha / a0); q->b1 = 0; q->b2 = (float)(-alpha / a0);
    q->a1 = (float)(-2 * cos(w) / a0); q->a2 = (float)((1 - alpha) / a0);
}

static inline float bq_run(Biquad *q, float x) {
    float y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

typedef struct {
    float dl[VINYL_DELAY], dr[VINYL_DELAY];
    int wpos;
    Biquad tone[2], dc[2], side, hiss_lp, hiss_hp, rumble, tick, pop;
    float applied_warmth, sat_k, sat_t, sat_norm;
    uint32_t seed;
    double platter, wow_phase, flutter_phase;
    float drift, drift_target, tick_env, tick_pan, pop_env, pop_pan;
    float tick_decay, pop_decay;
} Vinyl;

static void vinyl_init(Vinyl *v) {
    memset(v, 0, sizeof *v);
    v->seed = 0x2545F491u;
    v->applied_warmth = -1.0f;
    v->tick_pan = v->pop_pan = 0.5f;
    bq_set(&v->dc[0], 12, 0.707f, 1); bq_set(&v->dc[1], 12, 0.707f, 1);
    bq_set(&v->side, 160, 0.707f, 1);
    bq_set(&v->hiss_lp, 7500, 0.6f, 0); bq_set(&v->hiss_hp, 900, 0.6f, 1);
    bq_set(&v->rumble, 28, 0.9f, 0);
    bq_band(&v->tick, 2800, 0.9f);
    bq_set(&v->pop, 1400, 0.8f, 0);
    v->tick_decay = expf(-1.0f / (VINYL_RATE * 0.00025f));
    v->pop_decay = expf(-1.0f / (VINYL_RATE * 0.0025f));
}

static inline float vinyl_rnd(Vinyl *v) {
    uint32_t x = v->seed;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    v->seed = x;
    return (x >> 8) * (1.0f / 16777216.0f);
}

static void vinyl_run(Vinyl *v, float io[2], float rate, const VinylPreset *p) {
    float crackle = p->crackle, warmth = p->warmth, wow = p->wow, surface = p->surface;
    if (warmth != v->applied_warmth) {
        float cut = 18000.0f - 10500.0f * warmth;
        bq_set(&v->tone[0], cut, 0.6f, 0); bq_set(&v->tone[1], cut, 0.6f, 0);
        v->sat_k = 1.0f + 1.2f * warmth;
        v->sat_t = tanhf(v->sat_k * 0.12f);
        v->sat_norm = 1.0f / (v->sat_k * (1.0f - v->sat_t * v->sat_t));
        v->applied_warmth = warmth;
    }

    double rev = VINYL_RPM / 60.0 * rate / VINYL_RATE;
    double prev = v->platter;
    v->platter += rev;
    int crossed = v->platter >= 1.0;
    if (crossed) v->platter -= 1.0;
    v->wow_phase += rev;
    if (v->wow_phase >= 1.0) v->wow_phase -= 1.0;
    v->flutter_phase += 6.3 / VINYL_RATE;
    if (v->flutter_phase >= 1.0) v->flutter_phase -= 1.0;
    if (vinyl_rnd(v) < 2.0f / VINYL_RATE) v->drift_target = vinyl_rnd(v) * 2 - 1;
    v->drift += (v->drift_target - v->drift) * (1.0f / VINYL_RATE);
    float mod = wow * (38.0f * ((float)sin(2 * M_PI * v->wow_phase) + 0.35f * v->drift) +
                       0.8f * (float)sin(2 * M_PI * v->flutter_phase));

    v->dl[v->wpos] = io[0];
    v->dr[v->wpos] = io[1];
    float rp = v->wpos - (VINYL_BASE + mod);
    if (rp < 0) rp += VINYL_DELAY;
    int i0 = (int)rp, i1 = i0 + 1 >= VINYL_DELAY ? 0 : i0 + 1;
    float f = rp - i0;
    float l = v->dl[i0] + (v->dl[i1] - v->dl[i0]) * f;
    float r = v->dr[i0] + (v->dr[i1] - v->dr[i0]) * f;
    if (++v->wpos >= VINYL_DELAY) v->wpos = 0;

    if (warmth > 0.0f) {
        float mid = (l + r) * 0.5f;
        float side = bq_run(&v->side, (l - r) * 0.5f) * (1.0f - 0.22f * warmth);
        l = bq_run(&v->tone[0], mid + side);
        r = bq_run(&v->tone[1], mid - side);
        float sl = (tanhf(v->sat_k * (l + 0.12f)) - v->sat_t) * v->sat_norm;
        float sr = (tanhf(v->sat_k * (r + 0.12f)) - v->sat_t) * v->sat_norm;
        float mix = 0.7f * warmth;
        l = bq_run(&v->dc[0], l + (sl - l) * mix);
        r = bq_run(&v->dc[1], r + (sr - r) * mix);
    }

    float nl = 0, nr = 0;
    if (surface > 0.0f) {
        float h = bq_run(&v->hiss_hp, bq_run(&v->hiss_lp, vinyl_rnd(v) * 2 - 1)) * 0.006f * surface;
        float rum = bq_run(&v->rumble, vinyl_rnd(v) * 2 - 1) * 0.3f * surface;
        nl += h + rum;
        nr += h * 0.8f + rum;
    }
    if (crackle > 0.0f) {
        if (vinyl_rnd(v) < (3.0f + 70.0f * crackle) / VINYL_RATE) {
            float a = -logf(vinyl_rnd(v) + 1e-6f) * 0.018f * crackle;
            v->tick_env += vinyl_rnd(v) < 0.5f ? a : -a;
            v->tick_pan = vinyl_rnd(v);
        }
        if (vinyl_rnd(v) < (0.08f + 0.5f * crackle) / VINYL_RATE) {
            float a = (0.08f + vinyl_rnd(v) * 0.18f) * crackle;
            v->pop_env += vinyl_rnd(v) < 0.5f ? a : -a;
            v->pop_pan = 0.25f + vinyl_rnd(v) * 0.5f;
        }
        static const float at[2] = { 0.17f, 0.62f }, lvl[2] = { 0.55f, 0.3f };
        for (int s = 0; s < 2; s++) {
            int hit = crossed ? (prev < at[s] || v->platter >= at[s])
                              : (prev < at[s] && v->platter >= at[s]);
            if (hit) { v->pop_env += 0.11f * lvl[s] * (0.4f + crackle); v->pop_pan = 0.45f; }
        }
        float t = bq_run(&v->tick, v->tick_env) + v->tick_env * 0.4f;
        float pp = bq_run(&v->pop, v->pop_env);
        v->tick_env *= v->tick_decay;
        v->pop_env *= v->pop_decay;
        nl += t * (1 - v->tick_pan) * 2 + pp * (1 - v->pop_pan) * 2;
        nr += t * v->tick_pan * 2 + pp * v->pop_pan * 2;
    }
    io[0] = l + nl;
    io[1] = r + nr;
}

#endif
