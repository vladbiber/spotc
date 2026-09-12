#include "spotc.h"
#include <ncurses.h>
#include <curl/curl.h>
#include <signal.h>

static long long pipeline_started_ms;

static int pipeline_start(App *a) {
    int rc = player_start(a);
    if (rc == 0) pipeline_started_ms = now_ms();
    return rc;
}

static int reverse_position(App *a) {
    int pos = a->reverse_progress_ms;
    if (a->reverse && a->playing) {
        float rate = a->speed * a->retune;
        pos -= (int)((now_ms() - a->state_at_ms) * rate);
    }
    return pos > 0 ? pos : 0;
}

static void latch_reverse_position(App *a) {
    if (!a->reverse) return;
    a->reverse_progress_ms = reverse_position(a);
    a->state_at_ms = now_ms();
}

static int wait_local_device(App *a, char *id, size_t n) {
    for (int tries = 0; tries < 20; tries++) {
        if (api_fetch_devices(a) == 0) {
            for (int i = 0; i < a->n_devices; i++) {
                if (!strcmp(a->devices[i].name, a->cfg.device_name)) {
                    snprintf(id, n, "%s", a->devices[i].id);
                    return 0;
                }
            }
        }
        struct timespec ts = { 0, 250 * 1000000 };
        nanosleep(&ts, NULL);
    }
    return -1;
}

static void clear_reverse_queue(App *a) {
    free(a->reverse_tracks);
    a->reverse_tracks = NULL;
    a->n_reverse_tracks = 0;
    a->reverse_index = -1;
}

/* Freeze the current list when reverse mode starts. Browsing another album or
 * playlist must not silently replace the sequence that is already playing. */
static void capture_reverse_queue(App *a, int selected) {
    clear_reverse_queue(a);
    if (selected < 0 || selected >= a->n_tracks ||
        a->tracks[selected].is_album || strcmp(a->tracks[selected].id, a->now.id)) {
        selected = -1;
        for (int i = 0; i < a->n_tracks; i++)
            if (!a->tracks[i].is_album && !strcmp(a->tracks[i].id, a->now.id)) {
                selected = i;
                break;
            }
    }
    if (selected >= 0) {
        a->reverse_tracks = malloc((size_t)a->n_tracks * sizeof *a->reverse_tracks);
        if (a->reverse_tracks) {
            memcpy(a->reverse_tracks, a->tracks,
                   (size_t)a->n_tracks * sizeof *a->reverse_tracks);
            a->n_reverse_tracks = a->n_tracks;
            a->reverse_index = selected;
            return;
        }
    }
    /* A track started outside the list still gets exact reverse; there simply
     * is no reliable local next item after it. */
    a->reverse_tracks = malloc(sizeof *a->reverse_tracks);
    if (a->reverse_tracks) {
        a->reverse_tracks[0] = a->now;
        a->n_reverse_tracks = 1;
        a->reverse_index = 0;
    }
}

static void leave_reverse(App *a, int target, int play) {
    player_stop(a);
    a->reverse = 0;
    clear_reverse_queue(a);
    a->playing = 0;
    a->progress_ms = target;
    a->state_at_ms = now_ms();
    if (pipeline_start(a) != 0) {
        set_status(a, "reverse stopped — librespot could not restart");
        return;
    }

    set_status(a, "returning to normal playback...");
    ui_draw(a);
    char dev[128];
    if (wait_local_device(a, dev, sizeof dev) != 0 ||
        api_play_uri_at(a, a->now.uri, target, dev) != 0) {
        set_status(a, "reverse stopped — pick the spotc device again (d)");
        return;
    }
    if (!play) api_pause(a);
    a->playing = play;
    a->progress_ms = target;
    a->state_at_ms = now_ms();
    snprintf(a->active_device, sizeof a->active_device, "%s", a->cfg.device_name);
    set_status(a, play ? "reverse off — normal playback resumed"
                       : "reverse off — normal playback paused");
    a->last_poll_ms = now_ms();
}

