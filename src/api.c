#include "spotc.h"
#include <curl/curl.h>

#define API "https://api.spotify.com/v1"

struct buf { char *p; size_t n; };

static size_t wr(void *data, size_t sz, size_t nm, void *ud) {
    struct buf *b = ud;
    size_t len = sz * nm;
    char *grown = realloc(b->p, b->n + len + 1);
    if (!grown) return 0;   /* aborts the transfer */
    b->p = grown;
    memcpy(b->p + b->n, data, len);
    b->n += len;
    b->p[b->n] = 0;
    return len;
}

/* one request; returns HTTP status, body in *out (may be empty), -1 on transport error */
static long req(App *a, const char *method, const char *path, const char *body, char **out) {
    *out = NULL;
    if (auth_ensure(a) != 0) return 401;
    CURL *c = curl_easy_init();
    if (!c) return -1;
    struct buf b = {0};
    char url[1024], hdr[600];
    snprintf(url, sizeof url, "%s%s", API, path);
    snprintf(hdr, sizeof hdr, "Authorization: Bearer %s", a->tok.access);
    struct curl_slist *h = curl_slist_append(NULL, hdr);
    h = curl_slist_append(h, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    if (body)
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    else if (strcmp(method, "GET") != 0)
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, "");
    CURLcode rc = curl_easy_perform(c);
    long http = -1;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    *out = b.p;
    return http;
}

static void set_api_error(App *a, long http, const char *body) {
    const char *message = NULL;
    json_object *root = body ? json_tokener_parse(body) : NULL;
    json_object *error = NULL, *value = NULL;
    if (root && json_object_object_get_ex(root, "error", &error)) {
        if (json_object_is_type(error, json_type_string))
            message = json_object_get_string(error);
        else if (json_object_object_get_ex(error, "message", &value))
            message = json_object_get_string(value);
    }
    if ((!message || !*message) && root &&
        json_object_object_get_ex(root, "error_description", &value))
        message = json_object_get_string(value);

    if (http == 401)
        set_status(a, "Spotify session expired — sign in again");
    else if (http == 403)
        set_status(a, "Spotify denied access — relog only if permissions changed");
    else if (http == 404)
        set_status(a, "Spotify resource or active device was not found");
    else if (http == 429)
        set_status(a, "rate limited by Spotify — try again shortly");
    else if (message && *message)
        set_status(a, "Spotify error %ld: %s", http, message);
    else if (http < 0)
        set_status(a, "network error while contacting Spotify");
    else
        set_status(a, "Spotify request failed (HTTP %ld)", http);
    if (root) json_object_put(root);
}

json_object *api_get(App *a, const char *path) {
    char *body;
    long http = req(a, "GET", path, NULL, &body);
    if (http == 401) {   /* force refresh once */
        a->tok.access[0] = 0;
        free(body);
        http = req(a, "GET", path, NULL, &body);
    }
    a->last_api_http = http;
    if (http < 200 || http >= 300 || !body) {
        set_api_error(a, http, body);
        free(body);
        return NULL;
    }
    json_object *root = json_tokener_parse(body);
    free(body);
    return root;
}

int api_send(App *a, const char *method, const char *path, const char *jbody) {
    char *body;
    long http = req(a, method, path, jbody, &body);
    if (http == 401) {
        a->tok.access[0] = 0;
        free(body);
        http = req(a, method, path, jbody, &body);
    }
    a->last_api_http = http;
    if (http < 200 || http >= 300)
        set_api_error(a, http, body);
    free(body);
    return (http >= 200 && http < 300) ? 0 : -1;
}

static const char *jstr(json_object *o, const char *key) {
    json_object *v;
    if (o && json_object_object_get_ex(o, key, &v) && !json_object_is_type(v, json_type_null))
        return json_object_get_string(v);
    return "";
}

static int jint(json_object *o, const char *key) {
    json_object *v;
    if (o && json_object_object_get_ex(o, key, &v)) return json_object_get_int(v);
    return 0;
}

static json_object *jobj(json_object *o, const char *key) {
    json_object *v;
    if (o && json_object_object_get_ex(o, key, &v) && !json_object_is_type(v, json_type_null))
        return v;
    return NULL;
}

