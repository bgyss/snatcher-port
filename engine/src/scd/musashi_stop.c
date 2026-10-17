/* Musashi's `stop` state for the translated run loops (ADR-003 M2). Interrupts are taken in host code there, so a `stop` that
 * Musashi executes (an untranslated or mismatched code range) must become the loop's stopped_ flag: left set in Musashi, it would
 * keep m68k_execute from running the handler the host delivers. Musashi has no API for it, hence this file next to its internals. */
#include "m68kcpu.h"

int scd_musashi_take_stop(void)
{
	if (!(CPU_STOPPED & STOP_LEVEL_STOP)) return 0;
	CPU_STOPPED &= ~STOP_LEVEL_STOP;
	return 1;
}
