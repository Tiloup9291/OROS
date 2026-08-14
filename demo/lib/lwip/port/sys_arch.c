/*
 * sys_arch.c — Port système lwIP pour le RTOS RK3328 (NO_SYS=1). Phase 7.1.
 *
 * En mode NO_SYS=1, lwIP n'utilise NI threads, NI sémaphores, NI mailbox : la
 * seule fonction requise est sys_now() (base de temps en millisecondes) que
 * lwIP appelle pour cadencer ses timers cycliques (sys_check_timeouts).
 *
 * On la branche sur le ARM Generic Timer déjà en place (CNTPCT_EL0), converti
 * en microsecondes puis en millisecondes.
 */
#include "lwip/opt.h"
#include "lwip/sys.h"

#include "../../../arch/aarch64/timer.h"

/* Base de temps lwIP : millisecondes écoulées depuis le boot (monotone). */
u32_t sys_now(void)
{
    return (u32_t)(timer_ticks_to_us(timer_now_ticks()) / 1000ull);
}

/* Certains modules lwIP appellent sys_jiffies() (base à granularité libre). */
u32_t sys_jiffies(void)
{
    return sys_now();
}
