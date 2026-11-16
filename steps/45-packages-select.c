/*
 * steps/45-packages-select.c — resolve the software selection.
 *
 * The package selection is declarative (data/package-profiles.conf):
 *
 *   core.packages   mandatory base environment (always installed)
 *   group.<id>      optional package/module collections
 *   package.<id>    user-facing profiles that aggregate groups
 *
 * This step unions the selected profile's groups + explicit packages/modules,
 * deduplicates them, and writes the result for 50-packages:
 *
 *   plan.core_packages
 *   plan.optional_packages
 *   plan.modules
 *   plan.packages_selected
 *   package_profile
 *
 * Explicit optional_packages= / modules= in install.conf override the profile.
 * Unknown group names are warned about and skipped; they never abort install.
 */
#include "li.h"

#define PKG_PROFILES_PATH "/config/package-profiles.conf"

static char s_text[LI_TEXT_CAP];
static int  s_loaded;

static void profiles_load(void)
{
        if (s_loaded) return;
        s_loaded = 1;
        uint64_t got = xapi_ReadFile((WSTR)PKG_PROFILES_PATH, s_text, sizeof(s_text) - 1, 0);
        if (got == 0 || got >= sizeof(s_text)) {
                s_text[0] = '\0';
                return;
        }
        s_text[got] = '\0';
}

