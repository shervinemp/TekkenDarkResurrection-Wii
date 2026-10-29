/* Small scheduling/time helpers.  The pthread subset the runtime needs lives
   in pthread_lwp.c, on top of libogc's LWP scheduler. */
#include <stdint.h>

#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>

#include "wii_platform.h"

/* ticks_to_microsecs() multiplies and divides 64-bit numbers, which this
   CPU does in long libgcc routines, and the clock is read several times per
   primitive in the profiling build.  The time base runs at 60.75 MHz, so a
   microsecond is 243/4 ticks: the quotient is built from three 32-bit
   divisions instead. */
_Static_assert(PPC_TIMER_CLOCK == 60750000u, "wii_time_us assumes a 60.75 MHz time base");
uint64_t wii_time_us(void) {
    uint64_t x = gettime() << 2;
    uint32_t hi = (uint32_t)(x >> 32), lo = (uint32_t)x;
    uint32_t qh = hi / 243u;
    uint32_t t1 = (hi % 243u) << 16 | lo >> 16, q1 = t1 / 243u;
    uint32_t t2 = (t1 % 243u) << 16 | (lo & 0xFFFFu), q2 = t2 / 243u;
    return ((uint64_t)qh << 32) + ((uint64_t)q1 << 16) + q2;
}

void wii_sleep_us(uint64_t us) {
    if (us == 0) {
        LWP_YieldThread();
        return;
    }
    u64 start = gettime();
    u64 ticks = microsecs_to_ticks(us);
    while (diff_ticks(start, gettime()) < ticks)
        LWP_YieldThread();
}

void wii_yield(void) {
    LWP_YieldThread();
}
