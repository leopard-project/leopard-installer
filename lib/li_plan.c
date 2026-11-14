#include <stdlib.h>
/*
 * li_plan.c — build, persist and inspect a declarative InstallPlan.
 *
 * The layout math is intentionally here, not in steps/20-partition.c.  A plan
 * is resolved from config/state before any destructive action; the partition
 * step only writes the plan it was given.
 */
#include "li_plan.h"

#define LI_SECTOR    512ULL
#define LI_ALIGN     2048ULL
#define LI_GPT_TAIL  34ULL
#define LI_MIN_SECT  (16ULL * 1024 * 1024 / LI_SECTOR)

#define LI_PROFILES_PATH "/config/install-profiles.conf"

static char s_profiles[LI_TEXT_CAP];
static int  s_profiles_loaded;

static void profiles_load(void)
{
        if (s_profiles_loaded) return;
        s_profiles_loaded = 1;
        uint64_t got = xapi_ReadFile((WSTR)LI_PROFILES_PATH, s_profiles, sizeof(s_profiles) - 1, 0);
        if (got == 0 || got >= sizeof(s_profiles)) {
                s_profiles[0] = '\0';
                return;
        }
        s_profiles[got] = '\0';
}

static int profile_get(const char *id, const char *field, char *out, size_t cap)
{
        profiles_load();
        char text[LI_TEXT_CAP];
        snprintf(text, sizeof(text), "%s", s_profiles);
        char key[96];
        if (id != NULL && id[0] != '\0') snprintf(key, sizeof(key), "profile.%s.%s", id, field);
        else snprintf(key, sizeof(key), "profile.%s", field);

        char *line = text;
        while (*line != '\0') {
                char *nl = strchr(line, '\n');
                if (nl != NULL) *nl = '\0';
                char *p = line;
                while (*p == ' ' || *p == '\t' || *p == '\r') p++;
                if (*p != '\0' && *p != '#' && *p != ';') {
                        char *eq = strchr(p, '=');
                        if (eq != NULL) {
                                *eq = '\0';
                                char *k = p;
                                char *v = eq + 1;
                                size_t n = strlen(k);
                                while (n > 0 && (k[n - 1] == ' ' || k[n - 1] == '\t')) k[--n] = '\0';
                                while (*v == ' ' || *v == '\t') v++;
                                if (strcmp(k, key) == 0) {
                                        snprintf(out, cap, "%s", v);
                                        return 0;
                                }
                        }
                }
                if (nl == NULL) break;
                line = nl + 1;
        }
        return -1;
}

int li_plan_list_profiles(char ids[][24], char titles[][64], int max)
{
        char list[LI_VAL_LEN];
        if (profile_get(NULL, "list", list, sizeof(list)) != 0) {
                const char *fallback[] = { "modern", "upstream", "leopard-classic", "custom" };
                for (int i = 0; i < 4 && i < max; i++) {
                        snprintf(ids[i], 24, "%s", fallback[i]);
                        snprintf(titles[i], 64, "%s", fallback[i]);
                }
                return 4 < max ? 4 : max;
        }
        int count = 0;
        for (char *id = strtok(list, ", "); id != NULL && count < max; id = strtok(NULL, ", ")) {
                char title[64];
                if (profile_get(id, "title", title, sizeof(title)) != 0) snprintf(title, sizeof(title), "%s", id);
                snprintf(ids[count], 24, "%s", id);
                snprintf(titles[count], 64, "%s", title);
                count++;
        }
        return count;
}

static int size_from_token(const char *token, uint64_t available, uint64_t *out)
{
        uint64_t value = 0;
        char *end = NULL;
        if (strcmp(token, "third") == 0) {
                value = (available / 3) & ~(LI_ALIGN - 1);
        } else if (strcmp(token, "rest") == 0 || strcmp(token, "-") == 0) {
                value = available;
        } else {
                value = strtoul(token, &end, 10);
                if (end != NULL && *end == 'M') value = value * 1024 * 1024 / LI_SECTOR;
                else if (end != NULL && *end == '%') value = (available * value / 100) & ~(LI_ALIGN - 1);
                else return -1;
        }
        if (value < LI_MIN_SECT) return -1;
        *out = value;
        return 0;
}

