/*
 * shell_service — I/O half of the Cooja-NG shell (see
 * include/sim/shell_service.h).  Terminal/pipe input, prompt, output
 * routing (console mask + log files), the service-host glue, the
 * end-of-run report.  Command bodies live in shell_commands.c, the
 * command stream / script engine in shell_script.c.
 */
#include "shell_internal.h"
#include "sim_runtime.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* --- wall clock (input pacing only; never influences the simulation) --- */

static double wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* --- prompt + output ---------------------------------------------------- */

static FILE *out_stream(shell_service_t *s) {
    return s->out ? s->out : stdout;
}

/* --- JSON records (--shell-json) ----------------------------------------- */

/* A JSON string.  Node output is bytes, not text: valid UTF-8 passes
 * through, any other byte is written as the Latin-1 code point it would be. */
static void json_str(FILE *f, const char *p) {
    const unsigned char *u = (const unsigned char *)p;
    fputc('"', f);
    while (*u) {
        unsigned char c = *u;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); u++; continue; }
        if (c == '\n') { fputs("\\n", f); u++; continue; }
        if (c == '\t') { fputs("\\t", f); u++; continue; }
        if (c < 0x20 || c == 0x7f) { fprintf(f, "\\u%04x", c); u++; continue; }
        if (c < 0x80) { fputc(c, f); u++; continue; }
        int n = (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
        bool ok = n > 0 && c >= 0xc2 && c <= 0xf4;
        for (int i = 1; ok && i < n; i++)
            if ((u[i] & 0xc0) != 0x80) ok = false;
        if (ok) { fwrite(u, 1, (size_t)n, f); u += n; continue; }
        fprintf(f, "\\u%04x", c);
        u++;
    }
    fputc('"', f);
}

/* {"type":"<type>","t":<sim seconds>, ...  — the caller adds fields and
 * json_end closes the record. */
static FILE *json_begin(shell_service_t *s, const char *type) {
    FILE *f = out_stream(s);
    fprintf(f, "{\"type\":\"%s\",\"t\":%.6f", type,
            (double)sim_runtime_now_ns(s->sim) / 1e9);
    return f;
}

static void json_end(FILE *f) {
    fputs("}\n", f);
}

static void json_field(FILE *f, const char *name, const char *value) {
    fprintf(f, ",\"%s\":", name);
    json_str(f, value);
}

/* shell_out text in JSON mode: one "out" record per completed line. */
static void json_text(shell_service_t *s, const char *text) {
    for (const char *p = text; *p; p++) {
        if (*p != '\n' && s->json_len < (int)sizeof(s->json_buf) - 1) {
            s->json_buf[s->json_len++] = *p;
            continue;
        }
        if (*p != '\n') continue;                 /* overlong: truncated */
        s->json_buf[s->json_len] = '\0';
        FILE *f = json_begin(s, "out");
        json_field(f, "text", s->json_buf);
        json_end(f);
        s->json_len = 0;
    }
}

void shell_json_done_start(shell_service_t *s, const char *line) {
    if (!s->json) return;
    shell_json_done_check(s, false);    /* a sourced file ended just now */
    s->done_pending = true;
    s->done_depth = s->depth;
    snprintf(s->done_line, sizeof(s->done_line), "%s", line);
    s->done_errors = s->error_count;
    s->done_fails = s->expect_fail + s->cmd_fail;
    s->done_failed = s->failed;
    s->last_error[0] = '\0';
}

/* Emit the owed "done" once the line's command (and anything it blocked
 * on or sourced) has finished; `force` = the run is ending without it. */
void shell_json_done_check(shell_service_t *s, bool force) {
    if (!s->done_pending) return;
    if (!force && (s->block != SHELL_BLOCK_NONE || s->depth > s->done_depth ||
                   s->restart_pending))
        return;
    s->done_pending = false;
    bool ok = !force && s->error_count == s->done_errors &&
              s->expect_fail + s->cmd_fail == s->done_fails &&
              s->failed == s->done_failed;
    FILE *f = json_begin(s, "done");
    json_field(f, "line", s->done_line);
    fprintf(f, ",\"ok\":%s", ok ? "true" : "false");
    if (!ok)
        json_field(f, "error", force ? "the run ended before the command finished"
                                     : s->last_error);
    json_end(f);
}