static void parse_track(json_object *t, Track *out) {
    memset(out, 0, sizeof *out);
    snprintf(out->id, sizeof out->id, "%s", jstr(t, "id"));
    snprintf(out->uri, sizeof out->uri, "%s", jstr(t, "uri"));
    snprintf(out->name, sizeof out->name, "%s", jstr(t, "name"));
    out->duration_ms = jint(t, "duration_ms");
    snprintf(out->album, sizeof out->album, "%s", jstr(jobj(t, "album"), "name"));
    json_object *artists = jobj(t, "artists");
    if (artists && json_object_is_type(artists, json_type_array)) {
        size_t n = json_object_array_length(artists);
        for (size_t i = 0; i < n; i++) {
            const char *an = jstr(json_object_array_get_idx(artists, i), "name");
            if (i) strncat(out->artist, ", ", sizeof out->artist - strlen(out->artist) - 1);
            strncat(out->artist, an, sizeof out->artist - strlen(out->artist) - 1);
        }
    }
}

/* fetch every page of a paged listing; parse() consumes one item, returns
 * items written (0 = skipped).  Grows *arr; caps at MAX_LIST entries. */
enum { MAX_LIST = 400, MAX_TRACKS = 1000 };

static int load_pages(App *a, const char *base, void **arr, int *count,
                      size_t elem_sz, int max,
                      int (*parse)(json_object *item, void *slot)) {
    void *buf = NULL;
    int cnt = 0, cap = 0, offset = 0, total = max;
    int server_total = -1, retries = 0;
    while (offset < total && cnt < max) {
        char path[512];
        snprintf(path, sizeof path, "%s&offset=%d", base, offset);
        json_object *root = api_get(a, path);
        if (!root) break;
        json_object *tv;
        if (json_object_object_get_ex(root, "total", &tv))
            server_total = json_object_get_int(tv);
        if (server_total > 0 && server_total < total) total = server_total;
        json_object *items = jobj(root, "items");
        size_t n = items ? json_object_array_length(items) : 0;
        if (n == 0) {
            json_object_put(root);
            if (server_total == 0) break;    /* listing really is empty */
            /* the gateway sometimes answers 200 with an empty page although
             * total says there is more — don't believe it right away */
            if (retries++ < 2) {
                struct timespec ts = { 0, 300 * 1000000 };
                nanosleep(&ts, NULL);
                continue;
            }
            break;
        }
        if (cnt + (int)n > cap) {
            cap = cnt + (int)n + 128;
            void *grown = realloc(buf, (size_t)cap * elem_sz);
            if (!grown) { json_object_put(root); break; }
            buf = grown;
        }
        for (size_t i = 0; i < n && cnt < max; i++) {
            void *slot = (char *)buf + (size_t)cnt * elem_sz;
            memset(slot, 0, elem_sz);
            cnt += parse(json_object_array_get_idx(items, i), slot);
        }
        offset += (int)n;
        json_object_put(root);
        if (offset < total && offset < max) {
            set_status(a, "loading %d/%d...", cnt, total < max ? total : max);
            ui_draw(a);
        }
    }
    /* nothing loaded and the server never said "empty": keep the old list */
    if (cnt == 0 && server_total != 0) {
        free(buf);
        if (a->last_api_http >= 200 && a->last_api_http < 300)
            set_status(a, "Spotify sent an empty listing — try again");
        return -1;
    }
    free(*arr);
    *arr = buf ? buf : calloc(1, elem_sz);
    *count = cnt;
    return 0;
}

static int parse_playlist_item(json_object *p, void *slot) {
    Playlist *pl = slot;
    if (!jstr(p, "id")[0]) return 0;
    snprintf(pl->id, sizeof pl->id, "%s", jstr(p, "id"));
    snprintf(pl->uri, sizeof pl->uri, "%s", jstr(p, "uri"));
    snprintf(pl->name, sizeof pl->name, "%s", jstr(p, "name"));
    pl->tracks_total = jint(jobj(p, "tracks"), "total");
    return 1;
}

int api_load_playlists(App *a) {
    return load_pages(a, "/me/playlists?limit=50", (void **)&a->playlists,
                      &a->n_playlists, sizeof(Playlist), MAX_LIST,
                      parse_playlist_item);
}

static void parse_artists(json_object *obj, char *out, size_t outn) {
    out[0] = 0;
    json_object *artists;
    if (!json_object_object_get_ex(obj, "artists", &artists) ||
        !json_object_is_type(artists, json_type_array)) return;
    size_t n = json_object_array_length(artists);
    for (size_t i = 0; i < n; i++) {
        const char *an = jstr(json_object_array_get_idx(artists, i), "name");
        if (i) strncat(out, ", ", outn - strlen(out) - 1);
        strncat(out, an, outn - strlen(out) - 1);
    }
}

