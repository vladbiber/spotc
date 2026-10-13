#include "spotc.h"
#include <ncurses.h>
#include <locale.h>
#include <wchar.h>

enum { CP_ACCENT = 1, CP_DIM, CP_SEL, CP_TEXT };

void ui_init(void) {
    setlocale(LC_ALL, "");
    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    wtimeout(stdscr, 100);
    start_color();
    use_default_colors();
}

static void apply_colors(const Config *c) {
    int accent = (COLORS >= 256) ? c->accent : COLOR_MAGENTA;
    int dim = (COLORS >= 256) ? c->dim : COLOR_WHITE;
    init_pair(CP_ACCENT, accent, -1);
    init_pair(CP_DIM, dim, -1);
    init_pair(CP_SEL, COLOR_BLACK, accent);
    init_pair(CP_TEXT, -1, -1);
}

void ui_shutdown(void) {
    endwin();
}

/* copy s into out truncated to max display columns, ellipsis when cut */
static void trunc_cols(const char *s, int max, char *out, size_t outn) {
    wchar_t w[512];
    size_t n = mbstowcs(w, s, 511);
    if (n == (size_t)-1) { snprintf(out, outn, "%.*s", max, s); return; }
    w[n] = 0;
    int cols = 0;
    size_t i = 0;
    for (; i < n; i++) {
        int cw = wcwidth(w[i]);
        if (cw < 0) cw = 1;
        if (cols + cw > max) break;
        cols += cw;
    }
    if (i < n && i > 0) { w[i - 1] = L'…'; w[i] = 0; }
    else w[i] = 0;
    wcstombs(out, w, outn - 1);
    out[outn - 1] = 0;
}

/* display columns of a UTF-8 string (strlen over-counts multibyte chars) */
static int disp_cols(const char *s) {
    wchar_t w[512];
    size_t n = mbstowcs(w, s, 511);
    if (n == (size_t)-1) return (int)strlen(s);
    int cols = 0;
    for (size_t i = 0; i < n; i++) {
        int cw = wcwidth(w[i]);
        cols += cw < 0 ? 1 : cw;
    }
    return cols;
}

static void putline(int y, int x, int maxc, const char *s, int cp, int attr) {
    char buf[1024];
    trunc_cols(s, maxc, buf, sizeof buf);
    attron(COLOR_PAIR(cp) | attr);
    mvaddstr(y, x, buf);
    attroff(COLOR_PAIR(cp) | attr);
}

static void hline_str(int y, int x, int w, const char *ch, int cp) {
    attron(COLOR_PAIR(cp));
    move(y, x);
    for (int i = 0; i < w; i++) addstr(ch);
    attroff(COLOR_PAIR(cp));
}

static void sidebar_entry(App *a, int idx, char *out, size_t n) {
    if (idx == 0) snprintf(out, n, "♫ Local files");
    else if (idx == 1) snprintf(out, n, "♥ Liked Songs");
    else if (idx <= 1 + a->n_albums) snprintf(out, n, "⊚ %s", a->albums[idx - 2].name);
    else snprintf(out, n, "≡ %s", a->playlists[idx - 2 - a->n_albums].name);
}

static void draw_sidebar(App *a, int w, int h) {
    int rows = h - 2;
    int total = sidebar_total(a);
    if (a->pl_sel < a->pl_off) a->pl_off = a->pl_sel;
    if (a->pl_sel - a->pl_off > rows - 1) a->pl_off = a->pl_sel - rows + 1;
    for (int r = 0; r < rows && a->pl_off + r < total; r++) {
        int idx = a->pl_off + r;
        char name[256];
        sidebar_entry(a, idx, name, sizeof name);
        int sel = (idx == a->pl_sel);
        putline(1 + r, 2, w - 3, name,
                sel && a->focus == FOCUS_SIDEBAR ? CP_SEL : sel ? CP_ACCENT : CP_TEXT, 0);
    }
    for (int y = 1; y < h - 1; y++)
        putline(y, w, 1, "│", CP_DIM, 0);
}

