/*
 * steps/90-finish.c — Step 90: flush to disk, unmount in reverse order
 *
 * Run by main.c (the sequence orchestrator) by full path, no arguments; reboot is the orchestrator's job
 * (main.c reboots itself after all steps succeed), so here we only flush caches down and unmount in reverse order.
 *
 * Input: mounted.count, mounted.N.target
 */
#include "li.h"

int main(int argc, char *argv[], char *envp[])
{
        int count;

        (void)argc;
        (void)argv;
        (void)envp;

        li_step_begin("Flush / unmount");

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
