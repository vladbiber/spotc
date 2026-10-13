/* Local files: recursive scan of music_dir, tags and duration from ffprobe,
 * cached in ~/.cache/spotc/local.tsv by path + mtime. */
#include "spotc.h"
#include <ftw.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#define LOCAL_MAX 5000

typedef struct {
    char *path;
    long long mtime;
    int duration_ms;
    char title[192], artist[192], album[192];
} Entry;

static Entry *found;
static int n_found, cap_found;

static int is_audio(const char *name) {
    static const char *const ext[] = { "mp3", "flac", "ogg", "opus", "m4a", "wav", "aac" };
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    for (size_t i = 0; i < sizeof ext / sizeof *ext; i++)
        if (!strcasecmp(dot + 1, ext[i])) return 1;
    return 0;
}

static int collect(const char *path, const struct stat *st, int type, struct FTW *ftw) {
    (void)ftw;
    if (type != FTW_F || !is_audio(path)) return 0;
    if (n_found >= LOCAL_MAX) return 1;
    if (n_found == cap_found) {
        cap_found = cap_found ? cap_found * 2 : 256;
        Entry *grown = realloc(found, (size_t)cap_found * sizeof *found);
        if (!grown) return 1;
        found = grown;
    }
    Entry *e = &found[n_found++];
    memset(e, 0, sizeof *e);
    e->path = strdup(path);
    e->mtime = (long long)st->st_mtime;
    return 0;
}

static int by_path(const void *x, const void *y) {
    return strcmp(((const Entry *)x)->path, ((const Entry *)y)->path);
}

void local_hash(const char *path, char *hex, size_t n) {
    unsigned long long h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    snprintf(hex, n, "%016llx", h);
}

static void clean_field(char *s) {
    for (; *s; s++)
        if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

/* ffprobe without a shell, so any file name is safe */
static void probe(Entry *e) {
    int fd[2];
    if (pipe(fd) != 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fd[1], 1);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, 2);
        close(fd[0]); close(fd[1]);
        execlp("ffprobe", "ffprobe", "-v", "error", "-show_entries",
               "format=duration:format_tags=title,artist,album",
               "-of", "default=nw=1", e->path, (char *)NULL);
        _exit(127);
    }
    close(fd[1]);
    if (pid < 0) { close(fd[0]); return; }
    FILE *f = fdopen(fd[0], "r");
    char line[1024];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcasecmp(k, "duration")) e->duration_ms = (int)(atof(v) * 1000);
        else if (!strcasecmp(k, "TAG:title")) snprintf(e->title, sizeof e->title, "%s", v);
        else if (!strcasecmp(k, "TAG:artist")) snprintf(e->artist, sizeof e->artist, "%s", v);
        else if (!strcasecmp(k, "TAG:album")) snprintf(e->album, sizeof e->album, "%s", v);
    }
    if (f) fclose(f); else close(fd[0]);
    waitpid(pid, NULL, 0);
    clean_field(e->title); clean_field(e->artist); clean_field(e->album);
}

static Entry *cached;
static int n_cached;

static void load_cache(void) {
    char path[700];
    snprintf(path, sizeof path, "%s/local.tsv", cache_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[2048];
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *field[6] = {0};
        int nf = 0;
        char *p = line;
        while (nf < 6) {
            field[nf++] = p;
            char *tab = strchr(p, '\t');
            if (!tab) break;
            *tab = 0;
            p = tab + 1;
        }
        if (nf < 6) continue;
        if (n_cached == cap) {
            cap = cap ? cap * 2 : 256;
            Entry *grown = realloc(cached, (size_t)cap * sizeof *cached);
            if (!grown) break;
            cached = grown;
        }
        Entry *e = &cached[n_cached++];
        memset(e, 0, sizeof *e);
        e->path = strdup(field[0]);
        e->mtime = atoll(field[1]);
        e->duration_ms = atoi(field[2]);
        snprintf(e->title, sizeof e->title, "%s", field[3]);
        snprintf(e->artist, sizeof e->artist, "%s", field[4]);
        snprintf(e->album, sizeof e->album, "%s", field[5]);
    }
    fclose(f);
    qsort(cached, (size_t)n_cached, sizeof *cached, by_path);
}

