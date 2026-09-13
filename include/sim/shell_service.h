/*
 * shell_service — the Cooja-NG command shell and script engine as a kernel
 * service (docs/shell.md).
 *
 * One command stream feeds three inputs — interactive stdin (--shell; a
 * linenoise line editor on a TTY, plain lines from a pipe), script files
 * (--script FILE at start, `source FILE` later, nested), and the `at`
 * queue — into one executor whose commands act through sim_control.
 * Blocking commands (`expect`, `sleep`, `wait-until`) hold the stream
 * until a log line matches or a simulation instant is reached; the engine
 * is a state machine resumed from the service host's poll() at every slice
 * boundary, never a thread, so script-driven runs are deterministic.
 *
 * Wiring (test/test_mixed_multinode.c): attached right after the json-test
 * service — BEFORE the motes boot, so boot-time console lines route through
 * the shell's console mask like every other line; polled through
 * sim_service_poll_all; while the simulation is paused the loop calls
 * shell_service_pump_paused() so input is still read.
 *
 * Programs: --shell-port N moves the command stream onto one TCP client
 * (read like a pipe, so the run is deterministic) and --shell-json turns
 * the shell's output into one JSON object per line, with a "done" record
 * after each command line finishes.
 *
 * Console lines: when the shell is active the runner's own per-line printf
 * is off and this service prints the identical "  %7.3f [Node id/KIND] text"
 * line for every node in the console mask (default: all on, all off under
 * -q), plus into any `log-file`.
 */
#ifndef SHELL_SERVICE_H
#define SHELL_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "sim_service.h"
#include "sim_control.h"
#include "sim_event_queue.h"   /* SIM_EQ_MAX_NODES */
#include "linenoise.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SHELL_LINE_MAX      1024
#define SHELL_QUEUE_MAX     32
#define SHELL_SOURCE_DEPTH  8
#define SHELL_ATQ_MAX       64
#define SHELL_LOGFILES_MAX  8
#define SHELL_WATCH_MAX     16
#define SHELL_TRIGGER_MAX   16
#define SHELL_PATTERN_MAX   256
#define SHELL_PATH_MAX      512
#define SHELL_REASON_MAX    512
#define SHELL_VARS_MAX      32
#define SHELL_VAR_NAME_MAX  32
#define SHELL_FRAMES_MAX    8
#define SHELL_HISTORY_LINES 2000
#define SHELL_HISTORY_LEN   200
#define SHELL_DBG_MAX       16

/* Where a command line came from.  Errors fail the script only for lines
 * a script owns: FILE lines, and at/every/on commands a script file
 * scheduled (`script`).  A typo at the prompt never fails a script. */
typedef enum shell_origin_kind {
    SHELL_ORIGIN_STDIN = 0,   /* typed or piped                              */
    SHELL_ORIGIN_FILE,        /* a --script / source file line               */
    SHELL_ORIGIN_AT,          /* a due at/every entry                        */
    SHELL_ORIGIN_ON,          /* an `on` watch firing                        */
} shell_origin_kind_t;

typedef struct shell_origin {
    shell_origin_kind_t kind;
    bool script;              /* owned by a script file (errors fail it)     */
    char where[128];          /* "file.cnsh:12", "stdin", "at #3 (f:4)"      */
} shell_origin_t;

typedef struct shell_source {
    FILE *f;
    char  path[SHELL_PATH_MAX];
    int   lineno;
    /* sendfile: each line goes to a node as a `cmd` instead of running as
     * a shell command.  send_idx < 0 for a script. */
    int     send_idx, send_id;
    int64_t send_timeout_ns;
    /* repeat / if blocks open in this file. */
    struct {
        int  kind;                  /* 1 = repeat, 2 = if                  */
        long pos;                   /* repeat: file offset of the body      */
        int  lineno;                /* repeat: line number before the body  */
        long count, total;          /* repeat: current pass, passes         */
        char var[SHELL_VAR_NAME_MAX];
    } frames[SHELL_FRAMES_MAX];
    int     nframes;
} shell_source_t;

