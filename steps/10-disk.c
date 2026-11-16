/*
 * steps/10-disk.c — Step 10: choose the install target disk
 *
 * Run by main.c (the sequence orchestrator) by full path, no arguments.
 * Input: disk from options/state (optional)
 * Output: disk=<name>, disk.sectors=<n>
 *
 * In TUI mode the disk list is a menu; line mode prints the same list through
 * li_ui_menu's fallback.  auto=1 keeps the scripted disk and never prompts.
 */
#include "li.h"
#include "li_ui.h"

#define LI_SECTOR 512ULL
#define MAX_DISKS 16

static const char *CANDIDATES[] = {
        "sata0", "sata1", "sata2", "sata3", "sata4", "sata5", "sata6", "sata7",
        "nvme0", "nvme1", "nvme2", "nvme3",
        "ide0", "ide1", "ide2", "ide3", NULL
};

static int choose_disk(const char *current, char *out, size_t cap)
{
        static char        titles[MAX_DISKS][64];
        static li_ui_item  items[MAX_DISKS];
        static const char *names[MAX_DISKS];
        int                count = 0;
        int                selected = 0;
        int                idx;

        for (int i = 0; CANDIDATES[i] != NULL && count < MAX_DISKS; i++) {
                uint64_t sectors = li_dev_sectors(CANDIDATES[i]);

                if (sectors == 0) continue;
                snprintf(titles[count], sizeof(titles[count]),
                         "/dev/%-8s %8llu MiB",
                         CANDIDATES[i],
                         (unsigned long long)(sectors * LI_SECTOR / 1024 / 1024));
                items[count].title = titles[count];
                items[count].desc = NULL;
                names[count] = CANDIDATES[i];
                if (current != NULL && strcmp(current, CANDIDATES[i]) == 0)
                        selected = count;
                count++;
        }

        if (count == 0) {
                char answer[32];

                if (current != NULL && current[0] != '\0')
                        snprintf(out, cap, "%s", current);
                else
                        out[0] = '\0';
                if (li_ui_input("Target disk (e.g. sata0): ", answer, sizeof(answer)) == LI_UI_OK &&
                    answer[0] != '\0')
                        snprintf(out, cap, "%s", answer);
                return 0;
        }

        idx = li_ui_menu("Select target disk", items, count, selected);
        if (idx < 0) return -1;
        snprintf(out, cap, "%s", names[idx]);
        return 0;
}

int main(int argc, char *argv[], char *envp[])
{
        char        disk[32];
        const char *prev;

        (void)argc;
        (void)argv;
        (void)envp;

        li_step_begin("Select target disk");

        prev     = li_state_get("disk");
        disk[0]  = '\0';
        if (prev != NULL && prev[0] != '\0') snprintf(disk, sizeof(disk), "%s", prev);

        if (!li_is_auto()) {
                if (choose_disk(disk, disk, sizeof(disk)) != 0)
                        return li_step_end(li_fail("aborted by user", 0));
        }

        if (disk[0] == '\0') return li_step_end(li_fail("no target disk selected", 0));

        uint64_t sectors = li_dev_sectors(disk);
        if (sectors == 0) return li_step_end(li_fail("target disk not readable", 0));

        char value[32];
        snprintf(value, sizeof(value), "%llu", (unsigned long long)sectors);

        li_state_set("disk", disk);
        li_state_set("disk.sectors", value);
        li_log("target disk: /dev/%s (%llu sectors)", disk, (unsigned long long)sectors);

        /* auto mode = explicit scripted intent: skip the destructive confirmation (otherwise the default "no" would abort itself). */
        if (!li_is_auto() && !li_ui_confirm("All data on this disk will be destroyed. Continue?", 0))
                return li_step_end(li_fail("aborted by user", 0));

        return li_step_end(0);
}
