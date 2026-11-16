/*
 * lib/li_tui.c — full-screen installer widgets built on lpcurses.
 *
 * Rendering model:
 *   - stdscr holds the persistent frame (header, step list, progress, log).
 *   - menus/inputs/confirmations are temporary boxed windows drawn over it.
 *   - every widget restores stdscr with li_tui_render() when it closes.
 *
 * No step program links this file; the orchestrator is the only screen owner.
 */
#include "li_tui.h"

#include <curses.h>
#include <stdio.h>
#include <string.h>

#define LOG_LINES 256
#define LOG_COLS  256

#define PAIR_HEADER 1
#define PAIR_SELECT 2
#define PAIR_WARN   3
#define PAIR_ERROR  4
#define PAIR_DIM    5
#define PAIR_GOOD   6

static const li_tui_state *g_st;
static void (*g_pump)(void);
static int  g_colors;

static char g_log[LOG_LINES][LOG_COLS];
static int  g_log_head;
static int  g_log_count;

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static attr_t ui_attr(int pair, attr_t extra)
{
        if (!g_colors) return extra;
        return COLOR_PAIR(pair) | extra;
}

static int u8_decode(const unsigned char *s, unsigned int *cp)
{
        if (s[0] < 0x80U) {
                *cp = s[0];
                return 1;
        }
        if ((s[0] & 0xe0U) == 0xc0U && s[1] != '\0' && (s[1] & 0xc0U) == 0x80U) {
                *cp = ((unsigned int)(s[0] & 0x1fU) << 6) | (unsigned int)(s[1] & 0x3fU);
                return 2;
        }
        if ((s[0] & 0xf0U) == 0xe0U && s[1] != '\0' && s[2] != '\0' &&
            (s[1] & 0xc0U) == 0x80U && (s[2] & 0xc0U) == 0x80U) {
                *cp = ((unsigned int)(s[0] & 0x0fU) << 12) |
                      ((unsigned int)(s[1] & 0x3fU) << 6) |
                      (unsigned int)(s[2] & 0x3fU);
                return 3;
        }
        if ((s[0] & 0xf8U) == 0xf0U && s[1] != '\0' && s[2] != '\0' && s[3] != '\0' &&
            (s[1] & 0xc0U) == 0x80U && (s[2] & 0xc0U) == 0x80U &&
            (s[3] & 0xc0U) == 0x80U) {
                *cp = ((unsigned int)(s[0] & 0x07U) << 18) |
                      ((unsigned int)(s[1] & 0x3fU) << 12) |
                      ((unsigned int)(s[2] & 0x3fU) << 6) |
                      (unsigned int)(s[3] & 0x3fU);
                return 4;
        }
        *cp = s[0];
        return 1;
}

static int cp_cols(unsigned int cp)
{
        if (cp == 0) return 1;
        if ((cp >= 0x1100U && cp <= 0x115fU) ||
            (cp >= 0x2e80U && cp <= 0xa4cfU) ||
            (cp >= 0xac00U && cp <= 0xd7a3U) ||
            (cp >= 0xf900U && cp <= 0xfaffU) ||
            (cp >= 0xfe30U && cp <= 0xfe4fU) ||
            (cp >= 0xff00U && cp <= 0xff60U) ||
            (cp >= 0xffe0U && cp <= 0xffe6U) ||
            (cp >= 0x1f300U && cp <= 0x1faffU))
                return 2;
        return 1;
}

/* Draw UTF-8 text, clipping at maxcols screen cells. */
static void draw_text(WINDOW *w, int y, int x, attr_t attr, const char *text, int maxcols)
{
        const unsigned char *p = (const unsigned char *)text;
        int cx = x;

        if (w == NULL || text == NULL || maxcols <= 0) return;
        if (y < 0 || y >= getmaxy(w) || x < 0 || x >= getmaxx(w)) return;

        wattrset(w, attr);
        wmove(w, y, x);
        while (*p != '\0' && cx < x + maxcols) {
                unsigned int cp;
                int          used = u8_decode(p, &cp);
                int          cols = cp_cols(cp);
                char         tmp[5];

                if (cx + cols > x + maxcols) break;
                if (used <= 1) {
                        waddch(w, (chtype)*p);
                } else {
                        memcpy(tmp, p, (size_t)used);
                        tmp[used] = '\0';
                        waddstr(w, tmp);
                }
                p += used;
                cx += cols;
        }
        wattrset(w, A_NORMAL);
}

