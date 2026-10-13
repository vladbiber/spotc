/* spotc-fx: raw s16le stereo 44100 -> speed/reverb/bass -> stdout
 * usage: spotc-fx <control-fifo> [speed] [reverb] [bass]
 *        spotc-fx <control-fifo> [speed] [reverb] [bass]
 *                 --reverse-file <raw> [gain]
 * fifo commands: "speed 1.2" / "reverb 0.4" / "bass 1" / "gain 0.8" / "vinyl 2"
 *                / "rain 2"
 * SPOTC_FX_VINYL / SPOTC_FX_RAIN in the environment set the starting presets */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include "vinyl.h"
#include "rain.h"

#define CHUNK 2048          /* frames per read */
#define NCOMB 8
#define NALLP 4

static const int comb_len[NCOMB] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
static const int allp_len[NALLP] = { 556, 441, 341, 225 };
#define SPREAD 23

typedef struct { float *buf; int len, pos; float feedback, damp, store; } Comb;
typedef struct { float *buf; int len, pos; } Allpass;

typedef struct {
    Comb comb[NCOMB];
    Allpass allp[NALLP];
} Verb;

static void verb_init(Verb *v, int offset) {
    for (int i = 0; i < NCOMB; i++) {
        v->comb[i].len = comb_len[i] + offset;
        v->comb[i].buf = calloc(v->comb[i].len, sizeof(float));
        v->comb[i].feedback = 0.84f;
        v->comb[i].damp = 0.2f;
    }
    for (int i = 0; i < NALLP; i++) {
        v->allp[i].len = allp_len[i] + offset;
        v->allp[i].buf = calloc(v->allp[i].len, sizeof(float));
    }
}

static float verb_run(Verb *v, float in) {
    float out = 0.0f;
    for (int i = 0; i < NCOMB; i++) {
        Comb *c = &v->comb[i];
        float y = c->buf[c->pos];
        c->store = y * (1.0f - c->damp) + c->store * c->damp;
        c->buf[c->pos] = in + c->store * c->feedback;
        if (++c->pos >= c->len) c->pos = 0;
        out += y;
    }
    for (int i = 0; i < NALLP; i++) {
        Allpass *p = &v->allp[i];
        float b = p->buf[p->pos];
        p->buf[p->pos] = out + b * 0.5f;
        out = b - out;
        if (++p->pos >= p->len) p->pos = 0;
    }
    return out;
}

static void verb_set(Verb *v, float amount) {
    float fb = 0.70f + 0.20f * amount;
    for (int i = 0; i < NCOMB; i++) v->comb[i].feedback = fb;
}

/* 4-point cubic Hermite over interleaved-stereo history */
static float hermite(float xm1, float x0, float x1, float x2, float t) {
    float c = (x1 - xm1) * 0.5f;
    float w = x0 - x1;
    float z = c + w;
    float b = z + w + (x2 - x0) * 0.5f;
    return ((b * t - (z + b)) * t + c) * t + x0;
}

static volatile float g_speed = 1.0f, g_reverb = 0.0f;
static volatile float g_bass = 0.0f, g_gain = 1.0f;
static volatile int g_vinyl = 0, g_rain = 0;

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void poll_fifo(int fd) {
    static char acc[256];
    static size_t used;
    char tmp[128];
    ssize_t n;
    while ((n = read(fd, tmp, sizeof tmp)) > 0) {
        for (ssize_t i = 0; i < n && used < sizeof acc - 1; i++) acc[used++] = tmp[i];
    }
    acc[used] = 0;
    char *nl;
    while ((nl = strchr(acc, '\n'))) {
        *nl = 0;
        float val;
        if (sscanf(acc, "speed %f", &val) == 1 && val >= 0.4f && val <= 2.0f) g_speed = val;
        else if (sscanf(acc, "reverb %f", &val) == 1 && val >= 0.0f && val <= 1.0f) g_reverb = val;
        else if (sscanf(acc, "bass %f", &val) == 1 && val >= 0.0f && val <= 1.0f) g_bass = val;
        else if (sscanf(acc, "gain %f", &val) == 1 && val >= 0.0f && val <= 1.0f) g_gain = val;
        else if (sscanf(acc, "vinyl %f", &val) == 1 && val >= 0 && val < VINYL_PRESETS) g_vinyl = (int)val;
        else if (sscanf(acc, "rain %f", &val) == 1 && val >= 0 && val < RAIN_PRESETS) g_rain = (int)val;
        size_t rest = used - (nl + 1 - acc);
        memmove(acc, nl + 1, rest);
        used = rest;
        acc[used] = 0;
    }
}

