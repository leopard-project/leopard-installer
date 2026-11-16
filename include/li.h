/*
 * li.h — Shared library for install step programs (state / options / log / interaction / subprocess / block devices)
 *
 * Architecture (main.c = sequence orchestrator, do not modify):
 *   main.c reads /config/leopard-install-sequence.conf and runs each step program
 *   in line order by **full path, no arguments**, waiting for it to exit before
 *   running the next step; a non-zero step exit prompts retry or shutdown;
 *   if all succeed, reboot.
 *
 * So there are only two contracts between step programs:
 *   1. Exit code: 0 = success, non-zero = failure;
 *   2. State file /config/installer/state (key=value) — steps exchange data only
 *      through it, so any step can be replaced, reordered, and rerun on its own.
 *
 * Options file /config/leopard-install.conf (key=value, loaded first as defaults):
 *   auto=1             unattended (steps stop prompting; failure means shutdown, no retry, no blocking on keyboard)
 *   disk=sata0         target disk
 *   scheme=auto|custom partition scheme (custom also needs part.N.mount / part.N.size_mb)
 *   init=...           PID1 path in boot.conf
 */
#ifndef LI_H
#define LI_H

#include "xtuiapi.h"

#ifndef SYS_SYNC
#define SYS_SYNC 162 /* not exported by libsys.h; already wired in kernel syscall.cpp */
#endif

#define LI_TARGET       "/mnt"                        /* target root mount point */
#define LI_STATE_DIR    "/config/installer"           /* runtime state directory (writable in live) */
#define LI_STATE_PATH   LI_STATE_DIR "/state"
#define LI_LOG_PATH     LI_STATE_DIR "/install.log"
#define LI_OPTIONS_PATH "/config/leopard-install.conf" /* user-editable install options */
#define LI_TOOL_DIR     "/apps/system/"                /* lpdisk / mkfs-fat32 / xbps-install */

#define LI_MAX_KEYS    96
#define LI_KEY_LEN     40
#define LI_VAL_LEN     192
#define LI_LINE_LEN    512
#define LI_PATH_LEN    192
#define LI_TEXT_CAP    16384

/* ---- State store (options file loaded first, state overrides it) ---- */
int         li_state_load(void);
int         li_state_save(void);
const char *li_state_get(const char *key);
int         li_state_set(const char *key, const char *value);
int         li_state_get_int(const char *key, int def);
int         li_state_is(const char *key, const char *value);

/* ---- Log / interaction ---- */
void li_log(const char *fmt, ...);
void li_header(const char *text);
int  li_confirm(const char *prompt, int default_yes);
int  li_ask(const char *prompt, char *out, size_t cap);
int  li_is_auto(void);

/* ---- Subprocess ---- */
int li_run(const char *path, char *const argv[]);
int li_run_tool(const char *file, char *const argv[]);

/* ---- File / block device primitives ---- */
int      li_open(const char *path, uint64_t flags);
int64_t  li_lseek(int fd, int64_t offset, uint64_t whence);
int      li_close(int fd);
uint64_t li_dev_sectors(const char *dev);          /* dev is like sata0 / sata0p0 */
int      li_file_exists(const char *path);
int      li_read_text(const char *path, char *out, size_t cap);
int      li_write_text(const char *path, const char *text);
int      li_write_binary(const char *path, const void *data, size_t length);
int      li_copy_file(const char *src, const char *dst);
int      li_make_dirs(const char *path);

/* ---- Step program boilerplate ---- */
void li_step_begin(const char *title);
int  li_step_end(int rc);
int  li_fail(const char *what, int detail);

#endif /* LI_H */
