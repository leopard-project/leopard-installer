/*
 * steps/10-disk.c — 步骤 10：选择安装目标磁盘
 *
 * 由 main.c（序列编排器）以全路径、无参数执行。
 * 输入：选项/状态里的 disk（可选）
 * 输出：disk=<name>、disk.sectors=<n>
 */
#include "li.h"

#define LI_SECTOR 512ULL

static const char *CANDIDATES[] = {
        "sata0", "sata1", "sata2", "sata3", "sata4", "sata5", "sata6", "sata7",
        "nvme0", "nvme1", "nvme2", "nvme3",
        "ide0", "ide1", "ide2", "ide3", NULL
};

static void print_disks(void)
{
        xapi_Printf("可用磁盘：\n");
        for (int i = 0; CANDIDATES[i] != NULL; i++) {
                uint64_t sectors = li_dev_sectors(CANDIDATES[i]);

                if (sectors == 0) continue;
                xapi_Printf("  /dev/%-8s %8llu MiB  (%llu sectors)\n", CANDIDATES[i],
                            (unsigned long long)(sectors * LI_SECTOR / 1024 / 1024),
                            (unsigned long long)sectors);
        }
}

int main(int argc, char *argv[], char *envp[])
{
        char        disk[32];
        const char *prev;

        (void)argc;
        (void)argv;
        (void)envp;

        li_step_begin("选择目标磁盘");
        print_disks();

        prev     = li_state_get("disk");
        disk[0]  = '\0';
        if (prev != NULL && prev[0] != '\0') snprintf(disk, sizeof(disk), "%s", prev);

        if (!li_is_auto()) {
                char answer[32];

                if (disk[0] != '\0') xapi_Printf("当前目标：/dev/%s（回车保持不变）\n", disk);
                if (li_ask("目标磁盘（如 sata0）：", answer, sizeof(answer)) == 0 && answer[0] != '\0')
                        snprintf(disk, sizeof(disk), "%s", answer);
        }

        if (disk[0] == '\0') return li_step_end(li_fail("no target disk selected", 0));

        uint64_t sectors = li_dev_sectors(disk);
        if (sectors == 0) return li_step_end(li_fail("target disk not readable", 0));

        char value[32];
        snprintf(value, sizeof(value), "%llu", (unsigned long long)sectors);

        li_state_set("disk", disk);
        li_state_set("disk.sectors", value);
        li_log("target disk: /dev/%s (%llu sectors)", disk, (unsigned long long)sectors);

        if (!li_confirm("All data on this disk will be destroyed. Continue?", 0))
                return li_step_end(li_fail("aborted by user", 0));

        return li_step_end(0);
}