static int profile_layout(LiPlan *plan, uint64_t total, const char *id)
{
        char value[LI_VAL_LEN];
        int count = 0;
        uint64_t fixed = 0;

        if (profile_get(id, "part.0", value, sizeof(value)) != 0) return -1;
        uint64_t first = LI_ALIGN;
        uint64_t last = total - LI_GPT_TAIL;
        if (last <= first) return -1;
        uint64_t usable = last - first + 1;

        for (int i = 0; i < LI_PLAN_MAX_PARTS; i++) {
                char key[32], spec[LI_VAL_LEN];
                snprintf(key, sizeof(key), "part.%d", i);
                if (profile_get(id, key, spec, sizeof(spec)) != 0) break;
                char *bar = strchr(spec, '|');
                if (bar == NULL) return -1;
                *bar++ = '\0';
                char *label = bar;
                bar = strchr(label, '|');
                if (bar == NULL) return -1;
                *bar++ = '\0';
                char *size = bar;
                bar = strchr(size, '|');
                if (bar == NULL) return -1;
                *bar++ = '\0';
                char *fs = bar;
                bar = strchr(fs, '|');
                if (bar == NULL) return -1;
                *bar++ = '\0';
                char *mount = bar;
                bar = strchr(mount, '|');
                if (bar != NULL) *bar++ = '\0';

                snprintf(plan->parts[i].name, sizeof(plan->parts[i].name), "%s", spec);
                snprintf(plan->parts[i].label, sizeof(plan->parts[i].label), "%s", label);
                snprintf(plan->parts[i].fs, sizeof(plan->parts[i].fs), "%s", fs);
                snprintf(plan->parts[i].mount, sizeof(plan->parts[i].mount), "%s", mount);
                snprintf(plan->parts[i].type_code, sizeof(plan->parts[i].type_code), "%s",
                         (bar != NULL && bar[0] != '\0') ? bar : (i == 0 ? "ef00" : "8300"));
                count++;
        }
        if (count <= 0) return -1;

        for (int i = 0; i < count; i++) {
                char key[32], spec[LI_VAL_LEN];
                snprintf(key, sizeof(key), "part.%d", i);
                profile_get(id, key, spec, sizeof(spec));
                char *p = spec;
                for (int n = 0; n < 2; n++) { p = strchr(p, '|'); if (p) p++; }
                if (p != NULL && p[0] >= '0' && p[0] <= '9' && strchr(p, 'M') != NULL) {
                        uint64_t v = strtoul(p, NULL, 10) * 1024 * 1024 / LI_SECTOR;
                        fixed += v;
                }
        }
        if (fixed > usable) return -1;
        uint64_t available = usable - fixed;
        uint64_t cursor = first;
        for (int i = 0; i < count; i++) {
                char key[32], spec[LI_VAL_LEN];
                snprintf(key, sizeof(key), "part.%d", i);
                profile_get(id, key, spec, sizeof(spec));
                char *p = spec;
                for (int n = 0; n < 2; n++) { p = strchr(p, '|'); if (p) p++; }
                char *bar = p ? strchr(p, '|') : NULL;
                if (bar != NULL) *bar = '\0';
                uint64_t size = 0;
                uint64_t remaining = last - cursor + 1;
                if (size_from_token(p ? p : "rest", available, &size) != 0) return -1;
                if (strcmp(p, "rest") == 0 || strcmp(p, "-") == 0) size = remaining;
                size &= ~7ULL;
                if (size < LI_MIN_SECT || cursor + size - 1 > last) return -1;
                plan->parts[i].start = cursor;
                plan->parts[i].end = cursor + size - 1;
                cursor = plan->parts[i].end + 1;
        }
        plan->part_count = count;
        return li_plan_validate(plan);
}

static const char *state_or(const char *key, const char *fallback)
{
        const char *value = li_state_get(key);
        return (value != NULL && value[0] != '\0') ? value : fallback;
}

static uint64_t part_sectors(const LiPlanPart *p)
{
        return p->end - p->start + 1;
}

