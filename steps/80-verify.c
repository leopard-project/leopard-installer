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

static int check_at(const char *base, const char *rel, const char *why)
{
        char path[LI_PATH_LEN];

        snprintf(path, sizeof(path), "%s/%s", base, rel);
        if (li_file_exists(path) != 0) {
                xapi_Printf("  OK      %-34s %s\n", rel, why);
                return 0;
        }
        xapi_Printf("  MISSING %-34s %s\n", rel, why);
        li_log("missing %s (%s)", path, why);
        return 1;
}

int main(int argc, char *argv[], char *envp[])
{
        int missing = 0;
        const char *scheme;

        (void)argc;
        (void)argv;
        (void)envp;

        li_step_begin("Overall verification");

        scheme = li_state_get("scheme");
        if (scheme != NULL && strcmp(scheme, "ext4") == 0) {
                missing += check_at(LI_TARGET, "system/leopard/leopard-rc.elf", "PID1 (leopard-rc)");
                missing += check_at(LI_TARGET, "system/config/initrc", "service table (leopard-base)");
                missing += check_at(LI_TARGET, "system/config/fstab", "fstab (60-config)");
                missing += check_at(LI_TARGET, "system/kernel.krl", "kernel copy for binding");
                missing += check_at("/mnt/boot", "EFI/BOOT/BOOTX64.EFI", "bootloader on ESP");
                missing += check_at("/mnt/boot", "system/kernel.krl", "kernel on ESP");
                missing += check_at("/mnt/boot", "system/boot.conf", "boot parameters on ESP");
                missing += check_at("/mnt/boot", "EFI/XJ380/root.bin", "root binding (70)");
                if (missing != 0) return li_step_end(li_fail("required files missing", missing));
                xapi_Printf("\nESP + bound ext4 root verified; ready to reboot.\n");
                return li_step_end(0);
        }

        for (int i = 0; CHECKS[i].rel != NULL; i++)
                missing += check_at(LI_TARGET, CHECKS[i].rel, CHECKS[i].why);

        if (missing != 0) return li_step_end(li_fail("required files missing", missing));

        xapi_Printf("\nAll critical target-system files present; ready to reboot.\n");
        return li_step_end(0);
}
