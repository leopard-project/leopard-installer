/*
 * steps/90-finish.c — 步骤 90：落盘、逆序卸载
 *
 * 由 main.c（序列编排器）以全路径、无参数执行；重启由编排器负责
 * （所有步骤成功后 main.c 自己 reboot），这里只把缓存刷下去、按逆序卸载。
 *
 * 输入：mounted.count、mounted.N.target
 */
#include "li.h"

int main(int argc, char *argv[], char *envp[])
{
        int count;

        (void)argc;
        (void)argv;
        (void)envp;

        li_step_begin("落盘 / 卸载");

        enter_syscall(SYS_SYNC, 0, 0, 0, 0, 0, 0);
        li_log("sync requested");

        count = li_state_get_int("mounted.count", 0);
        for (int i = count - 1; i >= 0; i--) {
                char        key[40];
                const char *target;
                int64_t     rc;

                snprintf(key, sizeof(key), "mounted.%d.target", i);
                target = li_state_get(key);
                if (target == NULL || target[0] == '\0') continue;

                rc = (int64_t)enter_syscall(SYS_UMOUNT2, (uint64_t)target, 0, 0, 0, 0, 0);
                if (rc == 0) li_log("umounted %s", target);
                else li_log("umount %s failed (rc=%d)", target, (int)rc);
        }

        return li_step_end(0);
}