static int parse_album_item(json_object *it, void *slot) {
    json_object *alb = jobj(it, "album");
    if (!alb || !jstr(alb, "id")[0]) return 0;
    Playlist *p = slot;
    snprintf(p->id, sizeof p->id, "%s", jstr(alb, "id"));
    snprintf(p->uri, sizeof p->uri, "%s", jstr(alb, "uri"));
    snprintf(p->name, sizeof p->name, "%s", jstr(alb, "name"));
    p->tracks_total = jint(alb, "total_tracks");
    return 1;
}

int api_load_albums(App *a) {
    return load_pages(a, "/me/albums?limit=50", (void **)&a->albums,
                      &a->n_albums, sizeof(Playlist), MAX_LIST,
                      parse_album_item);
}

static int parse_wrapped_track(json_object *it, void *slot) {
    /* Current playlist responses use item; track is kept as a fallback
     * for older Spotify responses. */
    json_object *t = jobj(it, "item");
    if (!t) t = jobj(it, "track");
    if (!t || !jstr(t, "id")[0]) return 0;
    parse_track(t, slot);
    return 1;
}

static int parse_bare_track(json_object *t, void *slot) {
    if (!jstr(t, "id")[0]) return 0;
    parse_track(t, slot);
    return 1;
}

static int load_track_page(App *a, const char *path, int wrapped) {
    int rc = load_pages(a, path, (void **)&a->tracks, &a->n_tracks,
                        sizeof(Track), MAX_TRACKS,
                        wrapped ? parse_wrapped_track : parse_bare_track);
    if (rc == 0) a->tr_sel = a->tr_off = 0;
    return rc;
}

int api_load_playlist_tracks(App *a, const Playlist *p) {
    char path[256];
    snprintf(path, sizeof path, "/playlists/%s/items?limit=50", p->id);
    if (load_track_page(a, path, 1) != 0) {
        if (a->last_api_http == 403)
            set_status(a, "Public playlist: owner/collab only");
        return -1;
    }
    a->tracks_kind = TRACKS_PLAYLIST;
    snprintf(a->tracks_title, sizeof a->tracks_title, "%s", p->name);
    snprintf(a->tracks_ctx, sizeof a->tracks_ctx, "%s", p->uri);
    return 0;
}

int api_load_album_tracks(App *a, const Playlist *alb) {
    char path[256];
    snprintf(path, sizeof path, "/albums/%s/tracks?limit=50", alb->id);
    if (load_track_page(a, path, 0) != 0) return -1;
    a->tracks_kind = TRACKS_PLAYLIST;
    snprintf(a->tracks_title, sizeof a->tracks_title, "%s", alb->name);
    snprintf(a->tracks_ctx, sizeof a->tracks_ctx, "%s", alb->uri);
    return 0;
}

int api_load_liked(App *a) {
    if (load_track_page(a, "/me/tracks?limit=50", 1) != 0) return -1;
    a->tracks_kind = TRACKS_LIKED;
    snprintf(a->tracks_title, sizeof a->tracks_title, "Liked Songs");
    a->tracks_ctx[0] = 0;
    return 0;
}

int api_search(App *a, const char *query) {
    CURL *c = curl_easy_init();
    if (!c) return -1;
    char *q = curl_easy_escape(c, query, 0);
    if (!q) { curl_easy_cleanup(c); return -1; }
    /* Keep requests separate.  Besides avoiding URI parsing differences for
     * a comma-separated `type`, it makes album and track search reliable on
     * Spotify's newer Web API gateway. */
    char albums_path[1024], tracks_path[1024];
    snprintf(albums_path, sizeof albums_path, "/search?q=%s&type=album&limit=5", q);
    /* Spotify Search currently accepts at most 10 results per type. */
    snprintf(tracks_path, sizeof tracks_path, "/search?q=%s&type=track&limit=10", q);
    curl_free(q);
    curl_easy_cleanup(c);

    json_object *albums_root = api_get(a, albums_path);
    if (!albums_root) return -1;
    json_object *tracks_root = api_get(a, tracks_path);
    if (!tracks_root) { json_object_put(albums_root); return -1; }
    json_object *albs = jobj(jobj(albums_root, "albums"), "items");
    json_object *trks = jobj(jobj(tracks_root, "tracks"), "items");
    size_t na = albs ? json_object_array_length(albs) : 0;
    size_t nt = trks ? json_object_array_length(trks) : 0;
    if (na > 5) na = 5;   /* few albums on top, tracks below */
    free(a->tracks);
    a->tracks = calloc(na + nt ? na + nt : 1, sizeof(Track));
    a->n_tracks = 0;
    for (size_t i = 0; i < na; i++) {
        json_object *alb = json_object_array_get_idx(albs, i);
        if (!jstr(alb, "id")[0]) continue;
        Track *t = &a->tracks[a->n_tracks++];
        memset(t, 0, sizeof *t);
        t->is_album = 1;
        snprintf(t->id, sizeof t->id, "%s", jstr(alb, "id"));
        snprintf(t->uri, sizeof t->uri, "%s", jstr(alb, "uri"));
        snprintf(t->name, sizeof t->name, "%s", jstr(alb, "name"));
        parse_artists(alb, t->artist, sizeof t->artist);
    }
    for (size_t i = 0; i < nt; i++) {
        json_object *t = json_object_array_get_idx(trks, i);
        if (!jstr(t, "id")[0]) continue;
        parse_track(t, &a->tracks[a->n_tracks++]);
    }
    json_object_put(albums_root);
    json_object_put(tracks_root);
    a->tracks_kind = TRACKS_SEARCH;
    snprintf(a->tracks_title, sizeof a->tracks_title, "Search: %s", query);
    a->tracks_ctx[0] = 0;
    a->tr_sel = a->tr_off = 0;
    return 0;
}

