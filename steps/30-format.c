/*
 * steps/30-format.c — 步骤 30：把每个分区格式化成 FAT32
 *
 * 由 main.c（序列编排器）以全路径、无参数执行。
 * 输入：part.count、part.N.node、part.N.label
 * 输出：part.N.formatted=1
 */
#include "li.h"

static const char *field(int i, const char *name, char *key, size_t cap, const char *fallback)
{
        snprintf(key, cap, "part.%d.%s", i, name);

        const char *value = li_state_get(key);
        return (value != NULL && value[0] != '\0') ? value : fallback;
}

int main(int argc, char *argv[], char *envp[])
{
        int count;

        (void)argc;
        (void)argv;
        (void)envp;

        {
                const char *scheme = li_state_get("scheme");
                li_step_begin((scheme != NULL && strcmp(scheme, "ext4") == 0) ?
                              "Format ext4 + FAT32" : "Format FAT32");
        }

        count = li_state_get_int("part.count", 0);
        if (count <= 0)
                return li_step_end(li_fail("no partition in state (run 20-partition first)", 0));

        for (int i = 0; i < count; i++) {
                char        key[40];
                const char *node  = field(i, "node", key, sizeof(key), NULL);
                const char *label = field(i, "label", key, sizeof(key), "LEOPARD");
                const char *fs    = field(i, "fs", key, sizeof(key), "fatfs");

                if (node == NULL) return li_step_end(li_fail("partition has no device node", i));

                if (strcmp(fs, "ext4") == 0) {
                        char uuid_path[40];
                        char uuid_text[96];
                        char *mkfs_argv[] = { (char *)"mkfs-ext4", (char *)node,
                                              (char *)"--label", (char *)label,
                                              (char *)"--uuid-out", uuid_path, NULL };

                        snprintf(uuid_path, sizeof(uuid_path), "/tmp/root.uuid");
                        xapi_Printf("  mkfs.ext4 /dev/%s (label %s)\n", node, label);
                        if (li_run_tool("mkfs-ext4.elf", mkfs_argv) != 0) {
                                li_log("mkfs-ext4 failed on /dev/%s", node);
                                return li_step_end(1);
                        }
                        uuid_text[0] = '\0';
                        if (li_read_text(uuid_path, uuid_text, sizeof(uuid_text)) == 0) {
                                char *eq = strchr(uuid_text, '=');
                                char *nl = strchr(uuid_text, '\n');
                                char *mount = field(i, "mount", key, sizeof(key), NULL);

                                if (eq != NULL) {
                                        char uuid_key[40];
                                        *eq++ = '\0';
                                        if (nl != NULL) *nl = '\0';
                                        snprintf(uuid_key, sizeof(uuid_key), "part.%d.uuid", i);
                                        li_state_set(uuid_key, eq);
                                        /* 只有根分区的 UUID 用于 root.bin */
                                        if (mount != NULL && strcmp(mount, "/") == 0)
                                                li_state_set("part.root.uuid", eq);
                                }
                                xapi_DeleteFile((WSTR)uuid_path);
                        }
                } else {
                        char *mkfs_argv[] = { (char *)"mkfs-fat32", (char *)node, (char *)label, NULL };

                        xapi_Printf("  mkfs.fat32 /dev/%s (label %s)\n", node, label);
                        if (li_run_tool("mkfs-fat32.elf", mkfs_argv) != 0) {
                                li_log("mkfs-fat32 failed on /dev/%s", node);
                                return li_step_end(1);
                        }
                }

                snprintf(key, sizeof(key), "part.%d.formatted", i);
                li_state_set(key, "1");
        }

        return li_step_end(0);
}