static void default_labels(LiPlan *plan)
{
        static const char *names[]  = { "ESP", "apps", "users" };
        static const char *labels[] = { "LEOPARD", "LP-APPS", "LP-USERS" };
        static const char *mounts[] = { "/", "/apps", "/users" };

        for (int i = 0; i < 3; i++) {
                snprintf(plan->parts[i].name, sizeof(plan->parts[i].name), "%s", names[i]);
                snprintf(plan->parts[i].label, sizeof(plan->parts[i].label), "%s", labels[i]);
                snprintf(plan->parts[i].mount, sizeof(plan->parts[i].mount), "%s", mounts[i]);
                snprintf(plan->parts[i].type_code, sizeof(plan->parts[i].type_code), "%s", i == 0 ? "ef00" : "8300");
                snprintf(plan->parts[i].fs, sizeof(plan->parts[i].fs), "fatfs");
        }
}

static int classic_layout(LiPlan *plan, uint64_t total)
{
        uint64_t first, last, usable, chunk;

        if (total <= LI_ALIGN + LI_GPT_TAIL) return -1;
        first = LI_ALIGN;
        last = total - LI_GPT_TAIL;
        if (last <= first) return -1;

        usable = last - first + 1;
        chunk = (usable / 3) & ~(LI_ALIGN - 1);
        if (chunk < LI_MIN_SECT) return -1;
        if (usable - 2 * chunk < LI_MIN_SECT) return -1;

        default_labels(plan);
        plan->parts[0].start = first;
        plan->parts[0].end = first + chunk - 1;
        plan->parts[1].start = plan->parts[0].end + 1;
        plan->parts[1].end = plan->parts[1].start + chunk - 1;
        plan->parts[2].start = plan->parts[1].end + 1;
        plan->parts[2].end = last;
        plan->part_count = 3;
        return 0;
}

static int upstream_layout(LiPlan *plan, uint64_t total)
{
        uint64_t first = LI_ALIGN;
        uint64_t last = total - LI_GPT_TAIL;
        uint64_t esp = 64ULL * 1024 * 1024 / LI_SECTOR;

        if (last <= first) return -1;
        if (esp < LI_MIN_SECT) esp = LI_MIN_SECT;
        if (first + esp > last) return -1;
        if (last - (first + esp) + 1 < LI_MIN_SECT) return -1;

        snprintf(plan->parts[0].name, sizeof(plan->parts[0].name), "ESP");
        snprintf(plan->parts[0].label, sizeof(plan->parts[0].label), "LP-ESP");
        snprintf(plan->parts[0].mount, sizeof(plan->parts[0].mount), "/boot");
        snprintf(plan->parts[0].type_code, sizeof(plan->parts[0].type_code), "ef00");
        snprintf(plan->parts[0].fs, sizeof(plan->parts[0].fs), "fatfs");
        plan->parts[0].start = first;
        plan->parts[0].end = first + esp - 1;

        snprintf(plan->parts[1].name, sizeof(plan->parts[1].name), "root");
        snprintf(plan->parts[1].label, sizeof(plan->parts[1].label), "LEOPARD");
        snprintf(plan->parts[1].mount, sizeof(plan->parts[1].mount), "/");
        snprintf(plan->parts[1].type_code, sizeof(plan->parts[1].type_code), "8300");
        snprintf(plan->parts[1].fs, sizeof(plan->parts[1].fs), "ext4");
        plan->parts[1].start = first + esp;
        plan->parts[1].end = last;

        uint64_t root_sectors = plan->parts[1].end - plan->parts[1].start + 1;
        root_sectors &= ~7ULL;
        if (root_sectors == 0) return -1;
        plan->parts[1].end = plan->parts[1].start + root_sectors - 1;
        plan->part_count = 2;
        return 0;
}