typedef struct shell_dbg {           /* one breakpoint or watchpoint */
    int      id;
    int      node_id;
    int      kind;                   /* 1 = breakpoint, 2 = watchpoint */
    uint32_t addr;
    int      len;                    /* watchpoint bytes, 1..4 */
    int      hits;
} shell_dbg_t;

typedef struct shell_hline {        /* one remembered console line */
    int     node_id;
    int64_t ns;
    char    text[SHELL_HISTORY_LEN];
} shell_hline_t;

typedef struct shell_var {
    char name[SHELL_VAR_NAME_MAX];
    char value[SHELL_LINE_MAX];
} shell_var_t;

typedef enum shell_block {
    SHELL_BLOCK_NONE = 0,
    SHELL_BLOCK_EXPECT,
    SHELL_BLOCK_SLEEP,
    SHELL_BLOCK_WAIT_UNTIL,
    SHELL_BLOCK_RUN,        /* `run <dur>` / `step`: until the auto-pause */
    SHELL_BLOCK_CMD,        /* `cmd`: until the node prints its prompt    */
    SHELL_BLOCK_EXPECT_NOT, /* `expect-not`: fail on a match, pass at time */
    SHELL_BLOCK_FAULT,      /* `expect-fault`: until a node takes a fault */
    SHELL_BLOCK_HALT,       /* `expect-halt`: until a breakpoint/watchpoint hits */
} shell_block_t;

typedef struct shell_at_entry {
    int     id;
    int64_t at_ns;
    int64_t period_ns;     /* > 0: `every` — re-armed after each run */
    char    cmd[SHELL_LINE_MAX];
    shell_origin_t origin; /* where the `at`/`every` line was typed         */
} shell_at_entry_t;

typedef struct shell_logfile {
    FILE   *f;
    char    path[SHELL_PATH_MAX];
    uint8_t mask[SIM_EQ_MAX_NODES];   /* by slot index */
} shell_logfile_t;

typedef enum shell_watch_kind {
    SHELL_WATCH_COUNT = 1,   /* `count "pat" [sel]`   → assert count   */
    SHELL_WATCH_FAIL,        /* `fail-on "pat" [sel]` → script fails   */
    SHELL_WATCH_RUN,         /* `on sel "pat" cmd`    → run cmd        */
} shell_watch_kind_t;

typedef struct shell_watch {
    shell_watch_kind_t kind;
    char    pattern[SHELL_PATTERN_MAX];
    bool    any;
    int     ids[SIM_EQ_MAX_NODES];
    int     nids;
    int     count;
    char    cmd[SHELL_LINE_MAX];
    shell_origin_t origin; /* where the `on`/`fail-on`/`count` line was typed */
    bool    once;          /* `on --once`: removed after it fires            */
    bool    dead;          /* fired once; compacted at the next tick         */
} shell_watch_t;

typedef struct shell_trigger {
    char cmd[SHELL_LINE_MAX];
    shell_origin_t origin;
} shell_trigger_t;

