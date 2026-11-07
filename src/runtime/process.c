/*
 * process.c — the Process prototype: run programs and capture their output.
 *
 *   r = Process run(["git", "status", "--short"])   // no shell involved
 *   r stdout   r stderr   r exitCode   r ok
 *
 *   Process run("ls *.clay | wc -l", #{"shell": true})   // through sh / cmd
 *   Process run(["sort"], #{"input": "b\na\n", "cwd": "data"})
 *
 *   Process stream(["make"]) { |line| line println }      // each stdout line
 *
 * An argv array runs the program directly (searched on PATH), so arguments
 * need no quoting and can't inject shell syntax. A string command needs
 * "shell": true and runs via /bin/sh -c (cmd.exe /c on Windows).
 *
 * Options: "shell" (run a string through the shell), "cwd" (working
 * directory), "input" (string written to the program's stdin; otherwise
 * stdin is inherited). run captures stdout and stderr; stream passes each
 * stdout line to the block as it arrives (without the line ending), leaves
 * stderr on the terminal, and returns the exit code. A program killed by a
 * signal reports 128 + the signal number, as shells do.
 */
#include "clay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

/* ------------------------------------------------------------------ */
/*  Growable byte buffer                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *data;
    size_t len, cap;
} PBuf;

static void pbuf_add(PBuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        char *d = (char *)realloc(b->data, cap);
        if (!d) return;                 /* out of memory: drop output */
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static ClayValue pbuf_string(PBuf *b) {
    return CLAY_VAL(clay_string_new_len(b->data ? b->data : "", b->len));
}

/* ------------------------------------------------------------------ */
/*  Arguments                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char      **argv;       /* NULL-terminated; owned */
    int         argc;
    const char *cwd;        /* borrowed from the options Dict, or NULL */
    const char *input;      /* borrowed, or NULL */
    size_t      input_len;
} ProcSpec;

static void spec_free(ProcSpec *s) {
    if (s->argv) {
        for (int i = 0; i < s->argc; i++) free(s->argv[i]);
        free(s->argv);
    }
    s->argv = NULL;
}

static const char *opt_string(ClayObject *opts, const char *key, const char *who,
                              size_t *len) {
    if (!opts) return NULL;
    ClayValue v = clay_dict_get(opts, key);
    if (CLAY_IS_NIL(v)) return NULL;
    if (!CLAY_IS_TYPE(v, CLAY_TYPE_STRING)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "option \"%s\" must be a string", key);
        clay_raise_argument_error(who, msg);
    }
    if (len) *len = CLAY_OBJ(v)->as.string.length;
    return clay_string_data(CLAY_OBJ(v));
}

/* Build a ProcSpec from (command, options). Raises on bad arguments. */
static void spec_init(ProcSpec *s, ClayValue cmd, ClayValue optv, const char *who) {
    memset(s, 0, sizeof(*s));
    ClayObject *opts = NULL;
    if (!CLAY_IS_NIL(optv)) {
        if (!CLAY_IS_TYPE(optv, CLAY_TYPE_DICT))
            clay_raise_argument_error(who, "options must be a Dict, e.g. #{\"shell\": true}");
        opts = CLAY_OBJ(optv);
    }
    ClayValue shellv = opts ? clay_dict_get(opts, "shell") : CLAY_NIL;
    int shell = !CLAY_IS_NIL(shellv) && shellv != CLAY_FALSE;
    s->cwd = opt_string(opts, "cwd", who, NULL);
    s->input = opt_string(opts, "input", who, &s->input_len);

    if (CLAY_IS_TYPE(cmd, CLAY_TYPE_STRING)) {
        if (!shell)
            clay_raise_argument_error(who,
                "pass the command as an array, e.g. [\"ls\", \"-l\"], or use "
                "#{\"shell\": true} to run a string through the shell");
        const char *c = clay_string_data(CLAY_OBJ(cmd));
        s->argc = 3;
        s->argv = (char **)calloc(4, sizeof(char *));
#ifdef _WIN32
        s->argv[0] = strdup("cmd.exe");
        s->argv[1] = strdup("/c");
#else
        s->argv[0] = strdup("/bin/sh");
        s->argv[1] = strdup("-c");
#endif
        s->argv[2] = strdup(c);
        return;
    }
    if (!CLAY_IS_TYPE(cmd, CLAY_TYPE_ARRAY))
        clay_raise_argument_error(who, "the command must be an array of strings");
    if (shell)
        clay_raise_argument_error(who, "\"shell\": true takes the command as one string");
    ClayObject *arr = CLAY_OBJ(cmd);
    size_t n = clay_array_size(arr);
    if (n == 0) clay_raise_argument_error(who, "the command array is empty");
    for (size_t i = 0; i < n; i++)
        if (!CLAY_IS_TYPE(clay_array_at(arr, i), CLAY_TYPE_STRING))
            clay_raise_argument_error(who, "every command element must be a string");
    s->argc = (int)n;
    s->argv = (char **)calloc(n + 1, sizeof(char *));
    for (size_t i = 0; i < n; i++)
        s->argv[i] = strdup(clay_string_data(CLAY_OBJ(clay_array_at(arr, i))));
}

