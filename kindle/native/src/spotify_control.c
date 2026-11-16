#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef BASE
#define BASE "/mnt/us/extensions/kindle-spotify"
#endif
#ifndef RUN
#define RUN "/var/tmp/kindle-spotify"
#endif

static int read_file(const char *path, char *buf, size_t size) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, size - 1, f);
    int extra = fgetc(f);
    fclose(f);
    buf[n] = 0;
    if (extra != EOF) return -1;
    while (n && isspace((unsigned char)buf[n - 1])) buf[--n] = 0;
    return (int)n;
}

static void player_environment(void) {
    setenv("HOME", BASE "/private", 1);
    setenv("LD_LIBRARY_PATH", RUN "/lib", 1);
    setenv("LD_PRELOAD", RUN "/lib/libpulse.so.0", 1);
    setenv("ALSA_CONFIG_PATH", RUN "/alsa.conf", 1);
    setenv("APULSE_PLAYBACK_DEVICE", "kindle", 1);
    setenv("APULSE_DIAG", "1", 1);
    setenv("APULSE_MAX_TLENGTH_MS", "300", 1);
    setenv("APULSE_BUFFER_MS", "200", 1);
    unsetenv("STATE_DIRECTORY");
    unsetenv("CACHE_DIRECTORY");
}

static int launch_player(void) {
    char key[2048];
    int n = read_file(BASE "/private/soloist.key", key, sizeof(key));
    if (n < 16) { fputs("Private Spotify key is missing.\n", stderr); return 1; }
    for (int i = 0; i < n; ++i)
        if (isspace((unsigned char)key[i])) { fputs("Invalid private key file.\n", stderr); return 1; }
    player_environment();
    signal(SIGPIPE, SIG_IGN);
    char *args[] = { RUN "/soloist", "--device-name", "Kindle Spotify", "--api-key", key,
        "--data-dir", BASE "/private/state", "--cache-dir", BASE "/private/cache",
        "--cache-size", "100", "--initial-volume", "30", "--ws", "127.0.0.1:24879", NULL };
    execv(args[0], args);
    perror("Spotify executable");
    return 1;
}

/* Report only non-loopback TCP listeners owned by our player process. */
static int player_ports(void) {
    char pid[32], path[256];
    if (read_file(RUN "/player.pid", pid, sizeof(pid)) < 1) return 1;
    for (const char *p = pid; *p; ++p) if (!isdigit((unsigned char)*p)) return 1;
    snprintf(path, sizeof(path), "/proc/%s/fd", pid);
    DIR *dir = opendir(path);
    if (!dir) return 1;
    unsigned long inodes[256]; size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) && count < 256) {
        char linkpath[1024], target[128];
        snprintf(linkpath, sizeof(linkpath), "%s/%s", path, entry->d_name);
        ssize_t n = readlink(linkpath, target, sizeof(target) - 1);
        if (n > 0) {
            target[n] = 0; unsigned long inode;
            if (sscanf(target, "socket:[%lu]", &inode) == 1) inodes[count++] = inode;
        }
    }
    closedir(dir);
    FILE *f = fopen("/proc/net/tcp", "r");
    if (!f) return 1;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char address[9]; unsigned port, state; unsigned long inode;
        if (sscanf(line, " %*u: %8[0-9A-Fa-f]:%x %*s %x %*s %*s %*s %*s %*s %lu", address, &port, &state, &inode) != 4) continue;
        if (state != 10 || !strcmp(address, "0100007F")) continue;
        for (size_t i = 0; i < count; ++i)
            if (inodes[i] == inode) { printf("%u\n", port); break; }
    }
    fclose(f); return 0;
}