static void build_prompt(shell_service_t *s) {
    int64_t now = sim_runtime_now_ns(s->sim);
    const char *state = "";
    if (sim_control_paused(s->ctl)) state = " [paused]";
    else if (s->block == SHELL_BLOCK_EXPECT) state = " [expect]";
    else if (s->block == SHELL_BLOCK_SLEEP) state = " [sleep]";
    else if (s->block == SHELL_BLOCK_WAIT_UNTIL) state = " [wait]";
    else if (s->block == SHELL_BLOCK_RUN) state = " [run]";
    else if (s->block == SHELL_BLOCK_CMD) state = " [cmd]";
    else if (s->block == SHELL_BLOCK_EXPECT_NOT) state = " [expect-not]";
    else if (s->block == SHELL_BLOCK_FAULT) state = " [expect-fault]";
    else if (s->block == SHELL_BLOCK_HALT) state = " [expect-halt]";
    else if (s->depth > 0) state = " [script]";
    snprintf(s->prompt, sizeof(s->prompt), "cooja %.3fs%s> ",
             (double)now / 1e9, state);
    s->prompt_ns = now;
    s->ls.prompt = s->prompt;
    s->ls.plen = strlen(s->prompt);
}

static void edit_begin(shell_service_t *s) {
    if (!s->tty || s->editing) return;
    build_prompt(s);
    if (linenoiseEditStart(&s->ls, -1, -1, s->linebuf, sizeof(s->linebuf),
                           s->prompt) == 0) {
        s->editing = true;
        s->prompt_ms = wall_ms();
    } else {
        /* Not a usable terminal after all: fall back to plain lines. */
        s->tty = false;
    }
}

static void edit_end(shell_service_t *s) {
    if (!s->editing) return;
    linenoiseEditStop(&s->ls);
    s->editing = false;
}

/* Output bursts: while `hidden`, the prompt stays off the screen and every
 * line is printed plainly; one release redraws it.  Console lines printed
 * during a pump and the output of a tick's commands (including the runner's
 * own prints from add/reboot) are one burst each, instead of a
 * hide/print/show round trip per line. */
void shell_hold_output(shell_service_t *s) {
    if (s->hidden) return;
    if (s->editing) linenoiseHide(&s->ls);
    s->hidden = true;
}

void shell_release_output(shell_service_t *s) {
    if (!s->hidden) return;
    s->hidden = false;
    fflush(out_stream(s));
    if (s->editing) {
        build_prompt(s);
        linenoiseShow(&s->ls);
    }
}

void shell_out(shell_service_t *s, const char *fmt, ...) {
    va_list ap;
    if (s->json) {
        char text[SHELL_LINE_MAX * 2];
        va_start(ap, fmt);
        vsnprintf(text, sizeof(text), fmt, ap);
        va_end(ap);
        json_text(s, text);
        return;
    }
    bool redraw = s->editing && !s->hidden;
    if (redraw) linenoiseHide(&s->ls);
    va_start(ap, fmt);
    vfprintf(out_stream(s), fmt, ap);
    va_end(ap);
    if (redraw) {
        fflush(stdout);
        build_prompt(s);
        linenoiseShow(&s->ls);
    }
}

const char *shell_origin(shell_service_t *s, char *buf, size_t len) {
    snprintf(buf, len, "%s", s->origin.where[0] ? s->origin.where : "stdin");
    return buf;
}

void shell_error(shell_service_t *s, const char *fmt, ...) {
    char msg[SHELL_REASON_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    char origin[sizeof(s->origin.where)];
    shell_origin(s, origin, sizeof(origin));
    s->error_count++;
    snprintf(s->last_error, sizeof(s->last_error), "%s", msg);
    if (s->json) {
        FILE *f = json_begin(s, "error");
        json_field(f, "text", msg);
        json_field(f, "where", origin);
        json_end(f);
    } else {
        shell_out(s, "error: %s (%s)\n", msg, origin);
    }
    /* Only a script's own lines fail it — never a typo at the prompt. */
    if (s->origin.script) {
        char reason[SHELL_REASON_MAX];
        snprintf(reason, sizeof(reason), "%s: %.*s", origin,
                 (int)(sizeof(reason) - sizeof(origin) - 3), msg);
        shell_script_fail(s, reason);
    }
}

/* --- console routing ---------------------------------------------------- */

static void console_line(shell_service_t *s, int idx, int node_id,
                         const char *line, int64_t ns) {
    if (idx < 0 || idx >= SIM_EQ_MAX_NODES) return;
    if (s->hist) {
        int slot = (s->hist_head + s->hist_count) % SHELL_HISTORY_LINES;
        if (s->hist_count < SHELL_HISTORY_LINES) s->hist_count++;
        else s->hist_head = (s->hist_head + 1) % SHELL_HISTORY_LINES;
        s->hist[slot].node_id = node_id;
        s->hist[slot].ns = ns;
        snprintf(s->hist[slot].text, sizeof(s->hist[slot].text), "%s", line);
    }
    const char *type = "?";
    sim_control_node_info_t info;
    if (sim_control_describe(s->ctl, idx, &info) && info.type) type = info.type;
    /* In console mode the terminal shows only the console node's raw
     * bytes; prefixed lines would duplicate and interleave with them. */
    if (s->console_mask[idx] && s->json) {
        FILE *f = out_stream(s);
        fprintf(f, "{\"type\":\"console\",\"t\":%.6f,\"node\":%d", (double)ns / 1e9, node_id);
        json_field(f, "kind", type);
        json_field(f, "line", line);
        json_end(f);
    } else if (s->console_mask[idx] && !s->console_mode) {
        shell_hold_output(s);      /* released at the next poll */
        shell_out(s, "  %7.3f [Node %d/%s] %s\n", (double)ns / 1e9, node_id,
                  type, line);
    }
    for (int i = 0; i < s->logfile_count; i++) {
        shell_logfile_t *lf = &s->logfiles[i];
        if (!lf->f || !lf->mask[idx]) continue;
        fprintf(lf->f, "  %7.3f [Node %d/%s] %s\n", (double)ns / 1e9, node_id,
                type, line);
        fflush(lf->f);
    }
}

/* --- stdin ---------------------------------------------------------------- */

static bool fd_readable(int fd, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int rc = poll(&pfd, 1, timeout_ms);
    return rc > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR));
}