typedef struct shell_service {
    sim_runtime_t *sim;
    sim_control_t *ctl;
    bool active;            /* --shell or --script                         */
    bool interactive;       /* --shell: stdin is a command source          */
    bool tty;               /* stdin and stdout are a terminal             */
    /* Non-terminal stdin is read synchronously: when the command stream is
     * idle the simulation waits for the next line, so a piped session runs
     * exactly like a script (deterministic).  = interactive && !tty. */
    bool sync_stdin;
    /* Something outside the shell can resume a paused run (the web UI). */
    bool external_resume;
    bool verbose;           /* runner's -v/-q (console mask default, echo) */
    bool started;           /* first tick done (editor started lazily)     */

    /* linenoise multiplexing state (tty only). */
    struct linenoiseState ls;
    char   linebuf[SHELL_LINE_MAX];
    bool   editing;
    char   prompt[96];
    bool   hidden;          /* prompt hidden for a burst of output          */
    bool   paused_hint;     /* "blocked while paused" hint already shown    */
    char   history_path[SHELL_PATH_MAX];
    int64_t prompt_ns;      /* sim time the prompt currently shows          */
    double  prompt_ms;      /* wall time of the last prompt refresh         */

    /* --shell-port / --shell-json.  in_fd 0 = stdin; out NULL = stdout. */
    int    in_fd;
    int    port;            /* listening port, 0 = none                    */
    FILE  *out;
    bool   json;
    char   json_buf[SHELL_LINE_MAX];   /* shell_out text up to its newline  */
    int    json_len;
    int    error_count;     /* shell_error calls, for "done" ok             */
    char   last_error[SHELL_REASON_MAX];
    /* The stdin line whose "done" record is still owed: emitted once the
     * stream is idle again at (or above) the depth it ran at. */
    bool   done_pending;
    int    done_depth;
    char   done_line[SHELL_LINE_MAX];
    int    done_errors, done_fails;
    bool   done_failed;

    /* Raw stdin (non-tty). */
    char   inbuf[4096];
    int    inlen;
    bool   stdin_eof;

    /* Lines typed/piped but not yet executed (a script or block holds the
     * stream).  Ring buffer. */
    char   queue[SHELL_QUEUE_MAX][SHELL_LINE_MAX];
    int    qhead, qcount;
    bool   queue_warned;

    /* stdin poll gate (wall clock, see shell_service.c). */
    uint32_t iter;
    double   last_poll_ms;
    int64_t  last_poll_sim_ns;

    /* Console routing. */
    uint8_t console_mask[SIM_EQ_MAX_NODES];   /* by slot index */
    shell_logfile_t logfiles[SHELL_LOGFILES_MAX];
    int    logfile_count;

    /* Scheduled commands (`at`, `every`). */
    shell_at_entry_t atq[SHELL_ATQ_MAX];
    int    atq_count;
    int    next_at_id;

    /* Log-line watches and the commands they trigger (run at the tick). */
    shell_watch_t watches[SHELL_WATCH_MAX];
    int    watch_count;
    shell_trigger_t triggers[SHELL_TRIGGER_MAX];
    int    trigger_count;
    int    triggers_dropped;   /* reported at the next tick                 */

    /* The line being executed (see shell_origin_t), and whether it runs
     * outside the command stream ("!cmd" at a terminal), in which case a
     * run/step must not hold the stream. */
    shell_origin_t origin;
    bool   exec_immediate;

    /* Command stream: source stack (depth 0 = stdin) + blocking state. */
    shell_source_t stack[SHELL_SOURCE_DEPTH];
    int    depth;
    shell_block_t block;
    int64_t block_start_ns;
    int64_t block_deadline_ns;
    char   expect_pattern[SHELL_PATTERN_MAX];
    bool   expect_any;
    int    expect_ids[SIM_EQ_MAX_NODES];
    int    expect_n;
    bool   matched;
    /* expect options: -re (POSIX ERE, compiled into expect_re), -n count,
     * -c var (capture group 1, or the whole match / line). */
    void  *expect_re;               /* regex_t * or NULL (substring)       */
    int    expect_needed, expect_seen;
    char   expect_var[SHELL_VAR_NAME_MAX];
    char   matched_capture[SHELL_LINE_MAX];
    char   matched_line[SHELL_PATTERN_MAX];
    int    matched_node;
    int64_t matched_ns;
    int64_t default_expect_timeout_ns;

    /* `cmd`: one command line to one node, then wait for its shell prompt.
     * The prompt is matched (prompt_glob) against the node's console bytes
     * since the last newline — a prompt has no newline of its own, so it
     * never becomes a log line. */
    char    prompt_glob[64];
    int     cmd_idx, cmd_id;
    char    cmd_text[96];
    char    cmd_expect[SHELL_PATTERN_MAX];
    char    cmd_fail_on[SHELL_PATTERN_MAX];
    bool    cmd_expect_seen, cmd_fail_seen, cmd_prompt_seen;
    char    cmd_fail_line[SHELL_PATTERN_MAX];
    void   *cmd_re;                 /* -c: regex_t *                        */
    char    cmd_var[SHELL_VAR_NAME_MAX];
    bool    cmd_captured;
    char    cmd_capture[SHELL_LINE_MAX];
    int     cmd_lines;
    int64_t cmd_prompt_ns;
    char    cmd_partial[256];
    int     cmd_plen;
    int     cmd_candidate_len;   /* partial length at a prompt match, -1 = none */
    int64_t cmd_candidate_ns;    /* ... and when it matched                     */
    int     cmd_pass, cmd_fail;

    /* `expect-fault`: exception-entry counters of one ARM node at arm time. */
    int      fault_idx, fault_id;
    unsigned fault_mask;            /* bit n = exception number n (3..7)    */
    uint64_t fault_base[16];
    char     fault_what[64];

    /* Line currently read from a sendfile source (see shell_source_t). */
    bool     line_is_send;
    int      line_send_idx, line_send_id;
    int64_t  line_send_timeout_ns;

    /* Console history for tail/grep: a ring of the last lines (heap). */
    shell_hline_t *hist;
    int      hist_head, hist_count;

    /* `transcript <file>`: every command line typed or piped is appended. */
    FILE    *transcript;
    char     transcript_path[SHELL_PATH_MAX];

    /* Breakpoints and watchpoints (ARM nodes). */
    shell_dbg_t dbg[SHELL_DBG_MAX];
    int      dbg_count;
    int      next_dbg_id;
    int      halt_node_id;           /* expect-halt target                  */

    /* `restart` asked for; lines wait until the runner has restarted. */
    bool     restart_pending;

    /* `exit <code>`: the process exit status, overriding the verdict's. */
    bool     exit_code_set;
    int      exit_code;

    /* Variables ($name). */
    shell_var_t vars[SHELL_VARS_MAX];
    int      var_count;

    /* `console <id>`: the terminal talks to one node directly (lines in,
     * raw console bytes out) until `~.` or Ctrl-D. */
    bool    console_mode;
    int     console_idx, console_id;
    bool    console_dirty;
    int     max_line;       /* warn when one sent line exceeds it; 0 = off  */

    /* Verdict (docs/shell.md "Exit codes"). */
    bool   script_used;     /* a script ran or a verdict command was used  */
    bool   failed;
    char   fail_reason[SHELL_REASON_MAX];
    bool   passed;          /* `pass` seen                                 */
    bool   exited;          /* `exit`/`quit` executed                      */
    bool   finished;        /* root script reached its end                 */
    bool   waiting_note;    /* "script ended, waiting for at" note shown    */
    int    expect_pass, expect_fail;
    bool   pending_fail;    /* set from the observer, applied at the tick   */
    char   pending_fail_reason[SHELL_REASON_MAX];
    bool   stop_when_done;  /* --script without --shell: stop at pass/fail  */

    int64_t run_for_target_ns;   /* `run <dur>` in flight (for the note)   */
} shell_service_t;

