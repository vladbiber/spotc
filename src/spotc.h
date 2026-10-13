#ifndef SPOTC_H
#define SPOTC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <json-c/json.h>

#define SPOTC_VERSION "0.3"
#define REDIRECT_PORT 5588
#define REDIRECT_URI "http://127.0.0.1:5588/login"
/* Spotify desktop client id (same one librespot/spotifyd use for OAuth);
 * config client_id overrides it, in which case that app must have
 * REDIRECT_URI registered */
#define DEFAULT_CLIENT_ID "65b708073fc0480ea92a077233ca87bd"

typedef struct {
    char client_id[128];
    char device_name[64];
    int accent;      /* 256-color index */
    int dim;
    int bitrate;     /* 96 / 160 / 320 */
    float speed;     /* persisted fx defaults */
    float reverb;
    float bass;      /* 0.0 = off, 1.0 = full bass boost */
    float retune;    /* tuning ratio, 1.0 = A4 440 Hz */
    int vinyl;       /* record preset: 0 off, 1 mint, 2 played, 3 dusty, 4 lo-fi */
    int rain;        /* rain preset: 0 off, 1 drizzle, 2 rain, 3 storm */
    char music_dir[512]; /* local files root, ~ expanded on use */

    /* All textual controls can be changed in ~/.config/spotc/config. */
    int key_quit, key_focus, key_down, key_up, key_top, key_bottom;
    int key_play_pause, key_next, key_prev, key_seek_back, key_seek_forward;
    int key_volume_down, key_volume_up, key_shuffle, key_repeat, key_reverse, key_queue;
    int key_speed_up, key_speed_down, key_bass_boost, key_fx_reset, key_retune, key_vinyl, key_rain;
    int key_search, key_devices, key_help, key_reload, key_logout;
    int key_client_id, key_login;
} Config;

typedef struct {
    char access[512];
    char refresh[512];
    char cid[130];        /* client id the tokens were minted with */
    time_t expires_at;
} Tokens;

typedef struct {
    char id[64];
    char uri[128];
    char name[192];
    int tracks_total;
} Playlist;

typedef struct {
    char id[64];
    char uri[128];
    char name[192];
    char artist[192];
    char album[192];
    int duration_ms;
    int is_album;       /* search result row that opens an album */
    char path[512];     /* local file; empty for Spotify tracks */
} Track;

typedef struct {
    char id[128];
    char name[96];
    char type[32];
    int active;
    int volume;
} Device;

enum { FOCUS_SIDEBAR, FOCUS_TRACKS };
enum { POPUP_NONE, POPUP_DEVICES, POPUP_HELP, POPUP_RETUNE };
enum { SCREEN_LOGIN, SCREEN_MAIN };
enum { TRACKS_NONE, TRACKS_PLAYLIST, TRACKS_LIKED, TRACKS_SEARCH, TRACKS_LOCAL };

typedef struct App {
    Config cfg;
    Tokens tok;
    int screen;
    int focus;
    int popup;
    int running;

    Playlist *playlists;
    int n_playlists;
    Playlist *albums;
    int n_albums;
    int pl_sel, pl_off;          /* sidebar: 0 = Local, 1 = Liked, then albums, playlists */

    Track *tracks;
    int n_tracks;
    int tr_sel, tr_off;
    int tracks_kind;
    char tracks_title[192];
    char tracks_ctx[128];        /* context uri for playback */

    Device *devices;
    int n_devices;
    int dev_sel;

    /* player state (from API, interpolated locally) */
    int have_state;
    int playing;
    int progress_ms;
    Track now;
    int volume;
    int volume_target;            /* desired volume while Spotify catches up */
    int shuffle;
    int repeat;                  /* 0 off, 1 context, 2 track */
    char active_device[96];
    long long state_at_ms;       /* monotonic ms when state was fetched */
    long long last_poll_ms;
    long last_api_http;          /* status of the most recent Web API call */
    long long volume_send_at_ms;  /* debounce rapid +/- presses */
    long long volume_hold_until_ms;

    /* local playback pipeline */
    int pipeline_up;
    float speed;
    float reverb;
    float bass;
    float retune;    /* multiplies speed in the fx chain, never shown as "slow" */
    int vinyl;
    int rain;
    int reverse;     /* continuous exact-reverse mode is enabled */
    int reverse_was_playing;
    int reverse_progress_ms;
    Track *reverse_tracks;  /* snapshot of the list whose tracks are reversing */
    int n_reverse_tracks;
    int reverse_index;

    /* local files */
    int local;              /* ffmpeg | spotc-fx | pacat is playing a->now.path */
    Track *local_lib;       /* scanned music_dir, NULL = not scanned yet */
    int n_local_lib;
    Track *local_queue;     /* list the playing local track came from */
    int n_local_queue;
    int local_index;
    long long local_started_ms;

    char status[256];
} App;

