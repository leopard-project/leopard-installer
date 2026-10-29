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

        li_step_begin("格式化 FAT32");

        count = li_state_get_int("part.count", 0);
        if (count <= 0)
                return li_step_end(li_fail("no partition in state (run 20-partition first)", 0));

        for (int i = 0; i < count; i++) {
                char        key[40];
                const char *node  = field(i, "node", key, sizeof(key), NULL);
                const char *label = field(i, "label", key, sizeof(key), "LEOPARD");
                char       *mkfs_argv[4];

                if (node == NULL) return li_step_end(li_fail("partition has no device node", i));

                xapi_Printf("  mkfs /dev/%s（卷标 %s）\n", node, label);
                mkfs_argv[0] = (char *)"mkfs-fat32";
                mkfs_argv[1] = (char *)node;
                mkfs_argv[2] = (char *)label;
                mkfs_argv[3] = NULL;
                if (li_run_tool("mkfs-fat32.elf", mkfs_argv) != 0) {
                        li_log("mkfs-fat32 failed on /dev/%s", node);
                        return li_step_end(1);
                }

                snprintf(key, sizeof(key), "part.%d.formatted", i);
                li_state_set(key, "1");
        }

        return li_step_end(0);
}