/* Arm the service.  `interactive` = --shell (read stdin); `script_path` =
 * --script (may be NULL); `verbose` = the runner's flag (console mask
 * default + command echo); `port` >= 0 = --shell-port (0 picks a free
 * port): wait here for one TCP client on 127.0.0.1 and use it instead of
 * stdin/stdout; `json` = --shell-json.  Returns 0, or -1 if the script
 * cannot be opened or the port not served (message printed). */
int  shell_service_start(shell_service_t *s, sim_runtime_t *sim,
                         sim_control_t *ctl, bool interactive,
                         const char *script_path, bool verbose,
                         int port, bool json);

static inline bool shell_service_active(const shell_service_t *s) {
    return s && s->active;
}
static inline bool shell_service_interactive(const shell_service_t *s) {
    return s && s->active && s->interactive;
}

/* Wait up to timeout_ms for input while the simulation is paused, then run
 * one engine tick (so `run`/`exit` typed at a paused prompt take effect). */
void shell_service_pump_paused(shell_service_t *s, int timeout_ms);

/* Non-blocking stdin check for the loop's pacing waits. */
void shell_service_poll_input(shell_service_t *s);

/* The loop paused the run (run-for horizon / step budget / duration). */
void shell_service_on_autopause(shell_service_t *s);

/* End-of-run: print "--- Script Results ---" when a script/verdict was
 * used and return the exit code (0/1). */
int  shell_service_report(shell_service_t *s, int64_t now_ns);

/* UI restart: drop the at-queue and abort any script; keep masks/files. */
void shell_service_on_restart(shell_service_t *s);

extern const sim_service_ops_t shell_service_ops;

#ifdef __cplusplus
}
#endif

#endif /* SHELL_SERVICE_H */