static void draw_tracks(App *a, int x0, int w, int h) {
    char head[256];
    if (a->tracks_kind == TRACKS_NONE)
        snprintf(head, sizeof head, "pick a playlist  ·  / to search");
    else
        snprintf(head, sizeof head, "%s  ·  %d tracks", a->tracks_title, a->n_tracks);
    putline(1, x0 + 1, w - 2, head, CP_DIM, A_BOLD);

    int rows = h - 3;
    int list_y = 2;
    if (a->tr_sel < a->tr_off) a->tr_off = a->tr_sel;
    if (a->tr_sel - a->tr_off > rows - 2) a->tr_off = a->tr_sel - rows + 2;
    int dur_w = 6, idx_w = 4;
    int name_w = (w - idx_w - dur_w - 6) * 3 / 5;
    int art_w = w - idx_w - dur_w - 6 - name_w;

    for (int r = 0; r < rows - 1 && a->tr_off + r < a->n_tracks; r++) {
        Track *t = &a->tracks[a->tr_off + r];
        int sel = (a->tr_off + r == a->tr_sel);
        int is_now = a->now.id[0] && !strcmp(t->id, a->now.id);
        char dur[16], name[512], art[512];
        ms_to_clock(t->duration_ms, dur, sizeof dur);
        trunc_cols(t->name, name_w, name, sizeof name);
        trunc_cols(t->artist, art_w, art, sizeof art);
        char line[1200];
        if (t->is_album)
            snprintf(line, sizeof line, "  ⊚  %-*s  %-*s album",
                     name_w, name, art_w, art);
        else
            snprintf(line, sizeof line, "%3d  %-*s  %-*s %5s",
                     a->tr_off + r + 1, name_w, name, art_w, art, dur);
        int cp = sel && a->focus == FOCUS_TRACKS ? CP_SEL : is_now ? CP_ACCENT : CP_TEXT;
        putline(list_y + r, x0 + 1, w - 2, line, cp, is_now ? A_BOLD : 0);
    }
    if (a->tracks_kind != TRACKS_NONE && a->n_tracks == 0)
        putline(list_y + 1, x0 + 2, w - 3, "nothing here", CP_DIM, 0);
}

