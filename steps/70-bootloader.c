/*
 * steps/70-bootloader.c — 步骤 70：确认引导文件真的落在根分区上
 *
 * 由 main.c（序列编排器）以全路径、无参数执行。
 *
 * 内核 mount_root 只认「同一分区同时含 EFI/BOOT/BOOTX64.EFI 与 system/kernel.krl」，
 * 所以这两者缺一不可；BOOTX64.EFI 由 OpenXSBoot 包提供，kernel.krl 由 OpenXSKernel 包提供。
 * 本步骤不复制二进制（安装器只编排），只做前置硬校验。
 */
#include "li.h"

static int check_file(const char *rel, const char *who)
{
        char path[LI_PATH_LEN];

        snprintf(path, sizeof(path), "%s/%s", LI_TARGET, rel);
        if (li_file_exists(path) != 0) {
                xapi_Printf("  OK      %s\n", path);
                return 0;
        }
        xapi_Printf("  MISSING %s  (来自 %s 包)\n", path, who);
        li_log("missing %s (%s)", path, who);
        return 1;
}

int main(int argc, char *argv[], char *envp[])
{
        int missing = 0;

        (void)argc;
        (void)argv;
        (void)envp;

        li_step_begin("校验引导文件");

        missing += check_file("EFI/BOOT/BOOTX64.EFI", "OpenXSBoot");
        missing += check_file("system/kernel.krl", "OpenXSKernel");

        if (missing != 0) {
                xapi_Printf("\n引导文件不全，装出来的盘无法启动。\n"
                            "检查 live 介质的 /media 是否含 OpenXSBoot 与 OpenXSKernel 两个包。\n");
                return li_step_end(li_fail("boot markers incomplete", missing));
        }

        li_state_set("boot.marker", "1");
        return li_step_end(0);
}