/* Decode and play a->now from its final PCM frame to its first.  Once reverse
 * is enabled this same path is reused for every selected/adjacent track. */
static int launch_reverse_current(App *a) {
    if (!a->now.uri[0] || !a->now.duration_ms) {
        set_status(a, "this track cannot be played in reverse");
        return -1;
    }
    a->playing = 0;
    a->reverse_progress_ms = a->now.duration_ms;
    a->state_at_ms = now_ms();
    set_status(a, "reverse mode: preparing %s...", a->now.name);
    ui_draw(a);
    char path[800];
    int cached = player_cache_reverse(a, path, sizeof path);
    if (cached < 0) {
        set_status(a, "reverse preparation failed — see ~/.cache/spotc/reverse.log");
        return -1;
    }

    player_stop(a);
    a->reverse = 1;
    a->reverse_progress_ms = a->now.duration_ms;
    a->state_at_ms = now_ms();
    a->playing = 1;
    if (player_start_reverse(a, path) != 0) {
        a->playing = 0;
        set_status(a, "reverse could not start — press R to return to normal");
        return -1;
    }
    set_status(a, cached == 0 ? "reverse mode: end → beginning (from cache)"
                              : "reverse mode: end → beginning");
    return 0;
}

static void start_reverse(App *a) {
    if (!a->have_state || !a->now.uri[0] || !a->now.duration_ms) {
        set_status(a, "start a track before using reverse");
        return;
    }
    if (strcmp(a->active_device, a->cfg.device_name)) {
        set_status(a, "reverse only works on the local '%s' device (d)", a->cfg.device_name);
        return;
    }

    int was_playing = a->playing;
    int forward_pos = a->progress_ms;
    if (was_playing) forward_pos += (int)(now_ms() - a->state_at_ms);
    if (was_playing && api_pause(a) != 0) return;
    a->playing = 0;
    a->progress_ms = forward_pos;
    a->reverse_was_playing = was_playing;
    capture_reverse_queue(a, -1);

    if (launch_reverse_current(a) == 0) return;
    if (!a->reverse) {
        clear_reverse_queue(a);
        if (was_playing && api_resume(a) == 0) {
            a->playing = 1;
            a->state_at_ms = now_ms();
        }
        return;
    }
    a->reverse_progress_ms = forward_pos;
    leave_reverse(a, forward_pos, was_playing);
}

static int reverse_track_index(const App *a) {
    if (a->reverse_index >= 0 && a->reverse_index < a->n_reverse_tracks &&
        !strcmp(a->reverse_tracks[a->reverse_index].id, a->now.id))
        return a->reverse_index;
    for (int i = 0; i < a->n_reverse_tracks; i++)
        if (!a->reverse_tracks[i].is_album &&
            !strcmp(a->reverse_tracks[i].id, a->now.id)) return i;
    return -1;
}

static int adjacent_reverse_track(const App *a, int direction, int automatic) {
    int current = reverse_track_index(a);
    if (current < 0) return -1;
    if (automatic && a->repeat == 2) return current;

    for (int i = current + direction;; i += direction) {
        if (i < 0 || i >= a->n_reverse_tracks) {
            if (a->repeat != 1) return -1;
            i = direction > 0 ? 0 : a->n_reverse_tracks - 1;
        }
        if (i == current) return automatic && a->repeat == 1 ? current : -1;
        if (!a->reverse_tracks[i].is_album && a->reverse_tracks[i].uri[0] &&
            a->reverse_tracks[i].duration_ms > 0) return i;
    }
}

static void change_reverse_track(App *a, int direction, int automatic) {
    int next = adjacent_reverse_track(a, direction, automatic);
    if (next < 0) {
        if (automatic) {
            player_stop(a);
            a->playing = 0;
            a->reverse_progress_ms = direction > 0 ? 0 : a->now.duration_ms;
            a->state_at_ms = now_ms();
        }
        set_status(a, automatic ? "reverse mode: end of list — select a track or press R"
                                : "no more tracks in this direction");
        return;
    }
    player_stop(a);
    a->playing = 0;
    a->reverse_index = next;
    a->now = a->reverse_tracks[next];
    for (int i = 0; i < a->n_tracks; i++)
        if (!a->tracks[i].is_album && !strcmp(a->tracks[i].id, a->now.id)) {
            a->tr_sel = i;
            break;
        }
    a->have_state = 1;
    a->progress_ms = 0;
    a->reverse_progress_ms = a->now.duration_ms;
    a->state_at_ms = now_ms();
    launch_reverse_current(a);
}

