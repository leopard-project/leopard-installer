/*
 * steps/30-format.c — 步骤 30：把每个分区格式化成 FAT32
 *
 * 由 main.c（序列编排器）以全路径、无参数执行。
 * 输入：part.count、part.N.node、part.N.label
 * 输出：part.N.formatted=1
 */
#include "li.h"
#include "li_format.h"

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

                char uuid_path[40];
                snprintf(uuid_path, sizeof(uuid_path), "/tmp/root.uuid");
                xapi_Printf("  format /dev/%s as %s (label %s)\n", node, fs, label);
                if (li_format_run(fs, node, label, strcmp(fs, "ext4") == 0 ? uuid_path : NULL) != 0) {
                        li_log("format failed on /dev/%s (fs=%s)", node, fs);
                        return li_step_end(1);
                }
                if (strcmp(fs, "ext4") == 0) {
                        char uuid_text[96];
                        uuid_text[0] = '\0';
                        if (li_read_text(uuid_path, uuid_text, sizeof(uuid_text)) == 0) {
                                char *eq = strchr(uuid_text, '=');
                                char *nl = strchr(uuid_text, '\n');
                                const char *mount = field(i, "mount", key, sizeof(key), NULL);
                                if (eq != NULL) {
                                        char uuid_key[40];
                                        *eq++ = '\0';
                                        if (nl != NULL) *nl = '\0';
                                        snprintf(uuid_key, sizeof(uuid_key), "part.%d.uuid", i);
                                        li_state_set(uuid_key, eq);
                                        if (mount != NULL && strcmp(mount, "/") == 0)
                                                li_state_set("part.root.uuid", eq);
                                }
                                xapi_DeleteFile((WSTR)uuid_path);
                        }
                }

                snprintf(key, sizeof(key), "part.%d.formatted", i);
                li_state_set(key, "1");
        }

        return li_step_end(0);
}
