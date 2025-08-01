/****************************************************************************
 * include/nuttx/sched_note.h
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

#ifndef __INCLUDE_NUTTX_SCHED_NOTE_H
#define __INCLUDE_NUTTX_SCHED_NOTE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#include <nuttx/macro.h>
#include <nuttx/sched.h>
#include <nuttx/spinlock_type.h>

/* For system call numbers definition */

#ifdef CONFIG_SCHED_INSTRUMENTATION_SYSCALL
#ifdef CONFIG_LIB_SYSCALL
#include <syscall.h>
#else
#define CONFIG_LIB_SYSCALL
#include <syscall.h>
#undef CONFIG_LIB_SYSCALL
#endif
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NOTE_ALIGN(a) (((a) + sizeof(uintptr_t) - 1) & \
                       ~(sizeof(uintptr_t) - 1))

/* Provide defaults for some configuration settings (could be undefined with
 * old configuration files)
 */

#ifndef CONFIG_SCHED_INSTRUMENTATION_CPUSET
#  define CONFIG_SCHED_INSTRUMENTATION_CPUSET 0xffff
#endif

/* Note filter mode flag definitions */

#define NOTE_FILTER_MODE_FLAG_ENABLE       (1 << 0) /* Enable instrumentation */
#define NOTE_FILTER_MODE_FLAG_SWITCH       (1 << 1) /* Enable syscall instrumentation */
#define NOTE_FILTER_MODE_FLAG_SYSCALL      (1 << 2) /* Enable syscall instrumentation */
#define NOTE_FILTER_MODE_FLAG_IRQ          (1 << 3) /* Enable IRQ instrumentation */
#define NOTE_FILTER_MODE_FLAG_DUMP         (1 << 4) /* Enable dump instrumentation */
#define NOTE_FILTER_MODE_FLAG_SYSCALL_ARGS (1 << 5) /* Enable collecting syscall arguments */

/* Helper macros for syscall instrumentation filter */

#ifdef CONFIG_SCHED_INSTRUMENTATION_SYSCALL
#  define NOTE_FILTER_SYSCALLMASK_SET(nr, s) \
          ((s)->syscall_mask[(nr) / 8] |= (1 << ((nr) % 8)))
#  define NOTE_FILTER_SYSCALLMASK_CLR(nr, s) \
          ((s)->syscall_mask[(nr) / 8] &= ~(1 << ((nr) % 8)))
#  define NOTE_FILTER_SYSCALLMASK_ISSET(nr, s) \
          ((s)->syscall_mask[(nr) / 8] & (1 << ((nr) % 8)))
#  define NOTE_FILTER_SYSCALLMASK_ZERO(s) \
          memset((s), 0, sizeof(struct note_filter_syscall_s))
#else
#  define NOTE_FILTER_SYSCALLMASK_SET(nr, s)
#  define NOTE_FILTER_SYSCALLMASK_CLR(nr, s)
#  define NOTE_FILTER_SYSCALLMASK_ISSET(nr, s) (0)
#  define NOTE_FILTER_SYSCALLMASK_ZERO(s)
#endif

/* Helper macros for IRQ instrumentation filter */

#ifdef CONFIG_SCHED_INSTRUMENTATION_IRQHANDLER
#  define NOTE_FILTER_IRQMASK_SET(nr, s) \
          ((s)->irq_mask[(nr) / 8] |= (1 << ((nr) % 8)))
#  define NOTE_FILTER_IRQMASK_CLR(nr, s) \
          ((s)->irq_mask[(nr) / 8] &= ~(1 << ((nr) % 8)))
#  define NOTE_FILTER_IRQMASK_ISSET(nr, s) \
          ((s)->irq_mask[(nr) / 8] & (1 << ((nr) % 8)))
#  define NOTE_FILTER_IRQMASK_ZERO(s) \
          memset((s), 0, sizeof(struct note_filter_irq_s))