/* librespot/fx/pacat exited: restart once it ran long enough that this
 * isn't a crash loop, otherwise leave it down and point at the log */
static void pipeline_died(App *a) {
    if (now_ms() - pipeline_started_ms > 10000 && pipeline_start(a) == 0) {
        set_status(a, "audio pipeline restarted");
        return;
    }
    set_status(a, "librespot exited — see ~/.cache/spotc/librespot.log; reload retries");
}

static void open_sidebar_sel(App *a) {
    set_status(a, "loading...");
    ui_draw(a);
    int rc;
    if (a->pl_sel == 0) rc = api_load_liked(a);
    else if (a->pl_sel <= a->n_albums)
        rc = api_load_album_tracks(a, &a->albums[a->pl_sel - 1]);
    else
        rc = api_load_playlist_tracks(a, &a->playlists[a->pl_sel - 1 - a->n_albums]);
    if (!rc) set_status(a, "");
    if (!rc) a->focus = FOCUS_TRACKS;
}

static void play_selected(App *a) {
    if (a->tr_sel >= a->n_tracks) return;
    Track *t = &a->tracks[a->tr_sel];
    if (t->is_album) {
        Playlist alb = {0};
        snprintf(alb.id, sizeof alb.id, "%s", t->id);
        snprintf(alb.uri, sizeof alb.uri, "%s", t->uri);
        snprintf(alb.name, sizeof alb.name, "%s", t->name);
        set_status(a, "loading...");
        ui_draw(a);
        if (!api_load_album_tracks(a, &alb)) set_status(a, "");
        return;
    }
    if (a->reverse) {
        player_stop(a);
        a->tr_sel = (int)(t - a->tracks);
        a->now = *t;
        capture_reverse_queue(a, a->tr_sel);
        a->have_state = 1;
        a->progress_ms = 0;
        a->reverse_progress_ms = t->duration_ms;
        a->state_at_ms = now_ms();
        launch_reverse_current(a);
        return;
    }
    /* no active device? play straight on the local spotc device */
    const char *dev = NULL;
    if (!a->have_state || !a->active_device[0]) {
        api_fetch_devices(a);
        for (int i = 0; i < a->n_devices; i++)
            if (!strcmp(a->devices[i].name, a->cfg.device_name)) {
                dev = a->devices[i].id;
                break;
            }
    }
    int rc = a->tracks_ctx[0] ? api_play_context(a, a->tracks_ctx, a->tr_sel, dev)
                              : api_play_tracks(a, a->tracks, a->n_tracks, a->tr_sel, dev);
    if (!rc) {
        set_status(a, "");
        if (dev)
            snprintf(a->active_device, sizeof a->active_device, "%s", a->cfg.device_name);
        a->playing = 1;
        a->progress_ms = 0;
        a->now = *t;
        a->state_at_ms = now_ms();
        a->have_state = 1;
        a->last_poll_ms = now_ms() - 1000;   /* re-poll soon for real state */
    }
}

static void do_search(App *a) {
    char q[256] = {0};
    if (ui_prompt(" search › ", q, sizeof q) == 0) {
        set_status(a, "searching...");
        ui_draw(a);
        if (!api_search(a, q)) set_status(a, "");
        a->focus = FOCUS_TRACKS;
    }
}

