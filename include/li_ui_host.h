/*
 * li_ui_host.h — orchestrator-side TUI host.
 *
 * The installer main process owns the single lpcurses screen.  Each step is
 * still a separate program: the host creates the UI pipes, forks/execs the
 * step, routes menu/input/confirm requests to the screen, and captures the
 * step's stdout as log lines.
 */
#ifndef LI_UI_HOST_H
#define LI_UI_HOST_H

#define LI_UI_MAX_STEPS 64

/* Returns 0 on success, -1 if lpcurses could not start. */
int  li_ui_host_init(const char *const *step_paths, int step_count);
void li_ui_host_end(void);

/* 1 = begin, 0 = shut down. */
int  li_ui_host_welcome(void);

void li_ui_host_set_step(int index);

/* Run one step under the TUI.  Returns the step exit code (0..255) or -1. */
int  li_ui_host_run(const char *step_path);

/* 1 = retry, 0 = shut down. */
int  li_ui_host_retry(int index, int rc);

/* 1 = reboot, 0 = shut down. */
int  li_ui_host_finish(void);

#endif /* LI_UI_HOST_H */