#else
#  define NOTE_FILTER_IRQMASK_SET(nr, s)
#  define NOTE_FILTER_IRQMASK_CLR(nr, s)
#  define NOTE_FILTER_IRQMASK_ISSET(nr, s) (0)
#  define NOTE_FILTER_IRQMASK_ZERO(s)
#endif

/* Helper macros for dump instrumentation filter */

#ifdef CONFIG_SCHED_INSTRUMENTATION_DUMP
#  define NOTE_FILTER_TAGMASK_SET(tag, s) \
          ((s)->tag_mask[(tag) / 8] |= (1 << ((tag) % 8)))
#  define NOTE_FILTER_TAGMASK_CLR(tag, s) \
          ((s)->tag_mask[(tag) / 8] &= ~(1 << ((tag) % 8)))
#  define NOTE_FILTER_TAGMASK_ISSET(tag, s) \
          ((s)->tag_mask[(tag) / 8] & (1 << ((tag) % 8)))
#  define NOTE_FILTER_TAGMASK_ZERO(s) \
          memset((s), 0, sizeof(struct note_filter_tag_s));
#else
#  define NOTE_FILTER_TAGMASK_SET(tag, s)
#  define NOTE_FILTER_TAGMASK_CLR(tag, s)
#  define NOTE_FILTER_TAGMASK_ISSET(tag, s) (0)
#  define NOTE_FILTER_TAGMASK_ZERO(s)
#endif

/* Printf argument type */

#define NOTE_PRINTF_UINT32 0
#define NOTE_PRINTF_UINT64 1
#define NOTE_PRINTF_DOUBLE 2
#define NOTE_PRINTF_STRING 3

/* Get/set printf tag. each parameter occupies 2 bits. The highest
 * four bits are used to represent the number of parameters, So up to
 * 14 variable arguments can be passed.
 */

#define NOTE_PRINTF_GET_TYPE(tag, index) (((tag) >> (index) * 2) & 0x03)
#define NOTE_PRINTF_GET_COUNT(tag)       (((tag) >> 28) & 0x0f)

/* Check if a variable is 32-bit or 64-bit */

#define NOTE_PRINTF_INT_TYPE(arg) (sizeof((arg) + 0) <= sizeof(uint32_t) ? \
                                   NOTE_PRINTF_UINT32 : NOTE_PRINTF_UINT64)

/* Use object_size to mark strings of known size */

#define NOTE_PRINTF_OBJECT_SIZE(arg) object_size((FAR void *)(uintptr_t)(arg), 2)

/* Use _Generic to determine the type of the parameter */

#define NOTE_PRINTF_ARG_TYPE(__arg__) \
        _Generic((__arg__) + 0, \
                 float : NOTE_PRINTF_DOUBLE, \
                 double: NOTE_PRINTF_DOUBLE, \
                 char *: ({NOTE_PRINTF_OBJECT_SIZE(__arg__) > 0 ? \
                         NOTE_PRINTF_STRING : \
                         NOTE_PRINTF_INT_TYPE(__arg__);}), \
                 const char *: ({NOTE_PRINTF_OBJECT_SIZE(__arg__) > 0 ? \
                               NOTE_PRINTF_STRING : \
                               NOTE_PRINTF_INT_TYPE(__arg__);}), \
                 default: NOTE_PRINTF_INT_TYPE(__arg__))

/* Set the type of each parameter */

#define NOTE_PRINTF_TYPE(arg, index) + ((NOTE_PRINTF_ARG_TYPE(arg) << (index) * 2))
#define NOTE_PRINTF_TYPES(...)       FOREACH_ARG(NOTE_PRINTF_TYPE, ##__VA_ARGS__)

/* Using macro expansion to calculate the expression of tag, tag will
 * be a constant at compile time, which will reduce the number of
 * size in the code.
 */

#define NOTE_PRINTF_TAG(...) \
        ((GET_ARG_COUNT(__VA_ARGS__) << 28) + NOTE_PRINTF_TYPES(__VA_ARGS__))

#define SCHED_NOTE_IP \
        ({ __label__ __here; __here: (unsigned long)&&__here; })