static void draw_bottom(App *a) {
    int y = LINES - 4;
    hline_str(y, 0, COLS, "─", CP_DIM);

    char left[512], right[64], cur[16], tot[16];
    if (a->have_state && a->now.name[0]) {
        const char *icon = !a->playing ? "⏸" : a->reverse ? "◀" : "▶";
        snprintf(left, sizeof left, " %s %s — %s", icon,
                 a->now.name, a->now.artist);
        int prog;
        if (a->reverse) {
            prog = a->reverse_progress_ms;
            if (a->playing)
                prog -= (int)((now_ms() - a->state_at_ms) * a->speed * a->retune);
        } else {
            prog = a->progress_ms;
            float rate = a->local ? a->speed * a->retune : 1.0f;
            if (a->playing) prog += (int)((now_ms() - a->state_at_ms) * rate);
        }
        if (prog < 0) prog = 0;
        if (prog > a->now.duration_ms) prog = a->now.duration_ms;
        ms_to_clock(prog, cur, sizeof cur);
        ms_to_clock(a->now.duration_ms, tot, sizeof tot);
        snprintf(right, sizeof right, "%s / %s ", cur, tot);
        putline(y + 1, 0, COLS - 14, left, CP_TEXT, A_BOLD);
        putline(y + 1, COLS - disp_cols(right) - 1, 14, right, CP_DIM, 0);

        int bw = COLS - 2;
        float f = a->now.duration_ms ? (float)prog / a->now.duration_ms : 0;
        int fill = (int)(f * bw);
        if (fill >= bw) fill = bw - 1;
        if (fill < 0) fill = 0;
        move(y + 2, 1);
        attron(COLOR_PAIR(CP_ACCENT));
        for (int i = 0; i < fill; i++) addstr("━");
        addstr("●");
        attroff(COLOR_PAIR(CP_ACCENT));
        attron(COLOR_PAIR(CP_DIM));
        for (int i = fill + 1; i < bw; i++) addstr("─");
        attroff(COLOR_PAIR(CP_DIM));
    } else {
        putline(y + 1, 1, COLS - 2, "nothing playing — Enter on a track, d = devices",
                CP_DIM, 0);
    }

    char fx[256];
    static const char *rep_ind[] = { "", "  ⟳", "  ⟳¹" };
    /* Retune label is the preset's nominal target; the parenthesized value is
     * where that note lands if a hand-edited config also sets another speed. */
    char tune[48] = "";
    if (a->retune < 0.999f || a->retune > 1.001f) {
        int lbl = 0;
        for (int i = 1; i < RETUNE_PRESETS; i++)
            if (a->retune > retune_presets[i].ratio - 0.0005f &&
                a->retune < retune_presets[i].ratio + 0.0005f) {
                lbl = retune_presets[i].hz;
                break;
            }
        if (!lbl) lbl = (int)(440.0f * a->retune + 0.5f);  /* hand-edited config */
        int real = (int)(lbl * a->speed + 0.5f);
        if (real != lbl)
            snprintf(tune, sizeof tune, "  ·  ret %d Hz (now≈%d)", lbl, real);
        else
            snprintf(tune, sizeof tune, "  ·  ret %d Hz", lbl);
    }
    /* The x shown is the real playback rate: speed and retune multiplied. */
    float rate = a->speed * a->retune;
    const char *speed_state = rate < 0.999f ? " slow" : rate > 1.001f ? " fast" : "";
    static const char *const vinyl_ind[] = { "", "  ·  vinyl mint", "  ·  vinyl played",
                                             "  ·  vinyl dusty", "  ·  vinyl lo-fi" };
    static const char *const rain_ind[] = { "", "  ·  drizzle", "  ·  rain", "  ·  storm" };
    snprintf(fx, sizeof fx, " vol %d%%  ·  %.2fx%s%s  ·  rev %d%%%s%s%s%s%s%s%s",
             a->volume, rate, speed_state, tune,
             (int)(a->reverb * 100),
             a->bass > 0.001f ? "  ·  bass boost" : "",
             vinyl_ind[a->vinyl >= 0 && a->vinyl < 5 ? a->vinyl : 0],
             rain_ind[a->rain >= 0 && a->rain < 4 ? a->rain : 0],
             a->shuffle ? "  ⤨" : "", rep_ind[a->repeat],
             a->reverse ? "  ·  ↶ reverse mode" : a->local ? "  ·  local file" : "",
             a->pipeline_up || a->local ? "" : "  ·  fx need the spotc device");
    putline(y + 3, 0, COLS * 2 / 3, fx, CP_DIM, 0);
    if (a->status[0]) {
        int sw = COLS / 3 - 1;
        char st[300];
        trunc_cols(a->status, sw, st, sizeof st);
        putline(y + 3, COLS - disp_cols(st) - 1, sw, st, CP_ACCENT, 0);
    } else {
        putline(y + 3, COLS - 8, 7, "? keys", CP_DIM, 0);
    }
}

/* configured-key name, rotating buffers so several fit in one snprintf */
static const char *kn(int key) {
    static char bufs[16][12];
    static int i;
    i = (i + 1) % 16;
    return spotc_key_name(key, bufs[i], sizeof bufs[i]);
}

static void help_line(char *out, size_t n, const char *lk, const char *lv,
                      const char *rk, const char *rv) {
    char left[40];
    snprintf(left, sizeof left, "%-6s %s", lk, lv);
    snprintf(out, n, "%-27s%-6s %s", left, rk, rv);
}

