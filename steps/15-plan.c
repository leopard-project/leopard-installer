#include <stdlib.h>
/*
 * steps/15-plan.c — resolve the declarative InstallPlan.
 *
 * Input: disk, disk.sectors, profile (or legacy scheme).
 * Output: profile/scheme + part.* + plan.modules/plan.optional_packages.
 * This step is non-destructive: it only resolves and prints the plan.
 */
#include "li.h"
#include "li_ui.h"
#include "li_plan.h"

static int profile_index(char ids[][24], int count, const char *current)
{
        for (int i = 0; i < count; i++)
                if (current != NULL && strcmp(ids[i], current) == 0) return i;
        return 0;
}

static const char *choose_profile(const char *current)
{
        static char       ids[8][24];
        static char       titles[8][64];
        static li_ui_item items[8];
        int count = li_plan_list_profiles(ids, titles, 8);
        int selected = profile_index(ids, count, current);
        int idx;

        if (li_is_auto() || count <= 0) return current;

        for (int i = 0; i < count; i++) {
                items[i].title = titles[i];
                items[i].desc = NULL;
        }

        idx = li_ui_menu("Installation profile", items, count, selected);
        if (idx < 0 || idx >= count) return current;
        return ids[idx];
}

static int plan_actions(const char *profile)
{
        li_ui_item items[3];

        items[0].title = "Retry";
        items[0].desc = "Try the same profile again";
        items[1].title = "Change profile";
        items[1].desc = "Pick a different layout";
        items[2].title = "Shut down";
        items[2].desc = "Stop the installer";

        return li_ui_menu("Install plan failed", items, 3, 0);
}

int main(int argc, char *argv[], char *envp[])
{
        LiPlan plan;
        const char *disk;
        const char *profile;
        uint64_t total;
        int choose = 1;

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

        profile = li_state_get("profile");
        if (profile == NULL || profile[0] == '\0') profile = li_state_get("scheme");
        if (profile == NULL || profile[0] == '\0') profile = "modern";

        for (;;) {
                if (choose) profile = choose_profile(profile);
                choose = 0;

                li_state_set("profile", profile);
                li_state_set("scheme", profile);

                memset(&plan, 0, sizeof(plan));
                snprintf(plan.disk, sizeof(plan.disk), "%s", disk);
                plan.total_sectors = total;

                int rc = li_plan_build(&plan, disk, total, profile);
                if (rc != 0 && strcmp(profile, "custom") == 0 && !li_is_auto())
                        rc = li_plan_custom_ask(&plan);
                if (rc == 0) {
                        li_plan_print(&plan);
                        if (li_plan_store(&plan) != 0)
                                return li_step_end(li_fail("cannot persist install plan", 0));
                        return li_step_end(0);
                }

                li_log("installer: plan failed for profile '%s'.", profile);
                if (li_is_auto())
                        return li_step_end(li_fail("auto mode: install plan is incomplete or disk is too small", 0));

                {
                        int action = plan_actions(profile);

                        if (action == 2 || action < 0) {
                                li_log("cancelled by user");
                                return li_step_end(li_fail("cancelled by user", 0));
                        }
                        if (action == 1) choose = 1;
                }
        }
}
