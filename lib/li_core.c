/*
 * li_core.c — 安装步骤程序公共库（每个步骤程序链接它）
 *
 * 只用 XAPI 与内核 syscall 原语（SDK 没有 POSIX open/read/write）。
 * 状态以 key=value 存于 /config/installer/state；/config/leopard-install.conf
 * 的键作为默认值先加载，state 里的值覆盖它。
 */
#include "li.h"

#ifndef O_RDONLY
#define O_RDONLY 0
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

#define LI_SECTOR 512ULL

static char s_key[LI_MAX_KEYS][LI_KEY_LEN];
static char s_val[LI_MAX_KEYS][LI_VAL_LEN];
static int  s_count;
static int  s_loaded;

/* ---------------- 字符串小工具 ---------------- */

static void li_copy(char *dst, size_t cap, const char *src)
{
        size_t i = 0;

        if (cap == 0) return;
        while (src[i] != '\0' && (i + 1) < cap) {
                dst[i] = src[i];
                i++;
        }
        dst[i] = '\0';
}

static int li_isspace(char c)
{
        return c == ' ' || c == '\t' || c == '\r';
}

static char *li_trim(char *s)
{
        size_t n;

        while (*s != '\0' && li_isspace(*s)) s++;
        n = strlen(s);
        while (n > 0 && li_isspace(s[n - 1])) s[--n] = '\0';
        return s;
}

static int li_key_index(const char *key)
{
        for (int i = 0; i < s_count; i++)
                if (strcmp(s_key[i], key) == 0) return i;
        return -1;
}

static void li_put(const char *key, const char *value)
{
        int i = li_key_index(key);

        if (i >= 0) {
                li_copy(s_val[i], LI_VAL_LEN, value);
                return;
        }
        if (s_count >= LI_MAX_KEYS) return;
        li_copy(s_key[s_count], LI_KEY_LEN, key);
        li_copy(s_val[s_count], LI_VAL_LEN, value);
        s_count++;
}

/* 读一个 key=value 文件，逐行进状态表 */
static void li_load_kv(const char *path)
{
        static char text[LI_TEXT_CAP];
        char       *line;

        uint64_t got = xapi_ReadFile((WSTR)path, text, sizeof(text) - 1, 0);
        if (got == 0 || got >= sizeof(text)) return;
        text[got] = '\0';

        line = text;
        while (*line != '\0') {
                char *nl = strchr(line, '\n');

                if (nl != NULL) *nl = '\0';

                char *p = li_trim(line);
                if (*p != '\0' && *p != '#' && *p != ';') {
                        char *eq = strchr(p, '=');
                        if (eq != NULL) {
                                *eq = '\0';
                                char *key = li_trim(p);
                                if (*key != '\0') li_put(key, li_trim(eq + 1));
                        }
                }
                if (nl == NULL) break;
                line = nl + 1;
        }
}

/* ---------------- 状态存储 ---------------- */

int li_state_load(void)
{
        s_count  = 0;
        s_loaded = 1;

        xapi_Mkdir((WSTR)LI_STATE_DIR);
        li_load_kv(LI_OPTIONS_PATH); /* 用户选项作默认值 */
        li_load_kv(LI_STATE_PATH);   /* 已探测到的值覆盖它 */
        return 0;
}

int li_state_save(void)
{
        static char text[LI_TEXT_CAP];
        size_t      used;

        used = (size_t)snprintf(text, sizeof(text),
                                "# LeopardOS installer state (key=value) — written by installer step programs\n");
        for (int i = 0; i < s_count && used + 8 < sizeof(text); i++) {
                int n = snprintf(text + used, sizeof(text) - used, "%s=%s\n", s_key[i], s_val[i]);
                if (n <= 0) break;
                used += (size_t)n;
        }
        xapi_DeleteFile((WSTR)LI_STATE_PATH);
        uint64_t wrote = xapi_WriteFile((WSTR)LI_STATE_PATH, text, used, 0);
        return (wrote == 0) ? -1 : 0;
}