static size_t read_reverse(FILE *f, int16_t *in, long long *remaining) {
    if (*remaining <= 0) return 0;
    size_t want = *remaining < CHUNK ? (size_t)*remaining : CHUNK;
    long long start = *remaining - (long long)want;
    if (fseeko(f, (off_t)start * 4, SEEK_SET) != 0) return 0;
    size_t got = fread(in, 4, want, f);
    for (size_t i = 0; i < got / 2; i++) {
        size_t j = got - 1 - i;
        int16_t l = in[i * 2], r = in[i * 2 + 1];
        in[i * 2] = in[j * 2];
        in[i * 2 + 1] = in[j * 2 + 1];
        in[j * 2] = l;
        in[j * 2 + 1] = r;
    }
    *remaining = start;
    return got;
}

int main(int argc, char **argv) {
    int ctl = -1;
    if (argc > 1) ctl = open(argv[1], O_RDONLY | O_NONBLOCK);
    if (argc > 2) g_speed = strtof(argv[2], NULL);
    if (argc > 3) g_reverb = strtof(argv[3], NULL);
    int reverse_arg = 0;
    /* Accept the pre-bass reverse syntax too, so cached/test invocations made
     * for older builds still work. */
    if (argc > 4 && !strcmp(argv[4], "--reverse-file"))
        reverse_arg = 4;
    else {
        if (argc > 4) g_bass = strtof(argv[4], NULL);
        if (argc > 5 && !strcmp(argv[5], "--reverse-file")) reverse_arg = 5;
    }
    g_speed = clampf(g_speed, 0.4f, 2.0f);
    g_reverb = clampf(g_reverb, 0.0f, 1.0f);
    g_bass = clampf(g_bass, 0.0f, 1.0f);
    const char *vinyl_env = getenv("SPOTC_FX_VINYL");
    if (vinyl_env) {
        int vp = atoi(vinyl_env);
        if (vp >= 0 && vp < VINYL_PRESETS) g_vinyl = vp;
    }
    const char *gain_env = getenv("SPOTC_FX_GAIN");
    if (gain_env) g_gain = clampf(strtof(gain_env, NULL), 0.0f, 1.0f);
    const char *rain_env = getenv("SPOTC_FX_RAIN");
    if (rain_env) {
        int rp = atoi(rain_env);
        if (rp >= 0 && rp < RAIN_PRESETS) g_rain = rp;
    }
    FILE *reverse_file = NULL;
    long long reverse_frames = 0;
    if (reverse_arg && argc > reverse_arg + 1) {
        reverse_file = fopen(argv[reverse_arg + 1], "rb");
        if (!reverse_file) return 2;
        if (fseeko(reverse_file, 0, SEEK_END) != 0) return 2;
        off_t bytes = ftello(reverse_file);
        if (bytes < 4) return 2;
        reverse_frames = bytes / 4;
        if (argc > reverse_arg + 2) g_gain = strtof(argv[reverse_arg + 2], NULL);
        g_gain = clampf(g_gain, 0.0f, 1.0f);
    }

    Verb vl, vr;
    verb_init(&vl, 0);
    verb_init(&vr, SPREAD);

    /* history keeps last 3 frames for interpolation across chunk borders */
    float hist[2][4] = {{0}};
    double frac = 0.0;
    int16_t in[CHUNK * 2];
    static int16_t out[CHUNK * 8];
    float smooth_speed = g_speed, smooth_reverb = g_reverb;
    float smooth_bass = g_bass, smooth_gain = g_gain;
    float bass_low[2] = {0};
    float cur_reverb_set = -1.0f;
    static Vinyl vinyl;
    vinyl_init(&vinyl);
    int vinyl_preset = g_vinyl;
    float vinyl_mix = g_vinyl ? 1.0f : 0.0f;
    static Rain rain;
    rain_init(&rain);
    int rain_preset = g_rain;
    float rain_mix = g_rain ? 1.0f : 0.0f;

    for (;;) {
        size_t got = reverse_file ? read_reverse(reverse_file, in, &reverse_frames)
                                  : fread(in, 4, CHUNK, stdin);
        if (got == 0) break;
        if (ctl >= 0) poll_fifo(ctl);

        int nout = 0;
        for (size_t i = 0; i < got; i++) {
            /* shift 4-frame history */
            for (int ch = 0; ch < 2; ch++) {
                hist[ch][0] = hist[ch][1];
                hist[ch][1] = hist[ch][2];
                hist[ch][2] = hist[ch][3];
                hist[ch][3] = in[i * 2 + ch] / 32768.0f;
            }
            /* emit output samples while fractional cursor is inside this frame */
            while (frac < 1.0) {
                /* Smooth every live change so buttons never cause a click. */
                smooth_speed += (g_speed - smooth_speed) * 0.002f;
                smooth_reverb += (g_reverb - smooth_reverb) * 0.002f;
                smooth_bass += (g_bass - smooth_bass) * 0.002f;
                smooth_gain += (g_gain - smooth_gain) * 0.002f;
                float speed = smooth_speed, wet = smooth_reverb, bass = smooth_bass;
                if (wet != cur_reverb_set) {
                    verb_set(&vl, wet);
                    verb_set(&vr, wet);
                    cur_reverb_set = wet;
                }
                float t = (float)frac;
                float frame[2];
                for (int ch = 0; ch < 2; ch++) {
                    float dry = hermite(hist[ch][0], hist[ch][1], hist[ch][2], hist[ch][3], t);
                    /* One-pole low band plus level compensation gives roughly
                     * 6 dB more bass relative to the highs without a large
                     * overall volume jump. Each channel keeps its own state. */
                    bass_low[ch] += (dry - bass_low[ch]) * 0.025f;
                    float bassed = (dry * (1.0f - bass * 0.10f) +
                                    bass_low[ch] * bass * 0.90f) /
                                   (1.0f + bass * 0.20f);
                    float w = verb_run(ch ? &vr : &vl, bassed * 0.015f);
                    frame[ch] = bassed * (1.0f - wet * 0.35f) + w * wet;
                }
                /* fade the record in and out instead of switching hard */
                int want = g_vinyl;
                if (want) vinyl_preset = want;
                vinyl_mix += ((want ? 1.0f : 0.0f) - vinyl_mix) * 0.0005f;
                if (vinyl_mix > 0.0005f) {
                    float dry[2] = { frame[0], frame[1] };
                    vinyl_run(&vinyl, frame, speed, &vinyl_presets[vinyl_preset]);
                    frame[0] = dry[0] + (frame[0] - dry[0]) * vinyl_mix;
                    frame[1] = dry[1] + (frame[1] - dry[1]) * vinyl_mix;
                }
                int want_rain = g_rain;
                if (want_rain) rain_preset = want_rain;
                rain_mix += ((want_rain ? 1.0f : 0.0f) - rain_mix) * 0.0002f;
                int raining = rain_mix > 0.0005f;
                if (raining) {
                    float drops[2] = { 0, 0 };
                    rain_run(&rain, drops, &rain_presets[rain_preset]);
                    frame[0] += drops[0] * rain_mix;
                    frame[1] += drops[1] * rain_mix;
                }
                for (int ch = 0; ch < 2; ch++) {
                    float s = raining ? rain_soft_limit(frame[ch]) : frame[ch];
                    if (s > 1.0f) s = 1.0f;
                    if (s < -1.0f) s = -1.0f;
                    out[nout * 2 + ch] = (int16_t)(s * 32767.0f * smooth_gain);
                }
                nout++;
                frac += speed;
                if (nout >= CHUNK * 4 - 2) break;
            }
            frac -= 1.0;
        }
        if (nout > 0 && fwrite(out, 4, nout, stdout) != (size_t)nout) break;
        fflush(stdout);
    }
    if (reverse_file) fclose(reverse_file);
    if (ctl >= 0) close(ctl);
    return 0;
}
