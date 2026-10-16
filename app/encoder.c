#define _GNU_SOURCE
#include <errno.h>
#include <fcgiapp.h>
#include <fcgi_stdio.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define APP_SOCKET_ENV "FCGI_SOCKET_NAME"
#define MAX_BODY 4096
#define MAX_PATH PATH_MAX
#define MAX_LINE 4096
#define MAX_FRAMES 1000000
#define SD_ROOT "/var/spool/storage/SD_DISK/"
#define FFMPEG "/usr/local/packages/acap_video_encoder/lib/ffmpeg"
#define LOCK_FILE "/tmp/acap-video-encoder.lock"

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* One `key=value` from a URL query string, percent-decoded. */
static int query_string(const char *query, const char *key, char *out, size_t capacity) {
    size_t key_length = strlen(key);
    const char *at = query;
    while (at && *at) {
        if (strncmp(at, key, key_length) == 0 && at[key_length] == '=') {
            at += key_length + 1;
            size_t used = 0;
            while (*at && *at != '&') {
                char c = *at++;
                if (c == '+') {
                    c = ' ';
                } else if (c == '%') {
                    int high = hex_value(at[0]);
                    int low = high < 0 ? -1 : hex_value(at[1]);
                    if (low < 0) return -1;
                    c = (char)(high * 16 + low);
                    at += 2;
                }
                if ((unsigned char)c < 0x20 || used + 1 >= capacity) return -1;
                out[used++] = c;
            }
            out[used] = '\0';
            return 0;
        }
        at = strchr(at, '&');
        if (at) at++;
    }
    return -1;
}

static int query_integer(const char *query, const char *key, int min, int max, int *value) {
    char text[16];
    if (query_string(query, key, text, sizeof(text)) != 0) return -1;
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (end == text || *end != '\0' || parsed < min || parsed > max) return -1;
    *value = (int)parsed;
    return 0;
}

/*
 * Each check says which one failed and, for file access, the errno. The encoder runs as its
 * own ACAP user and the companion workflow writes the files as another, so "cannot read" and "outside
 * the card" need different fixes and must not share one message.
 */
static int safe_manifest(const char *path, char *resolved, size_t capacity, char *why, size_t why_size) {
    char root[PATH_MAX];
    if (!realpath(SD_ROOT, root)) {
        snprintf(why, why_size, "SD card root %s: %s", SD_ROOT, strerror(errno));
        return -1;
    }
    if (!realpath(path, resolved) || strlen(resolved) >= capacity) {
        int error = errno;
        // The folder's own mode tells file permissions apart from a sandbox refusal: a
        // folder that already allows others in, still refused, is not a chmod problem.
        char folder[PATH_MAX];
        snprintf(folder, sizeof(folder), "%s", path);
        char *slash = strrchr(folder, '/');
        if (slash) *slash = '\0';
        struct stat info;
        if (slash && stat(folder, &info) == 0) {
            snprintf(why, why_size, "manifest %s: %s (folder mode %04o owner %d:%d)", path, strerror(error),
                (unsigned)(info.st_mode & 07777), (int)info.st_uid, (int)info.st_gid);
        } else {
            snprintf(why, why_size, "manifest %s: %s (folder not readable: %s)", path, strerror(error), strerror(errno));
        }
        return -1;
    }
    size_t root_length = strlen(root);
    if (strncmp(resolved, root, root_length) != 0 || resolved[root_length] != '/') {
        snprintf(why, why_size, "manifest %s is outside the SD card %s", resolved, root);
        return -1;
    }
    struct stat info;
    if (stat(resolved, &info) != 0 || !S_ISREG(info.st_mode)) {
        snprintf(why, why_size, "manifest %s is not a regular file", resolved);
        return -1;
    }
    return 0;
}

