/*
 * steps/40-mount.c — Step 40: mount the root partition and the non-root partitions
 *
 * Run by main.c (the sequence orchestrator) by full path, no arguments.
 * Input: part.count, part.N.node, part.N.mount
 * Output: mounted.count, mounted.N.{target,source}
 *
 * Non-root partitions are mounted at /mnt<mount>, e.g. mount=/apps → /mnt/apps.
 * fstype must be "fatfs" (the kernel's get_filesystem does an exact strcmp; "vfat" gives -EINVAL).
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

static int mount_retry(const char *source, const char *target, const char *fstype)
{
        for (int attempt = 0; attempt < LI_MOUNT_TRIES; attempt++) {
                int64_t rc = (int64_t)enter_syscall(SYS_MOUNT, (uint64_t)source, (uint64_t)target,
                                                    (uint64_t)fstype, 0, 0, 0);
                if (rc == 0) return 0;
                xapi_Sleep(1000); /* no real flush; block device read-back may lag */
        }
        return -1;
}

static int mount_one(int i, int *mounted)
{
        char        key[40];
        const char *node   = field(i, "node", key, sizeof(key), NULL);
        const char *mount  = field(i, "mount", key, sizeof(key), NULL);
        const char *fstype = field(i, "fs", key, sizeof(key), "fatfs");
        char        source[64];
        char        target[LI_PATH_LEN];

        if (node == NULL || mount == NULL) return 0;
        if (strcmp(fstype, "ext4") != 0) fstype = LI_FSTYPE;

        snprintf(source, sizeof(source), "/dev/%s", node);
        if (strcmp(mount, "/") == 0) snprintf(target, sizeof(target), "%s", LI_TARGET);
        else snprintf(target, sizeof(target), "%s%s", LI_TARGET, mount);

        li_make_dirs(target);
        if (mount_retry(source, target, fstype) != 0) {
                li_log("mount %s -> %s failed", source, target);
                return -1;
        }
        xapi_Printf("  %s -> %s (%s)\n", source, target, fstype);

        snprintf(key, sizeof(key), "mounted.%d.target", *mounted);
        li_state_set(key, target);
        snprintf(key, sizeof(key), "mounted.%d.source", *mounted);
        li_state_set(key, source);
        (*mounted)++;
        return 0;
}

int main(int argc, char *argv[], char *envp[])
{
        int  mounted = 0;
        char value[32];

        (void)argc;
        (void)argv;
        (void)envp;

        li_step_begin("Mount target filesystems");

        int count = li_state_get_int("part.count", 0);
        if (count <= 0) return li_step_end(li_fail("no partition in state", 0));

        li_make_dirs(LI_TARGET);

        /* Mount root first, then subdirectories: an ext4 root mounted at /mnt must not already have /mnt/boot. */
        for (int pass = 0; pass < 2; pass++) {
                for (int i = 0; i < count; i++) {
                        char        key[40];
                        const char *mount = field(i, "mount", key, sizeof(key), NULL);
                        int         is_root = mount != NULL && strcmp(mount, "/") == 0;

                        if ((pass == 0) != is_root) continue;
                        if (mount_one(i, &mounted) != 0) return li_step_end(1);
                }
        }

        snprintf(value, sizeof(value), "%d", mounted);
        li_state_set("mounted.count", value);
        li_log("%d filesystem(s) mounted under %s", mounted, LI_TARGET);

        return li_step_end(0);
}