static void handle_ctrl_c(shell_service_t *s) {
    if (s->block != SHELL_BLOCK_NONE || s->depth > 0) {
        shell_out(s, "^C — script aborted\n");
        shell_script_abort(s);
        if (!s->failed) {
            s->failed = true;
            snprintf(s->fail_reason, sizeof(s->fail_reason),
                     "aborted by user (Ctrl-C)");
        }
    } else {
        shell_out(s, "^C (type exit to quit)\n");
    }
}

/* --- console mode ------------------------------------------------------------ */

static struct termios g_console_saved;

int shell_console_enter(shell_service_t *s, int idx, int node_id) {
    if (!s->tty || !s->interactive) return -1;
    shell_release_output(s);
    edit_end(s);                         /* linenoise restores cooked mode */
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &g_console_saved) != 0) return -1;
    t = g_console_saved;
    /* Line mode with local echo, like a serial terminal to a shell that
     * does not echo (Contiki-NG's does not).  ISIG off: Ctrl-C is a byte
     * for the node, not a signal that ends the simulation. */
    t.c_lflag |= (ICANON | ECHO);
    t.c_lflag &= ~ISIG;
    t.c_oflag |= OPOST;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    s->console_mode = true;
    s->console_idx = idx;
    s->console_id = node_id;
    s->inlen = 0;
    printf("[console to node %d: lines you type go to the node; ~. or Ctrl-D returns to the shell]\n",
           node_id);
    if (sim_control_paused(s->ctl))
        printf("[note: the simulation is paused; the node answers after `run`]\n");
    fflush(stdout);
    return 0;
}

static void console_leave(shell_service_t *s) {
    if (!s->console_mode) return;
    tcsetattr(STDIN_FILENO, TCSANOW, &g_console_saved);
    s->console_mode = false;
    s->inlen = 0;
    printf("\n[back in the Cooja-NG shell]\n");
    fflush(stdout);
}

static void console_send_line(shell_service_t *s, const char *line, int len) {
    char buf[SHELL_LINE_MAX + 1];
    if (len > SHELL_LINE_MAX - 1) len = SHELL_LINE_MAX - 1;
    memcpy(buf, line, (size_t)len);
    buf[len++] = '\n';
    if (!sim_control_node_active(s->ctl, s->console_idx)) {
        printf("[node %d is not running]\n", s->console_id);
        return;
    }
    int took = sim_control_send(s->ctl, s->console_id, (const uint8_t *)buf, len,
                                SIM_CONTROL_WAKE | SIM_CONTROL_RETRY);
    if (took < len)
        printf("[node %d: input truncated, %d of %d bytes queued]\n",
               s->console_id, took < 0 ? 0 : took, len);
}