/* "magic Hz" presets: the nearest note is retuned onto the target. The deep
 * presets also lower A4 and add reverb. Index 0 = off. */
typedef struct {
    int hz;
    const char *note;
    float ratio;
    float reverb;
    const char *claim;
} RetunePreset;
#define RETUNE_PRESETS 10
extern const RetunePreset retune_presets[RETUNE_PRESETS];

static inline int sidebar_total(const App *a) {
    return 2 + a->n_albums + a->n_playlists;
}

/* util */
long long now_ms(void);
void set_status(App *a, const char *fmt, ...);
char *xstrdup_trim(const char *s);
void ms_to_clock(int ms, char *out, size_t n); /* m:ss */

/* config.c */
const char *spotc_key_name(int key, char *buf, size_t n); /* "space", "g", ... */
void config_load(Config *c);
void config_save(const Config *c);
const char *spotc_dir(void);   /* ~/.config/spotc, created */
const char *cache_dir(void);   /* ~/.cache/spotc, created */

/* auth.c */
int tokens_load(Tokens *t);
void tokens_save(const Tokens *t);
int auth_login(App *a);              /* PKCE browser flow (custom client_id only) */
int auth_ensure(App *a);             /* refresh/mint if near expiry; 0 = have valid token */
int auth_have_creds(App *a);         /* librespot session credentials cached? */
void auth_logout(App *a);            /* forget Web API and librespot credentials */

/* api.c: all return 0 on success unless noted */
json_object *api_get(App *a, const char *path);
int api_send(App *a, const char *method, const char *path, const char *body);
int api_load_playlists(App *a);
int api_load_albums(App *a);
int api_load_playlist_tracks(App *a, const Playlist *p);
int api_load_album_tracks(App *a, const Playlist *alb);
int api_load_liked(App *a);
int api_search(App *a, const char *query);
int api_fetch_state(App *a);
int api_fetch_devices(App *a);
int api_transfer(App *a, const char *device_id);
int api_play_context(App *a, const char *ctx_uri, int offset, const char *dev_id);
int api_play_uri(App *a, const char *uri, const char *dev_id);
int api_play_uri_at(App *a, const char *uri, int position_ms, const char *dev_id);
int api_play_tracks(App *a, const Track *tracks, int n_tracks, int start,
                    const char *dev_id);
int api_pause(App *a);
int api_resume(App *a);
int api_next(App *a);
int api_prev(App *a);
int api_seek(App *a, int pos_ms);
int api_set_volume(App *a, int vol);
int api_set_shuffle(App *a, int on);
int api_set_repeat(App *a, int mode);
int api_queue_add(App *a, const char *uri);

/* local.c */
int local_scan(App *a);              /* fill a->local_lib from music_dir */
void local_hash(const char *path, char *hex, size_t n);

/* player.c */
int player_start(App *a);            /* spawn librespot | spotc-fx | pacat */
int player_cache_reverse(App *a, char *path, size_t n); /* decode current track */
int player_start_reverse(App *a, const char *path);     /* reverse file | pacat */
void player_reverse_pause(App *a, int paused);
int player_start_local(App *a, const char *path, int start_ms); /* ffmpeg | fx | pacat */
int player_oauth_login(App *a);      /* librespot -j browser sign-in; 0 on success */
int player_check(App *a);            /* reap children; 1 = pipeline died */
void player_stop(App *a);
void player_set_fx(App *a);          /* push speed/reverb/bass to spotc-fx */
const char *player_librespot_path(void);

/* ui.c */
void ui_init(void);
void ui_shutdown(void);
void ui_draw(App *a);
int ui_prompt(const char *label, char *out, size_t n); /* 0 = ok, -1 = cancel */
void ui_message(const char *msg);

#endif
