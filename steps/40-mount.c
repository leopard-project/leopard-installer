/*
 * steps/40-mount.c — 步骤 40：挂载根分区与非根分区
 *
 * 由 main.c（序列编排器）以全路径、无参数执行。
 * 输入：part.count、part.N.node、part.N.mount
 * 输出：mounted.count、mounted.N.{target,source}
 *
 * 非根分区挂到 /mnt<mount>，例如 mount=/apps → /mnt/apps。
 * fstype 必须写 "fatfs"（内核 get_filesystem 精确 strcmp，写 vfat 会 -EINVAL）。
 */
#include "li.h"

#define LI_FSTYPE      "fatfs"
#define LI_MOUNT_TRIES 6

static const char *field(int i, const char *name, char *key, size_t cap, const char *fallback)
{
        snprintf(key, cap, "part.%d.%s", i, name);

        const char *value = li_state_get(key);
        return (value != NULL && value[0] != '\0') ? value : fallback;
}

static int mount_retry(const char *source, const char *target)
{
        for (int attempt = 0; attempt < LI_MOUNT_TRIES; attempt++) {
                int64_t rc = (int64_t)enter_syscall(SYS_MOUNT, (uint64_t)source, (uint64_t)target,
                                                    (uint64_t)LI_FSTYPE, 0, 0, 0);
                if (rc == 0) return 0;
                xapi_Sleep(1000); /* 没有真正的 flush，块设备读回可能滞后 */
        }
        return -1;
}

int main(int argc, char *argv[], char *envp[])
{
        int  count;
        int  mounted = 0;
        char value[32];

        (void)argc;
        (void)argv;
        (void)envp;

        li_step_begin("Mount target filesystems");

        count = li_state_get_int("part.count", 0);
        if (count <= 0) return li_step_end(li_fail("no partition in state", 0));

        li_make_dirs(LI_TARGET);

        for (int i = 0; i < count; i++) {
                char        key[40];
                const char *node  = field(i, "node", key, sizeof(key), NULL);
                const char *mount = field(i, "mount", key, sizeof(key), NULL);
                char        source[64];
                char        target[LI_PATH_LEN];

                if (node == NULL || mount == NULL) continue;

                snprintf(source, sizeof(source), "/dev/%s", node);
                if (strcmp(mount, "/") == 0) snprintf(target, sizeof(target), "%s", LI_TARGET);
                else snprintf(target, sizeof(target), "%s%s", LI_TARGET, mount);

                li_make_dirs(target);
                if (mount_retry(source, target) != 0) {
                        li_log("mount %s -> %s failed", source, target);
                        return li_step_end(1);
                }
                xapi_Printf("  %s -> %s\n", source, target);

                snprintf(key, sizeof(key), "mounted.%d.target", mounted);
                li_state_set(key, target);
                snprintf(key, sizeof(key), "mounted.%d.source", mounted);
                li_state_set(key, source);
                mounted++;
        }

        snprintf(value, sizeof(value), "%d", mounted);
        li_state_set("mounted.count", value);
        li_log("%d filesystem(s) mounted under %s", mounted, LI_TARGET);

        return li_step_end(0);
}
