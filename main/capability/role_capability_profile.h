#pragma once

#include "capability/capability_registry.h"
#include <stdbool.h>

bool espagent_capability_visible_to_current_role(const espagent_capability_descriptor_t *cap);

bool espagent_capability_callable_by_current_role(const espagent_capability_descriptor_t *cap,
                                                 espagent_capability_caller_t caller);

void espagent_capability_profile_describe(char *buf, size_t size);
