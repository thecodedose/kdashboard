#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef RUN
#define RUN "/var/tmp/kindle-spotify"
#endif
#ifndef GST_LAUNCH
#define GST_LAUNCH "/usr/bin/gst-launch-0.10"
#endif

/* Kindle audiomgrd can leave A2DP suspended after an idle mixer. Keep one
 * native stream continuously fed, substituting silence whenever PCM is absent.
 * A small jitter buffer separates the ALSA producer from the native clock. */
static volatile sig_atomic_t stopped;
static void stop_mixer(int signal_number) { (void)signal_number; stopped = 1; }
static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}

int main(int argc, char **argv) {
    const char *mode = argc == 2 ? argv[1] : "clocked";
    int bare = !strcmp(mode, "kinamp") || !strcmp(mode, "kinamp-clocked");
    int unclocked = !strcmp(mode, "unclocked") || !strcmp(mode, "kinamp");
    if (argc > 2 || (!bare && strcmp(mode, "clocked") && strcmp(mode, "unclocked"))) return 2;
    struct sigaction action = {0}; action.sa_handler = stop_mixer;
    sigaction(SIGTERM, &action, NULL); sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    int input = open(RUN "/audio.fifo", O_RDWR | O_NONBLOCK | O_CLOEXEC), pipefd[2];
    if (input < 0 || pipe(pipefd)) { perror("Native mixer input"); return 1; }
    pid_t gst = fork();
    if (gst == 0) {
        setpgid(0, 0);
        close(pipefd[1]); dup2(pipefd[0], STDIN_FILENO); close(pipefd[0]);
        char *args[24] = { GST_LAUNCH, "-q", "fdsrc", "fd=0", "blocksize=1764", "!",
            "audio/x-raw-int,endianness=1234,signed=true,width=16,depth=16,rate=44100,channels=2", "!",
            "queue", "max-size-buffers=4", "max-size-bytes=0", "max-size-time=100000000", "!",
            "mixersink", "name=kindle_spotify_sink", NULL };
        unsigned arg = 0; while (args[arg]) ++arg;
        if (!bare) args[arg++] = "stream-type=Music";
        args[arg++] = unclocked ? "sync=false" : "sync=true";
        args[arg] = NULL;
        execv(args[0], args); perror("Native GStreamer"); _exit(127);
    }
    close(pipefd[0]);
    if (gst < 0) { close(input); close(pipefd[1]); return 1; }
    setpgid(gst, gst);
    fcntl(pipefd[1], F_SETFL, O_NONBLOCK);
    unsigned char ring[32768], frame[1764];
    size_t head = 0, queued = 0; int primed = 0, result = 0;
    uint64_t audio = 0, silence = 0, ticks = 0, next = now_ns();
    fputs("Native mixer keepalive started; 40 ms PCM jitter buffer.\n", stderr);
    while (!stopped) {
        int status;
        if (waitpid(gst, &status, WNOHANG) == gst) { result = 1; gst = -1; break; }
        while (queued < sizeof(ring)) {
            size_t tail = (head + queued) % sizeof(ring);
            size_t room = sizeof(ring) - queued;
            if (room > sizeof(ring) - tail) room = sizeof(ring) - tail;
            ssize_t n = read(input, ring + tail, room);
            if (n <= 0) break;
            queued += (size_t)n;
        }
        if (queued >= sizeof(frame) * 4) primed = 1;
        struct pollfd output = {pipefd[1], POLLOUT, 0};
        if (poll(&output, 1, 0) > 0 && (output.revents & POLLOUT)) {
            memset(frame, 0, sizeof(frame));
            size_t used = primed ? queued : 0;
            if (used > sizeof(frame)) used = sizeof(frame);
            for (size_t i = 0; i < used; ++i) frame[i] = ring[(head + i) % sizeof(ring)];
            ssize_t n = write(pipefd[1], frame, sizeof(frame));
            if (n == sizeof(frame)) {
                head = (head + used) % sizeof(ring); queued -= used;
                audio += used; silence += sizeof(frame) - used;
                if (!queued) primed = 0;
            } else if (n < 0 && errno != EAGAIN && errno != EINTR) { result = 1; break; }
        }
        if (++ticks % 100 == 0) {
            FILE *f = fopen(RUN "/mixer.stats", "w");
            if (f) {
                fprintf(f, "audio_bytes=%llu\nsilence_bytes=%llu\nqueued_bytes=%zu\n",
                    (unsigned long long)audio, (unsigned long long)silence, queued);
                fclose(f);
            }
        }
        next += 10000000ULL;
        uint64_t current = now_ns();
        if (next + 50000000ULL < current) next = current;
        struct timespec deadline = {(time_t)(next / 1000000000ULL), (long)(next % 1000000000ULL)};
        while (!stopped && clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL) == EINTR) {}
    }
    close(input); close(pipefd[1]);
    if (gst > 0) {
        kill(-gst, SIGTERM); kill(gst, SIGTERM);
        struct timespec step = {0, 10000000};
        unsigned attempt = 0;
        while (waitpid(gst, NULL, WNOHANG) == 0 && attempt++ < 20) nanosleep(&step, NULL);
        if (attempt >= 20) { kill(-gst, SIGKILL); kill(gst, SIGKILL); waitpid(gst, NULL, 0); }
    }
    return result;
}
