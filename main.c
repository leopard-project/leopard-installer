/*
 * leopard-installer — LeopardOS 安装器（序列化 RC 编排器）
 *
 * 读取 /config/leopard-install-sequence.conf，按顺序逐条执行步骤程序：
 * 每步等待其运行完成再执行下一步。序列文件或步骤程序缺失则关机；
 * 步骤返回非 0 时提示重试或关机；全部成功则重启。
 * 序列文件格式：每行一个步骤程序的全路径，无参数，# 开头为注释。
 */
#include "xtuiapi.h"

#define CONFIG_PATH "/config/leopard-install-sequence.conf"
#define BUF_CAP     16384
#define PATH_CAP    256

static void shutdown_now(void);
static void reboot_now(void);

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

static inline int ask_retry(void)
{
        xapi_Printf("retry or shutdown? [r/s]: ");
        char line[16];
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

        xapi_Printf("installer: all steps complete. rebooting...\n");
        reboot_now();
        return 0;
}

static void shutdown_now(void)
{
        /* 关机；电源动作失败则挂起，避免继续安装 */
        xapi_Printf("installer: shutting down...\n");
        xapi_PowerAction(XPOWER_SHUTDOWN);
        for (;;)
                xapi_Sleep(1000);
}

static void reboot_now(void)
{
        /* 重启；电源动作失败则挂起 */
        xapi_Printf("installer: rebooting...\n");
        xapi_PowerAction(XPOWER_REBOOT);
        for (;;)
                xapi_Sleep(1000);
}