int api_fetch_state(App *a) {
    char *body;
    long http = req(a, "GET", "/me/player", NULL, &body);
    if (http == 401) {
        a->tok.access[0] = 0;
        free(body);
        http = req(a, "GET", "/me/player", NULL, &body);
    }
    a->last_poll_ms = now_ms();
    if (http == 429) a->last_poll_ms = now_ms() + 10000;  /* back off */
    if (http == 204) { a->have_state = 0; free(body); return 0; }
    if (http != 200 || !body) { free(body); return -1; }
    json_object *root = json_tokener_parse(body);
    free(body);
    if (!root) return -1;

    a->have_state = 1;
    json_object *v;
    a->playing = json_object_object_get_ex(root, "is_playing", &v) && json_object_get_boolean(v);
    a->progress_ms = jint(root, "progress_ms");
    a->shuffle = json_object_object_get_ex(root, "shuffle_state", &v) && json_object_get_boolean(v);
    const char *rep = jstr(root, "repeat_state");
    a->repeat = !strcmp(rep, "context") ? 1 : !strcmp(rep, "track") ? 2 : 0;
    json_object *dev = jobj(root, "device");
    int reported_volume = jint(dev, "volume_percent");
    long long now = now_ms();
    if (a->volume_target >= 0) {
        /* Spotify Connect reports old volume for a short time after a PUT.
         * Keep showing (and using) the requested absolute value meanwhile. */
        if (reported_volume == a->volume_target || now > a->volume_hold_until_ms) {
            a->volume = reported_volume;
            a->volume_target = -1;
        } else {
            a->volume = a->volume_target;
        }
    } else {
        a->volume = reported_volume;
    }
    snprintf(a->active_device, sizeof a->active_device, "%s", jstr(dev, "name"));
    char prev_id[sizeof a->now.id];
    snprintf(prev_id, sizeof prev_id, "%s", a->now.id);
    json_object *item = jobj(root, "item");
    if (item) parse_track(item, &a->now);
    else memset(&a->now, 0, sizeof a->now);
    /* follow the playing track only when it changes, so polling never
     * yanks the selection away while the user is browsing the list */
    if (a->now.id[0] && strcmp(prev_id, a->now.id)) {
        for (int i = 0; i < a->n_tracks; i++) {
            if (!strcmp(a->tracks[i].id, a->now.id)) {
                a->tr_sel = i;
                break;
            }
        }
    }
    a->state_at_ms = now;
    json_object_put(root);
    return 0;
}

int api_fetch_devices(App *a) {
    json_object *root = api_get(a, "/me/player/devices");
    if (!root) return -1;
    json_object *items = jobj(root, "devices");
    size_t n = items ? json_object_array_length(items) : 0;
    free(a->devices);
    a->devices = calloc(n ? n : 1, sizeof(Device));
    a->n_devices = 0;
    for (size_t i = 0; i < n; i++) {
        json_object *d = json_object_array_get_idx(items, i);
        Device *dev = &a->devices[a->n_devices++];
        snprintf(dev->id, sizeof dev->id, "%s", jstr(d, "id"));
        snprintf(dev->name, sizeof dev->name, "%s", jstr(d, "name"));
        snprintf(dev->type, sizeof dev->type, "%s", jstr(d, "type"));
        json_object *v;
        dev->active = json_object_object_get_ex(d, "is_active", &v) && json_object_get_boolean(v);
        dev->volume = jint(d, "volume_percent");
    }
    json_object_put(root);
    if (a->dev_sel >= a->n_devices) a->dev_sel = 0;
    return 0;
}

