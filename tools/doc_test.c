/*
 * doc_test.c - run the ```clay code blocks of a Markdown file
 *
 *   doc_test <clay binary> <file.md> <work dir>
 *
 * Every fenced block whose info string starts with `clay` is written to
 * <work dir>/<file>-L<line>.clay and run with the given clay binary. A block
 * passes when the program exits with status 0. Blocks marked ```clay norun
 * (fragments, servers, windows, network access) are skipped.
 *
 * Used by CTest (clay_docs_* tests) to keep the guide in docs/ runnable.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

/* A block that runs longer than this is killed and counts as a failure, so a
 * server or window example accidentally left runnable can't hang CI */
#define BLOCK_TIMEOUT_SECONDS 30

/* Run `clay file` with stdin empty and stdout+stderr to out_path.
 * Returns the exit status, or -1 on timeout / failure to start. */
static int run_block(const char *clay, const char *file, const char *out_path) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE out = CreateFileA(out_path, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE || nul == INVALID_HANDLE_VALUE) return -1;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = out;
    si.hStdError = out;
    char cmd[3000];
    snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\"", clay, file);
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    CloseHandle(out);
    CloseHandle(nul);
    if (!ok) return -1;
    DWORD rc = 1;
    if (WaitForSingleObject(pi.hProcess, BLOCK_TIMEOUT_SECONDS * 1000) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        rc = (DWORD)-1;
    } else {
        GetExitCodeProcess(pi.hProcess, &rc);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)rc;
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setpgid(0, 0);   /* own process group: a timeout kills anything it started */
        int out = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int nul = open("/dev/null", O_RDONLY);
        if (out < 0 || nul < 0) _exit(127);
        dup2(nul, 0);
        dup2(out, 1);
        dup2(out, 2);
        execl(clay, clay, file, (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid);
    struct timespec tick = { 0, 50 * 1000 * 1000 };   /* 50 ms */
    for (int waited = 0; waited < BLOCK_TIMEOUT_SECONDS * 20; waited++) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (r < 0) return -1;
        nanosleep(&tick, NULL);
    }
    kill(-pid, SIGKILL);
    int status;
    waitpid(pid, &status, 0);
    return -1;
#endif
}

static const char *base_name(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

static void print_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) fputs(buf, stdout);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: doc_test <clay> <file.md> <work dir>\n");
        return 2;
    }
    const char *clay = argv[1], *md = argv[2], *work = argv[3];
    FILE *in = fopen(md, "r");
    if (!in) { fprintf(stderr, "doc_test: cannot open %s\n", md); return 2; }

    char name[256];
    snprintf(name, sizeof(name), "%s", base_name(md));
    char *dot = strrchr(name, '.');
    if (dot) *dot = '\0';

    char line[4096];
    int lineno = 0, block_line = 0, run = 0, skipped = 0, failed = 0;
    FILE *block = NULL;            /* open while inside a runnable block */
    int in_other_fence = 0;        /* inside a non-clay or norun fence */
    char block_path[1024];

    while (fgets(line, sizeof(line), in)) {
        lineno++;
        int is_fence = strncmp(line, "```", 3) == 0;
        if (block) {
            if (is_fence) {
                fclose(block);
                block = NULL;
                char out_path[1100];
                snprintf(out_path, sizeof(out_path), "%s.out", block_path);
                run++;
                int rc = run_block(clay, block_path, out_path);
                if (rc != 0) {
                    failed++;
                    if (rc == -1)
                        printf("FAIL %s:%d (killed after %d s, or could not run)\n--- block ---\n",
                               md, block_line, BLOCK_TIMEOUT_SECONDS);
                    else
                        printf("FAIL %s:%d\n--- block ---\n", md, block_line);
                    print_file(block_path);
                    printf("--- output ---\n");
                    print_file(out_path);
                    printf("-------------\n");
                }
            } else {
                fputs(line, block);
            }
            continue;
        }
        if (in_other_fence) {
            if (is_fence) in_other_fence = 0;
            continue;
        }
        if (is_fence) {
            const char *info = line + 3;
            int clay_block = strncmp(info, "clay", 4) == 0 &&
                             (info[4] == '\n' || info[4] == '\r' || info[4] == ' ' || info[4] == '\0');
            if (clay_block && !strstr(info, "norun")) {
                block_line = lineno;
                snprintf(block_path, sizeof(block_path), "%s/%s-L%d.clay", work, name, lineno);
                block = fopen(block_path, "w");
                if (!block) { fprintf(stderr, "doc_test: cannot write %s\n", block_path); return 2; }
            } else {
                if (clay_block) skipped++;
                in_other_fence = 1;
            }
        }
    }
    fclose(in);
    if (block) { fclose(block); fprintf(stderr, "doc_test: unterminated block at %s:%d\n", md, block_line); return 2; }

    printf("%s: %d blocks run, %d skipped (norun), %d failed\n", md, run, skipped, failed);
    return failed ? 1 : 0;
}