/* Frees the spec, then raises RuntimeError "<who>: cannot run 'prog': why" */
static void raise_start_failure(ProcSpec *s, const char *who, const char *why) {
    char msg[1200];
    snprintf(msg, sizeof(msg), "%s: cannot run '%s': %s", who, s->argv[0], why);
    spec_free(s);
    clay_raise(CLAY_VAL(clay_error_new(msg, "RuntimeError")));
}

/* ------------------------------------------------------------------ */
/*  Platform layer                                                     */
/*                                                                     */
/*  proc_start  launches the program with a pipe for stdout, a pipe   */
/*              for stderr (capture_err) and a pipe for stdin (input) */
/*  proc_pump   moves data until stdout closes: stdin from the input, */
/*              stderr into err; stdout is handed to on_out            */
/*  proc_finish waits and returns the exit code                       */
/* ------------------------------------------------------------------ */

/* Receives stdout as it arrives; returns nonzero to stop reading (break) */
typedef int (*OutFn)(void *ctx, const char *data, size_t n);

#ifndef _WIN32

typedef struct {
    pid_t pid;
    int out_fd, err_fd, in_fd;    /* -1 when not piped */
    int sigpipe_saved;            /* old SIGPIPE action to restore */
    struct sigaction old_sigpipe;
} Proc;

static void close_fd(int *fd) {
    if (*fd >= 0) close(*fd);
    *fd = -1;
}

/* 0 on success; on failure returns -1 with errno-text in why */
static int proc_start(Proc *p, const ProcSpec *s, int capture_err,
                      char *why, size_t why_sz) {
    int out[2] = {-1, -1}, err[2] = {-1, -1}, in[2] = {-1, -1}, ex[2] = {-1, -1};
    memset(p, 0, sizeof(*p));
    p->pid = -1;
    p->out_fd = p->err_fd = p->in_fd = -1;
    if (pipe(out) != 0 || (capture_err && pipe(err) != 0) ||
        (s->input && pipe(in) != 0) || pipe(ex) != 0) {
        snprintf(why, why_sz, "%s", strerror(errno));
        int fds[] = {out[0], out[1], err[0], err[1], in[0], in[1], ex[0], ex[1]};
        for (int i = 0; i < 8; i++) if (fds[i] >= 0) close(fds[i]);
        return -1;
    }
    fcntl(ex[1], F_SETFD, FD_CLOEXEC);   /* closes on a successful exec */

    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(why, why_sz, "%s", strerror(errno));
        int fds[] = {out[0], out[1], err[0], err[1], in[0], in[1], ex[0], ex[1]};
        for (int i = 0; i < 8; i++) if (fds[i] >= 0) close(fds[i]);
        return -1;
    }
    if (pid == 0) {
        /* child: only async-signal-safe calls from here to exec */
        int e;
        if (s->cwd && chdir(s->cwd) != 0) goto fail;
        if (dup2(out[1], STDOUT_FILENO) < 0) goto fail;
        if (capture_err && dup2(err[1], STDERR_FILENO) < 0) goto fail;
        if (s->input && dup2(in[0], STDIN_FILENO) < 0) goto fail;
        close(out[0]); close(out[1]);
        if (capture_err) { close(err[0]); close(err[1]); }
        if (s->input) { close(in[0]); close(in[1]); }
        close(ex[0]);
        execvp(s->argv[0], s->argv);
    fail:
        e = errno;
        if (write(ex[1], &e, sizeof(e)) < 0) { /* nothing more we can do */ }
        _exit(127);
    }

    /* parent */
    close(out[1]);
    if (capture_err) close(err[1]);
    if (s->input) close(in[0]);
    close(ex[1]);
    int child_errno = 0;
    ssize_t r;
    do { r = read(ex[0], &child_errno, sizeof(child_errno)); } while (r < 0 && errno == EINTR);
    close(ex[0]);
    if (r == (ssize_t)sizeof(child_errno)) {
        int st;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        close(out[0]);
        if (capture_err) close(err[0]);
        if (s->input) close(in[1]);
        snprintf(why, why_sz, "%s", strerror(child_errno));
        return -1;
    }
    p->pid = pid;
    p->out_fd = out[0];
    p->err_fd = capture_err ? err[0] : -1;
    p->in_fd = s->input ? in[1] : -1;
    if (p->in_fd >= 0) fcntl(p->in_fd, F_SETFL, fcntl(p->in_fd, F_GETFL) | O_NONBLOCK);
    return 0;
}