static int validate_manifest(const char *path, int expected_frames, char *why, size_t why_size) {
    FILE *file = fopen(path, "r");
    if (!file) {
        snprintf(why, why_size, "cannot open manifest %s: %s", path, strerror(errno));
        return -1;
    }
    char root[PATH_MAX];
    if (!realpath(SD_ROOT, root)) {
        snprintf(why, why_size, "SD card root %s: %s", SD_ROOT, strerror(errno));
        fclose(file);
        return -1;
    }
    size_t root_length = strlen(root);
    char line[MAX_LINE];
    size_t frames = 0;
    int valid = fgets(line, sizeof(line), file) && strcmp(line, "ffconcat version 1.0\n") == 0;
    if (!valid) snprintf(why, why_size, "manifest has no ffconcat header");
    while (valid && fgets(line, sizeof(line), file)) {
        if (strncmp(line, "file '", 6) == 0) {
            char *end = strrchr(line + 6, '\'');
            if (!end || end[1] != '\n') { snprintf(why, why_size, "malformed file line"); valid = 0; break; }
            *end = '\0';
            char real[MAX_PATH];
            if (strlen(line + 6) >= sizeof(real) || !realpath(line + 6, real)) {
                snprintf(why, why_size, "frame %s: %s", line + 6, strerror(errno));
                valid = 0;
                break;
            }
            if (strncmp(real, root, root_length) != 0 || real[root_length] != '/') {
                snprintf(why, why_size, "frame %s is outside the SD card %s", real, root);
                valid = 0;
                break;
            }
            struct stat info;
            if (stat(real, &info) != 0 || !S_ISREG(info.st_mode)) {
                snprintf(why, why_size, "frame %s is not a regular file", real);
                valid = 0;
                break;
            }
            if (access(real, R_OK) != 0) {
                snprintf(why, why_size, "frame %s: %s", real, strerror(errno));
                valid = 0;
                break;
            }
            if (++frames > MAX_FRAMES) { snprintf(why, why_size, "too many frames"); valid = 0; break; }
        } else if (strncmp(line, "duration ", 9) == 0) {
            char *end = NULL;
            double duration = strtod(line + 9, &end);
            if (end == line + 9 || duration <= 0.0 || duration > 1.0) {
                snprintf(why, why_size, "bad duration line");
                valid = 0;
                break;
            }
        } else {
            snprintf(why, why_size, "unexpected manifest line");
            valid = 0;
        }
    }
    fclose(file);
    if (valid && frames != (size_t)expected_frames + 1) {
        snprintf(why, why_size, "manifest lists %zu files, expected %d", frames, expected_frames + 1);
        valid = 0;
    }
    return valid ? 0 : -1;
}

static void fail_request(FCGX_Request *request, int status, const char *reason, const char *message) {
    // Messages can carry paths; keep the JSON valid whatever they contain.
    char safe[1024];
    size_t used = 0;
    for (const char *c = message; *c && used + 1 < sizeof(safe); c++) {
        safe[used++] = (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20) ? '\'' : *c;
    }
    safe[used] = '\0';
    FCGX_FPrintF(request->out,
        "Status: %d %s\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n\r\n"
        "{\"error\":{\"message\":\"%s\"}}",
        status, reason, safe);
}

static void reply(FCGX_Request *request, int status, const char *reason, const char *json) {
    FCGX_FPrintF(request->out,
        "Status: %d %s\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n\r\n%s",
        status, reason, json);
}

/*
 * Where the film goes. The companion workflow names it: `<capture dir>/exports/<job>.mp4`, in a folder
 * it owns and leaves writable for us. Only a plain job name is accepted, and the folder has
 * to be on the SD card, so a request cannot point the encoder anywhere else.
 */
static int validate_output(const char *output, char *why, size_t why_size) {
    const char *name = strrchr(output, '/');
    if (!name || name == output) { snprintf(why, why_size, "output %s has no folder", output); return -1; }
    name++;
    size_t length = strlen(name);
    if (length < 5 || length > 68 || strcmp(name + length - 4, ".mp4") != 0) {
        snprintf(why, why_size, "output name %s is not <job>.mp4", name);
        return -1;
    }
    for (size_t i = 0; i < length - 4; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            snprintf(why, why_size, "output name %s has characters outside a-z 0-9 -", name);
            return -1;
        }
    }
    char folder[MAX_PATH], real[MAX_PATH], root[MAX_PATH];
    snprintf(folder, sizeof(folder), "%.*s", (int)(name - 1 - output), output);
    if (!realpath(SD_ROOT, root)) { snprintf(why, why_size, "SD card root: %s", strerror(errno)); return -1; }
    if (!realpath(folder, real)) { snprintf(why, why_size, "output folder %s: %s", folder, strerror(errno)); return -1; }
    size_t root_length = strlen(root);
    if (strncmp(real, root, root_length) != 0 || real[root_length] != '/') {
        snprintf(why, why_size, "output folder %s is outside the SD card", real);
        return -1;
    }
    if (access(real, W_OK | X_OK) != 0) {
        snprintf(why, why_size, "output folder %s: %s", real, strerror(errno));
        return -1;
    }
    return 0;
}

