/*
 * leopard-installer — LeopardOS installer (serialized RC orchestrator)
 *
 * Reads /config/leopard-install-sequence.conf and runs step programs one by one in order:
 * each step is awaited to completion before the next runs. If the sequence file or a step
 * program is missing, shut down; when a step returns non-zero, prompt retry or shutdown;
 * after all succeed:
 *   - interactive mode: ask "reboot / shutdown" (on physical machines you can remove the install media before rebooting)
 *   - auto=1: follow finish=(reboot|shutdown) automatically, defaulting to reboot
 * Sequence file format: one full path to a step program per line, no arguments; lines starting with # are comments.
 */
#include "xtuiapi.h"

#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define CONFIG_PATH "/config/leopard-install-sequence.conf"
#define BUF_CAP     16384
#define PATH_CAP    256
#define MSG_CAP     512

static void shutdown_now(void);
static void reboot_now(void);
static int  install_is_auto(void);
static int  install_finish_is_shutdown(void);

/*
 * Top-level messages go to both the VT (xapi_Printf) and serial (xapi_OutputSerial).
 * The installer runs under conhost's PTY; serial is the only reliable channel for
 * headless tests and field diagnostics.
 */
static void install_msg(const char *fmt, ...)
{
        char    line[MSG_CAP];
        va_list ap;

        va_start(ap, fmt);
        vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);

        xapi_Printf("%s", line);
        xapi_OutputSerial((WSTR)line);
}

static inline void read_line(char *buf, size_t cap)
{
        char line[XAPI_GETLINE_BUFFER_SIZE];

        if (buf == NULL || cap == 0) return;
        xapi_Input((WSTR)line);
        snprintf(buf, cap, "%s", line);
}

static inline int run_step(const char *path)
{
        char *argv[2] = { (char *)path, NULL };
        pid_t pid = fork();
        int  status = 0;

        if (pid < 0)
                return -1;
        if (pid == 0) {
                /* execve keeps the installer's fd 0/1/2, so the step stays on the PTY. */
                execve(path, argv, environ);
                _exit(127);
        }
        if (waitpid(pid, &status, 0) < 0)
                return -1;
        return status;
}

/* Read the auto value from a key=value text file; returns 1/0, or -1 if not found. */
static int file_auto(const char *path)
{
        XFILE   *fp = xapi_OpenFile((WSTR)path);
        char     buf[4096];
        uint64_t len;
        uint64_t got;
        int      result = -1;
        char    *line;

        if (fp == NULL) return -1;
        len = fp->length;
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        memset(buf, 0, sizeof(buf));
        got = xapi_ReadFile((WSTR)path, buf, len, 0);
        xapi_CloseFile(fp);
        buf[got] = '\0';

        line = buf;
        while (*line) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = '\0';
                while (*line == ' ' || *line == '\t' || *line == '\r') line++;
                if (strncmp(line, "auto=", 5) == 0) {
                        const char *v = line + 5;
                        result = (strcmp(v, "1") == 0 || strcmp(v, "yes") == 0 || strcmp(v, "true") == 0);
                }
                if (!nl) break;
                line = nl + 1;
        }
        return result;
}

/* Same semantics as li_is_auto: options file first, state overrides; if neither, use interactive mode. */
static int install_is_auto(void)
{
        int opt = file_auto("/config/leopard-install.conf");
        int st  = file_auto("/config/installer/state");

        if (st >= 0) return st;
        if (opt >= 0) return opt;
        return 0;
}

/* Read the finish= value; returns 1 = shutdown, 0 = reboot (default). */
static int install_finish_is_shutdown(void)
{
        const char *paths[2] = { "/config/installer/state", "/config/leopard-install.conf" };

        for (int p = 0; p < 2; p++) {
                XFILE   *fp = xapi_OpenFile((WSTR)paths[p]);
                char     buf[4096];
                uint64_t len;
                uint64_t got;
                char    *line;

                if (fp == NULL) continue;
                len = fp->length;
                if (len >= sizeof(buf)) len = sizeof(buf) - 1;
                memset(buf, 0, sizeof(buf));
                got = xapi_ReadFile((WSTR)paths[p], buf, len, 0);
                xapi_CloseFile(fp);
                buf[got] = '\0';

                line = buf;
                while (*line) {
                        char *nl = strchr(line, '\n');
                        if (nl) *nl = '\0';
                        while (*line == ' ' || *line == '\t' || *line == '\r') line++;
                        if (strncmp(line, "finish=", 7) == 0) {
                                const char *v = line + 7;
                                return strcmp(v, "shutdown") == 0 || strcmp(v, "poweroff") == 0;
                        }
                        if (!nl) break;
                        line = nl + 1;
                }
        }
        return 0;
}

