/*
 * steps/80-verify.c — 步骤 80：整体校验（启动标记 + PID1 + initrc + 配置）
 *
 * 由 main.c（序列编排器）以全路径、无参数执行。
 * 任何一项缺失都说明系统起不来，直接失败，避免「装完重启黑屏/无限重启」。
 */
#include "li.h"

typedef struct {
        const char *rel;
        const char *why;
} check_t;

static const check_t CHECKS[] = {
        { "EFI/BOOT/BOOTX64.EFI",          "bootloader (OpenXSBoot)" },
        { "system/kernel.krl",             "kernel (OpenXSKernel)" },
        { "system/leopard/leopard-rc.elf", "PID1 (leopard-rc)" },
        { "system/config/initrc",          "service table (leopard-base)" },
        { "system/config/fstab",           "fstab (60-config)" },
        { "system/boot.conf",              "boot parameters (60-config)" },
        { NULL, NULL }
};

int main(int argc, char *argv[], char *envp[])
{
        int missing = 0;

        (void)argc;
        (void)argv;
        (void)envp;

        li_step_begin("Overall verification");

        for (int i = 0; CHECKS[i].rel != NULL; i++) {
                char path[LI_PATH_LEN];

                snprintf(path, sizeof(path), "%s/%s", LI_TARGET, CHECKS[i].rel);
                if (li_file_exists(path) != 0) {
                        xapi_Printf("  OK      %-34s %s\n", CHECKS[i].rel, CHECKS[i].why);
                } else {
                        xapi_Printf("  MISSING %-34s %s\n", CHECKS[i].rel, CHECKS[i].why);
                        li_log("missing %s (%s)", path, CHECKS[i].why);
                        missing++;
                }
        }

        if (missing != 0) return li_step_end(li_fail("required files missing", missing));

        xapi_Printf("\nAll critical target-system files present; ready to reboot.\n");
        return li_step_end(0);
}
