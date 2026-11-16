/*
 * lib/li_ui.c — step-side client for the installer UI protocol.
 *
 * Steps link this file through the shared step library.  It has no curses
 * dependency: in TUI mode it only writes newline-delimited requests to the
 * inherited pipe and reads the matching answer.  In line/auto mode it keeps
 * the historical xapi_Input behaviour.
 *
 * Wire format (UTF-8, tab-separated fields, newline terminated):
 *
 *   step -> host
 *     STEP <title>
 *     MENU <id>\t<title>
 *     ITEM <index>\t<title>\t<description>
 *     END
 *     INPUT <id>\t<prompt>\t<default>
 *     CONFIRM <id>\t<prompt>\t<y|n>
 *     PROGRESS <0..100>\t<text>
 *     LOG <text>
 *     DONE <rc>
 *
 *   host -> step
 *     ANSWER <id>\t<value>
 *     CANCEL <id>
 */
#include "li_ui.h"
#include "li.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UI_LINE_CAP 1024
#define UI_IN_CAP   4096

static int    s_mode;
static int    s_in_fd  = -1;
static int    s_out_fd = -1;
static int    s_req;
static char   s_in[UI_IN_CAP];
static size_t s_in_len;

/* Replace protocol separators so arbitrary titles remain one line/field. */
static void ui_field(char *dst, size_t cap, const char *src)
{
        size_t i;
        size_t j = 0;

        if (cap == 0) return;
        if (src != NULL) {
                for (i = 0; src[i] != '\0' && j + 1 < cap; i++) {
                        char c = src[i];

                        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                        dst[j++] = c;
                }
        }
        dst[j] = '\0';
}

static int ui_write_all(const char *buf, size_t len)
{
        size_t off = 0;

        if (s_out_fd < 0) return -1;
        while (off < len) {
                ssize_t n = write(s_out_fd, buf + off, len - off);

                if (n < 0) {
                        if (errno == EINTR) continue;
                        return -1;
                }
                if (n == 0) return -1;
                off += (size_t)n;
        }
        return 0;
}

static int ui_sendf(const char *fmt, ...)
{
        char    line[UI_LINE_CAP];
        va_list ap;
        int     n;

        va_start(ap, fmt);
        n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
        va_end(ap);
        if (n < 0) return -1;
        if (n >= (int)sizeof(line) - 1) n = (int)sizeof(line) - 2;
        line[n++] = '\n';
        return ui_write_all(line, (size_t)n);
}

static int ui_getline(char *out, size_t cap)
{
        for (;;) {
                char  *nl;
                size_t n;
                size_t rest;
                ssize_t got;

                if (cap == 0) return -1;
                nl = memchr(s_in, '\n', s_in_len);
                if (nl != NULL) {
                        n = (size_t)(nl - s_in);
                        if (n >= cap) n = cap - 1;
                        memcpy(out, s_in, n);
                        out[n] = '\0';
                        rest = s_in_len - (size_t)(nl - s_in) - 1;
                        memmove(s_in, nl + 1, rest);
                        s_in_len = rest;
                        return 0;
                }
                if (s_in_len + 1 >= sizeof(s_in)) s_in_len = 0;

                got = read(s_in_fd, s_in + s_in_len, sizeof(s_in) - s_in_len - 1);
                if (got < 0) {
                        if (errno == EINTR) continue;
                        return -1;
                }
                if (got == 0) return -1;
                s_in_len += (size_t)got;
        }
}

/* Wait for ANSWER/CANCEL belonging to req. */
static int ui_reply(int req, char *val, size_t cap)
{
        char line[UI_LINE_CAP];

        for (;;) {
                if (ui_getline(line, sizeof(line)) != 0) return LI_UI_ERROR;

                if (strncmp(line, "ANSWER ", 7) == 0) {
                        char *p = line + 7;
                        char *tab = strchr(p, '\t');

                        if (atoi(p) == req) {
                                if (val != NULL && cap > 0)
                                        snprintf(val, cap, "%s", tab != NULL ? tab + 1 : "");
                                return LI_UI_OK;
                        }
                } else if (strncmp(line, "CANCEL ", 7) == 0) {
                        if (atoi(line + 7) == req) return LI_UI_CANCEL;
                }
        }
}

int li_ui_mode(void)
{
        const char *env;
        const char *fd;
        const char *ofd;

        if (s_mode != 0) return s_mode;

        env = getenv("LI_UI_MODE");
        fd  = getenv("LI_UI_FD");

        if (env != NULL && strcmp(env, "auto") == 0) {
                s_mode = LI_UI_AUTO;
                return s_mode;
        }
        if (fd != NULL && fd[0] != '\0' &&
            (env == NULL || strcmp(env, "tui") == 0)) {
                ofd = getenv("LI_UI_OUT_FD");
                s_in_fd  = atoi(fd);
                s_out_fd = (ofd != NULL && ofd[0] != '\0') ? atoi(ofd) : s_in_fd;
                if (s_in_fd >= 0 && s_out_fd >= 0) {
                        s_mode = LI_UI_TUI;
                        return s_mode;
                }
        }
        if (env != NULL && strcmp(env, "line") == 0) {
                s_mode = LI_UI_LINE;
                return s_mode;
        }
        if (li_is_auto()) {
                s_mode = LI_UI_AUTO;
                return s_mode;
        }
        s_mode = LI_UI_LINE;
        return s_mode;
}