static int modern_layout(LiPlan *plan, uint64_t total)
{
        uint64_t first = LI_ALIGN;
        uint64_t last = total - LI_GPT_TAIL;
        uint64_t esp = 64ULL * 1024 * 1024 / LI_SECTOR;
        uint64_t root_sectors, remaining;

        if (last <= first) return -1;
        if (esp < LI_MIN_SECT) esp = LI_MIN_SECT;
        if (first + esp > last) return -1;
        remaining = last - (first + esp) + 1;
        if (remaining < LI_MIN_SECT * 2) return -1;

        root_sectors = remaining * 30 / 100;
        if (root_sectors < 512ULL * 1024 * 1024 / LI_SECTOR)
                root_sectors = 512ULL * 1024 * 1024 / LI_SECTOR;
        root_sectors &= ~7ULL;
        if (root_sectors + LI_MIN_SECT > remaining) {
                if (remaining < LI_MIN_SECT * 2) return -1;
                root_sectors = (remaining / 2) & ~7ULL;
        }
        if (root_sectors < LI_MIN_SECT) return -1;
        if (remaining - root_sectors < LI_MIN_SECT) return -1;

        snprintf(plan->parts[0].name, sizeof(plan->parts[0].name), "ESP");
        snprintf(plan->parts[0].label, sizeof(plan->parts[0].label), "LP-ESP");
        snprintf(plan->parts[0].mount, sizeof(plan->parts[0].mount), "/boot");
        snprintf(plan->parts[0].type_code, sizeof(plan->parts[0].type_code), "ef00");
        snprintf(plan->parts[0].fs, sizeof(plan->parts[0].fs), "fatfs");
        plan->parts[0].start = first;
        plan->parts[0].end = first + esp - 1;

        snprintf(plan->parts[1].name, sizeof(plan->parts[1].name), "root");
        snprintf(plan->parts[1].label, sizeof(plan->parts[1].label), "LEOPARD");
        snprintf(plan->parts[1].mount, sizeof(plan->parts[1].mount), "/");
        snprintf(plan->parts[1].type_code, sizeof(plan->parts[1].type_code), "8300");
        snprintf(plan->parts[1].fs, sizeof(plan->parts[1].fs), "ext4");
        plan->parts[1].start = first + esp;
        plan->parts[1].end = plan->parts[1].start + root_sectors - 1;

        snprintf(plan->parts[2].name, sizeof(plan->parts[2].name), "users");
        snprintf(plan->parts[2].label, sizeof(plan->parts[2].label), "LP-USERS");
        snprintf(plan->parts[2].mount, sizeof(plan->parts[2].mount), "/users");
        snprintf(plan->parts[2].type_code, sizeof(plan->parts[2].type_code), "8300");
        snprintf(plan->parts[2].fs, sizeof(plan->parts[2].fs), "ext4");
        plan->parts[2].start = plan->parts[1].end + 1;
        plan->parts[2].end = last - ((last - plan->parts[2].start + 1) & 7);
        plan->part_count = 3;
        return 0;
}

static int custom_layout(LiPlan *plan, uint64_t total)
{
        int count = li_state_get_int("part.count", 0);
        uint64_t cursor = LI_ALIGN;
        uint64_t last = total - LI_GPT_TAIL;

        if (count <= 0 || count > LI_PLAN_MAX_PARTS) return -1;
        for (int i = 0; i < count; i++) {
                char key[40];
                const char *mb, *mount, *name, *label;
                uint64_t size_sect = 0;

                snprintf(key, sizeof(key), "part.%d.size_mb", i);
                mb = li_state_get(key);
                if (mb != NULL && mb[0] != '\0') size_sect = (uint64_t)atoi(mb) * 1024 * 1024 / LI_SECTOR;
                snprintf(key, sizeof(key), "part.%d.mount", i);
                mount = li_state_get(key);
                snprintf(key, sizeof(key), "part.%d.name", i);
                name = li_state_get(key);
                snprintf(key, sizeof(key), "part.%d.label", i);
                label = li_state_get(key);

                if (size_sect < LI_MIN_SECT || cursor + size_sect - 1 > last) return -1;
                snprintf(plan->parts[i].mount, sizeof(plan->parts[i].mount), "%s",
                         (mount != NULL && mount[0] != '\0') ? mount : "");
                snprintf(plan->parts[i].name, sizeof(plan->parts[i].name), "%s",
                         (name != NULL && name[0] != '\0') ? name : "leopard");
                snprintf(plan->parts[i].label, sizeof(plan->parts[i].label), "%s",
                         (label != NULL && label[0] != '\0') ? label : "LEOPARD");
                snprintf(plan->parts[i].type_code, sizeof(plan->parts[i].type_code), "8300");
                snprintf(plan->parts[i].fs, sizeof(plan->parts[i].fs), "ext4");
                plan->parts[i].start = cursor;
                plan->parts[i].end = cursor + size_sect - 1;
                cursor = plan->parts[i].end + 1;
        }
        plan->part_count = count;
        return 0;
}