static void sibling(char *out, size_t size, const char *output, const char *suffix) {
    snprintf(out, size, "%s%s", output, suffix);
}

static void write_text(const char *path, const char *text) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return;
    ssize_t ignored = write(fd, text, strlen(text));
    (void)ignored;
    close(fd);
}

/*
 * The whole export, in a process of its own so the FastCGI loop can answer at once.
 *
 * Writes next to `<job>.mp4`: `.part` while encoding, `.progress` (ffmpeg's -progress
 * blocks), `.log` (ffmpeg's errors), `.pid` while ffmpeg runs, and finally `.done` holding
 * `exit=N` or `signal=N`. The MP4 only takes its real name once ffmpeg exits cleanly, so a
 * file called `<job>.mp4` is always a complete film. The companion workflow serves it from there and
 * deletes it an hour later.
 */
static void run_job(const char *manifest, const char *output, int frames, int fps, int max_height, int lowres, const char *quality) {
    char part[MAX_PATH], progress[MAX_PATH], log_path[MAX_PATH], done[MAX_PATH], pid_path[MAX_PATH];
    sibling(part, sizeof(part), output, ".part");
    sibling(progress, sizeof(progress), output, ".progress");
    sibling(log_path, sizeof(log_path), output, ".log");
    sibling(done, sizeof(done), output, ".done");
    sibling(pid_path, sizeof(pid_path), output, ".pid");
    unlink(output);
    unlink(part);
    unlink(progress);
    unlink(done);

    int crf = strcmp(quality, "high") == 0 ? 20 : strcmp(quality, "medium") == 0 ? 25 : 30;
    char frames_arg[16], crf_arg[16], fps_arg[8], video_filter[192];
    snprintf(frames_arg, sizeof(frames_arg), "%d", frames);
    snprintf(crf_arg, sizeof(crf_arg), "%d", crf);
    snprintf(fps_arg, sizeof(fps_arg), "%d", fps);
    // Frame N at exactly N/fps. The manifest's 1/fps durations are rounded, and with them
    // ffmpeg saw neighbouring frames share a timestamp and dropped about one in six.
    if (max_height) {
        snprintf(video_filter, sizeof(video_filter),
            "setpts=N/(%d*TB),scale=-2:min(%d\\,max(2\\,trunc(ih/2)*2)):flags=lanczos,format=yuv420p",
            fps, max_height);
    } else {
        snprintf(video_filter, sizeof(video_filter),
            "setpts=N/(%d*TB),scale=max(2\\,trunc(iw/2)*2):max(2\\,trunc(ih/2)*2),format=yuv420p", fps);
    }

    pid_t child = fork();
    if (child < 0) {
        write_text(done, "exit=127\n");
        return;
    }
    if (child == 0) {
        int nullfd = open("/dev/null", O_RDWR);
        if (nullfd >= 0) { dup2(nullfd, STDIN_FILENO); dup2(nullfd, STDOUT_FILENO); close(nullfd); }
        int logfd = open(log_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (logfd >= 0) { dup2(logfd, STDERR_FILENO); close(logfd); }
        // Captures keep their timing: the encode yields the CPU to everything else.
        setpriority(PRIO_PROCESS, 0, 10);
        // And their memory: if the camera runs short, the kernel stops this ffmpeg first,
        // not the companion workflow. Raising one's own score needs no privilege.
        int oom = open("/proc/self/oom_score_adj", O_WRONLY);
        if (oom >= 0) {
            ssize_t ignored = write(oom, "1000", 4);
            (void)ignored;
            close(oom);
        }
        // Measured on 4K JPEGs, four cores (peak memory, whole ffmpeg; speed against x264's
        // veryfast on one thread): 1080p superfast on three threads, 145 MB, 2.9x as fast;
        // original ultrafast on three threads, 277 MB, 2.6x. ffmpeg's defaults took 262 MB
        // and 803 MB, enough to make the kernel stop the companion workflow. One decode and one filter
        // thread, since each extra one holds whole frames; no B-frames, short or no
        // look-ahead. Files come out about twice the size — the camera has no hardware encoder
        // an app can feed photos to, so speed is bought with bytes. Three threads leave a core
        // free, and the lowered priority keeps captures first on the rest.
        const char *preset = max_height ? "superfast" : "ultrafast";
        char *x264_params = max_height
            ? "threads=3:rc-lookahead=5:sync-lookahead=0:bframes=0"
            : "threads=3";
        char *argv[] = {
            (char *)FFMPEG, "-hide_banner", "-nostdin", "-loglevel", "error", "-nostats",
            "-progress", progress, "-stats_period", "1",
            "-threads", "1", "-lowres", lowres ? "1" : "0",
            "-f", "concat", "-safe", "0", "-i", (char *)manifest, "-frames:v", frames_arg,
            "-filter_threads", "1", "-vf", video_filter, "-fps_mode", "cfr", "-r", fps_arg,
            "-c:v", "libx264", "-preset", (char *)preset, "-crf", crf_arg, "-x264-params", x264_params,
            "-pix_fmt", "yuv420p", "-movflags", "+faststart", "-f", "mp4", "-y", part, NULL
        };
        execv(FFMPEG, argv);
        _exit(127);
    }

    char pid_text[32];
    snprintf(pid_text, sizeof(pid_text), "%d\n", (int)child);
    write_text(pid_path, pid_text);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    unlink(pid_path);

    char result[32];
    if (WIFEXITED(status)) {
        if (WEXITSTATUS(status) == 0 && rename(part, output) != 0) {
            snprintf(result, sizeof(result), "exit=126\n");
        } else {
            snprintf(result, sizeof(result), "exit=%d\n", WEXITSTATUS(status));
        }
    } else {
        snprintf(result, sizeof(result), "signal=%d\n", WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    }
    if (strcmp(result, "exit=0\n") != 0) unlink(part);
    write_text(done, result);
    syslog(strcmp(result, "exit=0\n") == 0 ? LOG_INFO : LOG_ERR, "export %s finished: %s", output, result);
}

/* `action=start&manifest=…&output=…&fps=…&quality=…&maxHeight=…&frames=…`, form-encoded. */
static int start_export(FCGX_Request *request, const char *body) {
    char manifest_input[MAX_PATH], manifest[MAX_PATH], output[MAX_PATH], quality[16];
    int fps = 0, frames = 0, max_height = 0, lowres = 0;
    if (query_string(body, "manifest", manifest_input, sizeof(manifest_input)) != 0 ||
        query_string(body, "output", output, sizeof(output)) != 0 ||
        query_string(body, "quality", quality, sizeof(quality)) != 0 ||
        query_integer(body, "fps", 1, 60, &fps) != 0 ||
        query_integer(body, "frames", 2, MAX_FRAMES, &frames) != 0 ||
        query_integer(body, "maxHeight", 0, 1080, &max_height) != 0 ||
        // Optional: older packages do not send it.
        (query_integer(body, "lowres", 0, 1, &lowres) != 0 && (lowres = 0)) ||
        (max_height != 0 && max_height != 1080) ||
        (strcmp(quality, "high") && strcmp(quality, "medium") && strcmp(quality, "low")) ||
        (fps != 12 && fps != 24 && fps != 25 && fps != 30 && fps != 60)) {
        fail_request(request, 400, "Bad Request", "Invalid export settings");
        return -1;
    }
    char why[768] = "";
    if (validate_output(output, why, sizeof(why)) != 0 ||
        safe_manifest(manifest_input, manifest, sizeof(manifest), why, sizeof(why)) != 0 ||
        validate_manifest(manifest, frames, why, sizeof(why)) != 0) {
        char message[1024];
        snprintf(message, sizeof(message), "Invalid export: %s (encoder uid %d)", why, (int)getuid());
        syslog(LOG_ERR, "%s", message);
        fail_request(request, 400, "Bad Request", message);
        return -1;
    }

    // The lock is taken here and handed to the job process, which holds it until ffmpeg
    // exits. Taking it in the parent is what makes a second start refuse straight away.
    int lock = open(LOCK_FILE, O_CREAT | O_RDWR, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
        if (lock >= 0) close(lock);
        fail_request(request, 409, "Conflict", "An export is already running");
        return -1;
    }
    pid_t job = fork();
    if (job < 0) {
        close(lock);
        fail_request(request, 500, "Internal Server Error", "Could not start the export");
        return -1;
    }
    if (job == 0) {
        signal(SIGCHLD, SIG_DFL);
        setsid();
        // Drop the FastCGI sockets: holding them would keep this request open in the web
        // server for as long as the encode runs.
        for (int fd = 3; fd < 1024; fd++) {
            if (fd != lock) close(fd);
        }
        run_job(manifest, output, frames, fps, max_height, lowres, quality);
        _exit(0);
    }
    close(lock);
    syslog(LOG_INFO, "export %s started: %d frames at %d fps", output, frames, fps);
    reply(request, 202, "Accepted", "{\"ok\":true}");
    return 0;
}

/* `action=cancel&output=…` stops that export's ffmpeg. */
static int cancel_export(FCGX_Request *request, const char *body) {
    char output[MAX_PATH], why[768] = "";
    if (query_string(body, "output", output, sizeof(output)) != 0 || validate_output(output, why, sizeof(why)) != 0) {
        fail_request(request, 400, "Bad Request", why[0] ? why : "Invalid output");
        return -1;
    }
    char pid_path[MAX_PATH], text[32] = "";
    sibling(pid_path, sizeof(pid_path), output, ".pid");
    FILE *file = fopen(pid_path, "r");
    if (file) {
        if (!fgets(text, sizeof(text), file)) text[0] = '\0';
        fclose(file);
    }
    long pid = strtol(text, NULL, 10);
    // Only our own ffmpeg can be signalled: the encoder's user owns no other process.
    if (pid > 1) kill((pid_t)pid, SIGTERM);
    reply(request, 200, "OK", "{\"ok\":true}");
    return 0;
}

static int handle_request(FCGX_Request *request) {
    const char *method = FCGX_GetParam("REQUEST_METHOD", request->envp);
    if (method && strcmp(method, "GET") == 0) {
        // The companion workflow asks this to check the encoder is installed, running and this version.
        reply(request, 400, "Bad Request",
            "{\"error\":{\"message\":\"Use POST\"},\"encoder\":\"acap_video_encoder\",\"api\":3}");
        return -1;
    }
    if (!method || strcmp(method, "POST") != 0) {
        fail_request(request, 405, "Method Not Allowed", "Use POST");
        return -1;
    }
    const char *length_text = FCGX_GetParam("CONTENT_LENGTH", request->envp);
    char *end = NULL;
    long length = length_text ? strtol(length_text, &end, 10) : -1;
    if (!length_text || end == length_text || length < 1 || length > MAX_BODY) {
        fail_request(request, 400, "Bad Request", "Invalid request body size");
        return -1;
    }
    char body[MAX_BODY + 1];
    size_t received = 0;
    while (received < (size_t)length) {
        int got = FCGX_GetStr(body + received, (int)((size_t)length - received), request->in);
        if (got <= 0) break;
        received += (size_t)got;
    }
    if (received != (size_t)length) {
        fail_request(request, 400, "Bad Request", "Incomplete request body");
        return -1;
    }
    body[length] = '\0';

    char action[16];
    if (query_string(body, "action", action, sizeof(action)) != 0) {
        fail_request(request, 400, "Bad Request", "Missing action");
        return -1;
    }
    if (strcmp(action, "start") == 0) return start_export(request, body);
    if (strcmp(action, "cancel") == 0) return cancel_export(request, body);
    fail_request(request, 400, "Bad Request", "Unknown action");
    return -1;
}

int main(void) {
    openlog("acap-video-encoder", LOG_PID, LOG_USER);
    // Everything the encoder writes is read, and later deleted, by the companion workflow.
    umask(022);
    // Job processes are never waited for here; let the kernel reap them.
    signal(SIGCHLD, SIG_IGN);
    const char *socket_path = getenv(APP_SOCKET_ENV);
    if (!socket_path || FCGX_Init() != 0) return EXIT_FAILURE;
    int socket_fd = FCGX_OpenSocket(socket_path, 4);
    if (socket_fd < 0) return EXIT_FAILURE;
    chmod(socket_path, 0777);
    FCGX_Request request;
    if (FCGX_InitRequest(&request, socket_fd, 0) != 0) return EXIT_FAILURE;
    while (FCGX_Accept_r(&request) == 0) {
        handle_request(&request);
        FCGX_Finish_r(&request);
    }
    return EXIT_SUCCESS;
}
