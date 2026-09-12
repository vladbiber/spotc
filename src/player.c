#include "spotc.h"
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <ctype.h>
#include <errno.h>

static pid_t pids[3];
static char fifo_path[600];

const char *player_librespot_path(void) {
    static char path[600];
    if (path[0]) return path[1] ? path : NULL;
    const char *home = getenv("HOME");
    char cand[600];
    snprintf(cand, sizeof cand, "%s/.cargo/bin/librespot", home ? home : "");
    if (access(cand, X_OK) == 0) { snprintf(path, sizeof path, "%s", cand); return path; }
    FILE *p = popen("command -v librespot 2>/dev/null", "r");
    if (p) {
        if (fgets(cand, sizeof cand, p)) {
            cand[strcspn(cand, "\n")] = 0;
            if (cand[0]) snprintf(path, sizeof path, "%s", cand);
        }
        pclose(p);
    }
    if (!path[0]) { path[0] = 1; path[1] = 0; return NULL; }  /* remember "not found" */
    return path;
}

static char *self_dir(void) {
    static char dir[600];
    ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
    if (n > 0) {
        dir[n] = 0;
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0;
    } else {
        dir[0] = 0;
    }
    return dir;
}

static int start_pipeline(App *a, int oauth) {
    const char *lrs = player_librespot_path();
    if (!lrs) return -1;
    if (a->pipeline_up) return 0;

    char fx_bin[700];
    snprintf(fx_bin, sizeof fx_bin, "%s/spotc-fx", self_dir());
    if (access(fx_bin, X_OK) != 0)
        snprintf(fx_bin, sizeof fx_bin, "spotc-fx");

    snprintf(fifo_path, sizeof fifo_path, "%s/fx.ctl", cache_dir());
    unlink(fifo_path);
    if (mkfifo(fifo_path, 0600) != 0) return -1;

    char lrs_cache[700], lrs_log[700], bitrate[16], spd[16], rev[16], bass[16];
    snprintf(lrs_cache, sizeof lrs_cache, "%s/librespot", cache_dir());
    mkdir(lrs_cache, 0700);
    snprintf(lrs_log, sizeof lrs_log, "%s/librespot.log", cache_dir());
    snprintf(bitrate, sizeof bitrate, "%d", a->cfg.bitrate);
    snprintf(spd, sizeof spd, "%.3f", a->speed * a->retune);
    snprintf(rev, sizeof rev, "%.3f", a->reverb);
    snprintf(bass, sizeof bass, "%.3f", a->bass);

    int p1[2], p2[2];
    if (pipe(p1) || pipe(p2)) return -1;
    int logfd = open(lrs_log, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    /* force IPv4 name resolution in librespot: routers that advertise IPv6
     * without working upstream break the dealer websocket (ENETUNREACH on
     * the AAAA address) and Spirc gives up; Spotify is fine over IPv4 */
    char ipv4_so[700];
    snprintf(ipv4_so, sizeof ipv4_so, "%s/spotc-ipv4.so", self_dir());

    pids[0] = fork();
    if (pids[0] == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        dup2(p1[1], 1);
        if (logfd >= 0) dup2(logfd, 2);
        close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
        if (access(ipv4_so, R_OK) == 0) setenv("LD_PRELOAD", ipv4_so, 1);
        const char *argv[16];
        int n = 0;
        argv[n++] = "librespot";
        argv[n++] = "--name";    argv[n++] = a->cfg.device_name;
        argv[n++] = "--backend"; argv[n++] = "pipe";
        argv[n++] = "--format";  argv[n++] = "S16";
        argv[n++] = "--dither";  argv[n++] = "none";
        argv[n++] = "--bitrate"; argv[n++] = bitrate;
        argv[n++] = "--cache";   argv[n++] = lrs_cache;
        argv[n++] = "--disable-audio-cache";
        if (oauth) argv[n++] = "--enable-oauth";
        argv[n] = NULL;
        execv(lrs, (char *const *)argv);
        _exit(127);
    }

    pids[1] = fork();
    if (pids[1] == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        dup2(p1[0], 0);
        dup2(p2[1], 1);
        close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
        if (logfd >= 0) close(logfd);
        execl(fx_bin, "spotc-fx", fifo_path, spd, rev, bass, (char *)NULL);
        execlp("spotc-fx", "spotc-fx", fifo_path, spd, rev, bass, (char *)NULL);
        _exit(127);
    }

    pids[2] = fork();
    if (pids[2] == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        dup2(p2[0], 0);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
        close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
        if (logfd >= 0) close(logfd);
        execlp("pacat", "pacat", "--playback", "--raw",
               "--format=s16le", "--rate=44100", "--channels=2",
               "--latency-msec=120", "--process-time-msec=20",
               "--client-name=spotc", "--stream-name=spotc",
               (char *)NULL);
        _exit(127);
    }

    close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
    if (logfd >= 0) close(logfd);
    a->pipeline_up = 1;
    return 0;
}

int player_start(App *a) {
    return start_pipeline(a, 0);
}

static int valid_track_id(const char *id) {
    if (!id[0]) return 0;
    for (const unsigned char *p = (const unsigned char *)id; *p; p++)
        if (!isalnum(*p)) return 0;
    return 1;
}

/* Decode the complete track independently of Spotify Connect.  Since the
 * helper writes to a regular file, librespot is not paced by an audio device
 * and normally finishes in a few seconds. */
int player_cache_reverse(App *a, char *path, size_t n) {
    if (!valid_track_id(a->now.id) || !a->now.uri[0]) return -1;

    char dir[700], tmp[800], helper[700], lrs_cache[700], logp[700];
    snprintf(dir, sizeof dir, "%s/reverse", cache_dir());
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return -1;
    snprintf(path, n, "%s/%s-%d.s16", dir, a->now.id, a->cfg.bitrate);

    long long expected = (long long)a->now.duration_ms * 44100 * 4 / 1000;
    long long tolerance = 5LL * 44100 * 4;
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 44100 * 4 &&
        (!expected || (st.st_size >= expected - tolerance &&
                       st.st_size <= expected + tolerance)))
        return 0;

    snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid());
    unlink(tmp);
    snprintf(helper, sizeof helper, "%s/spotc-reverse-cache", self_dir());
    if (access(helper, X_OK) != 0) snprintf(helper, sizeof helper, "spotc-reverse-cache");
    snprintf(lrs_cache, sizeof lrs_cache, "%s/librespot", cache_dir());
    snprintf(logp, sizeof logp, "%s/reverse.log", cache_dir());
    char bitrate[16];
    snprintf(bitrate, sizeof bitrate, "%d", a->cfg.bitrate);

    pid_t pid = fork();
    if (pid == 0) {
        int logfd = open(logp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, 1);
        if (logfd >= 0) dup2(logfd, 2);
        execl(helper, "spotc-reverse-cache", lrs_cache, a->now.uri, tmp,
              bitrate, (char *)NULL);
        execlp("spotc-reverse-cache", "spotc-reverse-cache", lrs_cache,
               a->now.uri, tmp, bitrate, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return -1;
    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        stat(tmp, &st) != 0 || st.st_size < 44100 * 4) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 1;
}