static void plan_metadata(LiPlan *plan)
{
        char profile_modules[LI_VAL_LEN] = "";
        char profile_packages[LI_VAL_LEN] = "";
        char profile_init[LI_PATH_LEN] = "";
        profile_get(plan->scheme, "modules", profile_modules, sizeof(profile_modules));
        profile_get(plan->scheme, "packages", profile_packages, sizeof(profile_packages));
        profile_get(plan->scheme, "init", profile_init, sizeof(profile_init));

        const char *modules = li_state_get("modules");
        if (modules == NULL || modules[0] == '\0') modules = profile_modules[0] ? profile_modules : NULL;
        const char *packages = li_state_get("optional_packages");
        if (packages == NULL || packages[0] == '\0') packages = profile_packages[0] ? profile_packages : NULL;
        const char *init = li_state_get("init");
        if (init == NULL || init[0] == '\0') init = profile_init[0] ? profile_init : NULL;
        const char *finish = li_state_get("finish");

        snprintf(plan->modules, sizeof(plan->modules), "%s", modules ? modules : "");
        snprintf(plan->optional_packages, sizeof(plan->optional_packages), "%s", packages ? packages : "");
        snprintf(plan->init, sizeof(plan->init), "%s", init ? init : "/system/leopard/leopard-rc.elf");
        plan->finish_shutdown = finish != NULL &&
                                (strcmp(finish, "shutdown") == 0 || strcmp(finish, "poweroff") == 0);
        plan->auto_mode = li_is_auto();
}

int li_plan_build(LiPlan *plan, const char *disk, uint64_t total, const char *scheme)
{
        if (plan == NULL || disk == NULL || disk[0] == '\0' || total == 0) return -1;
        memset(plan, 0, sizeof(*plan));
        snprintf(plan->disk, sizeof(plan->disk), "%s", disk);
        plan->total_sectors = total;

        if (scheme == NULL || scheme[0] == '\0') scheme = "modern";
        if (strcmp(scheme, "ext4") == 0) scheme = "upstream";
        if (strcmp(scheme, "fat32") == 0 || strcmp(scheme, "auto") == 0) scheme = "leopard-classic";
        snprintf(plan->scheme, sizeof(plan->scheme), "%s", scheme);

        int rc;
        if (strcmp(scheme, "custom") == 0) rc = custom_layout(plan, total);
        else rc = profile_layout(plan, total, scheme);
        if (rc != 0) {
                if (strcmp(scheme, "leopard-classic") == 0) rc = classic_layout(plan, total);
                else if (strcmp(scheme, "upstream") == 0) rc = upstream_layout(plan, total);
                else if (strcmp(scheme, "modern") == 0) rc = modern_layout(plan, total);
                else return -1;
        }
        if (rc != 0) return rc;

        /* Classic: the root partition must be the first global index. */
        if (strcmp(scheme, "leopard-classic") == 0 && plan->parts[0].mount[0] != '/') {
                for (int i = 1; i < plan->part_count; i++) {
                        if (strcmp(plan->parts[i].mount, "/") == 0) {
                                LiPlanPart tmp = plan->parts[0];
                                plan->parts[0] = plan->parts[i];
                                plan->parts[i] = tmp;
                                break;
                        }
                }
        }
        plan_metadata(plan);
        return li_plan_validate(plan);
}

