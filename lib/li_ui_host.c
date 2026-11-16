/*
 * lib/li_ui_host.c — orchestrator side of the installer TUI.
 *
 * One step runs at a time.  The host:
 *   - owns stdscr and all termios/alternate-screen state (via li_tui);
 *   - creates LI_UI_FD / LI_UI_OUT_FD pipes for the step child;
 *   - captures the child's stdout/stderr into the log pane;
 *   - routes MENU/INPUT/CONFIRM requests to modals and answers them.
 *
 * Children never touch the real terminal: fd 0 is /dev/null and fd 1/2 are a
 * log pipe.  This keeps the screen stable across step boundaries and means a
 * step can still be run standalone (it then falls back to line mode).
 */
#include "li_ui_host.h"
#include "li_tui.h"
#include "xtuiapi.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define HOST_UI_CAP    4096
#define HOST_LOG_CAP   1024
#define HOST_MAX_ITEMS 16
#define HOST_ITEM_T    160
#define HOST_ITEM_D    256

#ifndef STDIN_FILENO
#define STDIN_FILENO  0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif

struct host {
        int              inited;
        int              count;
        char             names[LI_UI_MAX_STEPS][64];
        const char      *name_ptrs[LI_UI_MAX_STEPS];
        li_tui_state     st;
        char             step_title[192];

        int              reply_fd;
        int              ui_fd;
        int              log_fd;
        pid_t            child;
        int              ui_eof;
        int              log_eof;
        int              child_done;
        int              child_status;
        int              done_rc;

        char             ui_buf[HOST_UI_CAP];
        size_t           ui_len;

        char             log_partial[HOST_LOG_CAP];
        size_t           log_partial_len;

        /* MENU collection state. */
        int              in_menu;
        int              req_id;
        int              menu_selected;
        char             req_text[256];
        int              item_count;
        li_ui_item       items[HOST_MAX_ITEMS];
        char             item_t[HOST_MAX_ITEMS][HOST_ITEM_T];
        char             item_d[HOST_MAX_ITEMS][HOST_ITEM_D];
};

static struct host H;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static void copy_text(char *dst, size_t cap, const char *src)
{
        size_t i = 0;

        if (cap == 0) return;
        if (src != NULL) {
                while (src[i] != '\0' && i + 1 < cap) {
                        char c = src[i];

                        if ((unsigned char)c < 32U) c = ' ';
                        dst[i++] = c;
                }
        }
        dst[i] = '\0';
}