#define sched_note_event(tag, event, buf, len) \
        sched_note_event_ip(tag, SCHED_NOTE_IP, event, buf, len)
#define sched_note_vprintf(tag, fmt, va) \
        sched_note_vprintf_ip(tag, SCHED_NOTE_IP, fmt, 0, va)

#ifdef CONFIG_DRIVERS_NOTE_STRIP_FORMAT
#  define sched_note_printf(tag, fmt, ...) \
          do \
            { \
              static const locate_data(".printf_format") \
              char __fmt__[] = fmt; \
              uint32_t __type__ = NOTE_PRINTF_TAG(__VA_ARGS__); \
              static_assert(GET_ARG_COUNT(__VA_ARGS__) <= 14, \
                            "The number of sched_note_nprintf " \
                            "parameters needs to be less than 14"); \
              sched_note_printf_ip(tag, SCHED_NOTE_IP, __fmt__, \
                                   __type__, ##__VA_ARGS__); \
            } \
          while (0)
#else
#  define sched_note_printf(tag, fmt, ...) \
          sched_note_printf_ip(tag, SCHED_NOTE_IP, fmt, 0, ##__VA_ARGS__)
#endif

#define sched_note_begin(tag) \
        sched_note_event(tag, NOTE_DUMP_BEGIN, NULL, 0)
#define sched_note_end(tag) \
        sched_note_event(tag, NOTE_DUMP_END, NULL, 0)
#define sched_note_beginex(tag, str) \
        sched_note_event(tag, NOTE_DUMP_BEGIN, str, strlen(str))
#define sched_note_endex(tag, str) \
        sched_note_event(tag, NOTE_DUMP_END, str, strlen(str))
#define sched_note_mark(tag, str) \
        sched_note_event(tag, NOTE_DUMP_MARK, str, strlen(str))

#define sched_note_counter(tag, name_, value_) \
        do \
          { \
            struct note_counter_s counter; \
            counter.value = value_; \
            strlcpy(counter.name, name_, NAME_MAX); \
            sched_note_event(tag, NOTE_DUMP_COUNTER, \
                             &counter, sizeof(counter)); \
          } \
        while (0)

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* This type identifies a note structure */

enum note_type_e
{
  NOTE_START,
  NOTE_STOP,
  NOTE_SUSPEND,
  NOTE_RESUME,
  NOTE_CPU_START,
  NOTE_CPU_STARTED,
  NOTE_CPU_PAUSE,
  NOTE_CPU_PAUSED,
  NOTE_CPU_RESUME,
  NOTE_CPU_RESUMED,
  NOTE_PREEMPT_LOCK,
  NOTE_PREEMPT_UNLOCK,
  NOTE_CSECTION_ENTER,
  NOTE_CSECTION_LEAVE,
  NOTE_SPINLOCK_LOCK,
  NOTE_SPINLOCK_LOCKED,
  NOTE_SPINLOCK_UNLOCK,
  NOTE_SPINLOCK_ABORT,
  NOTE_SYSCALL_ENTER,
  NOTE_SYSCALL_LEAVE,
  NOTE_IRQ_ENTER,
  NOTE_IRQ_LEAVE,
  NOTE_WDOG_START,
  NOTE_WDOG_CANCEL,
  NOTE_WDOG_ENTER,
  NOTE_WDOG_LEAVE,
  NOTE_HEAP_ADD,
  NOTE_HEAP_REMOVE,
  NOTE_HEAP_ALLOC,
  NOTE_HEAP_FREE,
  NOTE_DUMP_PRINTF,

  NOTE_DUMP_BEGIN,
  NOTE_DUMP_END,
  NOTE_DUMP_MARK,
  NOTE_DUMP_COUNTER,

  /* Always last */

  NOTE_TYPE_LAST
};

enum note_tag_e
{
  NOTE_TAG_ALWAYS = 0,
  NOTE_TAG_LOG,
  NOTE_TAG_LOG_EMERG = NOTE_TAG_LOG,
  NOTE_TAG_LOG_ALERT,
  NOTE_TAG_LOG_CRIT,
  NOTE_TAG_LOG_ERR,
  NOTE_TAG_LOG_WARNING,
  NOTE_TAG_LOG_NOTICE,
  NOTE_TAG_LOG_INFO,
  NOTE_TAG_LOG_DEBUG,
  NOTE_TAG_APP,
  NOTE_TAG_ARCH,
  NOTE_TAG_AUDIO,
  NOTE_TAG_BOARDS,
  NOTE_TAG_CRYPTO,
  NOTE_TAG_DRIVERS,
  NOTE_TAG_FS,
  NOTE_TAG_GRAPHICS,
  NOTE_TAG_INPUT,
  NOTE_TAG_LIBS,
  NOTE_TAG_MM,
  NOTE_TAG_NET,
  NOTE_TAG_SCHED,
  NOTE_TAG_VIDEO,
  NOTE_TAG_WIRLESS,

  /* Always last */

  NOTE_TAG_LAST,
  NOTE_TAG_MAX = NOTE_TAG_LAST + 16
};

/* This structure provides the common header of each note */

struct note_common_s
{
  uint8_t nc_length;           /* Length of the note */
  uint8_t nc_type;             /* See enum note_type_e */
  uint8_t nc_priority;         /* Thread/task priority */
  uint8_t nc_cpu;              /* CPU thread/task running on */
  pid_t   nc_pid;              /* ID of the thread/task */
  clock_t nc_systime;          /* Time when note was buffered */
};

/* This is the specific form of the NOTE_START note */

struct note_start_s
{
  struct note_common_s nst_cmn; /* Common note parameters */
#if CONFIG_TASK_NAME_SIZE > 0
  char    nst_name[1];          /* Start of the name of the thread/task */
#endif
};

/* This is the specific form of the NOTE_STOP note */

struct note_stop_s
{
  struct note_common_s nsp_cmn; /* Common note parameters */
};

/* This is the specific form of the NOTE_SUSPEND note */

struct note_suspend_s
{
  struct note_common_s nsu_cmn; /* Common note parameters */
  uint8_t nsu_state;            /* Task state */
};

/* This is the specific form of the NOTE_RESUME note */

struct note_resume_s
{
  struct note_common_s nre_cmn; /* Common note parameters */
};

/* This is the specific form of the NOTE_CPU_START note */

struct note_cpu_start_s
{
  struct note_common_s ncs_cmn; /* Common note parameters */
  uint8_t ncs_target;           /* CPU being started */
};

/* This is the specific form of the NOTE_CPU_STARTED note */

struct note_cpu_started_s
{
  struct note_common_s ncs_cmn; /* Common note parameters */
};

/* This is the specific form of the NOTE_CPU_PAUSE note */

struct note_cpu_pause_s
{
  struct note_common_s ncp_cmn; /* Common note parameters */
  uint8_t ncp_target;           /* CPU being paused */
};

/* This is the specific form of the NOTE_CPU_PAUSED note */

struct note_cpu_paused_s
{
  struct note_common_s ncp_cmn; /* Common note parameters */
};

/* This is the specific form of the NOTE_CPU_RESUME note */

struct note_cpu_resume_s
{
  struct note_common_s ncr_cmn; /* Common note parameters */
  uint8_t ncr_target;           /* CPU being resumed */
};

/* This is the specific form of the NOTE_CPU_RESUMED note */

struct note_cpu_resumed_s
{
  struct note_common_s ncr_cmn; /* Common note parameters */
};

/* This is the specific form of the NOTE_PREEMPT_LOCK/UNLOCK note */

struct note_preempt_s
{
  struct note_common_s npr_cmn; /* Common note parameters */
  uint16_t npr_count;           /* Count of nested locks */
};

/* This is the specific form of the NOTE_CSECTION_ENTER/LEAVE note */

struct note_csection_s
{
  struct note_common_s ncs_cmn; /* Common note parameters */
#ifdef CONFIG_SMP
  uint16_t ncs_count;           /* Count of nested csections */
#endif
};

/* This is the specific form of the NOTE_SPINLOCK_LOCK/LOCKED/UNLOCK/ABORT
 * note.
 */

struct note_spinlock_s
{
  struct note_common_s nsp_cmn; /* Common note parameters */
  uintptr_t nsp_spinlock;       /* Address of spinlock */
  uint8_t nsp_value;            /* Value of spinlock */
};

/* This is the specific form of the NOTE_SYSCALL_ENTER/LEAVE notes */

#define MAX_SYSCALL_ARGS  6
#define SIZEOF_NOTE_SYSCALL_ENTER(n) (sizeof(struct note_common_s) + \
                                      sizeof(uint8_t) + sizeof(uint8_t) + \
                                      (sizeof(uintptr_t) * (n)))

struct note_syscall_enter_s
{
  struct note_common_s nsc_cmn;         /* Common note parameters */
  uint8_t nsc_nr;                       /* System call number */
  uint8_t nsc_argc;                     /* Number of system call arguments */
  uintptr_t nsc_args[MAX_SYSCALL_ARGS]; /* System call arguments */
};

struct note_syscall_leave_s
{
  struct note_common_s nsc_cmn;         /* Common note parameters */
  uint8_t nsc_nr;                       /* System call number */
  uintptr_t nsc_result;                 /* Result of the system call */
};

/* This is the specific form of the NOTE_IRQ_ENTER/LEAVE notes */

struct note_irqhandler_s
{
  struct note_common_s nih_cmn; /* Common note parameters */
  uintptr_t nih_handler;        /* IRQ handler address */
  uint8_t nih_irq;              /* IRQ number */
};

struct note_wdog_s
{
  struct note_common_s nwd_cmn;      /* Common note parameters */
  uintptr_t handler;
  uintptr_t arg;
};

struct note_heap_s
{
  struct note_common_s nhp_cmn;      /* Common note parameters */
  FAR void *heap;
  FAR void *mem;
  size_t size;
  size_t used;
};

struct note_printf_s
{
  struct note_common_s npt_cmn; /* Common note parameters */
  uintptr_t npt_ip;             /* Instruction pointer called from */
  FAR const char *npt_fmt;      /* Printf format string */
  uint32_t npt_type;            /* Printf parameter type */
  char npt_data[1];             /* Print arguments */
};

#define SIZEOF_NOTE_PRINTF(n) (sizeof(struct note_printf_s) + \
                              ((n) - 1) * sizeof(uint8_t))