int li_plan_custom_ask(LiPlan *plan)
{
        if (plan == NULL || li_is_auto()) return -1;
        int count = li_state_get_int("part.count", 0);
        if (count <= 0) {
                char answer[16];
                if (li_ask("Number of partitions (1-8): ", answer, sizeof(answer)) != 0) return -1;
                count = atoi(answer);
                if (count <= 0 || count > LI_PLAN_MAX_PARTS) return -1;
        }

        uint64_t cursor = LI_ALIGN;
        uint64_t last = plan->total_sectors - LI_GPT_TAIL;
        plan->part_count = 0;
        for (int i = 0; i < count; i++) {
                char answer[32];
                uint64_t size_sect;

                xapi_Printf("\nPartition %d / %d (%llu MiB remaining)\n", i + 1, count,
                            (unsigned long long)((last - cursor + 1) * LI_SECTOR / 1024 / 1024));
                if (i == 0) {
                        snprintf(plan->parts[0].mount, sizeof(plan->parts[0].mount), "/");
                        xapi_Printf("  Mount point fixed to / (root partition)\n");
                } else {
                        if (li_ask("  Mount point (empty to finish): ", answer, sizeof(answer)) != 0) return -1;
                        if (answer[0] == '\0') break;
                        snprintf(plan->parts[i].mount, sizeof(plan->parts[i].mount), "%s", answer);
                }
                if (li_ask("  Size (MiB): ", answer, sizeof(answer)) != 0) return -1;
                size_sect = (uint64_t)atoi(answer) * 1024 * 1024 / LI_SECTOR;
                if (size_sect < LI_MIN_SECT || cursor + size_sect - 1 > last) return -1;

                snprintf(plan->parts[i].name, sizeof(plan->parts[i].name), "%s", i == 0 ? "ESP" : "data");
                snprintf(plan->parts[i].label, sizeof(plan->parts[i].label), "%s", i == 0 ? "LEOPARD" : "LP-DATA");
                snprintf(plan->parts[i].type_code, sizeof(plan->parts[i].type_code), "8300");
                snprintf(plan->parts[i].fs, sizeof(plan->parts[i].fs), "ext4");
                plan->parts[i].start = cursor;
                plan->parts[i].end = cursor + size_sect - 1;
                cursor = plan->parts[i].end + 1;
                plan->part_count = i + 1;

                char key[40], value[32];
                snprintf(key, sizeof(key), "part.%d.size_mb", i);
                snprintf(value, sizeof(value), "%llu",
                         (unsigned long long)(size_sect * LI_SECTOR / 1024 / 1024));
                li_state_set(key, value);
                snprintf(key, sizeof(key), "part.%d.mount", i);
                li_state_set(key, plan->parts[i].mount);
        }
        if (plan->part_count == 0) return -1;
        snprintf(plan->scheme, sizeof(plan->scheme), "custom");
        char value[16];
        snprintf(value, sizeof(value), "%d", plan->part_count);
        li_state_set("part.count", value);
        plan_metadata(plan);
        return li_plan_validate(plan);
}

int li_plan_store(const LiPlan *plan)
{
        if (plan == NULL || plan->part_count <= 0) return -1;
        char key[40], value[32];

        li_state_set("profile", plan->scheme);
        li_state_set("scheme", plan->scheme);
        snprintf(value, sizeof(value), "%d", plan->part_count);
        li_state_set("part.count", value);
        for (int i = 0; i < plan->part_count; i++) {
                snprintf(key, sizeof(key), "part.%d.start", i); snprintf(value, sizeof(value), "%llu", (unsigned long long)plan->parts[i].start); li_state_set(key, value);
                snprintf(key, sizeof(key), "part.%d.end", i); snprintf(value, sizeof(value), "%llu", (unsigned long long)plan->parts[i].end); li_state_set(key, value);
                snprintf(key, sizeof(key), "part.%d.name", i); li_state_set(key, plan->parts[i].name);
                snprintf(key, sizeof(key), "part.%d.label", i); li_state_set(key, plan->parts[i].label);
                snprintf(key, sizeof(key), "part.%d.mount", i); li_state_set(key, plan->parts[i].mount);
                snprintf(key, sizeof(key), "part.%d.type", i); li_state_set(key, plan->parts[i].type_code);
                snprintf(key, sizeof(key), "part.%d.fs", i); li_state_set(key, plan->parts[i].fs);
        }
        li_state_set("plan.modules", plan->modules);
        li_state_set("plan.optional_packages", plan->optional_packages);
        li_state_set("plan.init", plan->init);
        li_state_set("plan.finish", plan->finish_shutdown ? "shutdown" : "reboot");
        return 0;
}