/* "45-packages-select.elf" -> "Packages select" */
static void pretty_name(const char *path, char *out, size_t cap)
{
        const char *base = strrchr(path, '/');
        size_t      len;
        const char *p;
        size_t      j = 0;
        int         upper = 1;

        base = base != NULL ? base + 1 : path;
        len = strlen(base);
        if (len > 4 && strcmp(base + len - 4, ".elf") == 0) len -= 4;

        p = base;
        while (p < base + len && *p >= '0' && *p <= '9') p++;
        if (p < base + len && (*p == '-' || *p == '_')) p++;

        for (; p < base + len && j + 1 < cap; p++) {
                char c = *p;

                if (c == '-' || c == '_') {
                        out[j++] = ' ';
                        upper = 1;
                } else {
                        if (upper && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
                        out[j++] = c;
                        upper = 0;
                }
        }
        out[j] = '\0';
        if (j == 0) copy_text(out, cap, base);
}

static void host_write_all(const char *buf, size_t len)
{
        size_t off = 0;

        if (H.reply_fd < 0) return;
        while (off < len) {
                ssize_t n = write(H.reply_fd, buf + off, len - off);

                if (n < 0) {
                        if (errno == EINTR) continue;
                        return;
                }
                if (n == 0) return;
                off += (size_t)n;
        }
}

static void host_reply(const char *fmt, ...)
{
        char    line[512];
        va_list ap;
        int     n;

        va_start(ap, fmt);
        n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if (n >= (int)sizeof(line) - 1) n = (int)sizeof(line) - 2;
        line[n++] = '\n';
        host_write_all(line, (size_t)n);
}

static void host_status(const char *fmt, ...)
{
        va_list ap;

        va_start(ap, fmt);
        vsnprintf(H.st.status, sizeof(H.st.status), fmt, ap);
        va_end(ap);
        for (size_t i = 0; H.st.status[i] != '\0'; i++)
                if ((unsigned char)H.st.status[i] < 32U) H.st.status[i] = ' ';
}

/* Keep modal boundaries observable on the serial port for headless tests. */
static void host_serial(const char *kind, const char *text)
{
        char line[320];

        snprintf(line, sizeof(line), "installer: ui %s %s\n", kind,
                 text != NULL ? text : "");
        xapi_OutputSerial((WSTR)line);
}

/* ------------------------------------------------------------------ */
/* Log capture                                                        */
/* ------------------------------------------------------------------ */

static void host_log_flush(void)
{
        if (H.log_partial_len == 0) return;
        H.log_partial[H.log_partial_len] = '\0';
        li_tui_log_append(H.log_partial);
        copy_text(H.st.status, sizeof(H.st.status), H.log_partial);
        H.log_partial_len = 0;
}

static void host_log_bytes(const char *buf, size_t len)
{
        for (size_t i = 0; i < len; i++) {
                char c = buf[i];

                if (c == '\n') {
                        host_log_flush();
                } else if (c != '\r') {
                        if (H.log_partial_len + 1 >= sizeof(H.log_partial))
                                host_log_flush();
                        H.log_partial[H.log_partial_len++] = c;
                }
        }
}

/* render=0 is used from modal key loops: redrawing stdscr would erase the
 * modal, so the log is recorded quietly and appears when the modal closes. */
static int host_read_log(int render)
{
        int got = 0;

        if (H.log_fd < 0) return 0;
        for (;;) {
                struct pollfd p;
                char          buf[512];
                ssize_t       n;

                p.fd = H.log_fd;
                p.events = POLLIN;
                p.revents = 0;
                if (poll(&p, 1, 0) <= 0) break;

                n = read(H.log_fd, buf, sizeof(buf));
                if (n < 0) {
                        if (errno == EINTR) continue;
                        H.log_eof = 1;
                        break;
                }
                if (n == 0) {
                        H.log_eof = 1;
                        break;
                }
                host_log_bytes(buf, (size_t)n);
                got = 1;
        }
        if (H.log_eof && H.log_partial_len > 0) {
                host_log_flush();
                got = 1;
        }
        if (got && render) li_tui_render();
        return got;
}

static void host_pump_cb(void)
{
        (void)host_read_log(0);
}

/* ------------------------------------------------------------------ */
/* Request routing                                                    */
/* ------------------------------------------------------------------ */

static void host_menu_finish(void)
{
        int selected;

        H.in_menu = 0;
        host_serial("menu", H.req_text);
        selected = li_tui_menu(H.req_text, H.items, H.item_count, H.menu_selected);
        if (selected < 0)
                host_reply("CANCEL %d", H.req_id);
        else
                host_reply("ANSWER %d\t%d", H.req_id, selected);
        H.item_count = 0;
}

static void host_handle_line(char *line)
{
        if (strncmp(line, "MENU ", 5) == 0) {
                char *p = line + 5;
                char *tab = strchr(p, '\t');

                if (tab == NULL) return;
                *tab = '\0';
                H.req_id = atoi(p);
                p = tab + 1;
                tab = strchr(p, '\t');
                if (tab != NULL) {
                        *tab = '\0';
                        H.menu_selected = atoi(tab + 1);
                } else {
                        H.menu_selected = 0;
                }
                copy_text(H.req_text, sizeof(H.req_text), p);
                H.item_count = 0;
                H.in_menu = 1;
        } else if (strncmp(line, "ITEM ", 5) == 0) {
                char *p = line + 5;
                char *tab = strchr(p, '\t');
                int   idx;

                if (!H.in_menu || tab == NULL) return;
                *tab = '\0';
                idx = atoi(p);
                if (idx < 0 || idx >= HOST_MAX_ITEMS) return;
                p = tab + 1;
                tab = strchr(p, '\t');
                if (tab != NULL) {
                        *tab = '\0';
                        copy_text(H.item_d[idx], sizeof(H.item_d[idx]), tab + 1);
                } else {
                        H.item_d[idx][0] = '\0';
                }
                copy_text(H.item_t[idx], sizeof(H.item_t[idx]), p);
                H.items[idx].title = H.item_t[idx];
                H.items[idx].desc = H.item_d[idx];
                if (idx + 1 > H.item_count) H.item_count = idx + 1;
        } else if (strcmp(line, "END") == 0) {
                if (H.in_menu) host_menu_finish();
        } else if (strncmp(line, "INPUT ", 6) == 0) {
                char *p = line + 6;
                char *tab = strchr(p, '\t');
                int   id;
                const char *prompt = "";
                const char *def = "";
                char  val[256];

                if (tab == NULL) return;
                *tab = '\0';
                id = atoi(p);
                p = tab + 1;
                tab = strchr(p, '\t');
                if (tab != NULL) {
                        *tab = '\0';
                        def = tab + 1;
                }
                prompt = p;

                host_serial("input", prompt);
                if (li_tui_input(prompt, def, val, sizeof(val)) == 0)
                        host_reply("ANSWER %d\t%s", id, val);
                else
                        host_reply("CANCEL %d", id);
        } else if (strncmp(line, "CONFIRM ", 8) == 0) {
                char *p = line + 8;
                char *tab = strchr(p, '\t');
                int   id;
                int   yes;
                int   def_yes;
                const char *prompt = "";

                if (tab == NULL) return;
                *tab = '\0';
                id = atoi(p);
                p = tab + 1;
                tab = strchr(p, '\t');
                def_yes = 0;
                if (tab != NULL) {
                        *tab = '\0';
                        def_yes = (tab[1] == 'y' || tab[1] == 'Y');
                }
                prompt = p;

                host_serial("confirm", prompt);
                yes = li_tui_confirm(prompt, def_yes);
                if (yes < 0)
                        host_reply("CANCEL %d", id);
                else
                        host_reply("ANSWER %d\t%d", id, yes ? 1 : 0);
        } else if (strncmp(line, "PROGRESS ", 9) == 0) {
                char *p = line + 9;
                char *tab = strchr(p, '\t');
                int   local;

                if (tab != NULL) {
                        *tab = '\0';
                        copy_text(H.st.status, sizeof(H.st.status), tab + 1);
                } else {
                        H.st.status[0] = '\0';
                }
                local = atoi(p);
                if (local < 0) local = 0;
                if (local > 100) local = 100;
                if (H.st.total_steps > 0)
                        H.st.percent = (H.st.current_step * 100 + local) / H.st.total_steps;
                else
                        H.st.percent = local;
                li_tui_render();
        } else if (strncmp(line, "STEP ", 5) == 0) {
                copy_text(H.step_title, sizeof(H.step_title), line + 5);
                H.st.step_title = H.step_title;
                li_tui_render();
        } else if (strncmp(line, "LOG ", 4) == 0) {
                li_tui_log_append(line + 4);
                copy_text(H.st.status, sizeof(H.st.status), line + 4);
                li_tui_render();
        } else if (strncmp(line, "DONE ", 5) == 0) {
                H.done_rc = atoi(line + 5);
        } else if (strncmp(line, "ERROR ", 6) == 0) {
                li_tui_log_append(line + 6);
                copy_text(H.st.status, sizeof(H.st.status), line + 6);
                li_tui_render();
        }
}

static void host_read_ui(void)
{
        if (H.ui_fd < 0) return;

        for (;;) {
                ssize_t n;

                if (H.ui_len + 1 >= sizeof(H.ui_buf)) H.ui_len = 0;
                n = read(H.ui_fd, H.ui_buf + H.ui_len,
                         sizeof(H.ui_buf) - H.ui_len - 1);
                if (n < 0) {
                        if (errno == EINTR) continue;
                        H.ui_eof = 1;
                        break;
                }
                if (n == 0) {
                        H.ui_eof = 1;
                        break;
                }
                H.ui_len += (size_t)n;
                H.ui_buf[H.ui_len] = '\0';

                for (;;) {
                        char  *nl = strchr(H.ui_buf, '\n');
                        size_t rest;

                        if (nl == NULL) break;
                        *nl = '\0';
                        host_handle_line(H.ui_buf);
                        rest = H.ui_len - (size_t)(nl - H.ui_buf) - 1;
                        memmove(H.ui_buf, nl + 1, rest + 1);
                        H.ui_len = rest;
                }

                struct pollfd p;

                p.fd = H.ui_fd;
                p.events = POLLIN;
                p.revents = 0;
                if (poll(&p, 1, 0) <= 0) break;
        }
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int li_ui_host_init(const char *const *step_paths, int step_count)
{
        if (step_paths == NULL || step_count <= 0 ||
            step_count > LI_UI_MAX_STEPS)
                return -1;

        memset(&H, 0, sizeof(H));
        H.count = step_count;
        H.reply_fd = -1;
        H.ui_fd = -1;
        H.log_fd = -1;

        for (int i = 0; i < step_count; i++) {
                pretty_name(step_paths[i], H.names[i], sizeof(H.names[i]));
                H.name_ptrs[i] = H.names[i];
        }

        if (li_tui_init() != 0) return -1;

        H.st.total_steps = step_count;
        H.st.current_step = 0;
        H.st.percent = 0;
        H.st.step_title = H.names[0];
        H.st.step_names = H.name_ptrs;
        H.st.step_name_count = step_count;
        H.st.log_visible = 1;
        host_status("Starting installer...");

        li_tui_log_reset();
        li_tui_set_state(&H.st);
        li_tui_set_pump(host_pump_cb);
        li_tui_render();

        H.inited = 1;
        return 0;
}

void li_ui_host_end(void)
{
        if (!H.inited) return;
        li_tui_set_pump(NULL);
        li_tui_end();
        H.inited = 0;
}

int li_ui_host_welcome(void)
{
        if (!H.inited) return 0;

        H.st.current_step = 0;
        H.st.percent = 0;
        H.st.step_title = "Welcome";
        host_status("Ready to install.");
        li_tui_render();

        host_serial("welcome", "");
        if (!li_tui_welcome()) return 0;

        li_ui_host_set_step(0);
        return 1;
}

void li_ui_host_set_step(int index)
{
        if (!H.inited) return;
        if (index < 0) index = 0;
        if (index >= H.count) index = H.count - 1;

        H.st.current_step = index;
        H.st.percent = H.count > 0 ? (index * 100) / H.count : 0;
        H.st.step_title = H.names[index];
        snprintf(H.step_title, sizeof(H.step_title), "%s", H.names[index]);
        H.st.step_title = H.step_title;
        host_status("Starting %s...", H.names[index]);
        li_tui_render();
}

int li_ui_host_run(const char *step_path)
{
        int   ui_in[2];
        int   ui_out[2];
        int   logp[2];
        pid_t pid;
        int   stage;

        if (!H.inited || step_path == NULL) return -1;

        if (pipe(ui_in) != 0) return -1;
        if (pipe(ui_out) != 0) {
                close(ui_in[0]);
                close(ui_in[1]);
                return -1;
        }
        if (pipe(logp) != 0) {
                close(ui_in[0]);
                close(ui_in[1]);
                close(ui_out[0]);
                close(ui_out[1]);
                return -1;
        }

        H.ui_len = 0;
        H.log_partial_len = 0;
        H.ui_eof = 0;
        H.log_eof = 0;
        H.child_done = 0;
        H.child_status = 0;
        H.done_rc = -1;
        H.in_menu = 0;
        H.item_count = 0;
        host_status("Running %s...", H.st.step_title);

        pid = fork();
        if (pid < 0) {
                close(ui_in[0]);
                close(ui_in[1]);
                close(ui_out[0]);
                close(ui_out[1]);
                close(logp[0]);
                close(logp[1]);
                return -1;
        }

        if (pid == 0) {
                char in_fd[16];
                char out_fd[16];
                char *argv[2];

                close(ui_in[1]);
                close(ui_out[0]);
                close(logp[0]);

                dup2(logp[1], STDOUT_FILENO);
                dup2(logp[1], STDERR_FILENO);
                if (logp[1] != STDOUT_FILENO && logp[1] != STDERR_FILENO)
                        close(logp[1]);

                /* Steps interact through the UI pipe only.  Close the real
                 * stdin so an unconverted prompt cannot consume TUI keys. */
                close(STDIN_FILENO);

                snprintf(in_fd, sizeof(in_fd), "%d", ui_in[0]);
                snprintf(out_fd, sizeof(out_fd), "%d", ui_out[1]);
                setenv("LI_UI_FD", in_fd, 1);
                setenv("LI_UI_OUT_FD", out_fd, 1);
                setenv("LI_UI_MODE", "tui", 1);
                setenv("LI_STATE_DIR", "/config/installer", 1);

                argv[0] = (char *)step_path;
                argv[1] = NULL;
                execve(step_path, argv, environ);
                _exit(127);
        }

        close(ui_in[0]);
        close(ui_out[1]);
        close(logp[1]);

        H.reply_fd = ui_in[1];
        H.ui_fd = ui_out[0];
        H.log_fd = logp[0];
        H.child = pid;

        while (!(H.ui_eof && H.log_eof && H.child_done)) {
                struct pollfd pf[2];
                int ui_i = -1;
                int log_i = -1;
                int nf = 0;
                int r;

                if (!H.ui_eof) {
                        pf[nf].fd = H.ui_fd;
                        pf[nf].events = POLLIN;
                        pf[nf].revents = 0;
                        ui_i = nf++;
                }
                if (!H.log_eof) {
                        pf[nf].fd = H.log_fd;
                        pf[nf].events = POLLIN;
                        pf[nf].revents = 0;
                        log_i = nf++;
                }

                r = nf > 0 ? poll(pf, (nfds_t)nf, 200) : 0;
                if (nf > 0 && r > 0) {
                        if (log_i >= 0 && pf[log_i].revents != 0)
                                (void)host_read_log(1);
                        if (ui_i >= 0 && pf[ui_i].revents != 0)
                                host_read_ui();
                }

                if (!H.child_done) {
                        int   status = 0;
                        pid_t w = waitpid(H.child, &status, WNOHANG);

                        if (w == H.child) {
                                H.child_done = 1;
                                H.child_status = status;
                        } else if (w < 0) {
                                H.child_done = 1;
                                H.child_status = 1 << 8;
                        }
                }

                if (nf == 0 && !H.child_done) (void)usleep(10000);
        }

        if (!H.child_done) {
                int status = 0;

                if (waitpid(H.child, &status, 0) == H.child)
                        H.child_status = status;
                H.child_done = 1;
        }

        (void)host_read_log(1);
        host_log_flush();

        close(H.reply_fd);
        close(H.ui_fd);
        close(H.log_fd);
        H.reply_fd = -1;
        H.ui_fd = -1;
        H.log_fd = -1;

        stage = WIFEXITED(H.child_status) ? WEXITSTATUS(H.child_status) : 1;
        if (stage == 0)
                host_status("Step complete: %s", H.st.step_title);
        else
                host_status("Step failed (rc=%d): %s", stage, H.st.step_title);
        li_tui_render();
        return stage;
}

int li_ui_host_retry(int index, int rc)
{
        char message[256];
        int  retry;

        if (!H.inited) return 0;
        if (index < 0 || index >= H.count) index = 0;

        snprintf(message, sizeof(message),
                 "Step \"%s\" exited with code %d.  The log pane shows the last output.",
                 H.names[index], rc);
        host_serial("error", H.names[index]);
        retry = li_tui_error("Installation step failed", message, 1);
        if (retry) {
                li_ui_host_set_step(index);
                return 1;
        }
        return 0;
}

int li_ui_host_finish(void)
{
        if (!H.inited) return 1;

        H.st.current_step = H.count > 0 ? H.count - 1 : 0;
        H.st.percent = 100;
        host_status("Installation complete.");
        li_tui_render();
        host_serial("finish", "");
        return li_tui_finish();
}