struct note_event_s
{
  struct note_common_s nev_cmn;      /* Common note parameters */
  uintptr_t nev_ip;                  /* Instruction pointer called from */
  uint8_t nev_data[1];               /* Event data */
};

#define SIZEOF_NOTE_EVENT(n) (sizeof(struct note_event_s) + \
                             ((n)) * sizeof(uint8_t))

struct note_counter_s
{
  long int value;
  char name[NAME_MAX];
};

/* This is the type of the argument passed to the NOTECTL_GETMODE and
 * NOTECTL_SETMODE ioctls
 */

struct note_filter_mode_s
{
  unsigned int flag;          /* Filter mode flag */
#ifdef CONFIG_SMP
  cpu_set_t cpuset;           /* The set of monitored CPUs */
#endif
};

struct note_filter_named_mode_s
{
  char name[NAME_MAX];
  struct note_filter_mode_s mode;
};

/* This is the type of the argument passed to the NOTECTL_GETSYSCALLFILTER
 * and NOTECTL_SETSYSCALLFILTER ioctls
 */

#ifdef CONFIG_SCHED_INSTRUMENTATION_SYSCALL
struct note_filter_syscall_s
{
  uint8_t syscall_mask[(SYS_nsyscalls + 7) / 8];
};

struct note_filter_named_syscall_s
{
  char name[NAME_MAX];
  struct note_filter_syscall_s syscall_mask;
};
#endif