/* Console mode input: whole lines from the cooked terminal. */
static void console_read(shell_service_t *s) {
    while (s->console_mode && fd_readable(STDIN_FILENO, 0)) {
        ssize_t n = read(STDIN_FILENO, s->inbuf + s->inlen,
                         sizeof(s->inbuf) - 1 - (size_t)s->inlen);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) break;
            n = 0;
        }
        if (n == 0) {                   /* Ctrl-D */
            console_leave(s);
            break;
        }
        s->inlen += (int)n;
        char *start = s->inbuf;
        char *nl;
        while (s->console_mode &&
               (nl = memchr(start, '\n', (size_t)(s->inbuf + s->inlen - start)))) {
            int len = (int)(nl - start);
            if ((len == 2 && start[0] == '~' && start[1] == '.') ||
                (len >= 1 && start[0] == 0x1d)) {
                console_leave(s);
                break;
            }
            console_send_line(s, start, len);
            start = nl + 1;
        }
        if (!s->console_mode) break;
        int rest = (int)(s->inbuf + s->inlen - start);
        if (rest >= (int)sizeof(s->inbuf) - 1) rest = 0;      /* overlong: drop */
        memmove(s->inbuf, start, (size_t)rest);
        s->inlen = rest;
    }
    if (!s->console_mode) edit_begin(s);
}

static void console_flush(shell_service_t *s) {
    if (!s->console_dirty) return;
    s->console_dirty = false;
    fflush(stdout);
}

/* Feed the editor / read the pipe until stdin runs dry. */
static void read_stdin(shell_service_t *s) {
    if (!s->interactive || s->stdin_eof) return;
    if (s->console_mode) {
        console_read(s);
        return;
    }
    if (s->tty) {
        edit_begin(s);
        int guard = 0;
        while (s->editing && fd_readable(STDIN_FILENO, 0) && guard++ < 4096) {
            char *r = linenoiseEditFeed(&s->ls);
            if (r == linenoiseEditMore) continue;
            if (r == NULL) {
                int e = errno;
                edit_end(s);
                if (e == EAGAIN) {          /* Ctrl-C */
                    handle_ctrl_c(s);
                    edit_begin(s);
                    continue;
                }
                /* Ctrl-D on an empty line, or read error: end of input. */
                s->stdin_eof = true;
                shell_enqueue_line(s, "exit");
                return;
            }
            edit_end(s);
            if (r[0]) {
                linenoiseHistoryAdd(r);
                if (s->history_path[0]) linenoiseHistorySave(s->history_path);
            }
            shell_enqueue_line(s, r);
            free(r);
            edit_begin(s);
        }
        return;
    }
    /* Pipe / file / --shell-port client: plain lines, no prompt. */
    while (fd_readable(s->in_fd, 0)) {
        if (s->inlen >= (int)sizeof(s->inbuf) - 1) {
            /* Overlong line: drop it. */
            shell_out(s, "error: input line too long, dropped\n");
            s->inlen = 0;
        }
        ssize_t n = read(s->in_fd, s->inbuf + s->inlen,
                         sizeof(s->inbuf) - 1 - (size_t)s->inlen);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) break;
            n = 0;
        }
        if (n == 0) {
            s->stdin_eof = true;
            if (s->port)
                fprintf(stderr, "shell: client on port %d disconnected; ending the run\n",
                        s->port);
            if (s->inlen > 0) {           /* unterminated last line */
                s->inbuf[s->inlen] = '\0';
                shell_enqueue_line(s, s->inbuf);
                s->inlen = 0;
            }
            shell_enqueue_line(s, "exit");
            return;
        }
        s->inlen += (int)n;
        s->inbuf[s->inlen] = '\0';
        char *start = s->inbuf;
        char *nl;
        while ((nl = memchr(start, '\n', (size_t)(s->inbuf + s->inlen - start)))) {
            *nl = '\0';
            if (nl > start && nl[-1] == '\r') nl[-1] = '\0';
            shell_enqueue_line(s, start);
            start = nl + 1;
        }
        int rest = (int)(s->inbuf + s->inlen - start);
        memmove(s->inbuf, start, (size_t)rest);
        s->inlen = rest;
    }
}

void shell_enqueue_line(shell_service_t *s, const char *line) {
    /* "!cmd" at a terminal: run now, if the command is immediate-safe.  A
     * pipe is sequential (sync_stdin): its "!" lines queue in order and
     * run like any other line. */
    if (line[0] == '!' && !s->sync_stdin) {
        shell_transcript_record(s, line);
        shell_origin_t o = { .kind = SHELL_ORIGIN_STDIN, .script = false, .where = "stdin" };
        shell_exec_line(s, line + 1, true, &o);
        return;
    }
    if (s->qcount >= SHELL_QUEUE_MAX) {
        if (!s->queue_warned) {
            shell_out(s, "error: input queue full (%d lines), dropping input\n",
                      SHELL_QUEUE_MAX);
            s->queue_warned = true;
        }
        return;
    }
    int slot = (s->qhead + s->qcount) % SHELL_QUEUE_MAX;
    snprintf(s->queue[slot], SHELL_LINE_MAX, "%s", line);
    s->qcount++;
}

