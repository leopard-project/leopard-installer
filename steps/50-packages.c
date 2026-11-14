/*
 * steps/50-packages.c — 步骤 50：从 live 介质的 /media 安装软件包
 *
 * 由 main.c（序列编排器）以全路径、无参数执行。
 * 核心包缺失 = 装出来必然起不来 → 失败即中止；
 * 可选包失败只告警（live 介质的包列表可能滞后）。
 */
#include "li.h"

#define LI_MEDIA "/media"

static const char *CORE_PACKAGES[] = {
        "OpenXSBoot", "OpenXSKernel", "leopard-base", "leopard-rc",
        "leopard-getty", "leopard-oobe", "leopard-shell", "leopard-mountd",
        "xbps", NULL
};

static const char *EXTRA_PACKAGES[] = {
        "leopard-utils", "leopard-coreutils", NULL
};

static int install_one(const char *pkg)
{
        char *argv[] = { (char *)"xbps-install", (char *)"-y", (char *)"-r", (char *)LI_TARGET,
                         (char *)"-R", (char *)LI_MEDIA, (char *)pkg, NULL };
        li_log("xbps-install -r %s -R %s %s", LI_TARGET, LI_MEDIA, pkg);
        return li_run_tool("xbps-install", argv);
}

static int install_list(const char *list, const char *kind)
{
        if (list == NULL || list[0] == '\0') return 0;

        char buffer[LI_VAL_LEN];
        snprintf(buffer, sizeof(buffer), "%s", list);
        int failed = 0;
        for (char *item = strtok(buffer, ", \t"); item != NULL; item = strtok(NULL, ", \t")) {
                if (item[0] == '\0') continue;
                if (install_one(item) != 0) {
                        li_log("WARNING: optional %s package missing or failed: %s", kind, item);
                        failed = 1;
                }
        }
        return failed ? 1 : 0;
}

static int install_modules(void)
{
        const char *list = li_state_get("plan.modules");
        if (list == NULL || list[0] == '\0') list = li_state_get("modules");
        return install_list(list, "module");
}

static int install_set(const char *const *pkgs, const char *set_name)
{
        char *argv[32];
        int   argc = 0;
        int   rc;

        argv[argc++] = (char *)"xbps-install";
        argv[argc++] = (char *)"-y";
        argv[argc++] = (char *)"-r";
        argv[argc++] = (char *)LI_TARGET;
        argv[argc++] = (char *)"-R";
        argv[argc++] = (char *)LI_MEDIA;

        for (int i = 0; pkgs[i] != NULL && argc < 31; i++)
                argv[argc++] = (char *)pkgs[i];
        argv[argc] = NULL;

        li_log("xbps-install -r %s -R %s <%s set>", LI_TARGET, LI_MEDIA, set_name);
        rc = li_run_tool("xbps-install", argv);
        if (rc != 0) li_log("xbps-install %s set failed (exit=%d)", set_name, rc);
        return rc;
}

int main(int argc, char *argv[], char *envp[])
{
        (void)argc;
        (void)argv;
        (void)envp;

        li_step_begin("Install packages");

        if (install_set(CORE_PACKAGES, "core") != 0)
                return li_step_end(li_fail("core packages incomplete — check /media .xbps and x86_64-repodata", 0));

        /* 45-packages-select writes plan.packages_selected even for an empty set,
         * so minimal means minimal instead of silently installing extras. */
        const char *optional = li_state_get("plan.optional_packages");
        if (optional != NULL && optional[0] != '\0') {
                if (install_list(optional, "profile") != 0)
                        li_log("WARNING: optional profile packages incomplete; the system can still boot");
        } else if (li_state_get("plan.packages_selected") == NULL) {
                if (install_set(EXTRA_PACKAGES, "extra") != 0)
                        li_log("WARNING: optional packages incomplete; the system can still boot");
        }

        if (install_modules() != 0)
                li_log("WARNING: some optional kernel modules were not installed");

        return li_step_end(0);
}