static void draw_popup(App *a) {
    /* help reflects the actual keybinds from ~/.config/spotc/config */
    const Config *c = &a->cfg;
    char pair[8][16];
    char help[12][96];
    snprintf(pair[0], sizeof pair[0], "%s/%s", kn(c->key_down), kn(c->key_up));
    snprintf(pair[1], sizeof pair[1], "%s/%s", kn(c->key_next), kn(c->key_prev));
    snprintf(pair[2], sizeof pair[2], "%s/%s", kn(c->key_seek_back), kn(c->key_seek_forward));
    snprintf(pair[3], sizeof pair[3], "%s/%s", kn(c->key_volume_down), kn(c->key_volume_up));
    help_line(help[0], sizeof help[0], pair[0], "navigate", "Enter", "play");
    help_line(help[1], sizeof help[1], kn(c->key_focus), "switch pane",
              kn(c->key_play_pause), "play/pause");
    help_line(help[2], sizeof help[2], pair[1], "next/prev", pair[2], "seek -/+ 10s");
    help_line(help[3], sizeof help[3], pair[3], "volume", kn(c->key_queue), "add to queue");
    help_line(help[4], sizeof help[4], kn(c->key_shuffle), "shuffle",
              kn(c->key_repeat), "repeat");
    help_line(help[5], sizeof help[5], kn(c->key_reverse), "reverse mode (all tracks)",
              kn(c->key_vinyl), "vinyl sound (cycle)");
    help_line(help[6], sizeof help[6], kn(c->key_speed_up), "faster (to 2.0x)",
              kn(c->key_speed_down), "slower (to 0.5x)");
    help_line(help[7], sizeof help[7], kn(c->key_bass_boost), "bass boost",
              kn(c->key_retune), "retune (432 Hz & co)");
    help_line(help[8], sizeof help[8], kn(c->key_fx_reset), "reset fx",
              kn(c->key_search), "search");
    snprintf(help[9], sizeof help[9], "%s devices   %s reload   %s logout (clears credentials)",
             kn(c->key_devices), kn(c->key_reload), kn(c->key_logout));
    snprintf(help[10], sizeof help[10], "change all keys: ~/.config/spotc/config");
    help_line(help[11], sizeof help[11], kn(c->key_quit), "quit",
              kn(c->key_rain), "rain sounds (cycle)");

    int w, h;
    const char *title;
    if (a->popup == POPUP_HELP) {
        /* Keep the config path and logout warning visible on normal terminals.
         * On very narrow screens, fit safely and let putline truncate. */
        w = 62; h = (int)(sizeof help / sizeof *help) + 4;
        if (w > COLS - 4) w = COLS - 4;
        if (w < 20) w = 20;
        title = " keys ";
    } else if (a->popup == POPUP_RETUNE) {
        w = 76; h = RETUNE_PRESETS + 7;
        if (w > COLS - 4) w = COLS - 4;
        if (w < 20) w = 20;
        title = " retune ";
    } else {
        w = 44; h = a->n_devices + 4;
        title = " devices ";
    }
    if (h > LINES - 2) h = LINES - 2;
    int x0 = (COLS - w) / 2, y0 = (LINES - h) / 2;
    attron(COLOR_PAIR(CP_TEXT));
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) mvaddch(y, x, ' ');
    attroff(COLOR_PAIR(CP_TEXT));
    attron(COLOR_PAIR(CP_ACCENT));
    mvaddstr(y0, x0, "╭");
    mvaddstr(y0, x0 + w - 1, "╮");
    mvaddstr(y0 + h - 1, x0, "╰");
    mvaddstr(y0 + h - 1, x0 + w - 1, "╯");
    for (int x = x0 + 1; x < x0 + w - 1; x++) {
        mvaddstr(y0, x, "─");
        mvaddstr(y0 + h - 1, x, "─");
    }
    for (int y = y0 + 1; y < y0 + h - 1; y++) {
        mvaddstr(y, x0, "│");
        mvaddstr(y, x0 + w - 1, "│");
    }
    mvaddstr(y0, x0 + 2, title);
    attroff(COLOR_PAIR(CP_ACCENT));

    if (a->popup == POPUP_HELP) {
        for (size_t i = 0; i < sizeof help / sizeof *help && (int)i < h - 3; i++)
            putline(y0 + 2 + i, x0 + 3, w - 6, help[i], CP_TEXT, 0);
    } else if (a->popup == POPUP_RETUNE) {
        /* list off-preset last so the popup reads 1..6 then 0 */
        for (int r = 0; r < RETUNE_PRESETS && r < h - 3; r++) {
            int i = (r + 1) % RETUNE_PRESETS;
            const RetunePreset *p = &retune_presets[i];
            char line[128];
            int active = a->retune > p->ratio - 0.0005f &&
                         a->retune < p->ratio + 0.0005f;
            snprintf(line, sizeof line, "%d  %3d Hz  %-3s  %.3fx%s  %s",
                     i, p->hz, p->note, p->ratio,
                     p->reverb > 0.001f ? "+rev" : "", p->claim);
            putline(y0 + 2 + r, x0 + 3, w - 6, line,
                    active ? CP_ACCENT : CP_TEXT, active ? A_BOLD : 0);
        }
        int fy = y0 + 2 + RETUNE_PRESETS;
        if (fy + 3 >= y0 + h - 1) fy = y0 + h - 5;   /* clamped popup: keep footer inside */
        putline(fy + 1, x0 + 3, w - 6,
                "the nearest note is tuned to hit the target Hz; no claim has", CP_DIM, 0);
        putline(fy + 2, x0 + 3, w - 6,
                "scientific evidence. changing playback speed shifts the pitch,", CP_DIM, 0);
        putline(fy + 3, x0 + 3, w - 6,
                "so choosing a preset returns playback speed to 1.0x.", CP_DIM, 0);
    } else {
        if (a->n_devices == 0)
            putline(y0 + 2, x0 + 3, w - 6, "no devices found", CP_DIM, 0);
        for (int i = 0; i < a->n_devices && i < h - 3; i++) {
            char line[256];
            snprintf(line, sizeof line, "%s %s (%s)",
                     a->devices[i].active ? "●" : " ",
                     a->devices[i].name, a->devices[i].type);
            putline(y0 + 2 + i, x0 + 3, w - 6, line,
                    i == a->dev_sel ? CP_SEL : a->devices[i].active ? CP_ACCENT : CP_TEXT, 0);
        }
    }
}