static void set_fx(App *a, float speed, float reverb, float bass) {
    latch_reverse_position(a);
    a->speed = speed;
    a->reverb = reverb;
    a->bass = bass;
    player_set_fx(a);
    a->cfg.speed = a->speed;
    a->cfg.reverb = a->reverb;
    a->cfg.bass = a->bass;
    config_save(&a->cfg);
    if (!a->pipeline_up || (!a->reverse && strcmp(a->active_device, a->cfg.device_name)))
        set_status(a, "local effects apply when playing on '%s' (d)", a->cfg.device_name);
}

static void set_retune(App *a, int preset) {
    const RetunePreset *p = &retune_presets[preset];
    latch_reverse_position(a);
    /* A speed change shifts pitch, so a named retune preset always returns
     * playback to 1.0x. Bass remains independent. */
    int had_speed = a->speed < 0.999f || a->speed > 1.001f;
    a->retune = p->ratio;
    a->cfg.retune = a->retune;
    if (preset != 0) {
        a->speed = 1.0f;
        a->reverb = p->reverb;   /* deep presets bring their own reverb */
        a->cfg.speed = a->speed;
        a->cfg.reverb = a->reverb;
    }
    player_set_fx(a);
    config_save(&a->cfg);
    if (!a->pipeline_up || strcmp(a->active_device, a->cfg.device_name)) {
        set_status(a, "retune applies when playing on '%s' (d)", a->cfg.device_name);
        return;
    }
    if (preset == 0)
        set_status(a, "retune off — standard 440 Hz tuning");
    else if (p->reverb > 0.001f)
        set_status(a, "retune %s = %d Hz (%.3fx) + reverb %d%%",
                   p->note, p->hz, a->retune, (int)(p->reverb * 100));
    else
        set_status(a, "retune %s = %d Hz (%.3fx)%s", p->note, p->hz, a->retune,
                   had_speed ? "  ·  speed reset to 1.0x" : "");
}

static int key_is(int ch, int configured) {
    return configured > 0 && ch == configured;
}

static void step_speed(App *a, int direction) {
    static const float speeds[] = {
        0.50f, 0.60f, 0.70f, 0.80f, 0.90f, 1.00f,
        1.10f, 1.20f, 1.35f, 1.50f, 1.75f, 2.00f
    };
    enum { SPEEDS = (int)(sizeof speeds / sizeof *speeds) };
    latch_reverse_position(a);
    int had_retune = a->retune < 0.999f || a->retune > 1.001f;
    if (had_retune) {
        a->retune = 1.0f;
        a->cfg.retune = 1.0f;
    }

    float target = a->speed;
    if (direction > 0) {
        for (int i = 0; i < SPEEDS; i++)
            if (speeds[i] > a->speed + 0.005f) { target = speeds[i]; break; }
    } else {
        for (int i = SPEEDS - 1; i >= 0; i--)
            if (speeds[i] < a->speed - 0.005f) { target = speeds[i]; break; }
    }
    set_fx(a, target, a->reverb, a->bass);
    if (a->pipeline_up && (a->reverse || !strcmp(a->active_device, a->cfg.device_name))) {
        const char *limit = direction > 0 && target >= 1.999f ? " (maximum)" :
                            direction < 0 && target <= 0.501f ? " (minimum)" : "";
        set_status(a, "speed %.2fx%s%s", target, limit,
                   had_retune ? "  ·  retune off" : "");
    }
}

static void toggle_bass(App *a) {
    float bass = a->bass > 0.001f ? 0.0f : 1.0f;
    set_fx(a, a->speed, a->reverb, bass);
    if (a->pipeline_up && (a->reverse || !strcmp(a->active_device, a->cfg.device_name)))
        set_status(a, bass > 0.0f ? "bass boost on" : "bass boost off");
}

static void logout(App *a) {
    player_stop(a);
    clear_reverse_queue(a);
    auth_logout(a);
    a->have_state = a->playing = 0;
    a->reverse = 0;
    a->active_device[0] = 0;
    a->screen = SCREEN_LOGIN;
    if (pipeline_start(a) != 0)
        set_status(a, "logged out — librespot missing");
    else
        set_status(a, "logged out — press l to sign in again");
}