int player_start_reverse(App *a, const char *path) {
    if (a->pipeline_up) return -1;

    char fx_bin[700], spd[16], rev[16], bass[16], gain[16];
    snprintf(fx_bin, sizeof fx_bin, "%s/spotc-fx", self_dir());
    if (access(fx_bin, X_OK) != 0) snprintf(fx_bin, sizeof fx_bin, "spotc-fx");
    snprintf(spd, sizeof spd, "%.3f", a->speed * a->retune);
    snprintf(rev, sizeof rev, "%.3f", a->reverb);
    snprintf(bass, sizeof bass, "%.3f", a->bass);
    snprintf(gain, sizeof gain, "%.3f", a->volume / 100.0f);

    snprintf(fifo_path, sizeof fifo_path, "%s/fx.ctl", cache_dir());
    unlink(fifo_path);
    if (mkfifo(fifo_path, 0600) != 0) return -1;

    int p2[2];
    if (pipe(p2) != 0) return -1;
    memset(pids, 0, sizeof pids);
    pids[1] = fork();
    if (pids[1] == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        dup2(p2[1], 1);
        close(p2[0]); close(p2[1]);
        execl(fx_bin, "spotc-fx", fifo_path, spd, rev, bass,
              "--reverse-file", path, gain, (char *)NULL);
        execlp("spotc-fx", "spotc-fx", fifo_path, spd, rev, bass,
               "--reverse-file", path, gain, (char *)NULL);
        _exit(127);
    }
    if (pids[1] < 0) { close(p2[0]); close(p2[1]); unlink(fifo_path); return -1; }

    pids[2] = fork();
    if (pids[2] == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        dup2(p2[0], 0);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
        close(p2[0]); close(p2[1]);
        execlp("pacat", "pacat", "--playback", "--raw",
               "--format=s16le", "--rate=44100", "--channels=2",
               "--latency-msec=120", "--process-time-msec=20",
               "--client-name=spotc", "--stream-name=spotc-reverse",
               (char *)NULL);
        _exit(127);
    }
    if (pids[2] < 0) {
        kill(pids[1], SIGTERM);
        waitpid(pids[1], NULL, 0);
        memset(pids, 0, sizeof pids);
        close(p2[0]); close(p2[1]); unlink(fifo_path);
        return -1;
    }
    close(p2[0]); close(p2[1]);
    a->pipeline_up = 1;
    return 0;
}