static void fill_row(WINDOW *w, int y, attr_t attr, chtype ch)
{
        int cols = getmaxx(w);

        if (w == NULL || y < 0 || y >= getmaxy(w)) return;
        wattrset(w, attr);
        wmove(w, y, 0);
        for (int x = 0; x < cols; x++)
                (void)waddch(w, ch);
        wattrset(w, A_NORMAL);
}

static void pump(void)
{
        if (g_pump != NULL) g_pump();
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

int li_tui_init(void)
{
        if (initscr() == NULL) return -1;

        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        curs_set(0);

        g_colors = 0;
        if (has_colors()) {
                start_color();
                if (init_pair(PAIR_HEADER, COLOR_WHITE, COLOR_BLUE) == OK &&
                    init_pair(PAIR_SELECT, COLOR_BLACK, COLOR_CYAN) == OK &&
                    init_pair(PAIR_WARN, COLOR_YELLOW, COLOR_BLACK) == OK &&
                    init_pair(PAIR_ERROR, COLOR_WHITE, COLOR_RED) == OK &&
                    init_pair(PAIR_DIM, COLOR_WHITE, COLOR_BLACK) == OK &&
                    init_pair(PAIR_GOOD, COLOR_GREEN, COLOR_BLACK) == OK)
                        g_colors = 1;
        }
        return 0;
}

void li_tui_end(void)
{
        if (stdscr != NULL) {
                curs_set(1);
                endwin();
        }
}

void li_tui_set_state(const li_tui_state *st)
{
        g_st = st;
}

void li_tui_set_pump(void (*fn)(void))
{
        g_pump = fn;
}

/* ------------------------------------------------------------------ */
/* Log ring                                                           */
/* ------------------------------------------------------------------ */

void li_tui_log_reset(void)
{
        g_log_head = 0;
        g_log_count = 0;
}

void li_tui_log_append(const char *text)
{
        size_t i = 0;

        if (text == NULL) return;
        while (text[i] != '\0' && i + 1 < LOG_COLS) {
                char c = text[i];

                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                g_log[g_log_head][i] = c;
                i++;
        }
        g_log[g_log_head][i] = '\0';
        g_log_head = (g_log_head + 1) % LOG_LINES;
        if (g_log_count < LOG_LINES) g_log_count++;
}

static const char *log_at(int index)
{
        int slot = (g_log_head - g_log_count + index + LOG_LINES) % LOG_LINES;

        return g_log[slot];
}

/* ------------------------------------------------------------------ */
/* Persistent frame                                                   */
/* ------------------------------------------------------------------ */

static void draw_progress(int y, int percent)
{
        int barw = COLS - 14;
        int filled;

        if (barw > 48) barw = 48;
        if (barw < 8) barw = 8;
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        filled = (barw * percent) / 100;

        wattrset(stdscr, ui_attr(PAIR_DIM, A_BOLD));
        wmove(stdscr, y, 2);
        (void)waddch(stdscr, '[');
        for (int i = 0; i < barw; i++)
                (void)waddch(stdscr, i < filled ? '#' : '-');
        (void)waddch(stdscr, ']');
        wattrset(stdscr, A_NORMAL);
        wmove(stdscr, y, 4 + barw);
        wprintw(stdscr, " %3d%%", percent);
}

static void draw_step_list(int top, int bottom)
{
        int cols = 3;
        int colw;
        int count;

        if (g_st == NULL || g_st->step_names == NULL) return;

        if (COLS < 48) cols = 1;
        else if (COLS < 76) cols = 2;
        colw = COLS / cols;
        count = g_st->step_name_count;

        for (int i = 0; i < count; i++) {
                int         row = top + i / cols;
                int         col = (i % cols) * colw;
                const char *mark;
                attr_t      attr;
                char        line[96];

                if (row > bottom) break;

                if (i < g_st->current_step) {
                        mark = "[x]";
                        attr = ui_attr(PAIR_GOOD, 0);
                } else if (i == g_st->current_step) {
                        mark = "[>]";
                        attr = ui_attr(PAIR_SELECT, A_BOLD);
                } else {
                        mark = "[ ]";
                        attr = ui_attr(PAIR_DIM, 0);
                }
                snprintf(line, sizeof(line), "%s %s", mark, g_st->step_names[i]);
                draw_text(stdscr, row, 1 + col, attr, line, colw - 1);
        }
}

void li_tui_render(void)
{
        int log_h;
        int log_top;
        int body_top;
        int body_bot;
        char line[256];

        if (stdscr == NULL || g_st == NULL || LINES <= 0 || COLS <= 0) return;

        werase(stdscr);

        /* Header */
        fill_row(stdscr, 0, ui_attr(PAIR_HEADER, A_BOLD), ' ');
        draw_text(stdscr, 0, 2, ui_attr(PAIR_HEADER, A_BOLD),
                  "LeopardOS Installer", COLS - 4);
        snprintf(line, sizeof(line), "%d/%d",
                 g_st->current_step + 1,
                 g_st->total_steps > 0 ? g_st->total_steps : 1);
        if ((int)strlen(line) + 2 < COLS)
                draw_text(stdscr, 0, COLS - (int)strlen(line) - 2,
                          ui_attr(PAIR_HEADER, A_BOLD), line, (int)strlen(line));

        if (LINES > 1) {
                snprintf(line, sizeof(line), "Step %d/%d  %s",
                         g_st->current_step + 1,
                         g_st->total_steps > 0 ? g_st->total_steps : 1,
                         g_st->step_title != NULL ? g_st->step_title : "");
                fill_row(stdscr, 1, A_NORMAL, ' ');
                draw_text(stdscr, 1, 2, A_BOLD, line, COLS - 4);
        }
        if (LINES > 2)
                draw_progress(2, g_st->percent);
        if (LINES > 3) {
                fill_row(stdscr, 3, A_NORMAL, ' ');
                draw_text(stdscr, 3, 2, ui_attr(PAIR_DIM, 0), g_st->status, COLS - 4);
        }
        if (LINES > 4)
                fill_row(stdscr, 4, ui_attr(PAIR_DIM, 0), '-');

        /* Log pane geometry */
        log_h = 0;
        if (g_st->log_visible && LINES >= 14) {
                log_h = LINES / 3;
                if (log_h < 5) log_h = 5;
                if (log_h > 10) log_h = 10;
        }
        log_top = LINES - log_h;
        body_top = 5;
        body_bot = log_h > 0 ? log_top - 2 : LINES - 2;
        if (body_bot < body_top) body_bot = body_top;

        draw_step_list(body_top, body_bot);

        if (log_h > 0 && log_top >= 2) {
                int visible = log_h - 1;
                int first;

                fill_row(stdscr, log_top - 1, ui_attr(PAIR_DIM, 0), '-');
                draw_text(stdscr, log_top - 1, 2, ui_attr(PAIR_DIM, A_BOLD), " log ", 5);

                if (visible > 0) {
                        first = g_log_count - visible;
                        if (first < 0) first = 0;
                        for (int i = 0; i < visible && first + i < g_log_count; i++)
                                draw_text(stdscr, log_top + i, 1,
                                          ui_attr(PAIR_DIM, 0),
                                          log_at(first + i), COLS - 2);
                }
        }

        /* Footer */
        if (LINES > 1) {
                fill_row(stdscr, LINES - 1, ui_attr(PAIR_HEADER, A_BOLD), ' ');
                draw_text(stdscr, LINES - 1, 2, ui_attr(PAIR_HEADER, A_BOLD),
                          "Enter Select  Esc Back  F2 Log", COLS - 4);
        }

        wrefresh(stdscr);
}

/* ------------------------------------------------------------------ */
/* Modal window helpers                                               */
/* ------------------------------------------------------------------ */

static WINDOW *modal_new(int want_h, int want_w, int *out_h, int *out_w)
{
        int w = want_w;
        int h = want_h;
        int y;
        int x;
        WINDOW *win;

        if (LINES < 6 || COLS < 20) return NULL;
        if (w > COLS - 2) w = COLS - 2;
        if (w < 20) w = 20;
        if (h > LINES - 2) h = LINES - 2;
        if (h < 5) h = 5;
        if (w > COLS) w = COLS;
        if (h > LINES) h = LINES;

        y = (LINES - h) / 2;
        x = (COLS - w) / 2;
        if (y < 0) y = 0;
        if (x < 0) x = 0;

        win = newwin(h, w, y, x);
        if (win == NULL) return NULL;
        werase(win);
        box(win, 0, 0);
        *out_h = h;
        *out_w = w;
        return win;
}

/* Word-wrap text into at most max_lines rows. Returns rows used. */
static int draw_wrapped(WINDOW *win, int y, int x, int width,
                        attr_t attr, const char *text, int max_lines)
{
        int   used = 0;
        char  line[256];
        size_t n = 0;

        if (text == NULL || width <= 1 || max_lines <= 0) return 0;

        while (*text != '\0' && used < max_lines) {
                const char *word = text;
                size_t      wlen = 0;

                while (*text == ' ') text++;
                while (text[wlen] != '\0' && text[wlen] != ' ') wlen++;
                text += wlen;
                if (wlen == 0) break;

                if (n + (n ? 1 : 0) + wlen > (size_t)width) {
                        if (n > 0) {
                                line[n] = '\0';
                                draw_text(win, y + used, x, attr, line, width);
                                used++;
                                n = 0;
                                if (used >= max_lines) break;
                        }
                        if (wlen > (size_t)width) wlen = (size_t)width;
                }
                if (n > 0) line[n++] = ' ';
                memcpy(line + n, word, wlen);
                n += wlen;
                if (*text == ' ') text++;
        }
        if (n > 0 && used < max_lines) {
                line[n] = '\0';
                draw_text(win, y + used, x, attr, line, width);
                used++;
        }
        return used;
}

/*
 * Shared selectable menu.  Returns the selected index or -1 for cancel.
 * note/message (optional) is wrapped above the items.
 */
static int menu_run(const char *title, const char *note,
                    const li_ui_item *items, int count, int selected)
{
        int result = -1;
        int finished = 0;

        if (count <= 0) return -1;
        if (selected < 0 || selected >= count) selected = 0;

        while (!finished) {
                WINDOW *win;
                int     h;
                int     w;
                int     title_w = title != NULL ? (int)strlen(title) : 8;
                int     item_w = 0;
                int     note_rows;
                int     item_top;
                int     visible;
                int     top;
                int     rebuild = 0;

                for (int i = 0; i < count; i++) {
                        int len = (int)strlen(items[i].title);

                        if (items[i].desc != NULL)
                                len += (int)strlen(items[i].desc) + 2;
                        if (len > item_w) item_w = len;
                }
                w = title_w + 6;
                if (item_w + 6 > w) w = item_w + 6;
                if (note != NULL && (int)strlen(note) + 6 > w)
                        w = (int)strlen(note) + 6;
                if (w > 78) w = 78;
                if (w < 30) w = 30;

                h = count + 5;
                if (note != NULL) h += 3;
                if (h > LINES - 2) h = LINES - 2;
                if (h < 7) h = 7;

                win = modal_new(h, w, &h, &w);
                if (win == NULL) return -1;

                draw_text(win, 0, 2, ui_attr(PAIR_HEADER, A_BOLD),
                          title != NULL ? title : "", w - 4);

                note_rows = 0;
                if (note != NULL)
                        note_rows = draw_wrapped(win, 1, 2, w - 4,
                                                 ui_attr(PAIR_DIM, 0), note, 2);

                item_top = 1 + note_rows + (note_rows > 0 ? 1 : 0);
                visible = h - item_top - 2;
                if (visible < 1) visible = 1;

                top = 0;
                if (selected >= visible) top = selected - visible + 1;

                for (int row = 0; row < visible; row++) {
                        int i = top + row;
                        int y = item_top + row;
                        attr_t attr;

                        if (i >= count) break;
                        if (i == selected) {
                                attr = ui_attr(PAIR_SELECT, A_BOLD);
                                fill_row(win, y, attr, ' ');
                        } else {
                                attr = A_NORMAL;
                        }
                        draw_text(win, y, 2, attr, items[i].title, w - 5);
                        if (items[i].desc != NULL && items[i].desc[0] != '\0') {
                                int tx = 2 + (int)strlen(items[i].title) + 2;

                                if (tx < w - 4)
                                        draw_text(win, y, tx,
                                                  i == selected ? attr : ui_attr(PAIR_DIM, 0),
                                                  items[i].desc, w - tx - 2);
                        }
                }

                draw_text(win, h - 1, 2, ui_attr(PAIR_DIM, 0),
                          "Enter Select   Esc Cancel", w - 4);
                wrefresh(win);
                wtimeout(win, 250);

                while (!rebuild && !finished) {
                        int ch = wgetch(win);

                        if (ch == ERR) {
                                pump();
                                continue;
                        }
                        if (ch == KEY_RESIZE) {
                                rebuild = 1;
                        } else if (ch == KEY_UP || ch == 'k') {
                                selected = (selected + count - 1) % count;
                                rebuild = 1;
                        } else if (ch == KEY_DOWN || ch == 'j') {
                                selected = (selected + 1) % count;
                                rebuild = 1;
                        } else if (ch == KEY_HOME) {
                                selected = 0;
                                rebuild = 1;
                        } else if (ch == KEY_END) {
                                selected = count - 1;
                                rebuild = 1;
                        } else if (ch == KEY_PPAGE) {
                                selected -= visible;
                                if (selected < 0) selected = 0;
                                rebuild = 1;
                        } else if (ch == KEY_NPAGE) {
                                selected += visible;
                                if (selected >= count) selected = count - 1;
                                rebuild = 1;
                        } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
                                result = selected;
                                finished = 1;
                        } else if (ch == 27 || ch == 'q' || ch == 'Q') {
                                result = -1;
                                finished = 1;
                        }
                }

                delwin(win);
                li_tui_render();
        }

        return result;
}

