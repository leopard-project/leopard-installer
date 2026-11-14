/*
 * li_format.c — filesystem formatter profiles for 30-format.
 *
 * Config: /config/format-profiles.conf
 *   format.<fstype>.tool=mkfs-ext4.elf
 *   format.<fstype>.args={node} --label {label} --uuid-out {uuid_out}
 *
 * Placeholders are substituted as whole arguments.  If the file or a fstype
 * entry is missing, a built-in fallback keeps installation working.
 */
#include "li_format.h"

#define LI_FORMAT_PATH "/config/format-profiles.conf"

static char s_text[LI_TEXT_CAP];
static int  s_loaded;

static void format_load(void)
{
        if (s_loaded) return;
        s_loaded = 1;
        uint64_t got = xapi_ReadFile((WSTR)LI_FORMAT_PATH, s_text, sizeof(s_text) - 1, 0);
        if (got == 0 || got >= sizeof(s_text)) {
                s_text[0] = '\0';
                return;
        }
        s_text[got] = '\0';
}

static int format_get(const char *fstype, const char *field, char *out, size_t cap)
{
        format_load();
        char text[LI_TEXT_CAP];
        snprintf(text, sizeof(text), "%s", s_text);
        char key[96];
        snprintf(key, sizeof(key), "format.%s.%s", fstype, field);

        char *line = text;
        while (*line != '\0') {
                char *nl = strchr(line, '\n');
                if (nl != NULL) *nl = '\0';
                char *p = line;
                while (*p == ' ' || *p == '\t' || *p == '\r') p++;
                if (*p != '\0' && *p != '#' && *p != ';') {
                        char *eq = strchr(p, '=');
                        if (eq != NULL) {
                                *eq = '\0';
                                char *k = p;
                                char *v = eq + 1;
                                size_t n = strlen(k);
                                while (n > 0 && (k[n - 1] == ' ' || k[n - 1] == '\t')) k[--n] = '\0';
                                while (*v == ' ' || *v == '\t') v++;
                                if (strcmp(k, key) == 0) {
                                        snprintf(out, cap, "%s", v);
                                        return 0;
                                }
                        }
                }
                if (nl == NULL) break;
                line = nl + 1;
        }
        return -1;
}

static int expand_arg(const char *input, const char *node, const char *label,
                      const char *uuid_out, char *out, size_t cap)
{
        if (strcmp(input, "{node}") == 0) snprintf(out, cap, "%s", node ? node : "");
        else if (strcmp(input, "{label}") == 0) snprintf(out, cap, "%s", label ? label : "");
        else if (strcmp(input, "{uuid_out}") == 0) snprintf(out, cap, "%s", uuid_out ? uuid_out : "");
        else snprintf(out, cap, "%s", input);
        return 0;
}

int li_format_run(const char *fstype, const char *node, const char *label, const char *uuid_out)
{
        char tool[64];
        char args[LI_VAL_LEN];

        if (fstype == NULL || node == NULL) return -1;

        if (format_get(fstype, "tool", tool, sizeof(tool)) != 0 ||
            format_get(fstype, "args", args, sizeof(args)) != 0) {
                if (strcmp(fstype, "ext4") == 0) {
                        snprintf(tool, sizeof(tool), "mkfs-ext4.elf");
                        snprintf(args, sizeof(args), "{node} --label {label} --uuid-out {uuid_out}");
                } else if (strcmp(fstype, "fatfs") == 0) {
                        snprintf(tool, sizeof(tool), "mkfs-fat32.elf");
                        snprintf(args, sizeof(args), "{node} {label}");
                } else {
                        li_log("no formatter profile for fstype=%s", fstype);
                        return -1;
                }
        }

        char buffer[LI_VAL_LEN];
        snprintf(buffer, sizeof(buffer), "%s", args);
        char *argv[16];
        int argc = 0;
        argv[argc++] = (char *)tool;
        for (char *token = strtok(buffer, " \t"); token != NULL && argc < 15; token = strtok(NULL, " \t")) {
                static char expanded[16][LI_VAL_LEN];
                expand_arg(token, node, label, uuid_out, expanded[argc - 1], sizeof(expanded[argc - 1]));
                argv[argc] = expanded[argc - 1];
                argc++;
        }
        argv[argc] = NULL;

        li_log("format %s: %s %s", fstype, tool, args);
        return li_run_tool(tool, argv);
}
