/*
 * steps/50-packages.c — Step 50: install software packages from the live media's /media
 *
 * Run by main.c (the sequence orchestrator) by full path, no arguments.
 *
 * Package selection is written by 45-packages-select after parsing package-profiles.conf:
 *   plan.core_packages      mandatory base environment
 *   plan.optional_packages  optional packages chosen by profile/group
 *   plan.modules            in-tree kernel modules chosen by profile/group
 *
 * Core + optional packages are installed in one xbps transaction (deduplicated); modules in one transaction.
 * If the combined transaction fails, retry "core packages only" so the installed system can still boot;
 * only a failure of the core packages themselves fails the step.
 */
#include "li.h"

#define LI_MEDIA "/media"

/* Fallback core set for old configs (without core.packages), matching package-profiles.conf. */
static const char *CORE_PACKAGES[] = {
        "leopard-boot", "leopard-xsk", "leopard-base", "leopard-rc",
        "leopard-getty", "leopard-conhost", "leopard-doas", "leopard-oobe", "leopard-shell", "leopard-mountd",
        "xbps", NULL
};

static int item_present(const char *list, const char *item, size_t item_len)
{
        const char *p = list;

        while (*p != '\0') {
                while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\r') p++;
                const char *start = p;
                while (*p != '\0' && *p != ',' && *p != ' ' && *p != '\t' && *p != '\r') p++;
                if ((size_t)(p - start) == item_len && strncmp(start, item, item_len) == 0)
                        return 1;
        }
        return 0;
}

/* Stable deduplicated union: keep first-seen order. */
static void list_add(char *dst, size_t cap, const char *src)
{
        const char *p;

        if (src == NULL || src[0] == '\0') return;
        p = src;
        while (*p != '\0') {
                while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\r') p++;
                const char *start = p;
                while (*p != '\0' && *p != ',' && *p != ' ' && *p != '\t' && *p != '\r') p++;
                size_t len = (size_t)(p - start);
                if (len == 0) continue;
                if (item_present(dst, start, len)) continue;

                char item[64];
                if (len >= sizeof(item)) len = sizeof(item) - 1;
                memcpy(item, start, len);
                item[len] = '\0';

                size_t used = strlen(dst);
                size_t need = len + (used ? 1 : 0) + 1;
                if (used + need > cap) {
                        li_log("WARNING: package/module list truncated at '%s'", item);
                        return;
                }
                if (used) dst[used++] = ',';
                memcpy(dst + used, item, len + 1);
        }
}

static void join_array(char *dst, size_t cap, const char *const *items)
{
        for (int i = 0; items[i] != NULL; i++)
                list_add(dst, cap, items[i]);
}

/*
 * Install the whole list in one xbps transaction. list is a comma/whitespace-separated list of package names.
 * Returns xbps-install's exit code (0 = success).
 */
static int install_list(const char *list, const char *what)
{
        char  buffer[LI_TEXT_CAP];
        char *argv[64];
        int   argc = 0;

        if (list == NULL || list[0] == '\0') return 0;
        snprintf(buffer, sizeof(buffer), "%s", list);

        argv[argc++] = (char *)"xbps-install";
        argv[argc++] = (char *)"-y";
        argv[argc++] = (char *)"-r";
        argv[argc++] = (char *)LI_TARGET;
        argv[argc++] = (char *)"-R";
        argv[argc++] = (char *)LI_MEDIA;

        for (char *item = strtok(buffer, ", \t\r"); item != NULL; item = strtok(NULL, ", \t\r")) {
                if (item[0] == '\0') continue;
                if (argc >= (int)(sizeof(argv) / sizeof(argv[0])) - 1) {
                        li_log("WARNING: %s list exceeds argv capacity", what);
                        break;
                }
                argv[argc++] = item;
        }
        argv[argc] = NULL;

        li_log("xbps-install -y -r %s -R %s %s", LI_TARGET, LI_MEDIA, list);
        return li_run_tool("xbps-install", argv);
}

int main(int argc, char *argv[], char *envp[])
{
        char core[LI_VAL_LEN] = "";
        char combined[LI_TEXT_CAP] = "";
        char modules[LI_VAL_LEN] = "";
        const char *value;
        int has_optional;

        (void)argc;
        (void)argv;
        (void)envp;

        li_step_begin("Install packages");

        value = li_state_get("plan.core_packages");
        if (value != NULL && value[0] != '\0') list_add(core, sizeof(core), value);
        else join_array(core, sizeof(core), CORE_PACKAGES);
        if (core[0] == '\0')
                return li_step_end(li_fail("no core package list available", 0));

        value = li_state_get("plan.optional_packages");
        has_optional = (value != NULL && value[0] != '\0');

        list_add(combined, sizeof(combined), core);
        if (has_optional) list_add(combined, sizeof(combined), value);

        /* Install core + optional packages in one transaction; on failure fall back to core packages only. */
        if (install_list(combined, "core+optional") != 0) {
                if (!has_optional)
                        return li_step_end(li_fail("core packages incomplete — check /media .xbps and x86_64-repodata", 0));

                li_log("WARNING: combined package transaction failed; retrying core packages only");
                if (install_list(core, "core") != 0)
                        return li_step_end(li_fail("core packages incomplete — check /media .xbps and x86_64-repodata", 0));
                li_log("WARNING: optional packages incomplete; the system can still boot");
        }

        value = li_state_get("plan.modules");
        if (value != NULL && value[0] != '\0') list_add(modules, sizeof(modules), value);
        value = li_state_get("modules");
        if (value != NULL && value[0] != '\0') list_add(modules, sizeof(modules), value);
        if (modules[0] != '\0' && install_list(modules, "modules") != 0)
                li_log("WARNING: some optional kernel modules were not installed");

        return li_step_end(0);
}
