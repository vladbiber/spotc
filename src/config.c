#include "spotc.h"
#include <sys/stat.h>
#include <stdarg.h>
#include <ctype.h>

const RetunePreset retune_presets[RETUNE_PRESETS] = {
    { 440, "A4",  1.0f,       0.0f,  "standard concert pitch (retune off)" },
    { 174, "F3",  0.996483f,  0.0f,  "claimed: natural anaesthetic, pain relief" },
    { 285, "C#4", 1.028204f,  0.0f,  "claimed: tissue regeneration" },
    { 396, "G4",  1.010216f,  0.0f,  "claimed: liberation from fear" },
    { 432, "A4",  0.981818f,  0.0f,  "claimed: natural tuning, relaxation" },
    { 528, "C5",  1.009075f,  0.0f,  "claimed: love frequency, DNA repair" },
    { 963, "B5",  0.974927f,  0.0f,  "claimed: pineal gland, third eye" },
    { 417, "A4",  0.947727f,  0.20f, "claimed: undoing trauma, change (deep)" },
    { 396, "A4",  0.9f,       0.30f, "claimed: liberation from fear (deep)" },
    { 369, "F#4", 0.997314f,  0.0f,  "claimed: Tesla's 3-6-9 key to the universe" },
};

static int parse_key(const char *s) {
    if (!strcmp(s, "space")) return ' ';
    if (!strcmp(s, "tab")) return '\t';
    if (!strcmp(s, "enter")) return '\n';
    if (!strcmp(s, "escape") || !strcmp(s, "esc")) return 27;
    return s[0] && !s[1] ? (unsigned char)s[0] : 0;
}

const char *spotc_key_name(int key, char *buf, size_t n) {
    if (key == ' ') return "space";
    if (key == '\t') return "tab";
    if (key == '\n') return "enter";
    if (key == 27) return "escape";
    if (key > 0 && key < 127) { snprintf(buf, n, "%c", key); return buf; }
    return "disabled";
}

static void default_keys(Config *c) {
    c->key_quit = 'q';             c->key_focus = '\t';
    c->key_down = 'j';             c->key_up = 'k';
    c->key_top = 'g';              c->key_bottom = 'G';
    c->key_play_pause = ' ';       c->key_next = 'n';
    c->key_prev = 'b';             c->key_seek_back = ',';
    c->key_seek_forward = '.';     c->key_volume_down = '-';
    c->key_volume_up = '+';        c->key_shuffle = 's';
    c->key_repeat = 'r';           c->key_reverse = 'R';
    c->key_queue = 'a';
    c->key_speed_up = 'o';         c->key_speed_down = 'p';
    c->key_bass_boost = 'B';       c->key_vinyl = 'v';
    c->key_rain = 'w';
    c->key_fx_reset = '0';         c->key_retune = 'h';
    c->key_search = '/';
    c->key_devices = 'd';          c->key_help = '?';
    c->key_reload = 'L';           c->key_logout = 'X';
    c->key_client_id = 'i';
    c->key_login = 'l';
}

static void read_key(const char *name, const char *value, const char *wanted, int *out) {
    if (!strcmp(name, wanted)) {
        int key = parse_key(value);
        if (key) *out = key;
    }
}

long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void set_status(App *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->status, sizeof a->status, fmt, ap);
    va_end(ap);
}

char *xstrdup_trim(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) len--;
    char *r = malloc(len + 1);
    memcpy(r, s, len);
    r[len] = 0;
    return r;
}

