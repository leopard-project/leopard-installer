/*
 * li.h — 安装步骤程序公共库（状态 / 选项 / 日志 / 交互 / 子进程 / 块设备）
 *
 * 架构（main.c = 序列编排器，不改）：
 *   main.c 读取 /config/leopard-install-sequence.conf，按行序以**全路径、无参数**
 *   执行每个步骤程序，等它退出后再跑下一步；步骤返回非 0 则提示重试或关机；
 *   全部成功则重启。
 *
 * 所以步骤程序之间只有两条契约：
 *   1. 退出码：0 = 成功，非 0 = 失败；
 *   2. 状态文件 /config/installer/state（key=value）—— 步骤只通过它交换数据，
 *      因此任何一步都能单独替换、重排、重跑。
 *
 * 选项文件 /config/leopard-install.conf（key=value，作为默认值先加载）：
 *   auto=1             无人值守（步骤不再提问；失败即关机，不重试、不阻塞等键盘）
 *   disk=sata0         目标磁盘
 *   scheme=auto|custom 分区方案（custom 需另给 part.N.mount / part.N.size_mb）
 *   init=...           boot.conf 里的 PID1 路径
 */
#ifndef LI_H
#define LI_H

#include "xtuiapi.h"

#ifndef SYS_SYNC
#define SYS_SYNC 162 /* libsys.h 未导出，内核 syscall.cpp 已接线 */
#endif

#define LI_TARGET       "/mnt"                        /* 目标根挂载点 */
#define LI_STATE_DIR    "/config/installer"           /* 运行期状态目录（live 可写） */
#define LI_STATE_PATH   LI_STATE_DIR "/state"
#define LI_LOG_PATH     LI_STATE_DIR "/install.log"
#define LI_OPTIONS_PATH "/config/leopard-install.conf" /* 用户可改的安装选项 */
#define LI_TOOL_DIR     "/apps/system/"                /* lpdisk / mkfs-fat32 / xbps-install */

#define LI_MAX_KEYS    96
#define LI_KEY_LEN     40
#define LI_VAL_LEN     192
#define LI_LINE_LEN    512
#define LI_PATH_LEN    192
#define LI_TEXT_CAP    16384

/* ---- 状态存储（选项文件先加载，state 覆盖之） ---- */
int         li_state_load(void);
int         li_state_save(void);
const char *li_state_get(const char *key);
int         li_state_set(const char *key, const char *value);
int         li_state_get_int(const char *key, int def);
int         li_state_is(const char *key, const char *value);

/* ---- 日志 / 交互 ---- */
void li_log(const char *fmt, ...);
void li_header(const char *text);
int  li_confirm(const char *prompt, int default_yes);
int  li_ask(const char *prompt, char *out, size_t cap);
int  li_is_auto(void);

/* ---- 子进程 ---- */
int li_run(const char *path, char *const argv[]);
int li_run_tool(const char *file, char *const argv[]);

/* ---- 文件 / 块设备原语 ---- */
int      li_open(const char *path, uint64_t flags);
int64_t  li_lseek(int fd, int64_t offset, uint64_t whence);
int      li_close(int fd);
uint64_t li_dev_sectors(const char *dev);          /* dev 形如 sata0 / sata0p0 */
int      li_file_exists(const char *path);
int      li_read_text(const char *path, char *out, size_t cap);
int      li_write_text(const char *path, const char *text);
int      li_make_dirs(const char *path);

/* ---- 步骤程序样板 ---- */
void li_step_begin(const char *title);
int  li_step_end(int rc);
int  li_fail(const char *what, int detail);

#endif /* LI_H */