void player_reverse_pause(App *a, int paused) {
    if (!a->reverse || !a->pipeline_up || pids[1] <= 0) return;
    kill(pids[1], paused ? SIGSTOP : SIGCONT);
}

/* relaunch librespot with --enable-oauth: it prints the authorize URL to its
 * log and runs its own redirect server; we open the URL and wait for the
 * cached credentials to appear */
int player_oauth_login(App *a) {
    player_stop(a);
    if (start_pipeline(a, 1) != 0) return -1;

    char logp[700];
    snprintf(logp, sizeof logp, "%s/librespot.log", cache_dir());
    int opened = 0;
    for (int i = 0; i < 360; i++) {   /* 3 min */
        if (auth_have_creds(a)) return 0;
        if (!opened) {
            FILE *f = fopen(logp, "r");
            if (f) {
                char line[2048];
                while (fgets(line, sizeof line, f)) {
                    char *u = strstr(line, "https://accounts.spotify.com/authorize");
                    if (u) {
                        u[strcspn(u, " \t\r\n'\"")] = 0;
                        char cmd[2300];
                        snprintf(cmd, sizeof cmd, "xdg-open '%s' >/dev/null 2>&1 &", u);
                        int rc = system(cmd);
                        (void)rc;
                        opened = 1;
                        break;
                    }
                }
                fclose(f);
            }
        }
        struct timespec ts = { 0, 500 * 1000000 };
        nanosleep(&ts, NULL);
    }
    player_stop(a);
    start_pipeline(a, 0);
    return -1;
}

/* reap dead pipeline children; 1 = pipeline went down (already cleaned up) */
int player_check(App *a) {
    if (!a->pipeline_up) return 0;
    int died = 0;
    for (int i = 0; i < 3; i++)
        if (pids[i] > 0 && waitpid(pids[i], NULL, WNOHANG) > 0) {
            pids[i] = 0;
            died = 1;
        }
    if (!died) return 0;
    /* In whole-track reverse mode the FX reader finishes first.  Let pacat
     * drain its final buffered samples and close naturally before declaring
     * the track complete. */
    if (a->reverse && pids[2] > 0) return 0;
    player_stop(a);
    return 1;
}

void player_stop(App *a) {
    if (!a->pipeline_up) return;
    for (int i = 0; i < 3; i++)
        if (pids[i] > 0) {
            kill(pids[i], SIGCONT); /* a paused reverse worker must be reapable */
            kill(pids[i], SIGTERM);
        }
    for (int i = 0; i < 3; i++)
        if (pids[i] > 0) waitpid(pids[i], NULL, 0);
    memset(pids, 0, sizeof pids);
    unlink(fifo_path);
    a->pipeline_up = 0;
}

void player_set_fx(App *a) {
    if (!a->pipeline_up) return;
    int fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) return;
    char msg[128];
    int n = snprintf(msg, sizeof msg,
                     "speed %.3f\nreverb %.3f\nbass %.3f\ngain %.3f\n",
                     a->speed * a->retune, a->reverb, a->bass,
                     a->reverse ? a->volume / 100.0f : 1.0f);
    ssize_t w = write(fd, msg, n);
    (void)w;
    close(fd);
}