static void request_volume(App *a, int delta) {
    int base = a->volume_target >= 0 ? a->volume_target : a->volume;
    int target = base + delta;
    if (target < 0) target = 0;
    if (target > 100) target = 100;
    if (a->reverse) {
        a->volume = target;
        a->volume_target = -1;
        player_set_fx(a);
        return;
    }
    a->volume = target;              /* never let a stale poll move the UI */
    a->volume_target = target;
    a->volume_send_at_ms = now_ms() + 140;  /* coalesce rapid +/- input */
    a->volume_hold_until_ms = now_ms() + 3000;
}

static void flush_volume(App *a) {
    if (a->volume_target < 0 || !a->volume_send_at_ms ||
        now_ms() < a->volume_send_at_ms) return;
    int target = a->volume_target;
    a->volume_send_at_ms = 0;
    if (api_set_volume(a, target) != 0) {
        a->volume_target = -1;
        return;
    }
    a->volume = target;
    a->volume_hold_until_ms = now_ms() + 3000;
    a->last_poll_ms = now_ms() - 1000;
}

static void seek_rel(App *a, int delta_ms) {
    if (!a->have_state || !a->now.duration_ms) return;
    if (a->reverse) {
        set_status(a, "seek is unavailable during exact reverse; R returns to normal");
        return;
    }
    int prog = a->progress_ms;
    if (a->playing) prog += (int)(now_ms() - a->state_at_ms);
    int target = prog + delta_ms;
    if (target > a->now.duration_ms) target = a->now.duration_ms - 1000;
    if (api_seek(a, target) == 0) {
        a->progress_ms = target < 0 ? 0 : target;
        a->state_at_ms = now_ms();
    }
}

static void handle_popup_key(App *a, int ch) {
    if (ch == 27 || key_is(ch, a->cfg.key_quit) || key_is(ch, a->cfg.key_devices) ||
        key_is(ch, a->cfg.key_help)) { a->popup = POPUP_NONE; return; }
    if (a->popup == POPUP_RETUNE) {
        if (ch >= '0' && ch < '0' + RETUNE_PRESETS) {
            set_retune(a, ch - '0');
            a->popup = POPUP_NONE;
        } else if (key_is(ch, a->cfg.key_retune)) {
            a->popup = POPUP_NONE;
        }
        return;
    }
    if (a->popup != POPUP_DEVICES) return;
    if (key_is(ch, a->cfg.key_down) || ch == KEY_DOWN) {
        if (a->dev_sel < a->n_devices - 1) a->dev_sel++;
    } else if (key_is(ch, a->cfg.key_up) || ch == KEY_UP) {
        if (a->dev_sel > 0) a->dev_sel--;
    } else if (ch == '\n' || ch == KEY_ENTER || key_is(ch, a->cfg.key_play_pause)) {
        if (a->dev_sel < a->n_devices) {
            if (api_transfer(a, a->devices[a->dev_sel].id) == 0)
                set_status(a, "playing on %s", a->devices[a->dev_sel].name);
            a->popup = POPUP_NONE;
            a->last_poll_ms = now_ms() - 1500;
        }
    }
}

