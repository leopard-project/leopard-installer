/*
 * li_plan.h — declarative installation plan.
 *
 * The plan is resolved once (15-plan) from config + state:
 * disk, scheme, partitions, mountpoints, package/module selection and finish
 * behaviour.  Later steps execute the plan; they do not re-invent the layout.
 */
#ifndef LI_PLAN_H
#define LI_PLAN_H

#include "li.h"

#define LI_PLAN_MAX_PARTS 8

typedef struct {
        char     name[24];
        char     label[16];
        char     mount[32];
        char     type_code[8];
        char     fs[8];
        uint64_t start;
        uint64_t end;
} LiPlanPart;

typedef struct {
        char       disk[32];
        uint64_t   total_sectors;
        char       scheme[24];
        int        part_count;
        LiPlanPart parts[LI_PLAN_MAX_PARTS];
        char       modules[LI_VAL_LEN];
        char       optional_packages[LI_VAL_LEN];
        char       init[LI_PATH_LEN];
        int        finish_shutdown;
        int        auto_mode;
} LiPlan;

/* Build a plan from the current state/options.  Returns 0 on success. */
int  li_plan_build(LiPlan *plan, const char *disk, uint64_t total_sectors, const char *scheme);
int  li_plan_load(LiPlan *plan);
int  li_plan_store(const LiPlan *plan);
int  li_plan_validate(const LiPlan *plan);
void li_plan_print(const LiPlan *plan);
int  li_plan_list_profiles(char ids[][24], char titles[][64], int max);

/* Interactive custom layout fallback; used only when scheme=custom and the
   option/state already does not describe valid partitions. */
int  li_plan_custom_ask(LiPlan *plan);

#endif