/* Fixed command vocabulary only; never execute a shell or request-supplied code. */
static int capture(char *const args[], int player_env, char *out, size_t size) {
    int pipes[2];
    if (pipe(pipes)) return 1;
    pid_t child = fork();
    if (child == 0) {
        setpgid(0, 0);
        close(pipes[0]); dup2(pipes[1], STDOUT_FILENO); dup2(pipes[1], STDERR_FILENO); close(pipes[1]);
        if (player_env) player_environment();
        else setenv("GST_DEBUG", "2", 1);
        execv(args[0], args); _exit(127);
    }
    close(pipes[1]);
    if (child < 0) { close(pipes[0]); return 1; }
    setpgid(child, child);
    fcntl(pipes[0], F_SETFL, O_NONBLOCK);
    size_t used = 0;
    time_t deadline = time(NULL) + 5;
    int done = 0, status = 0;
    while (time(NULL) <= deadline && used + 1 < size) {
        struct pollfd p = { pipes[0], POLLIN | POLLHUP, 0 };
        poll(&p, 1, 100);
        ssize_t n = read(pipes[0], out + used, size - used - 1);
        if (n > 0) used += (size_t)n;
        if (waitpid(child, &status, WNOHANG) == child) { done = 1; break; }
    }
    if (!done) { kill(-child, SIGKILL); kill(child, SIGKILL); waitpid(child, &status, 0); }
    for (;;) {
        ssize_t n = read(pipes[0], out + used, size - used - 1);
        if (n <= 0) break;
        used += (size_t)n;
        if (used + 1 == size) break;
    }
    close(pipes[0]); out[used] = 0;
    return done && WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int ctl(const char *command, const char *argument, int json, char *out, size_t size) {
    char *args[10] = { RUN "/soloist", "ctl", (char *)command,
        "--data-dir", BASE "/private/state", NULL };
    int i = 5;
    if (argument) args[i++] = (char *)argument;
    if (json) args[i++] = "--json";
    args[i] = NULL;
    return capture(args, 1, out, size);
}

static void redact(char *s) {
    char key[2048];
    if (read_file(BASE "/private/soloist.key", key, sizeof(key)) < 16) return;
    size_t n = strlen(key);
    char *p;
    while ((p = strstr(s, key))) memset(p, '*', n);
}

static void reply(int fd, int code, const char *kind, char *body) {
    redact(body);
    char header[512];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d Result\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n",
        code, kind, strlen(body));
    send(fd, header, (size_t)n, MSG_NOSIGNAL);
    size_t left = strlen(body); const char *p = body;
    while (left) {
        ssize_t count = send(fd, p, left, MSG_NOSIGNAL);
        if (count <= 0) break;
        p += count; left -= (size_t)count;
    }
}

static int same_token(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    unsigned diff = (unsigned)(na ^ nb);
    for (size_t i = 0; i < na; ++i) diff |= (unsigned char)a[i] ^ (i < nb ? (unsigned char)b[i] : 0);
    return diff == 0;
}

