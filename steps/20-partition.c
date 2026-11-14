#include <stdlib.h>
/*
 * steps/20-partition.c — execute the partition part of the InstallPlan.
 *
 * The layout is resolved by 15-plan (li_plan_build).  This step only writes the
 * plan's partitions, rescans, discovers the global partition node indices and
 * persists part.N.node for the later format/mount/config steps.
 */
#include "li.h"
#include "li_plan.h"

#define LI_GPT_TAIL 34ULL

static uint64_t part_sectors(const LiPlanPart *p)
{
        return p->end - p->start + 1;
}

static void node_name(const LiPlan *plan, int base, int index, char *out, size_t cap)
{
        snprintf(out, cap, "%sp%d", plan->disk, base + index);
}

static int write_table(const LiPlan *plan)
{
        char *create_argv[] = { (char *)"lpdisk", (char *)"create", (char *)plan->disk, NULL };

        if (li_run_tool("lpdisk.elf", create_argv) != 0) return li_fail("lpdisk create", 0);

        for (int i = 0; i < plan->part_count; i++) {
                const LiPlanPart *p = &plan->parts[i];
                char start_s[24], end_s[24], guid_path[40], guid_text[160];
                char *add_argv[] = { (char *)"lpdisk", (char *)"add", (char *)plan->disk, start_s, end_s,
                                     (char *)"--type", (char *)p->type_code,
                                     (char *)"--name", (char *)p->name,
                                     (char *)"--guid-out", guid_path, NULL };

                snprintf(start_s, sizeof(start_s), "%llu", (unsigned long long)p->start);
                snprintf(end_s, sizeof(end_s), "%llu", (unsigned long long)p->end);
                snprintf(guid_path, sizeof(guid_path), "/tmp/part%d.guid", i);
                if (li_run_tool("lpdisk.elf", add_argv) != 0) {
                        li_log("lpdisk add failed at partition %d", i);
                        return 1;
                }
                guid_text[0] = '\0';
                if (li_read_text(guid_path, guid_text, sizeof(guid_text)) == 0) {
                        char key[40];
                        char *line = guid_text;
                        while (*line != '\0') {
                                char *eol = strchr(line, '\n');
                                char *value = strchr(line, '=');
                                if (eol != NULL) *eol = '\0';
                                if (value != NULL) {
                                        *value++ = '\0';
                                        if (strcmp(line, "disk") == 0) li_state_set("disk.guid", value);
                                        else if (strcmp(line, "part") == 0) {
                                                snprintf(key, sizeof(key), "part.%d.guid", i);
                                                li_state_set(key, value);
                                        }
                                }
                                if (eol == NULL) break;
                                line = eol + 1;
                        }
                        xapi_DeleteFile((WSTR)guid_path);
                }
        }

        char *rescan_argv[] = { (char *)"lpdisk", (char *)"rescan", (char *)plan->disk, NULL };
        if (li_run_tool("lpdisk.elf", rescan_argv) != 0) return li_fail("lpdisk rescan", 0);
        return 0;
}

/* Partition node names are global kernel indices; match the planned sizes. */
static int discover_nodes(const LiPlan *plan, int *base_out)
{
        for (int base = 0; base + plan->part_count <= 16; base++) {
                int ok = 1;
                for (int i = 0; i < plan->part_count; i++) {
                        char node[40];
                        node_name(plan, base, i, node, sizeof(node));
                        if (li_dev_sectors(node) != part_sectors(&plan->parts[i])) {
                                ok = 0;
                                break;
                        }
                }
                if (ok) {
                        *base_out = base;
                        return 0;
                }
        }
        return -1;
}

static int store_state(const LiPlan *plan, int base)
{
        if (li_plan_store(plan) != 0) return -1;
        char value[32], key[40];

        for (int i = 0; i < plan->part_count; i++) {
                char node[40];
                node_name(plan, base, i, node, sizeof(node));
                snprintf(key, sizeof(key), "part.%d.node", i);
                li_state_set(key, node);
                if (strcmp(plan->parts[i].mount, "/boot") == 0) li_state_set("esp.node", node);
                if (strcmp(plan->parts[i].mount, "/") == 0) li_state_set("root.node", node);
                if (strcmp(plan->parts[i].mount, "/users") == 0) li_state_set("users.node", node);
                li_log("partition %d: %s LBA %llu..%llu label=%s mount=%s node=/dev/%s", i,
                       plan->parts[i].name, (unsigned long long)plan->parts[i].start,
                       (unsigned long long)plan->parts[i].end, plan->parts[i].label,
                       plan->parts[i].mount, node);
        }
        snprintf(value, sizeof(value), "%d", plan->part_count);
        li_state_set("part.count", value);
        return 0;
}

int main(int argc, char *argv[], char *envp[])
{
        LiPlan plan;
        int base = 0;

        (void)argc;
        (void)argv;
        (void)envp;

        li_step_begin("Create partition table");

        if (li_plan_load(&plan) != 0) {
                const char *disk = li_state_get("disk");
                const char *scheme = li_state_get("scheme");
                uint64_t total = disk ? (uint64_t)strtoul(
                        li_state_get("disk.sectors") ? li_state_get("disk.sectors") : "0", NULL, 10) : 0;
                if (disk == NULL || total == 0 ||
                    li_plan_build(&plan, disk, total, scheme ? scheme : "modern") != 0)
                        return li_step_end(li_fail("no valid install plan (run 15-plan first)", 0));
                li_plan_store(&plan);
        }

        li_plan_print(&plan);
        if (!li_confirm("Write this partition table?", 1))
                return li_step_end(li_fail("aborted by user", 0));

        if (write_table(&plan) != 0) return li_step_end(1);

        if (discover_nodes(&plan, &base) != 0) {
                li_log("WARNING: partition nodes not found by size, assuming p0..p%d", plan.part_count - 1);
                base = 0;
        } else {
                li_log("partition nodes start at %sp%d", plan.disk, base);
        }

        if (store_state(&plan, base) != 0) return li_step_end(1);
        return li_step_end(0);
}
