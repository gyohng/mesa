/*
 * Copyright (C) 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io>
 * © Copyright 2019 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef __AGX_BO_H
#define __AGX_BO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "util/list.h"

struct agx_device;

enum agx_alloc_type {
   AGX_ALLOC_REGULAR = 0,
   AGX_ALLOC_MEMMAP = 1,
   AGX_ALLOC_CMDBUF = 2,
   AGX_NUM_ALLOC,
};

enum agx_bo_flags {
   /* BO is shared across processes (imported or exported) and therefore cannot
    * be cached locally
    */
   AGX_BO_SHARED = 1 << 0,

   /* BO must be allocated in the low 32-bits of VA space */
   AGX_BO_LOW_VA = 1 << 1,

   /* BO is executable */
   AGX_BO_EXEC = 1 << 2,

   /* BO should be mapped write-back on the CPU (else, write combine) */
   AGX_BO_WRITEBACK = 1 << 3,
};

struct agx_ptr {
   /* If CPU mapped, CPU address. NULL if not mapped */
   void *cpu;

   /* If type REGULAR, mapped GPU address */
   uint64_t gpu;
};

struct agx_bo {
   /* Must be first for casting */
   struct list_head bucket_link;

   /* Used to link the BO to the BO cache LRU list. */
   struct list_head lru_link;

   /* The time this BO was used last, so we can evict stale BOs. */
   time_t last_used;

   enum agx_alloc_type type;

   /* Creation attributes */
   enum agx_bo_flags flags;
   size_t size;

   /* Mapping */
   struct agx_ptr ptr;

   /* Index unique only up to type, process-local */
   uint32_t handle;

   /* Globally unique value (system wide) for tracing. Exists for resources,
    * command buffers, GPU submissions, segments, segmentent lists, encoders,
    * accelerators, and channels. Corresponds to Instruments' magic table
    * metal-gpu-submission-to-command-buffer-id */
   uint64_t guid;

   /* Human-readable label, or NULL if none */
   char *name;

   /* Owner */
   struct agx_device *dev;

   /* Update atomically */
   int32_t refcnt;

   /* Used while decoding, marked read-only */
   bool ro;

   /* Used while decoding, mapped */
   bool mapped;

   /* For debugging */
   const char *label;
};

struct agx_bo *agx_bo_create(struct agx_device *dev, unsigned size,
                             enum agx_bo_flags flags, const char *label);

void agx_bo_reference(struct agx_bo *bo);
void agx_bo_unreference(struct agx_bo *bo);
struct agx_bo *agx_bo_import(struct agx_device *dev, int fd);
int agx_bo_export(struct agx_bo *bo);

void agx_bo_free(struct agx_device *dev, struct agx_bo *bo);
struct agx_bo *agx_bo_alloc(struct agx_device *dev, size_t size,
                            enum agx_bo_flags flags);
struct agx_bo *agx_bo_cache_fetch(struct agx_device *dev, size_t size,
                                  uint32_t flags, const bool dontwait);
void agx_bo_cache_evict_all(struct agx_device *dev);

#endif
