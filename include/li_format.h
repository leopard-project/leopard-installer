#pragma once

#include "li.h"

/* Run the configured formatter for fstype.  uuid_out may be NULL. */
int li_format_run(const char *fstype, const char *node, const char *label, const char *uuid_out);