/* Read an arbitrary key; the caller builds the full key string. */
static int conf_get(const char *key, char *out, size_t cap)
{
        profiles_load();
        char text[LI_TEXT_CAP];
        snprintf(text, sizeof(text), "%s", s_text);

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

static int profile_get(const char *id, const char *field, char *out, size_t cap)
{
        char key[96];

        if (id != NULL && id[0] != '\0')
                snprintf(key, sizeof(key), "package.%s.%s", id, field);
        else
                snprintf(key, sizeof(key), "package.%s", field);
        return conf_get(key, out, cap);
}

static int group_get(const char *id, const char *field, char *out, size_t cap)
{
        char key[96];
        snprintf(key, sizeof(key), "group.%s.%s", id, field);
        return conf_get(key, out, cap);
}

static int group_exists(const char *id)
{
        char value[LI_VAL_LEN];

        return group_get(id, "title", value, sizeof(value)) == 0 ||
               group_get(id, "packages", value, sizeof(value)) == 0 ||
               group_get(id, "modules", value, sizeof(value)) == 0;
}

/* Whether list already contains item, compared as a whole comma/space token. */
static int item_present(const char *list, const char *item, size_t item_len)
{
        const char *p = list;

        while (*p != '\0') {
                while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\r') p++;
                const char *start = p;
                while (*p != '\0' && *p != ',' && *p != ' ' && *p != '\t' && *p != '\r') p++;
                if ((size_t)(p - start) == item_len && strncmp(start, item, item_len) == 0)
                        return 1;
        }
        return 0;
}

/* Union src tokens into dst, deduplicate stably, keep first-seen order. */
static void list_add(char *dst, size_t cap, const char *src)
{
        const char *p;

        if (src == NULL || src[0] == '\0') return;
        p = src;
        while (*p != '\0') {
                while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\r') p++;
                const char *start = p;
                while (*p != '\0' && *p != ',' && *p != ' ' && *p != '\t' && *p != '\r') p++;
                size_t len = (size_t)(p - start);
                if (len == 0) continue;
                if (item_present(dst, start, len)) continue;

                char item[64];
                if (len >= sizeof(item)) len = sizeof(item) - 1;
                memcpy(item, start, len);
                item[len] = '\0';

                size_t used = strlen(dst);
                size_t need = len + (used ? 1 : 0) + 1;
                if (used + need > cap) {
                        li_log("WARNING: package/module list truncated at '%s'", item);
                        return;
                }
                if (used) dst[used++] = ',';
                memcpy(dst + used, item, len + 1);
        }
}

static int profile_known(const char *id)
{
        char value[LI_VAL_LEN];

        if (id == NULL || id[0] == '\0') return 0;
        return profile_get(id, "title", value, sizeof(value)) == 0 ||
               profile_get(id, "groups", value, sizeof(value)) == 0 ||
               profile_get(id, "packages", value, sizeof(value)) == 0;
}

/*
 * Resolve one package profile:
 *   core      = core.packages
 *   packages  = group packages + profile.packages
 *   modules   = group modules + profile.modules
 */
static void resolve_profile(const char *id, char *core, size_t core_cap,
                            char *packages, size_t packages_cap,
                            char *modules, size_t modules_cap)
{
        char value[LI_VAL_LEN];

        core[0] = packages[0] = modules[0] = '\0';
        if (conf_get("core.packages", value, sizeof(value)) == 0)
                list_add(core, core_cap, value);

        if (profile_get(id, "groups", value, sizeof(value)) == 0) {
                char groups[LI_VAL_LEN];
                snprintf(groups, sizeof(groups), "%s", value);
                for (char *group = strtok(groups, ", \t\r"); group != NULL;
                     group = strtok(NULL, ", \t\r")) {
                        char gv[LI_VAL_LEN];

                        if (!group_exists(group)) {
                                li_log("WARNING: profile '%s' references unknown package group '%s' (skipped)",
                                       id, group);
                                continue;
                        }
                        if (group_get(group, "packages", gv, sizeof(gv)) == 0)
                                list_add(packages, packages_cap, gv);
                        if (group_get(group, "modules", gv, sizeof(gv)) == 0)
                                list_add(modules, modules_cap, gv);
                }
        }

        if (profile_get(id, "packages", value, sizeof(value)) == 0)
                list_add(packages, packages_cap, value);
        if (profile_get(id, "modules", value, sizeof(value)) == 0)
                list_add(modules, modules_cap, value);
}

static int list_profiles(char ids[][24], char titles[][80], char descs[][160], int max)
{
        char list[LI_VAL_LEN];

        if (profile_get("", "list", list, sizeof(list)) != 0) {
                snprintf(ids[0], 24, "minimal");
                snprintf(titles[0], 80, "Minimal");
                snprintf(descs[0], 160, "Core system only");
                snprintf(ids[1], 24, "standard");
                snprintf(titles[1], 80, "Standard");
                snprintf(descs[1], 160, "Console tools and IME (recommended)");
                return 2;
        }

        int count = 0;
        for (char *id = strtok(list, ", \t\r"); id != NULL && count < max; id = strtok(NULL, ", \t\r")) {
                char title[80];
                char desc[160];

                if (profile_get(id, "title", title, sizeof(title)) != 0)
                        snprintf(title, sizeof(title), "%s", id);
                if (profile_get(id, "description", desc, sizeof(desc)) != 0)
                        desc[0] = '\0';
                snprintf(ids[count], 24, "%s", id);
                snprintf(titles[count], 80, "%s", title);
                snprintf(descs[count], 160, "%s", desc);
                count++;
        }
        return count;
}

static const char *choose_profile(const char *current)
{
        static char ids[8][24];
        static char titles[8][80];
        static char descs[8][160];
        int count = list_profiles(ids, titles, descs, 8);
        int selected = -1;

        for (int i = 0; i < count; i++)
                if (strcmp(ids[i], current) == 0) selected = i;

        /* Unknown/missing package_profile falls back to standard, else item 0. */
        if (selected < 0) {
                for (int i = 0; i < count; i++)
                        if (strcmp(ids[i], "standard") == 0) selected = i;
        }
        if (selected < 0) selected = 0;

        if (count <= 0) return (current != NULL && current[0] != '\0') ? current : "standard";
        if (li_is_auto()) return ids[selected];

        xapi_Printf("Package profile:\n");
        for (int i = 0; i < count; i++) {
                xapi_Printf("  %d) %s\n", i + 1, titles[i]);
                if (descs[i][0] != '\0')
                        xapi_Printf("     %s\n", descs[i]);
        }
        char answer[8];
        xapi_Printf("Choice [%d]: ", selected + 1);
        if (li_ask("", answer, sizeof(answer)) == 0 && answer[0] >= '1' && answer[0] <= '9') {
                int choice = answer[0] - '1';
                if (choice < count) selected = choice;
        }
        return ids[selected];
}

static int confirm_selection(const char *core, const char *packages, const char *modules)
{
        xapi_Printf("Core packages:     %s\n", core[0] ? core : "(none)");
        xapi_Printf("Optional packages: %s\n", packages[0] ? packages : "(none)");
        xapi_Printf("Kernel modules:    %s\n", modules[0] ? modules : "(none)");
        return li_confirm("Install this software selection?", 1);
}

int main(int argc, char *argv[], char *envp[])
{
        char core[LI_VAL_LEN] = "";
        char packages[LI_VAL_LEN] = "";
        char modules[LI_VAL_LEN] = "";
        const char *profile;

        (void)argc;
        (void)argv;
        (void)envp;

        li_step_begin("Select optional software");

        profile = li_state_get("package_profile");
        if (profile == NULL || profile[0] == '\0') profile = "standard";
        if (!profile_known(profile)) {
                li_log("WARNING: unknown package profile '%s', using standard", profile);
                profile = "standard";
        }

        for (;;) {
                profile = choose_profile(profile);
                li_state_set("package_profile", profile);

                resolve_profile(profile, core, sizeof(core), packages, sizeof(packages),
                                modules, sizeof(modules));

                /* Explicit install.conf keys override the profile defaults;
                 * custom inherits whatever the install plan resolved. */
                const char *user_packages = li_state_get("optional_packages");
                const char *user_modules = li_state_get("modules");
                if (strcmp(profile, "custom") == 0) {
                        if (user_packages == NULL || user_packages[0] == '\0')
                                user_packages = li_state_get("plan.optional_packages");
                        if (user_modules == NULL || user_modules[0] == '\0')
                                user_modules = li_state_get("plan.modules");
                }
                if (user_packages != NULL && user_packages[0] != '\0')
                        snprintf(packages, sizeof(packages), "%s", user_packages);
                if (user_modules != NULL && user_modules[0] != '\0')
                        snprintf(modules, sizeof(modules), "%s", user_modules);

                if (strcmp(profile, "custom") == 0 && !li_is_auto() &&
                    ((user_packages == NULL || user_packages[0] == '\0') ||
                     (user_modules == NULL || user_modules[0] == '\0'))) {
                        char answer[LI_VAL_LEN];
                        if (user_packages == NULL || user_packages[0] == '\0') {
                                if (li_ask("Packages (comma separated, empty = none): ", answer, sizeof(answer)) == 0 &&
                                    answer[0] != '\0') snprintf(packages, sizeof(packages), "%s", answer);
                        }
                        if (user_modules == NULL || user_modules[0] == '\0') {
                                if (li_ask("Kernel modules (comma separated, empty = none): ", answer, sizeof(answer)) == 0 &&
                                    answer[0] != '\0') snprintf(modules, sizeof(modules), "%s", answer);
                        }
                }

                if (li_is_auto() || confirm_selection(core, packages, modules)) break;
        }

        li_log("core packages: %s", core);
        li_log("optional packages: %s", packages[0] ? packages : "(none)");
        li_log("kernel modules: %s", modules[0] ? modules : "(none)");

        li_state_set("plan.core_packages", core);
        li_state_set("plan.optional_packages", packages);
        li_state_set("plan.modules", modules);
        li_state_set("plan.packages_selected", "1");
        return li_step_end(0);
}
