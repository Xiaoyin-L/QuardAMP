#include "shmem.h"

/*
 * Shared-memory layout definitions live in shmem.h so both the low-level ICC
 * path and future helpers use one ABI description.  Stage 4 moved active
 * doorbell handling into driver/icc.c; this file is intentionally kept as the
 * reserved home for later shared-memory helpers.
 */