int li_ui_active(void)
{
        return li_ui_mode() == LI_UI_TUI;
}

int li_ui_menu(const char *title, const li_ui_item *items, int count, int selected)
{
        int mode;

        if (count <= 0) return LI_UI_ERROR;
        if (selected < 0 || selected >= count) selected = 0;

        mode = li_ui_mode();
        if (mode == LI_UI_AUTO) return selected;

        if (mode == LI_UI_TUI) {
                char t[256];
                int  req = ++s_req;
                char val[64];
                int  rc;

                ui_field(t, sizeof(t), title);
                if (ui_sendf("MENU %d\t%s\t%d", req, t, selected) != 0) return LI_UI_ERROR;
                for (int i = 0; i < count; i++) {
                        char it[160];
                        char id[256];

                        ui_field(it, sizeof(it), items[i].title);
                        ui_field(id, sizeof(id), items[i].desc != NULL ? items[i].desc : "");
                        if (ui_sendf("ITEM %d\t%s\t%s", i, it, id) != 0) return LI_UI_ERROR;
                }
                if (ui_sendf("END") != 0) return LI_UI_ERROR;

                rc = ui_reply(req, val, sizeof(val));
                if (rc != LI_UI_OK) return rc;
                int idx = atoi(val);
                return (idx >= 0 && idx < count) ? idx : selected;
        }

        /* line mode: keep the historical numbered prompt */
        xapi_Printf("%s:\n", title != NULL ? title : "");
        for (int i = 0; i < count; i++) {
                xapi_Printf("  %d) %s\n", i + 1, items[i].title);
                if (items[i].desc != NULL && items[i].desc[0] != '\0')
                        xapi_Printf("     %s\n", items[i].desc);
        }
        {
                char ans[16];
                char prompt[64];

                snprintf(prompt, sizeof(prompt), "Choice [%d]: ", selected + 1);
                if (li_ask(prompt, ans, sizeof(ans)) != 0) return selected;
                if (ans[0] >= '1' && ans[0] <= '9') {
                        int idx = ans[0] - '1';

                        if (idx < count) return idx;
                }
        }
        return selected;
}

int li_ui_input(const char *prompt, char *out, size_t cap)
{
        int mode;

        if (out == NULL || cap == 0) return LI_UI_ERROR;
        out[0] = '\0';

        mode = li_ui_mode();
        if (mode == LI_UI_AUTO) return LI_UI_CANCEL;

        if (mode == LI_UI_TUI) {
                char p[256];
                char d[256];
                char val[512];
                int  req = ++s_req;
                int  rc;

                ui_field(p, sizeof(p), prompt);
                ui_field(d, sizeof(d), out);
                if (ui_sendf("INPUT %d\t%s\t%s", req, p, d) != 0) return LI_UI_ERROR;

                rc = ui_reply(req, val, sizeof(val));
                if (rc == LI_UI_OK) snprintf(out, cap, "%s", val);
                return rc;
        }

        if (li_ask(prompt, out, cap) != 0) return LI_UI_ERROR;
        return LI_UI_OK;
}

int li_ui_confirm(const char *prompt, int default_yes)
{
        int mode = li_ui_mode();

        if (mode == LI_UI_AUTO) return default_yes;

        if (mode == LI_UI_TUI) {
                char p[256];
                char val[16];
                int  req = ++s_req;
                int  rc;

                ui_field(p, sizeof(p), prompt);
                if (ui_sendf("CONFIRM %d\t%s\t%s", req, p,
                             default_yes ? "y" : "n") != 0)
                        return 0;

                rc = ui_reply(req, val, sizeof(val));
                if (rc == LI_UI_OK) return atoi(val) != 0;
                return 0;
        }

        return li_confirm(prompt, default_yes);
}

void li_ui_step(const char *title)
{
        char t[256];

        if (li_ui_mode() != LI_UI_TUI) return;
        ui_field(t, sizeof(t), title);
        (void)ui_sendf("STEP %s", t);
}

void li_ui_progress(int percent, const char *text)
{
        char t[200];

        if (li_ui_mode() != LI_UI_TUI) return;
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        ui_field(t, sizeof(t), text != NULL ? text : "");
        (void)ui_sendf("PROGRESS %d\t%s", percent, t);
}

void li_ui_log(const char *fmt, ...)
{
        char    line[LI_LINE_LEN];
        va_list ap;

        va_start(ap, fmt);
        vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);

        if (li_ui_mode() == LI_UI_TUI) {
                char t[LI_LINE_LEN];

                ui_field(t, sizeof(t), line);
                (void)ui_sendf("LOG %s", t);
        } else {
                li_log("%s", line);
        }
}

void li_ui_done(int rc)
{
        if (li_ui_mode() != LI_UI_TUI) return;
        (void)ui_sendf("DONE %d", rc);
}