static void draw_login(App *a) {
    int y = LINES / 2 - 7;
    putline(y, 4, COLS - 8, "spotc", CP_ACCENT, A_BOLD);
    putline(y + 2, 4, COLS - 8, "one-time setup, ~2 minutes (shared API keys get rate-limited"
            " by Spotify, so you need your own free one):", CP_DIM, 0);
    putline(y + 4, 4, COLS - 8, "1  open  https://developer.spotify.com/dashboard  →  Create app",
            CP_TEXT, 0);
    putline(y + 5, 7, COLS - 11, "any name  ·  Redirect URI:  http://127.0.0.1:5588/login",
            CP_TEXT, 0);
    putline(y + 6, 7, COLS - 11, "check 'Web API'  ·  Save  ·  copy the Client ID", CP_TEXT, 0);
    putline(y + 7, 4, COLS - 8, "2  press  i  and paste the Client ID", CP_TEXT, A_BOLD);
    putline(y + 8, 4, COLS - 8, "3  press  l  and sign in in your browser", CP_TEXT, A_BOLD);
    putline(y + 10, 4, COLS - 8, "sound on this PC + local effects:  Spotify on your phone"
            "  →  Devices  →  'spotc'", CP_DIM, 0);
    putline(y + 12, 4, COLS - 8, "Spotify Premium required  ·  q quit", CP_DIM, 0);
    if (a->status[0]) putline(y + 14, 4, COLS - 8, a->status, CP_ACCENT, A_BOLD);
}

void ui_draw(App *a) {
    static int colors_done;
    if (!colors_done) { apply_colors(&a->cfg); colors_done = 1; }
    erase();

    if (a->screen == SCREEN_LOGIN) { draw_login(a); refresh(); return; }

    char hdr[256];
    snprintf(hdr, sizeof hdr, " spotc");
    putline(0, 0, 20, hdr, CP_ACCENT, A_BOLD);
    if (a->active_device[0]) {
        char dev[128];
        snprintf(dev, sizeof dev, "◈ %s ", a->active_device);
        putline(0, COLS - disp_cols(dev), 40, dev, CP_DIM, 0);
    }

    int sb_w = COLS / 4;
    if (sb_w < 18) sb_w = 18;
    if (sb_w > 30) sb_w = 30;
    int list_h = LINES - 4;
    draw_sidebar(a, sb_w, list_h);
    draw_tracks(a, sb_w + 1, COLS - sb_w - 1, list_h);
    draw_bottom(a);
    if (a->popup != POPUP_NONE) draw_popup(a);
    refresh();
}

int ui_prompt(const char *label, char *out, size_t n) {
    int y = LINES - 1;
    move(y, 0);
    clrtoeol();
    attron(COLOR_PAIR(CP_ACCENT) | A_BOLD);
    mvaddstr(y, 0, label);
    attroff(COLOR_PAIR(CP_ACCENT) | A_BOLD);
    echo();
    curs_set(1);
    wtimeout(stdscr, -1);
    int rc = getnstr(out, n - 1);
    wtimeout(stdscr, 100);
    curs_set(0);
    noecho();
    return rc == OK && out[0] ? 0 : -1;
}

void ui_message(const char *msg) {
    putline(LINES - 1, 0, COLS - 1, msg, CP_DIM, 0);
    refresh();
}
