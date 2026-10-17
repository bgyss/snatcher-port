# Third-party code

| Path | Upstream | Version | License | Local changes |
|---|---|---|---|---|
| `third_party/musashi` | https://github.com/kstenerud/Musashi | 313ebf1 | MIT | `m68kconf.h`: 68000 only, `scd_illg_handler` and `scd_instruction_hook` prototype; `scd_illg_handler` and `scd_int_ack` handlers enabled; `m68kcpu.h`: bus-error trap uses `_setjmp`/`_longjmp` on macOS/Unix (no signal-mask syscalls per `m68k_execute`) |
| `third_party/nuked-opn2` | https://github.com/nukeykt/Nuked-OPN2 | 3357471 | LGPL-2.1 | none |

`src/scd/musashi_stop.c` (ours, not under `third_party/`) includes `m68kcpu.h` and reads and clears Musashi's internal stop state (`CPU_STOPPED`, i.e. `m68ki_cpu.stopped`, and `STOP_LEVEL_STOP`). These are not a public API: re-check that file whenever Musashi is updated.