/* Called from the engine (shell_script.c) to fetch a stdin line. */
const char *shell_dequeue_line(shell_service_t *s, char *buf, size_t len);
const char *shell_dequeue_line(shell_service_t *s, char *buf, size_t len) {
    if (s->qcount <= 0) return NULL;
    snprintf(buf, len, "%s", s->queue[s->qhead]);
    s->qhead = (s->qhead + 1) % SHELL_QUEUE_MAX;
    s->qcount--;
    s->queue_warned = false;
    return buf;
}

/* --- signals ---------------------------------------------------------------- */

static volatile sig_atomic_t g_shell_signal = 0;

static void shell_on_signal(int sig) {
    g_shell_signal = sig;
}

static void install_signals(bool on) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    if (on) {
        sa.sa_handler = shell_on_signal;
        /* No SA_RESTART: a blocking poll on stdin returns EINTR.  A second
         * signal gets the default action, so a stuck run can still be
         * killed. */
        sa.sa_flags = SA_RESETHAND;
    } else {
        sa.sa_handler = SIG_DFL;
    }
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

bool shell_check_signal(shell_service_t *s) {
    int sig = g_shell_signal;
    if (!sig) return false;
    g_shell_signal = 0;
    shell_out(s, "\n%s: ending the run (reports and --save-config still run)\n",
              sig == SIGINT ? "SIGINT" : "SIGTERM");
    s->exited = true;
    sim_control_request_exit(s->ctl);
    return true;
}

/* sync_stdin: block until piped stdin yields at least one line, or EOF. */
void shell_read_stdin_sync(shell_service_t *s) {
    while (s->interactive && !s->stdin_eof && s->qcount == 0) {
        struct pollfd pfd = { .fd = s->in_fd, .events = POLLIN };
        fflush(out_stream(s));
        int rc = poll(&pfd, 1, -1);
        if (rc < 0) {
            if (errno == EINTR) {
                if (shell_check_signal(s)) return;
                continue;
            }
            s->stdin_eof = true;
            shell_enqueue_line(s, "exit");
            return;
        }
        read_stdin(s);
    }
}

/* Cheap gate: one poll(2) on stdin at most every 20 ms of wall time, with
 * the clock read only every 1024 loop iterations or 10 ms of sim time.
 * Terminal only: piped stdin is read synchronously by the engine. */
static void gated_read_stdin(shell_service_t *s) {
    if (!s->interactive || s->sync_stdin) return;
    int64_t now = sim_runtime_now_ns(s->sim);
    if (((++s->iter) & 1023) != 0 &&
        now - s->last_poll_sim_ns < 10 * SHELL_MS_TO_NS)
        return;
    double t = wall_ms();
    if (t - s->last_poll_ms < 20.0) return;
    s->last_poll_ms = t;
    s->last_poll_sim_ns = now;
    shell_release_output(s);
    read_stdin(s);
    /* Keep the prompt's clock fresh (once a second, only when it changed). */
    if (s->editing && !s->hidden && t - s->prompt_ms >= 1000.0 && s->prompt_ns != now) {
        s->prompt_ms = t;
        linenoiseHide(&s->ls);
        build_prompt(s);
        linenoiseShow(&s->ls);
    }
}

void shell_service_poll_input(shell_service_t *s) {
    if (!shell_service_active(s)) return;
    gated_read_stdin(s);
}

static const char *block_name(shell_block_t b) {
    switch (b) {
    case SHELL_BLOCK_EXPECT:     return "expect";
    case SHELL_BLOCK_SLEEP:      return "sleep";
    case SHELL_BLOCK_WAIT_UNTIL: return "wait-until";
    case SHELL_BLOCK_RUN:        return "run";
    case SHELL_BLOCK_CMD:        return "cmd";
    case SHELL_BLOCK_EXPECT_NOT: return "expect-not";
    case SHELL_BLOCK_FAULT:      return "expect-fault";
    case SHELL_BLOCK_HALT:       return "expect-halt";
    default:                     return "nothing";
    }
}