const char *li_state_get(const char *key)
{
        int i;

        if (!s_loaded) li_state_load();
        i = li_key_index(key);
        return (i < 0) ? NULL : s_val[i];
}

int li_state_set(const char *key, const char *value)
{
        if (!s_loaded) li_state_load();
        li_put(key, value);
        return 0;
}

int li_state_get_int(const char *key, int def)
{
        const char *v = li_state_get(key);

        if (v == NULL || *v == '\0') return def;
        return atoi(v);
}

int li_state_is(const char *key, const char *value)
{
        const char *v = li_state_get(key);

        return (v != NULL) && strcmp(v, value) == 0;
}

/* ---------------- 日志 ---------------- */

static void li_log_append(const char *line)
{
        static char text[LI_TEXT_CAP];
        uint64_t    got = xapi_ReadFile((WSTR)LI_LOG_PATH, text, sizeof(text) - 1, 0);
        size_t      used;
        size_t      n = strlen(line);

        if (got >= sizeof(text) - 2) got = 0; /* 日志过大就截断重来 */
        used = (size_t)got;

        while (n > 0 && (used + n + 2) > sizeof(text)) n--;
        memcpy(text + used, line, n);
        used += n;
        text[used++] = '\n';
        text[used]   = '\0';

        xapi_DeleteFile((WSTR)LI_LOG_PATH);
        xapi_WriteFile((WSTR)LI_LOG_PATH, text, used, 0);
}

void li_log(const char *fmt, ...)
{
        char    line[LI_LINE_LEN];
        va_list ap;

        va_start(ap, fmt);
        vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);

        xapi_Printf("%s\n", line);
        li_log_append(line);
}

void li_header(const char *text)
{
        xapi_Printf("\n== %s ==\n", text);
}

/* ---------------- 交互 ---------------- */

int li_is_auto(void)
{
        const char *v = li_state_get("auto");

        return (v != NULL) && (strcmp(v, "1") == 0 || strcmp(v, "yes") == 0 || strcmp(v, "true") == 0);
}

int li_confirm(const char *prompt, int default_yes)
{
        char line[8];

        if (li_is_auto()) return default_yes;

        xapi_Printf("%s %s: ", prompt, default_yes ? "[Y/n]" : "[y/N]");
        line[0] = '\0';
        enter_syscall(XAPI_INPUT, (uint64_t)line, sizeof(line), 0, 0, 0, 0);
        if (line[0] == '\0' || line[0] == '\r' || line[0] == '\n') return default_yes;
        return (line[0] == 'y' || line[0] == 'Y');
}

int li_ask(const char *prompt, char *out, size_t cap)
{
        size_t n;

        if (cap == 0) return -1;
        out[0] = '\0';
        if (li_is_auto()) return -1; /* 无人值守时不阻塞 */

        xapi_Printf("%s", prompt);
        enter_syscall(XAPI_INPUT, (uint64_t)out, (uint64_t)cap, 0, 0, 0, 0);
        n = strlen(out);
        while (n > 0 && (out[n - 1] == '\r' || out[n - 1] == '\n' || out[n - 1] == ' ')) out[--n] = '\0';
        return 0;
}

/* ---------------- 子进程 ---------------- */

int li_run(const char *path, char *const argv[])
{
        int64_t pid = (int64_t)xapi_RunArgs((WSTR)path, (WSTR *)argv);
        int     status = 0;

        if (pid < 0) {
                li_log("cannot start %s", path);
                return -1;
        }
        enter_syscall(SYS_WAIT4, (uint64_t)pid, (uint64_t)&status, 0, 0, 0, 0);
        return (status >> 8) & 0xFF;
}

int li_run_tool(const char *file, char *const argv[])
{
        char path[LI_PATH_LEN];

        snprintf(path, sizeof(path), "%s%s", LI_TOOL_DIR, file);
        return li_run(path, argv);
}

/* ---------------- 文件 / 块设备 ---------------- */

