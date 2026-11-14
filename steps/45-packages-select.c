/*
 * steps/45-packages-select.c — choose the optional software profile.
 *
 * Core packages are always installed by 50-packages.  This step selects the
 * optional package/module set from /config/package-profiles.conf and writes
 * plan.optional_packages / plan.modules for the install step.
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

static int profile_get(const char *id, const char *field, char *out, size_t cap)
{
        profiles_load();
        char text[LI_TEXT_CAP];
        snprintf(text, sizeof(text), "%s", s_text);
        char key[96];
        if (id != NULL && id[0] != '\0') snprintf(key, sizeof(key), "package.%s.%s", id, field);
        else snprintf(key, sizeof(key), "package.%s", field);

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

static int list_profiles(char ids[][24], char titles[][64], int max)
{
        char list[LI_VAL_LEN];
        if (profile_get("", "list", list, sizeof(list)) != 0) {
                snprintf(ids[0], 24, "minimal");
                snprintf(titles[0], 64, "Minimal");
                snprintf(ids[1], 24, "standard");
                snprintf(titles[1], 64, "Standard");
                return 2;
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

static const char *choose_profile(const char *current)
{
        static char ids[8][24];
        static char titles[8][64];
        int count = list_profiles(ids, titles, 8);
        int selected = 0;
        for (int i = 0; i < count; i++) if (strcmp(ids[i], current) == 0) selected = i;

        if (li_is_auto() || count <= 0) return current;

        xapi_Printf("Optional software profile:\n");
        for (int i = 0; i < count; i++) xapi_Printf("  %d) %s\n", i + 1, titles[i]);
        char answer[8];
        xapi_Printf("Choice [%d]: ", selected + 1);
        if (li_ask("", answer, sizeof(answer)) == 0 && answer[0] >= '1' && answer[0] <= '9') {
                int choice = answer[0] - '1';
                if (choice < count) selected = choice;
        }
        return ids[selected];
}

static int confirm_selection(const char *packages, const char *modules)
{
        xapi_Printf("Optional packages: %s\n", packages[0] ? packages : "(none)");
        xapi_Printf("Optional modules:  %s\n", modules[0] ? modules : "(none)");
        return li_confirm("Install this optional set?", 1);
}

int main(int argc, char *argv[], char *envp[])
{
        char packages[LI_VAL_LEN] = "";
        char modules[LI_VAL_LEN] = "";
        const char *profile;

        (void)argc;
        (void)argv;
        (void)envp;

        li_step_begin("Select optional software");

        profile = li_state_get("package_profile");
        if (profile == NULL || profile[0] == '\0') profile = "standard";

        for (;;) {
                profile = choose_profile(profile);
                li_state_set("package_profile", profile);

                packages[0] = modules[0] = '\0';
                profile_get(profile, "packages", packages, sizeof(packages));
                profile_get(profile, "modules", modules, sizeof(modules));

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

                if (li_is_auto() || confirm_selection(packages, modules)) break;
        }

        li_state_set("plan.optional_packages", packages);
        li_state_set("plan.modules", modules);
        li_state_set("plan.packages_selected", "1");
        return li_step_end(0);
}