/* ------------------------------------------------------------------ */
/* Public widgets                                                     */
/* ------------------------------------------------------------------ */

int li_tui_welcome(void)
{
        li_ui_item items[2];

        items[0].title = "Begin installation";
        items[0].desc = "Select a disk and install LeopardOS";
        items[1].title = "Shut down";
        items[1].desc = "Power off without installing";

        li_tui_render();
        return menu_run("LeopardOS installer",
                        "This will erase the selected disk and install LeopardOS.",
                        items, 2, 0) == 0;
}

int li_tui_menu(const char *title, const li_ui_item *items, int count, int selected)
{
        return menu_run(title, NULL, items, count, selected);
}

int li_tui_confirm(const char *prompt, int default_yes)
{
        li_ui_item items[2];
        int        rc;

        items[0].title = "Yes";
        items[0].desc = NULL;
        items[1].title = "No";
        items[1].desc = NULL;

        rc = menu_run("Confirm", prompt, items, 2, default_yes ? 0 : 1);
        if (rc < 0) return -1;
        return rc == 0;
}

int li_tui_input(const char *prompt, const char *def, char *out, size_t cap)
{
        char buf[256];
        int  cursor;
        int  result = -1;
        int  finished = 0;

        if (out == NULL || cap == 0) return -1;
        out[0] = '\0';

        snprintf(buf, sizeof(buf), "%s", def != NULL ? def : "");
        cursor = (int)strlen(buf);

        while (!finished) {
                WINDOW *win;
                int     h;
                int     w;
                int     field_y;
                int     rebuild = 0;

                w = (prompt != NULL ? (int)strlen(prompt) : 8) + 6;
                if (w < 40) w = 40;
                if (w > 78) w = 78;
                win = modal_new(8, w, &h, &w);
                if (win == NULL) return -1;

                draw_text(win, 0, 2, ui_attr(PAIR_HEADER, A_BOLD), "Input", w - 4);
                draw_wrapped(win, 1, 2, w - 4, A_NORMAL,
                             prompt != NULL ? prompt : "", 2);

                field_y = 4;
                wattrset(win, A_REVERSE);
                wmove(win, field_y, 1);
                for (int x = 1; x < w - 1; x++)
                        (void)waddch(win, ' ');
                wattrset(win, A_NORMAL);
                draw_text(win, field_y, 2, A_REVERSE, buf, w - 4);
                wmove(win, field_y, 2 + (cursor < w - 4 ? cursor : w - 5));
                curs_set(1);
                draw_text(win, h - 1, 2, ui_attr(PAIR_DIM, 0),
                          "Enter Accept   Esc Cancel", w - 4);
                wrefresh(win);
                wtimeout(win, 250);

                while (!rebuild && !finished) {
                        int ch = wgetch(win);

                        if (ch == ERR) {
                                pump();
                                continue;
                        }

                        curs_set(0);
                        if (ch == KEY_RESIZE) {
                                rebuild = 1;
                        } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
                                result = 0;
                                finished = 1;
                        } else if (ch == 27) {
                                result = -1;
                                finished = 1;
                        } else {
                                if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                                        if (cursor > 0) {
                                                memmove(buf + cursor - 1, buf + cursor,
                                                        strlen(buf + cursor) + 1);
                                                cursor--;
                                        }
                                } else if (ch == KEY_DC) {
                                        size_t len = strlen(buf);

                                        if (cursor < (int)len)
                                                memmove(buf + cursor, buf + cursor + 1,
                                                        len - (size_t)cursor);
                                } else if (ch == KEY_LEFT) {
                                        if (cursor > 0) cursor--;
                                } else if (ch == KEY_RIGHT) {
                                        if (buf[cursor] != '\0') cursor++;
                                } else if (ch == KEY_HOME) {
                                        cursor = 0;
                                } else if (ch == KEY_END) {
                                        cursor = (int)strlen(buf);
                                } else if (ch >= 32 && ch < 256) {
                                        size_t len = strlen(buf);

                                        if (len + 1 < sizeof(buf)) {
                                                memmove(buf + cursor + 1, buf + cursor,
                                                        len - (size_t)cursor + 1);
                                                buf[cursor++] = (char)ch;
                                        }
                                }
                                rebuild = 1;
                        }
                }

                delwin(win);
                li_tui_render();
        }

        if (result == 0) snprintf(out, cap, "%s", buf);
        li_tui_render();
        return result;
}

int li_tui_error(const char *title, const char *message, int allow_retry)
{
        li_ui_item items[2];
        int        rc;
        int        count = allow_retry ? 2 : 1;

        items[0].title = "Retry step";
        items[0].desc = "Run the failed step again";
        items[1].title = "Shut down";
        items[1].desc = "Stop the installer";

        if (!allow_retry) {
                items[0] = items[1];
                items[1].title = NULL;
                items[1].desc = NULL;
        }

        rc = menu_run(title, message, items, count, 0);
        if (!allow_retry) return 0;
        return rc == 0;
}

int li_tui_finish(void)
{
        li_ui_item items[2];
        int        rc;

        items[0].title = "Reboot";
        items[0].desc = "Remove the installation media first";
        items[1].title = "Shut down";
        items[1].desc = "Power off the machine";

        rc = menu_run("Installation complete",
                      "All steps finished successfully.", items, 2, 0);
        return rc == 1 ? 0 : 1;
}
