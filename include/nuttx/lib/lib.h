/****************************************************************************
 * include/nuttx/lib/lib.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __INCLUDE_NUTTX_LIB_LIB_H
#define __INCLUDE_NUTTX_LIB_LIB_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/fs/fs.h>
#include <nuttx/kmalloc.h>

#include <limits.h>
#include <alloca.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The NuttX C library can be built in two modes: (1) as a standard,
 * C-library that can be used by normal, user-space applications, or
 * (2) as a special, kernel-mode C-library only used within the OS.
 * If NuttX is not being built as separated kernel- and user-space modules,
 * then only the first mode is supported.
 */

#if defined(__KERNEL__)

  /* Domain-specific allocations */

#  define lib_malloc(s)       kmm_malloc(s)
#  define lib_calloc(n,s)     kmm_calloc(n,s)
#  define lib_malloc_size(p)  kmm_malloc_size(p)
#  define lib_zalloc(s)       kmm_zalloc(s)
#  define lib_realloc(p,s)    kmm_realloc(p,s)
#  define lib_memalign(p,s)   kmm_memalign(p,s)
#  define lib_free(p)         kmm_free(p)

  /* User-accessible allocations */

#  define lib_umalloc(s)      kumm_malloc(s)
#  define lib_ucalloc(n,s)    kumm_calloc(n,s)
#  define lib_umalloc_size(p) kumm_malloc_size(p)
#  define lib_uzalloc(s)      kumm_zalloc(s)
#  define lib_urealloc(p,s)   kumm_realloc(p,s)
#  define lib_umemalign(p,s)  kumm_memalign(p,s)
#  define lib_ufree(p)        kumm_free(p)

#else

  /* Domain-specific allocations */

#  define lib_malloc(s)       malloc(s)
#  define lib_calloc(n,s)     calloc(n,s)
#  define lib_malloc_size(p)  malloc_size(p)
#  define lib_zalloc(s)       zalloc(s)
#  define lib_realloc(p,s)    realloc(p,s)
#  define lib_memalign(p,s)   memalign(p,s)
#  define lib_free(p)         free(p)

  /* User-accessible allocations */

#  define lib_umalloc(s)      malloc(s)
#  define lib_ucalloc(n,s)    calloc(n,s)
#  define lib_umalloc_size(p) malloc_size(p)
#  define lib_uzalloc(s)      zalloc(s)
#  define lib_urealloc(p,s)   realloc(p,s)
#  define lib_umemalign(p,s)  memalign(p,s)
#  define lib_ufree(p)        free(p)

#endif

/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifndef __ASSEMBLY__

#ifdef __cplusplus
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef CONFIG_FILE_STREAM
/* Functions contained in lib_getstreams.c **********************************/

FAR struct streamlist *lib_get_streams(void);
FAR struct file_struct *lib_get_stream(int fd);
#endif /* CONFIG_FILE_STREAM */

/* Functions defined in lib_srand.c *****************************************/

unsigned long nrand(unsigned long limit);

/* Functions defined in lib_tempbuffer.c ************************************/

#ifdef CONFIG_LIBC_TEMPBUFFER
FAR char *lib_get_tempbuffer(size_t nbytes);
void lib_put_tempbuffer(FAR char *buffer);
#else
#  define lib_get_tempbuffer(n) alloca(n)
#  define lib_put_tempbuffer(b)
#endif

#define lib_get_pathbuffer() lib_get_tempbuffer(PATH_MAX)
#define lib_put_pathbuffer(b) lib_put_tempbuffer(b)

/* Functions defined in lib_realpath.c **************************************/

FAR char *lib_realpath(FAR const char *path, FAR char *resolved,
                       bool notfollow);

#undef EXTERN
#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLY__ */
#endif /* __INCLUDE_NUTTX_LIB_LIB_H */