/* This is the type of the argument passed to the NOTECTL_GETIRQFILTER and
 * NOTECTL_SETIRQFILTER ioctls
 */

struct note_filter_irq_s
{
  uint8_t irq_mask[(NR_IRQS + 7) / 8];
};

struct note_filter_named_irq_s
{
  char name[NAME_MAX];
  struct note_filter_irq_s irq_mask;
};

struct note_filter_tag_s
{
  uint8_t tag_mask[(NOTE_TAG_MAX + 7) / 8];
};

struct note_filter_named_tag_s
{
  char name[NAME_MAX];
  struct note_filter_tag_s tag_mask;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: sched_note_*
 *
 * Description:
 *   If instrumentation of the scheduler is enabled, then some outboard
 *   logic must provide the following interfaces.  These interfaces are not
 *   available to application code.
 *
 * Input Parameters:
 *   tcb - The TCB of the thread.
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifdef CONFIG_SCHED_INSTRUMENTATION
void sched_note_add(FAR const void *note, size_t notelen);
#else
#  define sched_note_add(n,l)
#endif

#ifdef CONFIG_SCHED_INSTRUMENTATION_SWITCH
void sched_note_start(FAR struct tcb_s *tcb);
void sched_note_stop(FAR struct tcb_s *tcb);
void sched_note_suspend(FAR struct tcb_s *tcb);
void sched_note_resume(FAR struct tcb_s *tcb);
#else
#  define sched_note_stop(t)
#  define sched_note_start(t)
#  define sched_note_suspend(t)
#  define sched_note_resume(t)
#endif

#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_INSTRUMENTATION_SWITCH)
void sched_note_cpu_start(FAR struct tcb_s *tcb, int cpu);
void sched_note_cpu_started(FAR struct tcb_s *tcb);
void sched_note_cpu_pause(FAR struct tcb_s *tcb, int cpu);
void sched_note_cpu_paused(FAR struct tcb_s *tcb);
void sched_note_cpu_resume(FAR struct tcb_s *tcb, int cpu);
void sched_note_cpu_resumed(FAR struct tcb_s *tcb);
#else
#  define sched_note_cpu_start(t,c)
#  define sched_note_cpu_started(t)
#  define sched_note_cpu_pause(t,c)
#  define sched_note_cpu_paused(t)
#  define sched_note_cpu_resume(t,c)
#  define sched_note_cpu_resumed(t)
#endif

#ifdef CONFIG_SCHED_INSTRUMENTATION_PREEMPTION
void sched_note_preemption(FAR struct tcb_s *tcb, bool locked);
#else
#  define sched_note_preemption(t,l)
#endif

#ifdef CONFIG_SCHED_INSTRUMENTATION_CSECTION
void sched_note_csection(FAR struct tcb_s *tcb, bool enter);
#else
#  define sched_note_csection(t,e)
#endif

#ifdef CONFIG_SCHED_INSTRUMENTATION_SPINLOCKS
void sched_note_spinlock(FAR struct tcb_s *tcb,
                         FAR volatile spinlock_t *spinlock,
                         int type);
#else
#  define sched_note_spinlock(tcb, spinlock, type)
#endif

#ifdef CONFIG_SCHED_INSTRUMENTATION_SYSCALL
void sched_note_syscall_enter(int nr, int argc, ...);
void sched_note_syscall_leave(int nr, uintptr_t result);
#else
#  define sched_note_syscall_enter(n,a,...)
#  define sched_note_syscall_leave(n,r)
#endif

#ifdef CONFIG_SCHED_INSTRUMENTATION_IRQHANDLER
void sched_note_irqhandler(int irq, FAR void *handler, bool enter);
#else
#  define sched_note_irqhandler(i,h,e)
#endif

#ifdef CONFIG_SCHED_INSTRUMENTATION_WDOG
void sched_note_wdog(uint8_t event, FAR void *handler, FAR const void *arg);
#else
#  define sched_note_wdog(e,h,a)
#endif

#ifdef CONFIG_SCHED_INSTRUMENTATION_HEAP
void sched_note_heap(uint8_t event, FAR void *heap, FAR void *mem,
                     size_t size, size_t used);
#else
#  define sched_note_heap(e,h,m,s,c)
#endif

#ifdef CONFIG_SCHED_INSTRUMENTATION_DUMP
void sched_note_event_ip(uint32_t tag, uintptr_t ip, uint8_t event,
                         FAR const void *buf, size_t len);
void sched_note_vprintf_ip(uint32_t tag, uintptr_t ip, FAR const char *fmt,
                           uint32_t type, va_list va) printf_like(3, 0);
void sched_note_printf_ip(uint32_t tag, uintptr_t ip, FAR const char *fmt,
                          uint32_t type, ...) printf_like(3, 5);
#else
#  define sched_note_event_ip(t,ip,e,b,l)
#  define sched_note_vprintf_ip(t,ip,f,p,v)
#  define sched_note_printf_ip(t,ip,f,p,...)
#endif /* CONFIG_SCHED_INSTRUMENTATION_DUMP */

#if defined(__KERNEL__) || defined(CONFIG_BUILD_FLAT)

/****************************************************************************
 * Name: sched_note_filter_mode
 *
 * Description:
 *   Set and get note filter mode.
 *   (Same as NOTECTL_GETMODE / NOTECTL_SETMODE ioctls)
 *
 * Input Parameters:
 *   oldm - A writable pointer to struct note_filter_mode_s to get current
 *          filter mode
 *          If 0, no data is written.
 *   newm - A read-only pointer to struct note_filter_mode_s which holds the
 *          new filter mode
 *          If 0, the filter mode is not updated.
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifdef CONFIG_SCHED_INSTRUMENTATION_FILTER
void sched_note_filter_mode(FAR struct note_filter_named_mode_s *oldm,
                            FAR struct note_filter_named_mode_s *newm);
#endif

/****************************************************************************
 * Name: sched_note_filter_syscall
 *
 * Description:
 *   Set and get syscall filter setting
 *   (Same as NOTECTL_GETSYSCALLFILTER / NOTECTL_SETSYSCALLFILTER ioctls)
 *
 * Input Parameters:
 *   oldf - A writable pointer to struct note_filter_syscall_s to get
 *          current syscall filter setting
 *          If 0, no data is written.
 *   newf - A read-only pointer to struct note_filter_syscall_s of the
 *          new syscall filter setting
 *          If 0, the setting is not updated.
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#if defined(CONFIG_SCHED_INSTRUMENTATION_FILTER) && \
    defined(CONFIG_SCHED_INSTRUMENTATION_SYSCALL)
void sched_note_filter_syscall(FAR struct note_filter_named_syscall_s *oldf,
                               FAR struct note_filter_named_syscall_s *newf);
#endif

/****************************************************************************
 * Name: sched_note_filter_irq
 *
 * Description:
 *   Set and get IRQ filter setting
 *   (Same as NOTECTL_GETIRQFILTER / NOTECTL_SETIRQFILTER ioctls)
 *
 * Input Parameters:
 *   oldf - A writable pointer to struct note_filter_irq_s to get
 *          current IRQ filter setting
 *          If 0, no data is written.
 *   newf - A read-only pointer to struct note_filter_irq_s of the new
 *          IRQ filter setting
 *          If 0, the setting is not updated.
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#if defined(CONFIG_SCHED_INSTRUMENTATION_FILTER) && \
    defined(CONFIG_SCHED_INSTRUMENTATION_IRQHANDLER)
void sched_note_filter_irq(FAR struct note_filter_named_irq_s *oldf,
                           FAR struct note_filter_named_irq_s *newf);
#endif

#if defined(CONFIG_SCHED_INSTRUMENTATION_FILTER) && \
    defined(CONFIG_SCHED_INSTRUMENTATION_DUMP)
void sched_note_filter_tag(FAR struct note_filter_named_tag_s *oldf,
                           FAR struct note_filter_named_tag_s *newf);
#endif

#endif /* defined(__KERNEL__) || defined(CONFIG_BUILD_FLAT) */

#undef EXTERN
#if defined(__cplusplus)
}
#endif
#endif /* __INCLUDE_NUTTX_SCHED_NOTE_H */