void shell_service_pump_paused(shell_service_t *s, int timeout_ms) {
    if (!shell_service_active(s)) return;
    if (shell_check_signal(s)) return;
    shell_release_output(s);
    console_flush(s);
    if (s->interactive && !s->stdin_eof) {
        if (s->tty) edit_begin(s);
        if (fd_readable(s->in_fd, timeout_ms)) read_stdin(s);
    } else if (timeout_ms > 0) {
        usleep((useconds_t)timeout_ms * 1000);
    }
    shell_script_tick(s);

    /* Nothing advances simulated time while paused, so a stream blocked on
     * time or on a console line can only be released by a resume.  If
     * nothing can resume — no terminal input left (a pipe is sequential:
     * its lines queue behind the block), no web UI — that is a deadlock:
     * fail it instead of hanging. */
    if (sim_control_paused(s->ctl) && !sim_runtime_stop_requested(s->sim)) {
        bool can_resume = (s->tty && !s->stdin_eof) || s->external_resume;
        bool time_block = s->block == SHELL_BLOCK_SLEEP ||
                          s->block == SHELL_BLOCK_WAIT_UNTIL ||
                          s->block == SHELL_BLOCK_EXPECT ||
                          s->block == SHELL_BLOCK_CMD ||
                          s->block == SHELL_BLOCK_EXPECT_NOT ||
                          s->block == SHELL_BLOCK_FAULT ||
                          s->block == SHELL_BLOCK_HALT;
        if (time_block && !can_resume) {
            char reason[SHELL_REASON_MAX];
            snprintf(reason, sizeof(reason),
                     "%s blocks the command stream while the simulation is "
                     "paused, and nothing can resume it (deadlock)",
                     block_name(s->block));
            shell_script_fail(s, reason);
            if (!s->interactive) sim_control_request_exit(s->ctl);
        } else if (time_block && s->tty && !s->paused_hint) {
            s->paused_hint = true;
            shell_out(s, "note: %s is blocked while the simulation is paused; "
                      "type !run to continue\n", block_name(s->block));
        } else if (!s->interactive && !s->external_resume &&
                   s->block == SHELL_BLOCK_NONE && s->qcount == 0 &&
                   s->depth == 0) {
            /* --script alone, script done, paused: nothing will ever run. */
            shell_out(s, "note: the simulation is paused and no input is left; ending the run\n");
            sim_control_request_exit(s->ctl);
        }
    } else {
        s->paused_hint = false;
    }

    shell_release_output(s);
    if (s->editing && s->prompt_ns != sim_runtime_now_ns(s->sim)) {
        linenoiseHide(&s->ls);
        build_prompt(s);
        linenoiseShow(&s->ls);
    }
}

void shell_service_on_autopause(shell_service_t *s) {
    if (!shell_service_active(s)) return;
    char t[32];
    shell_format_time(sim_runtime_now_ns(s->sim), t, sizeof(t));
    if (!s->interactive) return;
    if (s->block == SHELL_BLOCK_RUN)      /* the user's own run/step */
        shell_out(s, "paused at %s\n", t);
    else                                  /* the -t horizon */
        shell_out(s, "-t duration reached: paused at %s (run to continue, exit to quit)\n", t);
}

/* --- service ops --------------------------------------------------------- */

static void shell_poll(sim_runtime_t *sim, void *state) {
    (void)sim;
    shell_service_t *s = (shell_service_t *)state;
    if (!s->active) return;
    if (shell_check_signal(s)) return;
    shell_release_output(s);          /* console lines from the last pump */
    console_flush(s);
    if (!s->started) {
        s->started = true;
        if (s->tty) edit_begin(s);
    }
    gated_read_stdin(s);
    shell_script_tick(s);
    shell_release_output(s);
}

static void shell_on_event(sim_runtime_t *sim, void *state,
                           const sim_observer_event_t *ev) {
    (void)sim;
    shell_service_t *s = (shell_service_t *)state;
    if (!s->active) return;
    if (ev->kind == SIM_OBS_MOTE_UART_BYTE) {
        if (s->console_mode && ev->mote_index == s->console_idx && ev->u.uart.byte != '\r') {
            putchar(ev->u.uart.byte);
            s->console_dirty = true;
        }
        shell_script_on_uart_byte(s, ev->mote_index, ev->u.uart.byte, ev->time_ns);
        return;
    }
    if (ev->kind != SIM_OBS_MOTE_LOG_LINE) return;
    console_line(s, ev->mote_index, ev->u.log_line.node_id,
                 ev->u.log_line.line, ev->time_ns);
    shell_script_on_log_line(s, ev->mote_index, ev->u.log_line.node_id,
                             ev->u.log_line.line, ev->time_ns);
}

void shell_transcript_record(shell_service_t *s, const char *line) {
    if (!s->transcript) return;
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || !strncmp(p, "transcript", 10)) return;
    fprintf(s->transcript, "%s\n", line);
    fflush(s->transcript);
}

static void close_logfiles(shell_service_t *s) {
    for (int i = 0; i < s->logfile_count; i++) {
        if (s->logfiles[i].f) { fclose(s->logfiles[i].f); s->logfiles[i].f = NULL; }
    }
    s->logfile_count = 0;
}

