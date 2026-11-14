#include <stdlib.h>
/*
 * steps/15-plan.c — resolve the declarative InstallPlan.
 *
 * Input: disk, disk.sectors, scheme (config/state).
 * Output: scheme + part.* + plan.modules/plan.init/plan.finish.
 * This step is non-destructive: it only resolves and prints the plan.
 */
#include "li.h"
#include "li_plan.h"

static const char *choose_scheme(const char *current)
{
        if (li_is_auto() || li_state_is("scheme", "custom")) return current;

        char answer[8];
        xapi_Printf("Partition scheme:\n");
        xapi_Printf("  1) modern: ESP (FAT32) + ext4 root + ext4 /users [default]\n");
        xapi_Printf("  2) upstream: ESP (FAT32) + ext4 root (OpenXJ380 layout)\n");
        xapi_Printf("  3) leopard-classic: 3x FAT32 (root/apps/users)\n");
        xapi_Printf("  4) custom\n");
        if (li_ask("Choice [1]: ", answer, sizeof(answer)) != 0) return current;
        if (answer[0] == '2') return "upstream";
        if (answer[0] == '3') return "leopard-classic";
        if (answer[0] == '4') return "custom";
        return "modern";
}

int main(int argc, char *argv[], char *envp[])
{
        LiPlan plan;
        const char *disk;
        const char *scheme;
        uint64_t total;

        (void)argc;
        (void)argv;
        (void)envp;

        li_step_begin("Resolve install plan");

        disk = li_state_get("disk");
        if (disk == NULL || disk[0] == '\0')
                return li_step_end(li_fail("no disk selected (run 10-disk first)", 0));
        total = (uint64_t)strtoul(li_state_get("disk.sectors") ? li_state_get("disk.sectors") : "0", NULL, 10);
        if (total == 0) total = li_dev_sectors(disk);
        if (total == 0) return li_step_end(li_fail("cannot read disk size", 0));

        scheme = li_state_get("scheme");
        if (scheme == NULL || scheme[0] == '\0') scheme = "modern";
        scheme = choose_scheme(scheme);
        li_state_set("scheme", scheme);

        memset(&plan, 0, sizeof(plan));
        snprintf(plan.disk, sizeof(plan.disk), "%s", disk);
        plan.total_sectors = total;

        int rc = li_plan_build(&plan, disk, total, scheme);
        if (rc != 0 && strcmp(scheme, "custom") == 0 && !li_is_auto())
                rc = li_plan_custom_ask(&plan);
        if (rc != 0)
                return li_step_end(li_fail(li_is_auto()
                        ? "auto mode: install plan is incomplete or disk is too small"
                        : "cannot resolve install plan", 0));

        li_plan_print(&plan);
        if (li_plan_store(&plan) != 0)
                return li_step_end(li_fail("cannot persist install plan", 0));

        return li_step_end(0);
}