int li_plan_load(LiPlan *plan)
{
        if (plan == NULL) return -1;
        memset(plan, 0, sizeof(*plan));
        const char *disk = li_state_get("disk");
        const char *scheme = li_state_get("scheme");
        int count = li_state_get_int("part.count", 0);
        if (disk == NULL || scheme == NULL || count <= 0 || count > LI_PLAN_MAX_PARTS) return -1;

        snprintf(plan->disk, sizeof(plan->disk), "%s", disk);
        snprintf(plan->scheme, sizeof(plan->scheme), "%s", scheme);
        plan->total_sectors = (uint64_t)strtoul(state_or("disk.sectors", "0"), NULL, 10);
        plan->part_count = count;
        for (int i = 0; i < count; i++) {
                char key[40];
                snprintf(key, sizeof(key), "part.%d.start", i);
                plan->parts[i].start = (uint64_t)strtoul(state_or(key, "0"), NULL, 10);
                snprintf(key, sizeof(key), "part.%d.end", i);
                plan->parts[i].end = (uint64_t)strtoul(state_or(key, "0"), NULL, 10);
                snprintf(key, sizeof(key), "part.%d.name", i);
                snprintf(plan->parts[i].name, sizeof(plan->parts[i].name), "%s", state_or(key, ""));
                snprintf(key, sizeof(key), "part.%d.label", i);
                snprintf(plan->parts[i].label, sizeof(plan->parts[i].label), "%s", state_or(key, ""));
                snprintf(key, sizeof(key), "part.%d.mount", i);
                snprintf(plan->parts[i].mount, sizeof(plan->parts[i].mount), "%s", state_or(key, ""));
                snprintf(key, sizeof(key), "part.%d.type", i);
                snprintf(plan->parts[i].type_code, sizeof(plan->parts[i].type_code), "%s", state_or(key, "8300"));
                snprintf(key, sizeof(key), "part.%d.fs", i);
                snprintf(plan->parts[i].fs, sizeof(plan->parts[i].fs), "%s", state_or(key, "ext4"));
        }
        const char *modules = li_state_get("plan.modules");
        const char *packages = li_state_get("plan.optional_packages");
        const char *init = li_state_get("plan.init");
        const char *finish = li_state_get("plan.finish");
        snprintf(plan->modules, sizeof(plan->modules), "%s", modules ? modules : "");
        snprintf(plan->optional_packages, sizeof(plan->optional_packages), "%s", packages ? packages : "");
        snprintf(plan->init, sizeof(plan->init), "%s", init ? init : "/system/leopard/leopard-rc.elf");
        plan->finish_shutdown = finish != NULL && strcmp(finish, "shutdown") == 0;
        plan->auto_mode = li_is_auto();
        return li_plan_validate(plan);
}

int li_plan_validate(const LiPlan *plan)
{
        if (plan == NULL || plan->part_count <= 0 || plan->part_count > LI_PLAN_MAX_PARTS) return -1;
        int root = 0;
        for (int i = 0; i < plan->part_count; i++) {
                const LiPlanPart *p = &plan->parts[i];
                if (p->end < p->start || p->mount[0] != '/' || p->fs[0] == '\0') return -1;
                if (strcmp(p->mount, "/") == 0) root = 1;
        }
        return root ? 0 : -1;
}

void li_plan_print(const LiPlan *plan)
{
        if (plan == NULL) return;
        xapi_Printf("Install plan: disk=/dev/%s scheme=%s parts=%d\n",
                    plan->disk, plan->scheme, plan->part_count);
        for (int i = 0; i < plan->part_count; i++) {
                const LiPlanPart *p = &plan->parts[i];
                xapi_Printf("  p%d %-8s LBA %llu..%llu  %llu MiB  %-6s -> %s\n",
                            i, p->name, (unsigned long long)p->start, (unsigned long long)p->end,
                            (unsigned long long)(part_sectors(p) * LI_SECTOR / 1024 / 1024),
                            p->fs, p->mount);
        }
        if (plan->modules[0]) xapi_Printf("  modules: %s\n", plan->modules);
}