static void handle(int fd, const char *token) {
    char request[8192], method[16], path[256], supplied[512] = "";
    size_t used = 0;
    while (used + 1 < sizeof(request)) {
        ssize_t n = recv(fd, request + used, sizeof(request) - used - 1, 0);
        if (n <= 0) return;
        used += (size_t)n; request[used] = 0;
        if (strstr(request, "\r\n\r\n")) break;
    }
    char body[65536] = "Invalid request\n";
    if (!strstr(request, "\r\n\r\n") || sscanf(request, "%15s %255s", method, path) != 2) {
        reply(fd, 400, "text/plain", body); return;
    }
    int origin = 0;
    char *save, *line = strtok_r(request, "\r\n", &save);
    while ((line = strtok_r(NULL, "\r\n", &save))) {
        if (!strncasecmp(line, "X-Dashboard-Toggle-Token:", 25)) {
            line += 25; while (*line == ' ' || *line == '\t') ++line;
            snprintf(supplied, sizeof(supplied), "%s", line);
        }
        if (!strncasecmp(line, "Origin:", 7)) origin = 1;
    }
    if (origin || !same_token(token, supplied)) {
        strcpy(body, "Unauthorized\n"); reply(fd, 401, "text/plain", body); return;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/health")) {
        strcpy(body, "{\"ok\":true,\"player\":\"Kindle Spotify\"}\n"); reply(fd, 200, "application/json", body); return;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/audio-status")) {
        char *args[] = { "/bin/sh", BASE "/audio-probe.sh", NULL };
        int rc = capture(args, 0, body, sizeof(body));
        reply(fd, rc ? 409 : 200, "text/plain", body); return;
    }
    if (!strcmp(method, "GET") && (!strcmp(path, "/status") || !strcmp(path, "/now"))) {
        int now = !strcmp(path, "/now");
        int rc = ctl(now ? "now" : "status", NULL, now, body, sizeof(body));
        reply(fd, rc ? 409 : 200, now && !rc ? "application/json" : "text/plain", body); return;
    }
    if (!strcmp(method, "GET") && (!strcmp(path, "/log") || !strcmp(path, "/sink-log") || !strcmp(path, "/supervisor-log"))) {
        const char *file = !strcmp(path, "/log") ? RUN "/player.log" :
            !strcmp(path, "/sink-log") ? RUN "/sink.log" : RUN "/supervisor.log";
        FILE *f = fopen(file, "r");
        body[0] = 0;
        if (f) {
            fseek(f, 0, SEEK_END); long size = ftell(f);
            fseek(f, size > 50000 ? size - 50000 : 0, SEEK_SET);
            size_t n = fread(body, 1, sizeof(body) - 1, f); body[n] = 0; fclose(f);
        }
        reply(fd, 200, "text/plain", body); return;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/metrics")) {
        char pid[32], file[128]; body[0] = 0;
        if (read_file(RUN "/player.pid", pid, sizeof(pid)) > 0) {
            int valid = 1;
            for (const char *p = pid; *p; ++p) if (!isdigit((unsigned char)*p)) valid = 0;
            if (valid) { snprintf(file, sizeof(file), "/proc/%s/status", pid); read_file(file, body, sizeof(body)); }
        }
        size_t n = strlen(body);
        if (n + 100 < sizeof(body)) read_file("/proc/meminfo", body + n, sizeof(body) - n);
        reply(fd, 200, "text/plain", body); return;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/mixer-log")) {
        FILE *f = fopen("/var/log/messages", "r"); body[0] = 0;
        if (f) {
            fseek(f, 0, SEEK_END); long size = ftell(f);
            fseek(f, size > 50000 ? size - 50000 : 0, SEEK_SET);
            char line[2048]; size_t n = 0;
            while (fgets(line, sizeof(line), f)) {
                if (!strstr(line, "MixerSink") && !strstr(line, "mixersink") && !strstr(line, "gst_segment")) continue;
                size_t len = strlen(line);
                if (n + len >= sizeof(body)) break;
                memcpy(body + n, line, len); n += len; body[n] = 0;
            }
            fclose(f);
        }
        reply(fd, 200, "text/plain", body); return;
    }
    if (strcmp(method, "POST")) { strcpy(body, "Not found\n"); reply(fd, 404, "text/plain", body); return; }
    const char *command = NULL, *arg = NULL;
    char value[64];
    if (!strcmp(path, "/reconnect-speaker")) {
        int f = open(RUN "/reconnect-speaker", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (f >= 0) close(f);
        strcpy(body, "Speaker audio route reset requested\n");
        reply(fd, f < 0 ? 500 : 202, "text/plain", body); return;
    }
    if (!strncmp(path, "/native-tone/", 13)) {
        const char *mode = path + 13;
        if (strcmp(mode, "clocked") && strcmp(mode, "unclocked")) {
            strcpy(body, "Invalid tone mode\n"); reply(fd, 400, "text/plain", body); return;
        }
        char *args[] = { "/bin/sh", BASE "/native-tone.sh", (char *)mode, NULL };
        int rc = capture(args, 0, body, sizeof(body));
        unlink(RUN "/sink-test");
        reply(fd, rc ? 409 : 200, "text/plain", body); return;
    }
    if (!strncmp(path, "/audio-volume/", 14)) {
        char *end; long v = strtol(path + 14, &end, 10);
        if (*end || end == path + 14 || v < 0 || v > 100) {
            strcpy(body, "Invalid audio volume\n"); reply(fd, 400, "text/plain", body); return;
        }
        snprintf(value, sizeof(value), "%ld", v);
        char *args[] = { "/usr/bin/lipc-set-prop", "com.lab126.audiomgrd", "speakerVolume", value, NULL };
        int rc = capture(args, 0, body, sizeof(body));
        reply(fd, rc ? 409 : 200, "text/plain", body); return;
    }
    if (!strncmp(path, "/sink-mode/", 11)) {
        const char *mode = path + 11;
        if (strcmp(mode, "clocked") && strcmp(mode, "unclocked") && strcmp(mode, "kinamp") && strcmp(mode, "kinamp-clocked")) {
            strcpy(body, "Invalid sink mode\n"); reply(fd, 400, "text/plain", body); return;
        }
        int f = open(RUN "/sink-mode", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (f >= 0) { write(f, mode, strlen(mode)); close(f); }
        int saved = open(BASE "/private/sink.mode", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (saved >= 0) { write(saved, mode, strlen(mode)); close(saved); }
        int marker = open(RUN "/restart-sink", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (marker >= 0) close(marker);
        strcpy(body, "Sink restart requested\n"); reply(fd, f < 0 || marker < 0 ? 500 : 202, "text/plain", body); return;
    }
    if (!strcmp(path, "/tone")) {
        int output = open(RUN "/audio.fifo", O_WRONLY | O_NONBLOCK);
        unsigned char pcm[1764];
        for (unsigned frame = 0; frame < 441; ++frame) {
            short sample = (frame / 50) % 2 ? 1000 : -1000;
            for (unsigned ch = 0; ch < 2; ++ch) {
                pcm[frame * 4 + ch * 2] = (unsigned char)sample;
                pcm[frame * 4 + ch * 2 + 1] = (unsigned char)((unsigned short)sample >> 8);
            }
        }
        unsigned count = 0; time_t end = time(NULL) + 2;
        while (output >= 0 && count < 25 && time(NULL) <= end) {
            ssize_t n = write(output, pcm, sizeof(pcm));
            if (n == sizeof(pcm)) ++count;
            else if (n < 0 && errno != EAGAIN && errno != EINTR) break;
            struct timespec step = {0, 10000000}; nanosleep(&step, NULL);
        }
        if (output >= 0) close(output);
        snprintf(body, sizeof(body), "Test tone: %u of 25 blocks sent\n", count);
        reply(fd, count == 25 ? 200 : 409, "text/plain", body); return;
    }
    if (!strcmp(path, "/audio-focus")) {
        char *args[] = { "/usr/bin/lipc-set-prop", "com.lab126.audiomgrd", "setFocus", "Music", NULL };
        int rc = capture(args, 0, body, sizeof(body));
        reply(fd, rc ? 409 : 200, "text/plain", body); return;
    }
    if (!strcmp(path, "/play")) command = "play";
    if (!strcmp(path, "/pause")) command = "pause";
    if (!strcmp(path, "/next")) command = "next";
    if (!strcmp(path, "/previous")) command = "prev";
    if (!strncmp(path, "/volume/", 8)) {
        char *end; long v = strtol(path + 8, &end, 10);
        if (*end || end == path + 8 || v < 0 || v > 100) { strcpy(body, "Invalid volume\n"); reply(fd, 400, "text/plain", body); return; }
        snprintf(value, sizeof(value), "%ld", v); command = "volume"; arg = value;
    }
    if (!strncmp(path, "/playlist/", 10)) {
        const char *id = path + 10; int valid = strlen(id) == 22;
        for (const char *p = id; *p; ++p) if (!isalnum((unsigned char)*p)) valid = 0;
        if (!valid) { strcpy(body, "Invalid playlist\n"); reply(fd, 400, "text/plain", body); return; }
        snprintf(value, sizeof(value), "spotify:playlist:%.22s", id); command = "play"; arg = value;
    }
    if (!strcmp(path, "/restart")) {
        int f = open(RUN "/restart", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (f >= 0) close(f);
        strcpy(body, "Restart requested\n"); reply(fd, f < 0 ? 500 : 202, "text/plain", body); return;
    }
    if (!strcmp(path, "/shutdown")) {
        int f = open(RUN "/stop", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (f >= 0) close(f);
        strcpy(body, "Player shutdown requested\n"); reply(fd, f < 0 ? 500 : 202, "text/plain", body); return;
    }
    if (!command) { strcpy(body, "Not found\n"); reply(fd, 404, "text/plain", body); return; }
    int rc = ctl(command, arg, 0, body, sizeof(body));
    reply(fd, rc ? 409 : 200, "text/plain", body);
}

int main(int argc, char **argv) {
    umask(0077);
    if (argc == 2 && !strcmp(argv[1], "--launch")) return launch_player();
    if (argc == 2 && !strcmp(argv[1], "--ports")) return player_ports();
    if (argc == 2 && !strcmp(argv[1], "--status")) {
        char out[65536]; int rc = ctl("status", NULL, 0, out, sizeof(out));
        redact(out); fputs(out, stdout); return rc;
    }
    if (argc != 2 || strcmp(argv[1], "--server")) return 2;
    char token[512];
    if (read_file(BASE "/private/control.token", token, sizeof(token)) < 24) return 1;
    signal(SIGPIPE, SIG_IGN);
    int server = socket(AF_INET, SOCK_STREAM, 0), yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(24880) };
    addr.sin_addr.s_addr = htonl(getenv("SPOTIFY_DEBUG_LAN") ? INADDR_ANY : INADDR_LOOPBACK);
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) || listen(server, 4)) { perror("Control socket"); return 1; }
    /* Discovery contains no credentials and exposes no control commands. */
    int discovery = socket(AF_INET, SOCK_DGRAM, 0);
    if (discovery >= 0 && bind(discovery, (struct sockaddr *)&addr, sizeof(addr))) {
        close(discovery); discovery = -1;
    }
    for (;;) {
        struct pollfd ready[2] = {{server, POLLIN, 0}, {discovery, POLLIN, 0}};
        if (poll(ready, discovery >= 0 ? 2 : 1, -1) < 0) continue;
        if (discovery >= 0 && (ready[1].revents & POLLIN)) {
            struct sockaddr_in peer; socklen_t length = sizeof(peer); char query[64];
            ssize_t count = recvfrom(discovery, query, sizeof(query), 0, (struct sockaddr *)&peer, &length);
            if (count == 17 && !memcmp(query, "KINDLE_SPOTIFY/1?", 17))
                sendto(discovery, "KINDLE_SPOTIFY/1!", 17, 0, (struct sockaddr *)&peer, length);
        }
        if (!(ready[0].revents & POLLIN)) continue;
        int fd = accept(server, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; return 1; }
        struct timeval timeout = { 3, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        handle(fd, token); close(fd);
    }
}