/* Runs until stdout and stderr are both closed, or on_out asks to stop. on_out
 * may raise (stream's block); the caller then calls proc_kill. */
static void proc_pump(Proc *p, const ProcSpec *s, PBuf *err, OutFn on_out, void *ctx) {
    /* A program that exits without reading all its input must not kill us
     * with SIGPIPE: ignore it while we write (proc_finish restores it). */
    if (p->in_fd >= 0) {
        struct sigaction ign;
        memset(&ign, 0, sizeof(ign));
        ign.sa_handler = SIG_IGN;
        p->sigpipe_saved = sigaction(SIGPIPE, &ign, &p->old_sigpipe) == 0;
    }
    size_t in_off = 0;
    char buf[8192];

    if (p->in_fd >= 0 && s->input_len == 0) close_fd(&p->in_fd);
    while (p->out_fd >= 0 || p->err_fd >= 0) {
        struct pollfd fds[3];
        int n = 0, io = -1, ie = -1, ii = -1;
        if (p->out_fd >= 0) { fds[n].fd = p->out_fd; fds[n].events = POLLIN; io = n++; }
        if (p->err_fd >= 0) { fds[n].fd = p->err_fd; fds[n].events = POLLIN; ie = n++; }
        if (p->in_fd >= 0)  { fds[n].fd = p->in_fd;  fds[n].events = POLLOUT; ii = n++; }
        if (poll(fds, (nfds_t)n, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ii >= 0 && (fds[ii].revents & (POLLOUT | POLLERR | POLLHUP))) {
            ssize_t w = write(p->in_fd, s->input + in_off, s->input_len - in_off);
            if (w > 0) in_off += (size_t)w;
            if (w < 0 && errno != EAGAIN && errno != EINTR) in_off = s->input_len;
            if (in_off >= s->input_len) close_fd(&p->in_fd);
        }
        if (ie >= 0 && (fds[ie].revents & (POLLIN | POLLERR | POLLHUP))) {
            ssize_t r = read(p->err_fd, buf, sizeof(buf));
            if (r > 0) pbuf_add(err, buf, (size_t)r);
            else if (r == 0 || errno != EINTR) close_fd(&p->err_fd);
        }
        if (io >= 0 && (fds[io].revents & (POLLIN | POLLERR | POLLHUP))) {
            ssize_t r = read(p->out_fd, buf, sizeof(buf));
            if (r > 0) {
                if (on_out(ctx, buf, (size_t)r)) break;
            } else if (r == 0 || errno != EINTR) {
                close_fd(&p->out_fd);
            }
        }
    }
    close_fd(&p->in_fd);
}

static int proc_finish(Proc *p) {
    close_fd(&p->out_fd);
    close_fd(&p->err_fd);
    close_fd(&p->in_fd);
    if (p->sigpipe_saved) {
        sigaction(SIGPIPE, &p->old_sigpipe, NULL);
        p->sigpipe_saved = 0;
    }
    int st = 0;
    while (waitpid(p->pid, &st, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return -1;
}

static void proc_kill(Proc *p) {
    kill(p->pid, SIGTERM);
    proc_finish(p);
}

#else /* _WIN32 */

typedef struct {
    HANDLE process;
    HANDLE out_r, err_r, in_w;    /* NULL when not piped */
    HANDLE err_thread, in_thread;
    PBuf  *err_buf;
    const ProcSpec *spec;
} Proc;

static void close_h(HANDLE *h) {
    if (*h) CloseHandle(*h);
    *h = NULL;
}

/* Append one argument to a command line using the quoting rules that
 * CommandLineToArgvW and the MSVC runtime undo. */
static void append_arg(PBuf *cl, const char *a) {
    if (cl->len) pbuf_add(cl, " ", 1);
    if (a[0] && !strpbrk(a, " \t\n\v\"")) {
        pbuf_add(cl, a, strlen(a));
        return;
    }
    pbuf_add(cl, "\"", 1);
    for (const char *p = a; ; p++) {
        size_t slashes = 0;
        while (*p == '\\') { slashes++; p++; }
        if (!*p) {
            for (size_t i = 0; i < slashes * 2; i++) pbuf_add(cl, "\\", 1);
            break;
        }
        if (*p == '"') {
            for (size_t i = 0; i < slashes * 2 + 1; i++) pbuf_add(cl, "\\", 1);
            pbuf_add(cl, "\"", 1);
        } else {
            for (size_t i = 0; i < slashes; i++) pbuf_add(cl, "\\", 1);
            pbuf_add(cl, p, 1);
        }
    }
    pbuf_add(cl, "\"", 1);
}

static unsigned __stdcall err_reader(void *arg) {
    Proc *p = (Proc *)arg;
    char buf[8192];
    DWORD n;
    while (ReadFile(p->err_r, buf, sizeof(buf), &n, NULL) && n > 0)
        pbuf_add(p->err_buf, buf, n);
    return 0;
}

static unsigned __stdcall in_writer(void *arg) {
    Proc *p = (Proc *)arg;
    size_t off = 0;
    while (off < p->spec->input_len) {
        DWORD chunk = (DWORD)((p->spec->input_len - off) > 65536 ? 65536 : (p->spec->input_len - off));
        DWORD n;
        if (!WriteFile(p->in_w, p->spec->input + off, chunk, &n, NULL)) break;
        off += n;
    }
    close_h(&p->in_w);
    return 0;
}

static void win_error_text(DWORD code, char *why, size_t why_sz) {
    if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                        NULL, code, 0, why, (DWORD)why_sz, NULL))
        snprintf(why, why_sz, "error %lu", (unsigned long)code);
    size_t n = strlen(why);
    while (n > 0 && (why[n - 1] == '\n' || why[n - 1] == '\r' || why[n - 1] == '.'))
        why[--n] = '\0';
}

static int proc_start(Proc *p, const ProcSpec *s, int capture_err,
                      char *why, size_t why_sz) {
    memset(p, 0, sizeof(*p));
    p->spec = s;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE out_w = NULL, err_w = NULL, in_r = NULL;
    if (!CreatePipe(&p->out_r, &out_w, &sa, 0) ||
        (capture_err && !CreatePipe(&p->err_r, &err_w, &sa, 0)) ||
        (s->input && !CreatePipe(&in_r, &p->in_w, &sa, 0))) {
        win_error_text(GetLastError(), why, why_sz);
        close_h(&p->out_r); close_h(&out_w); close_h(&p->err_r); close_h(&err_w);
        close_h(&in_r); close_h(&p->in_w);
        return -1;
    }
    /* our ends must not leak into the child */
    SetHandleInformation(p->out_r, HANDLE_FLAG_INHERIT, 0);
    if (p->err_r) SetHandleInformation(p->err_r, HANDLE_FLAG_INHERIT, 0);
    if (p->in_w) SetHandleInformation(p->in_w, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out_w;
    si.hStdError = capture_err ? err_w : GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = s->input ? in_r : GetStdHandle(STD_INPUT_HANDLE);

    PBuf cl = {0};
    for (int i = 0; i < s->argc; i++) {
        if (i == 1 && s->argc == 3 && strcmp(s->argv[0], "cmd.exe") == 0 &&
            strcmp(s->argv[1], "/c") == 0) {
            /* cmd.exe parses its own command line: /d skips AutoRun, and
             * /s /c "..." strips just the outer quotes, keeping any inside */
            static const char shell_flags[] = " /d /s /c \"";
            pbuf_add(&cl, shell_flags, strlen(shell_flags));
            pbuf_add(&cl, s->argv[2], strlen(s->argv[2]));
            pbuf_add(&cl, "\"", 1);
            break;
        } else {
            append_arg(&cl, s->argv[i]);
        }
    }

    fflush(stdout);
    fflush(stderr);
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(NULL, cl.data, NULL, NULL, TRUE, 0, NULL, s->cwd, &si, &pi);
    DWORD code = GetLastError();
    free(cl.data);
    close_h(&out_w);
    close_h(&err_w);
    close_h(&in_r);
    if (!ok) {
        win_error_text(code, why, why_sz);
        close_h(&p->out_r); close_h(&p->err_r); close_h(&p->in_w);
        return -1;
    }
    CloseHandle(pi.hThread);
    p->process = pi.hProcess;
    return 0;
}

static void proc_pump(Proc *p, const ProcSpec *s, PBuf *err, OutFn on_out, void *ctx) {
    (void)s;
    p->err_buf = err;
    if (p->err_r) p->err_thread = (HANDLE)_beginthreadex(NULL, 0, err_reader, p, 0, NULL);
    if (p->in_w) p->in_thread = (HANDLE)_beginthreadex(NULL, 0, in_writer, p, 0, NULL);
    char buf[8192];
    DWORD n;
    while (ReadFile(p->out_r, buf, sizeof(buf), &n, NULL) && n > 0)
        if (on_out(ctx, buf, n)) break;
    close_h(&p->out_r);
}

static void proc_join(Proc *p) {
    if (p->err_thread) { WaitForSingleObject(p->err_thread, INFINITE); close_h(&p->err_thread); }
    if (p->in_thread)  { WaitForSingleObject(p->in_thread, INFINITE);  close_h(&p->in_thread); }
    close_h(&p->out_r);
    close_h(&p->err_r);
    close_h(&p->in_w);
}

static int proc_finish(Proc *p) {
    WaitForSingleObject(p->process, INFINITE);
    proc_join(p);
    DWORD code = 0;
    GetExitCodeProcess(p->process, &code);
    close_h(&p->process);
    return (int)code;
}

static void proc_kill(Proc *p) {
    TerminateProcess(p->process, 1);
    proc_finish(p);
}

#endif /* _WIN32 */

/* ------------------------------------------------------------------ */
/*  Process run                                                        */
/* ------------------------------------------------------------------ */

static int collect_out(void *ctx, const char *data, size_t n) {
    pbuf_add((PBuf *)ctx, data, n);
    return 0;
}

static ClayValue process_run(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    const char *who = "Process run";
    if (argc < 1) clay_raise_argument_error(who, "missing the command");
    ProcSpec spec;
    spec_init(&spec, argv[0], argc > 1 ? argv[1] : CLAY_NIL, who);

    Proc p;
    char why[512];
    if (proc_start(&p, &spec, 1, why, sizeof(why)) != 0) {
        raise_start_failure(&spec, who, why);
        return CLAY_NIL;
    }
    PBuf out = {0}, err = {0};
    proc_pump(&p, &spec, &err, collect_out, &out);
    int code = proc_finish(&p);
    spec_free(&spec);

    ClayObject *r = clay_object_new();
    r->parent = clay_Object;
    clay_set_slot_take(r, "stdout", pbuf_string(&out));
    clay_set_slot_take(r, "stderr", pbuf_string(&err));
    clay_set_slot_take(r, "exitCode", clay_int(code));
    clay_set_slot_take(r, "ok", code == 0 ? CLAY_TRUE : CLAY_FALSE);
    free(out.data);
    free(err.data);
    return CLAY_VAL(r);
}

/* ------------------------------------------------------------------ */
/*  Process stream                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    PBuf       pending;      /* bytes after the last newline */
    ClayObject *block;
    int        stop;         /* break from the block */
} StreamCtx;

static void stream_line(StreamCtx *c, const char *s, size_t n) {
    if (n > 0 && s[n - 1] == '\r') n--;
    ClayValue line = CLAY_VAL(clay_string_new_len(s, n));
    ClayTryFrame tf;
    if (clay_try_push(&tf) == 0) {
        ClayValue r = clay_send(c->block, "value", 1, &line);
        clay_try_pop();
        clay_release_val(r);
        clay_release_val(line);
    } else {
        /* clay_raise already popped tf */
        clay_release_val(line);
        ClayValue e = clay_error_stack.current_error;
        int sig = clay_loop_signal(e);
        if (sig == CLAY_SIGNAL_BREAK) { c->stop = 1; return; }
        if (sig == CLAY_SIGNAL_CONTINUE) return;
        clay_raise(e);
    }
}

static int stream_out(void *ctx, const char *data, size_t n) {
    StreamCtx *c = (StreamCtx *)ctx;
    size_t start = 0;
    for (size_t i = 0; i < n && !c->stop; i++) {
        if (data[i] != '\n') continue;
        if (c->pending.len) {
            pbuf_add(&c->pending, data + start, i - start);
            size_t len = c->pending.len;
            c->pending.len = 0;
            stream_line(c, c->pending.data, len);
        } else {
            stream_line(c, data + start, i - start);
        }
        start = i + 1;
    }
    if (!c->stop && start < n) pbuf_add(&c->pending, data + start, n - start);
    return c->stop;                      /* break: stop reading, kill below */
}

static ClayValue process_stream(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    const char *who = "Process stream";
    if (argc < 2 || !CLAY_IS_TYPE(argv[argc - 1], CLAY_TYPE_BLOCK))
        clay_raise_argument_error(who, "needs a block: Process stream(cmd) { |line| ... }");
    ClayObject *block = CLAY_OBJ(argv[argc - 1]);
    ProcSpec spec;
    spec_init(&spec, argv[0], argc > 2 ? argv[1] : CLAY_NIL, who);

    Proc p;
    char why[512];
    if (proc_start(&p, &spec, 0, why, sizeof(why)) != 0) {
        raise_start_failure(&spec, who, why);
        return CLAY_NIL;
    }

    StreamCtx c;
    memset(&c, 0, sizeof(c));
    c.block = block;
    PBuf unused = {0};
    ClayTryFrame tf;
    if (clay_try_push(&tf) == 0) {
        proc_pump(&p, &spec, &unused, stream_out, &c);
        if (!c.stop && c.pending.len) {          /* a last line with no newline */
            size_t len = c.pending.len;
            c.pending.len = 0;
            stream_line(&c, c.pending.data, len);
        }
        clay_try_pop();
    } else {
        /* the block raised (tf is already popped): stop the program, then
         * pass the error on */
        ClayValue e = clay_error_stack.current_error;
        proc_kill(&p);
        free(c.pending.data);
        spec_free(&spec);
        clay_raise(e);
        return CLAY_NIL;
    }
    int code = c.stop ? (proc_kill(&p), -1) : proc_finish(&p);
    free(c.pending.data);
    spec_free(&spec);
    return code < 0 ? CLAY_NIL : clay_int(code);
}

void clay_process_init_methods(ClayObject *proto) {
    clay_set_method(proto, "run",    process_run);
    clay_set_method(proto, "stream", process_stream);
}