static void handle_key(App *a, int ch) {
    if (a->popup != POPUP_NONE) { handle_popup_key(a, ch); return; }

    if (key_is(ch, a->cfg.key_quit)) { a->running = 0; return; }
    if (key_is(ch, a->cfg.key_focus) || ch == KEY_LEFT || ch == KEY_RIGHT) {
        a->focus = a->focus == FOCUS_SIDEBAR ? FOCUS_TRACKS : FOCUS_SIDEBAR;
    } else if (key_is(ch, a->cfg.key_down) || ch == KEY_DOWN) {
        if (a->focus == FOCUS_SIDEBAR) {
            if (a->pl_sel < sidebar_total(a) - 1) a->pl_sel++;
        } else if (a->tr_sel < a->n_tracks - 1) a->tr_sel++;
    } else if (key_is(ch, a->cfg.key_up) || ch == KEY_UP) {
        if (a->focus == FOCUS_SIDEBAR) {
            if (a->pl_sel > 0) a->pl_sel--;
        } else if (a->tr_sel > 0) a->tr_sel--;
    } else if (ch == KEY_NPAGE || ch == KEY_PPAGE) {
        if (a->focus == FOCUS_TRACKS) {
            int page = LINES - 8;   /* visible track rows */
            if (page < 1) page = 1;
            a->tr_sel += ch == KEY_NPAGE ? page : -page;
            if (a->tr_sel >= a->n_tracks) a->tr_sel = a->n_tracks - 1;
            if (a->tr_sel < 0) a->tr_sel = 0;
        }
    } else if (key_is(ch, a->cfg.key_top)) {
        if (a->focus == FOCUS_TRACKS) a->tr_sel = 0; else a->pl_sel = 0;
    } else if (key_is(ch, a->cfg.key_bottom)) {
        if (a->focus == FOCUS_TRACKS && a->n_tracks) a->tr_sel = a->n_tracks - 1;
        else a->pl_sel = sidebar_total(a) - 1;
    } else if (key_is(ch, a->cfg.key_play_pause)) {
        if (a->reverse) {
            if (!a->pipeline_up) {
                launch_reverse_current(a);
            } else {
                latch_reverse_position(a);
                a->playing = !a->playing;
                player_reverse_pause(a, !a->playing);
                a->state_at_ms = now_ms();
            }
        } else if (a->playing) { if (!api_pause(a)) { a->playing = 0;
            a->progress_ms += (int)(now_ms() - a->state_at_ms); } }
        else if (!api_resume(a)) { a->playing = 1; a->state_at_ms = now_ms(); }
    } else if (ch == '\n' || ch == KEY_ENTER) {
        if (a->focus == FOCUS_SIDEBAR) open_sidebar_sel(a);
        else play_selected(a);
    } else if (key_is(ch, a->cfg.key_next)) {
        if (a->reverse) {
            change_reverse_track(a, +1, 0);
        } else if (!api_next(a)) {
            a->last_poll_ms = now_ms() - 1500;
        }
    } else if (key_is(ch, a->cfg.key_prev)) {
        if (a->reverse) {
            change_reverse_track(a, -1, 0);
        } else if (!api_prev(a)) {
            a->last_poll_ms = now_ms() - 1500;
        }
    } else if (key_is(ch, a->cfg.key_seek_back)) {
        seek_rel(a, -10000);
    } else if (key_is(ch, a->cfg.key_seek_forward)) {
        seek_rel(a, 10000);
    } else if (key_is(ch, a->cfg.key_volume_down)) {
        request_volume(a, -5);
    } else if (key_is(ch, a->cfg.key_volume_up)) {
        request_volume(a, +5);
    } else if (key_is(ch, a->cfg.key_shuffle)) {
        if (!api_set_shuffle(a, !a->shuffle)) a->shuffle = !a->shuffle;
    } else if (key_is(ch, a->cfg.key_repeat)) {
        if (!api_set_repeat(a, (a->repeat + 1) % 3)) a->repeat = (a->repeat + 1) % 3;
    } else if (key_is(ch, a->cfg.key_reverse)) {
        if (a->reverse)
            leave_reverse(a, reverse_position(a), a->reverse_was_playing);
        else
            start_reverse(a);
    } else if (key_is(ch, a->cfg.key_queue)) {
        if (a->focus == FOCUS_TRACKS && a->tr_sel < a->n_tracks) {
            if (!api_queue_add(a, a->tracks[a->tr_sel].uri))
                set_status(a, "queued: %s", a->tracks[a->tr_sel].name);
        }
    } else if (key_is(ch, a->cfg.key_speed_up)) {
        step_speed(a, +1);
    } else if (key_is(ch, a->cfg.key_speed_down)) {
        step_speed(a, -1);
    } else if (key_is(ch, a->cfg.key_bass_boost)) {
        toggle_bass(a);
    } else if (key_is(ch, a->cfg.key_fx_reset)) {
        latch_reverse_position(a);
        a->retune = 1.0f;
        a->cfg.retune = 1.0f;
        set_fx(a, 1.0f, 0.0f, 0.0f);
        if (a->pipeline_up && (a->reverse || !strcmp(a->active_device, a->cfg.device_name)))
            set_status(a, "local effects reset");
    } else if (key_is(ch, a->cfg.key_retune)) {
        a->popup = POPUP_RETUNE;
    } else if (key_is(ch, a->cfg.key_search)) {
        do_search(a);
    } else if (key_is(ch, a->cfg.key_devices)) {
        api_fetch_devices(a);
        a->popup = POPUP_DEVICES;
    } else if (key_is(ch, a->cfg.key_help)) {
        a->popup = POPUP_HELP;
    } else if (key_is(ch, a->cfg.key_reload)) {
        int pipeline_ok = a->pipeline_up || pipeline_start(a) == 0;
        int playlists_ok = api_load_playlists(a) == 0;
        int albums_ok = api_load_albums(a) == 0;
        if (playlists_ok && albums_ok)
            set_status(a, pipeline_ok ? "reloaded" : "reloaded — librespot still missing");
    } else if (key_is(ch, a->cfg.key_logout)) {
        logout(a);
    }
}