static void save_cache(void) {
    char path[700], tmp[720];
    snprintf(path, sizeof path, "%s/local.tsv", cache_dir());
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < n_found; i++) {
        Entry *e = &found[i];
        if (strchr(e->path, '\t') || strchr(e->path, '\n')) continue;
        fprintf(f, "%s\t%lld\t%d\t%s\t%s\t%s\n", e->path, e->mtime, e->duration_ms,
                e->title, e->artist, e->album);
    }
    fclose(f);
    rename(tmp, path);
}

static void free_entries(Entry **arr, int *n) {
    for (int i = 0; i < *n; i++) free((*arr)[i].path);
    free(*arr);
    *arr = NULL;
    *n = 0;
}

static void expand_home(const char *in, char *out, size_t n) {
    const char *home = getenv("HOME");
    if (in[0] == '~' && (in[1] == '/' || !in[1]) && home)
        snprintf(out, n, "%s%s", home, in + 1);
    else
        snprintf(out, n, "%s", in);
}

int local_scan(App *a) {
    char root[600];
    expand_home(a->cfg.music_dir, root, sizeof root);
    free_entries(&found, &n_found);
    cap_found = 0;
    if (nftw(root, collect, 32, FTW_PHYS) < 0 && n_found == 0) {
        free(a->local_lib);
        a->local_lib = calloc(1, sizeof(Track));
        a->n_local_lib = 0;
        set_status(a, "no music folder at %s (music_dir in config)", root);
        return -1;
    }
    qsort(found, (size_t)n_found, sizeof *found, by_path);

    load_cache();
    int probed = 0;
    for (int i = 0; i < n_found; i++) {
        Entry key = { .path = found[i].path };
        Entry *hit = n_cached ? bsearch(&key, cached, (size_t)n_cached, sizeof *cached, by_path) : NULL;
        if (hit && hit->mtime == found[i].mtime) {
            found[i].duration_ms = hit->duration_ms;
            memcpy(found[i].title, hit->title, sizeof hit->title);
            memcpy(found[i].artist, hit->artist, sizeof hit->artist);
            memcpy(found[i].album, hit->album, sizeof hit->album);
            continue;
        }
        probe(&found[i]);
        if (++probed % 10 == 0) {
            set_status(a, "reading local files %d/%d...", i + 1, n_found);
            ui_draw(a);
        }
    }
    free_entries(&cached, &n_cached);
    if (probed) save_cache();

    free(a->local_lib);
    a->local_lib = calloc(n_found ? (size_t)n_found : 1, sizeof(Track));
    a->n_local_lib = 0;
    if (!a->local_lib) return -1;
    for (int i = 0; i < n_found; i++) {
        Entry *e = &found[i];
        Track *t = &a->local_lib[a->n_local_lib++];
        char hex[24];
        local_hash(e->path, hex, sizeof hex);
        snprintf(t->id, sizeof t->id, "l%s", hex);
        snprintf(t->uri, sizeof t->uri, "local:%s", hex);
        snprintf(t->path, sizeof t->path, "%s", e->path);
        t->duration_ms = e->duration_ms;
        if (e->title[0]) {
            snprintf(t->name, sizeof t->name, "%s", e->title);
        } else {
            const char *base = strrchr(e->path, '/');
            snprintf(t->name, sizeof t->name, "%s", base ? base + 1 : e->path);
            char *dot = strrchr(t->name, '.');
            if (dot) *dot = 0;
        }
        snprintf(t->artist, sizeof t->artist, "%s", e->artist);
        snprintf(t->album, sizeof t->album, "%s", e->album);
    }
    free_entries(&found, &n_found);
    cap_found = 0;
    return 0;
}
