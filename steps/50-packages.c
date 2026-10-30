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
        "leopard-utils", NULL
};

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

        if (install_set(EXTRA_PACKAGES, "extra") != 0)
                li_log("WARNING: optional packages incomplete; the system can still boot");

        return li_step_end(0);
}
