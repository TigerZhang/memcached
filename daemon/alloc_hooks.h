/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#ifndef MEM_HOOKS_H
#define MEM_HOOKS_H

#include <memcached/allocator_hooks.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef __cplusplus
#include <stdbool.h>
#endif

#include "memcached/extension_loggers.h"

typedef enum alloc_hooks_type {
    none = 0,
    tcmalloc
} alloc_hooks_type;

void init_alloc_hooks(void);

bool mc_add_new_hook(void (*)(const void* ptr, size_t size));
bool mc_remove_new_hook(void (*)(const void* ptr, size_t size));
bool mc_add_delete_hook(void (*)(const void* ptr));
bool mc_remove_delete_hook(void (*)(const void* ptr));
void mc_get_allocator_stats(allocator_stats*);
int mc_get_extra_stats_size(void);
size_t mc_get_allocation_size(void*);
void mc_get_detailed_stats(char*, int);

alloc_hooks_type get_alloc_hooks_type(void);

#endif /* MEM_HOOKS_H */