int li_open(const char *path, uint64_t flags)
{
        return (int)(int64_t)enter_syscall(SYS_OPEN, (uint64_t)path, flags, 0644, 0, 0, 0);
}

int64_t li_lseek(int fd, int64_t offset, uint64_t whence)
{
        return (int64_t)enter_syscall(SYS_LSEEK, (uint64_t)fd, (uint64_t)offset, whence, 0, 0, 0);
}

int li_close(int fd)
{
        return (int)enter_syscall(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0, 0);
}

uint64_t li_dev_sectors(const char *dev)
{
        char    path[64];
        int64_t size;
        int     fd;

        snprintf(path, sizeof(path), "/dev/%s", dev);
        fd = li_open(path, O_RDONLY);
        if (fd < 0) return 0;
        size = li_lseek(fd, 0, SEEK_END);
        li_close(fd);
        return (size <= 0) ? 0 : (uint64_t)size / LI_SECTOR;
}

int li_file_exists(const char *path)
{
        char byte = 0;

        return xapi_ReadFile((WSTR)path, &byte, 1, 0) > 0;
}

int li_read_text(const char *path, char *out, size_t cap)
{
        uint64_t got;

        if (cap == 0) return -1;
        got = xapi_ReadFile((WSTR)path, out, cap - 1, 0);
        if (got == 0 || got >= cap) {
                out[0] = '\0';
                return -1;
        }
        out[got] = '\0';
        return 0;
}

int li_write_text(const char *path, const char *text)
{
        uint64_t wrote;

        /* xapi_WriteFile 不截断，必须先删（重跑时避免尾部残留） */
        if (li_file_exists(path)) xapi_DeleteFile((WSTR)path);
        wrote = xapi_WriteFile((WSTR)path, (char *)text, strlen(text), 0);
        return (wrote == 0) ? -1 : 0;
}

int li_write_binary(const char *path, const void *data, size_t length)
{
        uint64_t wrote;

        if (li_file_exists(path)) xapi_DeleteFile((WSTR)path);
        wrote = xapi_WriteFile((WSTR)path, (char *)data, length, 0);
        return (wrote == length) ? 0 : -1;
}

int li_copy_file(const char *src, const char *dst)
{
        char    *buffer;
        uint64_t offset = 0;

        buffer = (char *)malloc(65536);
        if (buffer == NULL) return -1;
        if (li_file_exists(dst)) xapi_DeleteFile((WSTR)dst);

        for (;;) {
                uint64_t got = xapi_ReadFile((WSTR)src, buffer, 65536, offset);
                uint64_t wrote;

                if (got == 0) break;
                if (got == (uint64_t)-1) { free(buffer); return -1; }
                wrote = xapi_WriteFile((WSTR)dst, buffer, got, offset);
                if (wrote != got) { free(buffer); return -1; }
                offset += got;
        }
        free(buffer);
        return 0;
}

int li_make_dirs(const char *path)
{
        char   buf[LI_PATH_LEN];
        size_t len = strlen(path);

        if (len == 0 || len >= sizeof(buf)) return -1;
        memcpy(buf, path, len + 1);
        for (size_t i = 1; i <= len; i++) {
                char saved;

                if (buf[i] != '/' && buf[i] != '\0') continue;
                saved  = buf[i];
                buf[i] = '\0';
                xapi_Mkdir((WSTR)buf);
                buf[i] = saved;
                if (saved == '\0') break;
        }
        return 0;
}

/* ---------------- 步骤程序样板 ---------------- */

void li_step_begin(const char *title)
{
        li_header(title);
        li_state_load();
        li_log("[step] %s", title);
}

int li_step_end(int rc)
{
        li_state_save();
        li_log("[step] result=%s", (rc == 0) ? "ok" : "failed");
        return rc;
}

int li_fail(const char *what, int detail)
{
        if (detail != 0) li_log("FAILED: %s (code=%d)", what, detail);
        else li_log("FAILED: %s", what);
        return 1;
}