static void shell_destroy(sim_runtime_t *sim, void *state) {
    (void)sim;
    shell_service_t *s = (shell_service_t *)state;
    if (!s->active) return;
    shell_release_output(s);
    console_leave(s);
    edit_end(s);
    if (s->history_path[0]) linenoiseHistorySave(s->history_path);
    close_logfiles(s);
    if (s->transcript) { fclose(s->transcript); s->transcript = NULL; }
    if (s->out) { fclose(s->out); s->out = NULL; }
    if (s->in_fd > 0) { close(s->in_fd); s->in_fd = 0; }
    free(s->hist);
    s->hist = NULL;
    shell_script_abort(s);
    install_signals(false);
    s->active = false;
}

const sim_service_ops_t shell_service_ops = {
    .name     = "shell",
    .init     = NULL,          /* adopt the runner-constructed struct */
    .destroy  = shell_destroy,
    .on_event = shell_on_event,
    .poll     = shell_poll,
};

/* --- lifecycle ------------------------------------------------------------ */

/* --shell-port: listen on 127.0.0.1 and wait for one client.  The
 * simulation has not started, so when the client connects cannot change
 * what it sees.  Returns the connected socket, or -1. */
static int accept_client(shell_service_t *s, int port) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("--shell-port: socket"); return -1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    socklen_t alen = sizeof(a);
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 1) != 0 ||
        getsockname(ls, (struct sockaddr *)&a, &alen) != 0) {
        fprintf(stderr, "--shell-port %d: %s\n", port, strerror(errno));
        close(ls);
        return -1;
    }
    s->port = ntohs(a.sin_port);
    printf("shell: listening on 127.0.0.1:%d; the simulation starts when a client connects\n",
           s->port);
    fflush(stdout);
    int fd = -1;
    while (fd < 0) {
        fd = accept(ls, NULL, NULL);
        if (fd < 0 && errno != EINTR) { perror("--shell-port: accept"); break; }
        if (fd < 0 && g_shell_signal) break;      /* Ctrl-C while waiting */
    }
    close(ls);
    return fd;
}

int shell_service_start(shell_service_t *s, sim_runtime_t *sim,
                        sim_control_t *ctl, bool interactive,
                        const char *script_path, bool verbose,
                        int port, bool json) {
    memset(s, 0, sizeof(*s));
    s->sim = sim;
    s->ctl = ctl;
    s->active = true;
    s->interactive = interactive || port >= 0;
    interactive = s->interactive;
    s->verbose = verbose;
    s->json = json;
    install_signals(true);
    if (port >= 0) {
        int fd = accept_client(s, port);
        if (fd < 0) return -1;
        signal(SIGPIPE, SIG_IGN);       /* a vanished client reads as EOF */
        s->in_fd = fd;
        s->out = fdopen(dup(fd), "w");
        if (!s->out) { close(fd); return -1; }
        setvbuf(s->out, NULL, _IOLBF, 0);
    }
    /* Line editing only for a person at a terminal: a program on a port or
     * reading JSON gets plain, sequential lines like a pipe. */
    s->tty = interactive && port < 0 && !json &&
             isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    s->sync_stdin = interactive && !s->tty;
    s->origin.kind = SHELL_ORIGIN_STDIN;
    snprintf(s->origin.where, sizeof(s->origin.where), "stdin");
    s->next_at_id = 1;
    s->next_dbg_id = 1;
    s->default_expect_timeout_ns = 30LL * 1000 * SHELL_MS_TO_NS;
    s->max_line = 128;   /* Contiki-NG SERIAL_LINE_CONF_BUFSIZE default */
    s->hist = calloc(SHELL_HISTORY_LINES, sizeof(*s->hist));
    snprintf(s->prompt_glob, sizeof(s->prompt_glob), "#*> ");  /* Contiki-NG */
    s->stop_when_done = !interactive;
    memset(s->console_mask, verbose ? 1 : 0, sizeof(s->console_mask));
    shell_script_init(s);

    /* Pipes: make command echo / prompts visible promptly. */
    if (!s->tty) setvbuf(stdout, NULL, _IOLBF, 0);

    if (s->tty) {
        const char *hp = getenv("CSIM_SHELL_HISTORY");
        if (hp) {
            snprintf(s->history_path, sizeof(s->history_path), "%s", hp);
        } else {
            const char *home = getenv("HOME");
            if (home && home[0])
                snprintf(s->history_path, sizeof(s->history_path),
                         "%s/.cooja-ng_history", home);
        }
        if (s->history_path[0]) {
            linenoiseHistorySetMaxLen(500);
            linenoiseHistoryLoad(s->history_path);
        }
        linenoiseSetCompletionCallback(shell_complete);
    }
    if (script_path && script_path[0]) {
        if (shell_script_source(s, script_path) != 0) {
            fprintf(stderr, "--script: cannot open %s\n", script_path);
            return -1;
        }
    }
    if (json) {
        FILE *f = json_begin(s, "hello");
        fprintf(f, ",\"protocol\":1,\"interactive\":%s", interactive ? "true" : "false");
        json_end(f);
    } else if (interactive) {
        shell_out(s, "Cooja-NG shell: type 'help' for commands%s\n",
                  s->tty ? "" : " (no terminal: lines run in order, like a script)");
    }
    return 0;
}

