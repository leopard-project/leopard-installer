/*
 * li_tui.h — lpcurses-based full-screen widgets for the installer.
 *
 * Only the installer orchestrator links this file.  Step programs talk to it
 * through the li_ui pipe protocol and never call curses themselves.
 */
#ifndef LI_TUI_H
#define LI_TUI_H

#include "li_ui.h"

typedef struct {
        int              total_steps;
        int              current_step;
        int              percent;      /* overall 0..100 */
        const char      *step_title;   /* current step title */
        const char *const *step_names; /* display names for the step list */
        int              step_name_count;
        char             status[192];  /* latest activity line */
        int              log_visible;
} li_tui_state;

int  li_tui_init(void);
void li_tui_end(void);

void li_tui_set_state(const li_tui_state *st);
void li_tui_render(void);

/* Log ring buffer shown in the bottom pane. */
void li_tui_log_reset(void);
void li_tui_log_append(const char *text);

/* Hook used by modal key loops to keep draining the step's log pipe. */
void li_tui_set_pump(void (*fn)(void));

/* Widgets. */
int  li_tui_welcome(void);                 /* 1 begin, 0 shut down */
int  li_tui_menu(const char *title, const li_ui_item *items, int count, int selected);
int  li_tui_input(const char *prompt, const char *def, char *out, size_t cap);
int  li_tui_confirm(const char *prompt, int default_yes);   /* 1 yes, 0 no, -1 cancel */
int  li_tui_error(const char *title, const char *message, int allow_retry);
int  li_tui_finish(void);                  /* 1 reboot, 0 shutdown */

#endif /* LI_TUI_H */