void ms_to_clock(int ms, char *out, size_t n) {
    if (ms < 0) ms = 0;
    int s = ms / 1000;
    snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

static const char *make_dir(const char *env, const char *fallback_suffix, const char *sub, char *buf, size_t n) {
    const char *base = getenv(env);
    if (base && *base)
        snprintf(buf, n, "%s/%s", base, sub);
    else
        snprintf(buf, n, "%s/%s/%s", getenv("HOME") ? getenv("HOME") : ".", fallback_suffix, sub);
    mkdir(buf, 0700);
    return buf;
}

const char *spotc_dir(void) {
    static char buf[512];
    if (!buf[0]) make_dir("XDG_CONFIG_HOME", ".config", "spotc", buf, sizeof buf);
    return buf;
}

const char *cache_dir(void) {
    static char buf[512];
    if (!buf[0]) make_dir("XDG_CACHE_HOME", ".cache", "spotc", buf, sizeof buf);
    return buf;
}

void config_load(Config *c) {
    memset(c, 0, sizeof *c);
    snprintf(c->device_name, sizeof c->device_name, "spotc");
    c->accent = 141;
    c->dim = 244;
    c->bitrate = 320;
    c->speed = 1.0f;
    c->reverb = 0.0f;
    c->bass = 0.0f;
    c->retune = 1.0f;
    default_keys(c);

    char path[600];
    snprintf(path, sizeof path, "%s/config", spotc_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    int saw_key_reverse = 0, saw_speed_up = 0, saw_speed_down = 0;
    int legacy_speed_up = 0, legacy_speed_down = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = xstrdup_trim(line);
        char *val = xstrdup_trim(eq + 1);
        if (!strcmp(key, "client_id")) snprintf(c->client_id, sizeof c->client_id, "%s", val);
        else if (!strcmp(key, "device_name")) snprintf(c->device_name, sizeof c->device_name, "%s", val);
        else if (!strcmp(key, "accent")) c->accent = atoi(val);
        else if (!strcmp(key, "dim")) c->dim = atoi(val);
        else if (!strcmp(key, "bitrate")) c->bitrate = atoi(val);
        else if (!strcmp(key, "speed")) c->speed = strtof(val, NULL);
        else if (!strcmp(key, "reverb")) c->reverb = strtof(val, NULL);
        else if (!strcmp(key, "bass")) c->bass = strtof(val, NULL);
        else if (!strcmp(key, "retune")) c->retune = strtof(val, NULL);
        else if (!strcmp(key, "vinyl")) c->vinyl = atoi(val);
        else if (!strcmp(key, "rain")) c->rain = atoi(val);
        else if (!strcmp(key, "music_dir")) snprintf(c->music_dir, sizeof c->music_dir, "%s", val);
        else {
            read_key(key, val, "key_quit", &c->key_quit);
            read_key(key, val, "key_focus", &c->key_focus);
            read_key(key, val, "key_down", &c->key_down);
            read_key(key, val, "key_up", &c->key_up);
            read_key(key, val, "key_top", &c->key_top);
            read_key(key, val, "key_bottom", &c->key_bottom);
            read_key(key, val, "key_play_pause", &c->key_play_pause);
            read_key(key, val, "key_next", &c->key_next);
            read_key(key, val, "key_prev", &c->key_prev);
            read_key(key, val, "key_seek_back", &c->key_seek_back);
            read_key(key, val, "key_seek_forward", &c->key_seek_forward);
            read_key(key, val, "key_volume_down", &c->key_volume_down);
            read_key(key, val, "key_volume_up", &c->key_volume_up);
            read_key(key, val, "key_shuffle", &c->key_shuffle);
            read_key(key, val, "key_repeat", &c->key_repeat);
            if (!strcmp(key, "key_reverse")) {
                read_key(key, val, "key_reverse", &c->key_reverse);
                saw_key_reverse = 1;
            }
            read_key(key, val, "key_queue", &c->key_queue);
            if (!strcmp(key, "key_speed_up")) {
                read_key(key, val, "key_speed_up", &c->key_speed_up);
                saw_speed_up = 1;
            } else if (!strcmp(key, "key_slow_reverb")) {
                read_key(key, val, "key_slow_reverb", &legacy_speed_up);
            }
            if (!strcmp(key, "key_speed_down")) {
                read_key(key, val, "key_speed_down", &c->key_speed_down);
                saw_speed_down = 1;
            } else if (!strcmp(key, "key_more_slow_reverb")) {
                read_key(key, val, "key_more_slow_reverb", &legacy_speed_down);
            }
            read_key(key, val, "key_bass_boost", &c->key_bass_boost);
            read_key(key, val, "key_vinyl", &c->key_vinyl);
            read_key(key, val, "key_rain", &c->key_rain);
            read_key(key, val, "key_fx_reset", &c->key_fx_reset);
            read_key(key, val, "key_retune", &c->key_retune);
            read_key(key, val, "key_search", &c->key_search);
            read_key(key, val, "key_devices", &c->key_devices);
            read_key(key, val, "key_help", &c->key_help);
            read_key(key, val, "key_reload", &c->key_reload);
            read_key(key, val, "key_logout", &c->key_logout);
            read_key(key, val, "key_client_id", &c->key_client_id);
            read_key(key, val, "key_login", &c->key_login);
        }
        free(key);
        free(val);
    }
    fclose(f);
    /* Before reverse mode existed, uppercase R was the reload default.  Old
     * configs should gain the new binding without making the two actions
     * collide; reload moves to the new uppercase-L default. */
    if (!saw_key_reverse && c->key_reload == 'R') c->key_reload = 'L';
    /* Keep customized bindings from configs written before the speed controls
     * were split from the old slow+reverb presets. */
    if (!saw_speed_up && legacy_speed_up) c->key_speed_up = legacy_speed_up;
    if (!saw_speed_down && legacy_speed_down) c->key_speed_down = legacy_speed_down;
    if (c->speed < 0.5f || c->speed > 2.0f) c->speed = 1.0f;
    if (c->reverb < 0.0f || c->reverb > 1.0f) c->reverb = 0.0f;
    if (c->bass < 0.0f || c->bass > 1.0f) c->bass = 0.0f;
    if (c->retune < 0.8f || c->retune > 1.3f) c->retune = 1.0f;
    if (c->vinyl < 0 || c->vinyl > 4) c->vinyl = 0;
    if (c->rain < 0 || c->rain > 3) c->rain = 0;
    if (!c->music_dir[0]) snprintf(c->music_dir, sizeof c->music_dir, "~/Music");
}

void config_save(const Config *c) {
    char path[600];
    snprintf(path, sizeof path, "%s/config", spotc_dir());
    FILE *f = fopen(path, "w");
    if (!f) return;
    char quit[8], focus[8], down[8], up[8], top[8], bottom[8], play[8];
    char next[8], prev[8], back[8], forward[8], vdown[8], vup[8], shuffle[8];
    char repeat[8], reverse[8], queue[8], speed_up[8], speed_down[8], bass[8];
    char reset[8], retune[8], search[8], vinyl[8], rain[8];
    char devices[8], help[8], reload[8], logout[8], client_id[8], login[8];
    fprintf(f,
        "# spotc config\n"
        "client_id = %s   # optional: your own Spotify app id (empty = built-in)\n"
        "device_name = %s\n"
        "accent = %d          # 256-color index for highlights\n"
        "dim = %d             # 256-color index for secondary text\n"
        "bitrate = %d         # 96 / 160 / 320\n"
        "speed = %.2f         # 0.50 - 2.00, 1.00 = normal\n"
        "reverb = %.2f        # 0.00 - 1.00\n"
        "bass = %.2f          # 0.00 = off, 1.00 = bass boost\n"
        "retune = %.4f       # tuning ratio vs A4=440 (0.9818 = 432 Hz), 1.0 = off\n"
        "vinyl = %d           # record sound: 0 off, 1 mint, 2 played, 3 dusty, 4 lo-fi\n"
        "rain = %d            # rain sounds: 0 off, 1 drizzle, 2 rain, 3 storm\n"
        "music_dir = %s     # local files shown under \"Local files\"\n"
        "\n# Keybinds: one printable character, or space/tab/enter/escape.\n"
        "key_quit = %s\nkey_focus = %s\nkey_down = %s\nkey_up = %s\n"
        "key_top = %s\nkey_bottom = %s\nkey_play_pause = %s\n"
        "key_next = %s\nkey_prev = %s\nkey_seek_back = %s\nkey_seek_forward = %s\n"
        "key_volume_down = %s\nkey_volume_up = %s\nkey_shuffle = %s\n"
        "key_repeat = %s\nkey_reverse = %s\nkey_queue = %s\nkey_speed_up = %s\n"
        "key_speed_down = %s\nkey_bass_boost = %s\nkey_vinyl = %s\nkey_rain = %s\nkey_fx_reset = %s\nkey_retune = %s\nkey_search = %s\n"
        "key_devices = %s\nkey_help = %s\nkey_reload = %s\nkey_logout = %s\n"
        "key_client_id = %s\nkey_login = %s\n",
        c->client_id, c->device_name, c->accent, c->dim, c->bitrate,
        c->speed, c->reverb, c->bass, c->retune, c->vinyl, c->rain, c->music_dir,
        spotc_key_name(c->key_quit, quit, sizeof quit), spotc_key_name(c->key_focus, focus, sizeof focus),
        spotc_key_name(c->key_down, down, sizeof down), spotc_key_name(c->key_up, up, sizeof up),
        spotc_key_name(c->key_top, top, sizeof top), spotc_key_name(c->key_bottom, bottom, sizeof bottom),
        spotc_key_name(c->key_play_pause, play, sizeof play), spotc_key_name(c->key_next, next, sizeof next),
        spotc_key_name(c->key_prev, prev, sizeof prev), spotc_key_name(c->key_seek_back, back, sizeof back),
        spotc_key_name(c->key_seek_forward, forward, sizeof forward), spotc_key_name(c->key_volume_down, vdown, sizeof vdown),
        spotc_key_name(c->key_volume_up, vup, sizeof vup), spotc_key_name(c->key_shuffle, shuffle, sizeof shuffle),
        spotc_key_name(c->key_repeat, repeat, sizeof repeat), spotc_key_name(c->key_reverse, reverse, sizeof reverse),
        spotc_key_name(c->key_queue, queue, sizeof queue),
        spotc_key_name(c->key_speed_up, speed_up, sizeof speed_up),
        spotc_key_name(c->key_speed_down, speed_down, sizeof speed_down),
        spotc_key_name(c->key_bass_boost, bass, sizeof bass),
        spotc_key_name(c->key_vinyl, vinyl, sizeof vinyl),
        spotc_key_name(c->key_rain, rain, sizeof rain),
        spotc_key_name(c->key_fx_reset, reset, sizeof reset), spotc_key_name(c->key_retune, retune, sizeof retune),
        spotc_key_name(c->key_search, search, sizeof search),
        spotc_key_name(c->key_devices, devices, sizeof devices), spotc_key_name(c->key_help, help, sizeof help),
        spotc_key_name(c->key_reload, reload, sizeof reload), spotc_key_name(c->key_logout, logout, sizeof logout),
        spotc_key_name(c->key_client_id, client_id, sizeof client_id),
        spotc_key_name(c->key_login, login, sizeof login));
    fclose(f);
}