void shell_service_on_restart(shell_service_t *s) {
    if (!shell_service_active(s)) return;
    /* A restart the stream itself asked for keeps the stream: stdin lines
     * after `restart` run against the new simulation.  Script files are
     * aborted — their state described the old run. */
    shell_script_abort(s);
    shell_script_at_remove(s, -1);
    s->trigger_count = 0;
    s->triggers_dropped = 0;
    s->restart_pending = false;
}

/* The results block goes to stdout, and to a --shell-port client too. */
static void report_line(shell_service_t *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (s->out && !s->json) {
        va_start(ap, fmt);
        vfprintf(s->out, fmt, ap);
        va_end(ap);
    }
}

static int report_verdict(shell_service_t *s, int64_t now_ns);

int shell_service_report(shell_service_t *s, int64_t now_ns) {
    if (!shell_service_active(s)) return 0;
    shell_release_output(s);
    edit_end(s);
    shell_json_done_check(s, true);
    int rc = report_verdict(s, now_ns);
    if (s->json) {
        FILE *f = json_begin(s, "result");
        fprintf(f, ",\"status\":%d", rc);
        if (s->script_used) {
            fprintf(f, ",\"verdict\":\"%s\"", s->failed ? "failed" : "passed");
            if (s->failed) json_field(f, "reason", s->fail_reason);
            fprintf(f, ",\"expects\":{\"passed\":%d,\"failed\":%d}", s->expect_pass, s->expect_fail);
            fprintf(f, ",\"cmds\":{\"passed\":%d,\"failed\":%d}", s->cmd_pass, s->cmd_fail);
        }
        json_end(f);
    }
    fflush(out_stream(s));
    return rc;
}

static int report_verdict(shell_service_t *s, int64_t now_ns) {
    if (!s->script_used) return s->exit_code_set ? s->exit_code : 0;
    /* A blocked command or an unfinished script file at the end of the run
     * is a failure however the run ended — duration reached, `exit` typed
     * at the prompt, or a signal.  (`exit` inside a script file closes the
     * script first, so it counts as finished.)  Without either, an
     * interactive session that ended with `exit` is not a failure. */
    bool in_flight = s->block != SHELL_BLOCK_NONE || s->depth > 0;
    if (!s->failed && !s->passed &&
        (in_flight || (!s->exited && !s->finished))) {
        s->failed = true;
        if (s->block == SHELL_BLOCK_EXPECT)
            snprintf(s->fail_reason, sizeof(s->fail_reason),
                     "script did not complete: still waiting for \"%s\"",
                     s->expect_pattern);
        else if (s->block == SHELL_BLOCK_CMD)
            snprintf(s->fail_reason, sizeof(s->fail_reason),
                     "script did not complete: cmd %d \"%.60s\" still waiting for the prompt",
                     s->cmd_id, s->cmd_text);
        else if (s->block != SHELL_BLOCK_NONE)
            snprintf(s->fail_reason, sizeof(s->fail_reason),
                     "script did not complete: blocked at %.3f s",
                     (double)s->block_deadline_ns / 1e9);
        else
            snprintf(s->fail_reason, sizeof(s->fail_reason),
                     "script did not complete");
    }
    report_line(s, "\n--- Script Results ---\n");
    report_line(s, "  expects: %d passed, %d failed\n", s->expect_pass, s->expect_fail);
    if (s->cmd_pass || s->cmd_fail)
        report_line(s, "  cmds:    %d passed, %d failed\n", s->cmd_pass, s->cmd_fail);
    if (!s->failed) {
        report_line(s, "\n  SCRIPT PASSED (%lld ms simulated)\n",
                    (long long)(now_ns / SHELL_MS_TO_NS));
        return s->exit_code_set ? s->exit_code : 0;
    }
    report_line(s, "\n  SCRIPT FAILED: %s\n", s->fail_reason);
    return s->exit_code_set ? s->exit_code : 1;
}