static void enter_main(App *a) {
    a->screen = SCREEN_MAIN;
    set_status(a, "loading library...");
    ui_draw(a);
    int playlists_ok = api_load_playlists(a) == 0;
    int albums_ok = api_load_albums(a) == 0;
    int state_ok = api_fetch_state(a) == 0;
    if (playlists_ok && albums_ok && state_ok) set_status(a, "");
}

int main(void) {
    App a = {0};
    a.running = 1;
    a.volume_target = -1;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    config_load(&a.cfg);
    a.speed = a.cfg.speed;
    a.reverb = a.cfg.reverb;
    a.bass = a.cfg.bass;
    a.retune = a.cfg.retune;
    signal(SIGPIPE, SIG_IGN);

    /* stale tokens minted with a different client id must not short-circuit login */
    int logged = a.cfg.client_id[0] && tokens_load(&a.tok) == 0
                 && strcmp(a.tok.cid, a.cfg.client_id) == 0;
    if (!logged) memset(&a.tok, 0, sizeof a.tok);
    a.screen = logged ? SCREEN_MAIN : SCREEN_LOGIN;

    ui_init();

    if (pipeline_start(&a) != 0)
        set_status(&a, "librespot missing — remote control only");
    if (a.screen == SCREEN_MAIN)
        enter_main(&a);

    while (a.running) {
        ui_draw(&a);
        int ch = getch();

        if (a.screen == SCREEN_LOGIN) {
            if (key_is(ch, a.cfg.key_quit)) break;
            if (key_is(ch, a.cfg.key_client_id)) {
                char id[128] = {0};
                if (ui_prompt(" Client ID › ", id, sizeof id) == 0) {
                    char *trimmed = xstrdup_trim(id);
                    snprintf(a.cfg.client_id, sizeof a.cfg.client_id, "%s", trimmed);
                    free(trimmed);
                    config_save(&a.cfg);
                    set_status(&a, "saved — now press l to sign in");
                }
            } else if (key_is(ch, a.cfg.key_login)) {
                if (!a.cfg.client_id[0]) {
                    set_status(&a, "press i first and paste your Client ID");
                } else {
                    set_status(&a, "browser opened — finish signing in there...");
                    ui_draw(&a);
                    if (auth_login(&a) == 0) enter_main(&a);
                    else set_status(&a, "sign-in failed — check the Redirect URI, then press l");
                }
            }
            continue;
        }

        if (ch != ERR) handle_key(&a, ch);

        if (player_check(&a)) {
            if (a.reverse) change_reverse_track(&a, +1, 1);
            else pipeline_died(&a);
        }
        flush_volume(&a);

        if (!a.reverse && now_ms() - a.last_poll_ms > 2500)
            api_fetch_state(&a);
    }

    ui_shutdown();
    player_stop(&a);
    clear_reverse_queue(&a);
    curl_global_cleanup();
    return 0;
}