static inline int ask_retry(void)
{
        char line[16];

        /* auto mode: zero input, shut down on failure, never block waiting for the keyboard */
        if (install_is_auto()) {
                install_msg("auto mode: step failed, shutting down (no retry)\n");
                return 0;
        }
        xapi_Printf("retry or shutdown? [r/s]: ");
        read_line(line, sizeof(line));
        return (line[0] == 'r' || line[0] == 'R');
}

int main(int argc, char *argv[], char *envp[])
{
        (void)argc;
        (void)argv;
        (void)envp;

        XFILE *fp = xapi_OpenFile((WSTR)CONFIG_PATH);
        if (fp == NULL) {
                install_msg("installer: sequence file not found: %s\n", CONFIG_PATH);
                shutdown_now();
        }

        uint64_t len = fp->length;
        char buf[BUF_CAP];
        if (len >= sizeof(buf))
                len = sizeof(buf) - 1;
        memset(buf, 0, sizeof(buf));
        uint64_t got = xapi_ReadFile((WSTR)CONFIG_PATH, buf, len, 0);
        xapi_CloseFile(fp);
        buf[got] = '\0';

        char *line = buf;
        while (*line) {
                char *nl = strchr(line, '\n');
                if (nl)
                        *nl = '\0';
                while (*line == ' ' || *line == '\t' || *line == '\r')
                        line++;
                if (*line == '\0' || *line == '#')
                        goto next_line;

                char path[PATH_CAP];
                memset(path, 0, sizeof(path));
                int i = 0;
                while (*line != '\0' && *line != ' ' && *line != '\t' && *line != '\r'
                       && i < (int)sizeof(path) - 1)
                        path[i++] = *line++;

                for (;;) {
                        int rc = run_step(path);
                        if (rc == 0)
                                break;
                        if (rc < 0) {
                                install_msg("installer: step not found: %s\n", path);
                                shutdown_now();
                        }
                        install_msg("installer: step failed (rc=%d): %s\n", rc, path);
                        if (!ask_retry())
                                shutdown_now();
                }

next_line:
                if (!nl)
                        break;
                line = nl + 1;
        }

        install_msg("installer: all steps complete.\n");
        if (install_is_auto()) {
                if (install_finish_is_shutdown()) shutdown_now();
                install_msg("installer: rebooting...\n");
                reboot_now();
        }

        /* Interactive mode: give the user a chance to remove the install media / choose shutdown. */
        for (;;) {
                char choice[16];

                xapi_Printf("\nInstallation complete.\n");
                xapi_Printf("  1) Reboot  (remove the installation media first)\n");
                xapi_Printf("  2) Shut down\n");
                xapi_Printf("Choice [1]: ");
                read_line(choice, sizeof(choice));

                if (choice[0] == '2' || choice[0] == 's' || choice[0] == 'S') {
                        shutdown_now();
                }
                if (choice[0] == '\0' || choice[0] == '1' || choice[0] == 'r' || choice[0] == 'R') {
                        reboot_now();
                }
                xapi_Printf("invalid choice\n");
        }
        return 0;
}

static void shutdown_now(void)
{
        /* Shut down; if the power action fails, hang instead of continuing the install */
        install_msg("installer: shutting down...\n");
        int64_t rc = (int64_t)xapi_PowerAction(XPOWER_SHUTDOWN);
        if (rc != 0) install_msg("installer: power action failed rc=%lld\n", (long long)rc);
        for (;;)
                xapi_Sleep(1000);
}

static void reboot_now(void)
{
        /* Reboot; if the power action fails, hang */
        install_msg("installer: rebooting...\n");
        int64_t rc = (int64_t)xapi_PowerAction(XPOWER_REBOOT);
        if (rc != 0) install_msg("installer: power action failed rc=%lld\n", (long long)rc);
        for (;;)
                xapi_Sleep(1000);
}
