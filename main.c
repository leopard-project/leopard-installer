/*
 * leopard-installer — LeopardOS 安装器（序列化 RC 编排器）
 *
 * 读取 /config/leopard-install-sequence.conf，按顺序逐条执行步骤程序：
 * 每步等待其运行完成再执行下一步。序列文件或步骤程序缺失则关机；
 * 步骤返回非 0 时提示重试或关机；全部成功后：
 *   - 交互模式：询问「重启 / 关机」（实体机可先拔掉安装介质再重启）
 *   - auto=1：按 finish=（reboot|shutdown）自动执行，缺省重启
 * 序列文件格式：每行一个步骤程序的全路径，无参数，# 开头为注释。
 */
#include "xtuiapi.h"

#define CONFIG_PATH "/config/leopard-install-sequence.conf"
#define BUF_CAP     16384
#define PATH_CAP    256

static void shutdown_now(void);
static void reboot_now(void);
static int  install_is_auto(void);
static int  install_finish_is_shutdown(void);

static inline void read_line(char *buf, size_t cap)
{
        buf[0] = '\0';
        enter_syscall(XAPI_INPUT, (uint64_t)buf, (uint64_t)cap, 0, 0, 0, 0);
}

static inline int run_step(const char *path)
{
        char *argv[2] = { (char *)path, NULL };
        int64_t pid = (int64_t)xapi_RunArgs((WSTR)path, (WSTR *)argv);
        if (pid < 0)
                return -1;
        int status = 0;
        enter_syscall(SYS_WAIT4, (uint64_t)pid, (uint64_t)&status, 0, 0, 0, 0);
        return status;
}

/* 读一个 key=value 文本文件里的 auto 值；返回 1/0，未找到返回 -1。 */
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

/* 与 li_is_auto 语义一致：选项文件先，state 覆盖；都没有则按交互模式。 */
static int install_is_auto(void)
{
        int opt = file_auto("/config/leopard-install.conf");
        int st  = file_auto("/config/installer/state");

        if (st >= 0) return st;
        if (opt >= 0) return opt;
        return 0;
}

/* 读 finish= 值；返回 1 = shutdown，0 = reboot（缺省）。 */
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

        /* auto 模式：零输入，失败即关机，绝不阻塞等键盘 */
        if (install_is_auto()) {
                xapi_Printf("auto mode: step failed, shutting down (no retry)\n");
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
                xapi_Printf("installer: sequence file not found: %s\n", CONFIG_PATH);
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
                                xapi_Printf("installer: step not found: %s\n", path);
                                shutdown_now();
                        }
                        xapi_Printf("installer: step failed (rc=%d): %s\n", rc, path);
                        if (!ask_retry())
                                shutdown_now();
                }

next_line:
                if (!nl)
                        break;
                line = nl + 1;
        }

        xapi_Printf("installer: all steps complete.\n");
        if (install_is_auto()) {
                if (install_finish_is_shutdown()) shutdown_now();
                xapi_Printf("installer: rebooting...\n");
                reboot_now();
        }

        /* 交互模式：给用户拔掉安装介质/选择关机的机会。 */
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
        /* 关机；电源动作失败则挂起，避免继续安装 */
        xapi_Printf("installer: shutting down...\n");
        int64_t rc = (int64_t)xapi_PowerAction(XPOWER_SHUTDOWN);
        if (rc != 0) xapi_Printf("installer: power action failed rc=%lld\n", (long long)rc);
        for (;;)
                xapi_Sleep(1000);
}

static void reboot_now(void)
{
        /* 重启；电源动作失败则挂起 */
        xapi_Printf("installer: rebooting...\n");
        int64_t rc = (int64_t)xapi_PowerAction(XPOWER_REBOOT);
        if (rc != 0) xapi_Printf("installer: power action failed rc=%lld\n", (long long)rc);
        for (;;)
                xapi_Sleep(1000);
}
