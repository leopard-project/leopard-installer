/*
 * li_ui.h — step-side UI abstraction for the installer.
 *
 * A step program never owns the screen.  The installer main process owns the
 * single lpcurses screen and talks to each step over an inherited pipe:
 *
 *   main (TUI owner)
 *     +-- lpcurses screen
 *     +-- step sequencer
 *     +-- LI_UI_FD / LI_UI_OUT_FD  <-->  step child
 *
 * If the pipe is absent (line mode), the same API falls back to the classic
 * xapi_Input prompts.  In auto mode every call returns its default without
 * blocking, so auto=1 stays zero-input.
 */
#ifndef LI_UI_H
#define LI_UI_H

#include <stdint.h>

/* li_ui_mode() results. */
#define LI_UI_TUI   1
#define LI_UI_LINE  2
#define LI_UI_AUTO  3

/* Result codes shared by the interactive calls. */
#define LI_UI_OK      0
#define LI_UI_CANCEL (-1)
#define LI_UI_ERROR  (-2)

#define LI_UI_PROTOCOL 1

typedef struct {
        const char *title;
        const char *desc;   /* optional, may be NULL */
} li_ui_item;

/* Mode helpers. */
int  li_ui_mode(void);
int  li_ui_active(void);

/* Interactive calls.
 *   menu:    returns the chosen index, LI_UI_CANCEL, or LI_UI_ERROR.
 *   input:   LI_UI_OK and out filled, LI_UI_CANCEL, or LI_UI_ERROR.
 *   confirm: 1 = yes, 0 = no/cancel, or the default in auto mode.
 */
int  li_ui_menu(const char *title, const li_ui_item *items, int count, int selected);
int  li_ui_input(const char *prompt, char *out, size_t cap);
int  li_ui_confirm(const char *prompt, int default_yes);

/* Fire-and-forget status.  No-op outside TUI mode. */
void li_ui_step(const char *title);
void li_ui_progress(int percent, const char *text);
void li_ui_log(const char *fmt, ...);
void li_ui_done(int rc);

#endif /* LI_UI_H */