int api_transfer(App *a, const char *device_id) {
    char body[256];
    snprintf(body, sizeof body, "{\"device_ids\":[\"%s\"],\"play\":true}", device_id);
    return api_send(a, "PUT", "/me/player", body);
}

static void play_path(char *out, size_t n, const char *dev_id) {
    snprintf(out, n, "/me/player/play%s%s",
             dev_id ? "?device_id=" : "", dev_id ? dev_id : "");
}

int api_play_context(App *a, const char *ctx_uri, int offset, const char *dev_id) {
    char path[192], body[320];
    play_path(path, sizeof path, dev_id);
    snprintf(body, sizeof body,
        "{\"context_uri\":\"%s\",\"offset\":{\"position\":%d}}", ctx_uri, offset);
    return api_send(a, "PUT", path, body);
}

int api_play_uri(App *a, const char *uri, const char *dev_id) {
    char path[192], body[256];
    play_path(path, sizeof path, dev_id);
    snprintf(body, sizeof body, "{\"uris\":[\"%s\"]}", uri);
    return api_send(a, "PUT", path, body);
}

int api_play_uri_at(App *a, const char *uri, int position_ms, const char *dev_id) {
    char path[192], body[320];
    if (position_ms < 0) position_ms = 0;
    play_path(path, sizeof path, dev_id);
    snprintf(body, sizeof body, "{\"uris\":[\"%s\"],\"position_ms\":%d}",
             uri, position_ms);
    return api_send(a, "PUT", path, body);
}

int api_play_tracks(App *a, const Track *tracks, int n_tracks, int start,
                    const char *dev_id) {
    if (start < 0 || start >= n_tracks) return -1;
    char path[192];
    play_path(path, sizeof path, dev_id);

    /* A standalone URI has no following item on a Spotify Connect device.
     * Give it a small explicit queue so Liked Songs and search continue. */
    size_t cap = 32 + 100 * 140;
    char *body = malloc(cap);
    if (!body) return -1;
    size_t used = (size_t)snprintf(body, cap, "{\"uris\":[");
    int added = 0;
    for (int i = start; i < n_tracks && added < 100; i++) {
        if (!tracks[i].uri[0] || tracks[i].is_album) continue;
        int wrote = snprintf(body + used, cap - used, "%s\"%s\"",
                             added ? "," : "", tracks[i].uri);
        if (wrote < 0 || (size_t)wrote >= cap - used) break;
        used += (size_t)wrote;
        added++;
    }
    if (!added) { free(body); return -1; }
    snprintf(body + used, cap - used, "]}");
    int rc = api_send(a, "PUT", path, body);
    free(body);
    return rc;
}

int api_pause(App *a)  { return api_send(a, "PUT", "/me/player/pause", NULL); }
int api_resume(App *a) { return api_send(a, "PUT", "/me/player/play", NULL); }
int api_next(App *a)   { return api_send(a, "POST", "/me/player/next", NULL); }
int api_prev(App *a)   { return api_send(a, "POST", "/me/player/previous", NULL); }

int api_seek(App *a, int pos_ms) {
    char path[128];
    if (pos_ms < 0) pos_ms = 0;
    snprintf(path, sizeof path, "/me/player/seek?position_ms=%d", pos_ms);
    return api_send(a, "PUT", path, NULL);
}

int api_set_volume(App *a, int vol) {
    char path[128];
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    snprintf(path, sizeof path, "/me/player/volume?volume_percent=%d", vol);
    return api_send(a, "PUT", path, NULL);
}

int api_set_shuffle(App *a, int on) {
    char path[128];
    snprintf(path, sizeof path, "/me/player/shuffle?state=%s", on ? "true" : "false");
    return api_send(a, "PUT", path, NULL);
}

int api_set_repeat(App *a, int mode) {
    static const char *names[] = { "off", "context", "track" };
    char path[128];
    snprintf(path, sizeof path, "/me/player/repeat?state=%s", names[mode % 3]);
    return api_send(a, "PUT", path, NULL);
}

int api_queue_add(App *a, const char *uri) {
    CURL *c = curl_easy_init();
    if (!c) return -1;
    char *e = curl_easy_escape(c, uri, 0);
    if (!e) { curl_easy_cleanup(c); return -1; }
    char path[256];
    snprintf(path, sizeof path, "/me/player/queue?uri=%s", e);
    curl_free(e);
    curl_easy_cleanup(c);
    return api_send(a, "POST", path, NULL);
}
