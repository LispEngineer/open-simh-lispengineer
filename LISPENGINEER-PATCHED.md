# lispengineer-patched: Open SIMH plus reviewed fixes

This branch of `LispEngineer/open-simh-lispengineer` is **Open SIMH master plus a set of
bug fixes that have each passed review**, merged together so that one build carries all of
them. It moves only after a review has passed; each reviewed state is tagged
`lispengineer-patched-YYYY-MM-DD`. Every fix is also offered to Open SIMH as its own pull
request; as those are merged upstream, this branch converges on Open SIMH master.

Build it exactly as Open SIMH, for example:

    git clone -b lispengineer-patched https://github.com/LispEngineer/open-simh-lispengineer.git
    make -C open-simh-lispengineer vaxstation4000m60 microvax3900

## Fixes carried (reviewed 2026-10-13)

| Fix | Files | Open SIMH PR |
| :-- | :-- | :-- |
| KA46: memory controller registers listed before the option ROM window (MEMERR read as all ones; a memory parity error logged every minute) | `VAX/vax440_sysdev.c` | #586 |
| LANCE (XS): raise a pending interrupt when the driver sets CSR0<INEA> again (lost interrupts, stalled transfers) | `VAX/vax_xs.c` | #587 |
| LANCE (XS): report a failed transmit in the descriptor, not as CSR0<BABL> (an unattached LANCE made OpenVMS bugcheck) | `VAX/vax_xs.c` | #588 |
| SCSI: report disk I/O errors instead of transferring unset data | `sim_scsi.c` | #589 |
| SCSI: bound data transfers by the buffer size | `sim_scsi.c` | #590 |
| SCSI: report write protection (MODE SENSE) and enforce it, including `SET <unit> LOCKED` on an attached unit | `sim_scsi.c` | #591 |
| SCSI CD-ROM: implement READ SUB-CHANNEL (0x42), all four formats | `sim_scsi.c` | #592 |
| DISK: `sim_disk_rdsect()` sets the returned sector count on every path (with the two SCSI fixes above, `SET <unit> FORMAT=AUTO` on an attached unit no longer crashes the simulator) | `sim_disk.c` | #593 |
| TIMER: bound what `sim_idle()` credits for a long or negative sleep (with idling enabled, a host pause of 10 s or more, or a step of the host clock, left the simulator sleeping seconds to minutes per clock tick) | `sim_timer.c` | #596 |
| TIMER: at the console, start a re-initialized clock from the lower of its initial value and the last calibration (`BOOT CPU` after a stop made the MicroVAX 3900 ROM's power-on tests take minutes, with `?53` errors) | `sim_timer.c` | #598 |
| TIMER: measure the calibrated rate over the time the simulator ran (after a short host stall the guest clock ran up to 2.6 s ahead; needs #596) | `sim_timer.c` | #599 |
| TIMER: keep catch-up ticks on after a stop at the `sim>` prompt (the first stop switched them off for the rest of the process, so later stalls were repaid at only 1 s per second, and that stop cost a MicroVAX 3900 guest 1.0-1.6 s) | `sim_timer.c` | #602 |
| TIMER: the guest clock follows elapsed time (`SET CLOCK ELAPSED`, default here; `NOELAPSED` restores Open SIMH) | `sim_timer.c`, `scp.c`, `sim_timer.h`, seven VAX files | #603 (**behaviour change**, see "Behaviour that differs from Open SIMH") |
| SLIRP: never loop for ever choosing the NAT doorbell port (`ATTACH ... NAT:` looped printing `bind error 13` when the clock drew a privileged port, and on a port already in use) | `slirp_glue/sim_slirp.c` | #604 |
| SCP: end the simulator cleanly on SIGTERM or SIGHUP at the command level (at `sim>` the signal killed SIMH without writing NVRAM or flushing logs) | `scp.c` | #605 |
| SCP: flush the console log before waiting for a command (the log lacked the stop message and later command output while at `sim>`) | `scp.c` | #606 |
| VAX440: map the whole network address ROM (the KA46 power-on NI test failed subtest B) | `VAX/vax440_sysdev.c` | #607 |
| VAX440: the invalidate filter holds 16 bits per entry (a KA46 power-on test failed) | `VAX/vax440_sysdev.c` | #608 |
| VAX4XX: DZ maintenance loopback, and the KA46's per-line parameter registers (the power-on DZ test failed) | `VAX/vax4xx_dz.c` and VAX440 files | #609 |
| VAX440: make the KA46 diagnostic timer count time (the power-on IT test failed) | `VAX/vax440_sysdev.c` | #610 |
| VAX440: present the secondary cache controller as revision B, as the real VAXstation 4000/60 reads it (the power-on CACHE test failed; OpenVMS now enables the cache) | `VAX/vax440_sysdev.c` | #611 |
| VAX: make the RZUSER disk type a fixed disk (an RZUSER unit was listed as removable) | `VAX/vax_rzdev.h` | #612 |
| TIMER: keep the servo's virtual time in step during catch-up seconds (a stop at `sim>`, or `SET CLOCK NOCATCHUP`, cost the guest about a second) | `sim_timer.c` | #613 |
| TIMER: a catch-up dropped at `sim>` no longer counts the stop twice (with `SET CLOCK ELAPSED`) | `sim_timer.c` | on this fork only for now; offered after #603 |
| SCP: end the command loop when its terminal has hung up (a hangup during a run left SIMH spinning at `sim>`, NVRAM unwritten) | `scp.c` | #619 |
| SCP: do not report a lost Telnet console as an internal error (no misleading `Unexpected internal error` line before the stop) | `scp.c` | #620 |
| SOCK: bind a named listen address such as `localhost:P` to IPv4 when it has both families, falling back to the other family when a socket cannot be created (`localhost:P` listened on `[::1]` only, refusing clients that used 127.0.0.1) | `sim_sock.c` | #621 (two commits) |

## Behaviour that differs from Open SIMH

### The guest clock follows elapsed time (SET CLOCK ELAPSED, the default here)

Open SIMH measures a simulated clock against the host's wall clock.
A step of that clock (NTP, a manual change) is taken for lost time,
or for time running backwards; a pause of the simulator over 30
seconds -- a host suspend, a long stall, a debugger -- is forgiven,
so the guest ends up behind by the whole pause (hours after a laptop
suspend); and time spent at the sim> prompt is always forgiven.

This branch measures the clock with the host's elapsed clock, which
keeps counting during a host suspend and is not moved by steps of the
wall clock (Linux CLOCK_BOOTTIME, macOS CLOCK_MONOTONIC, Windows
interrupt time). On simulators with catch-up ticks -- the VAX
families -- a gap of any length, including a stop at sim> followed by
CONTINUE, is repaid as a burst of clock ticks that must finish within
30 seconds (the rest is dropped). The simulator prints
"Clock catch-up: N ms owed" and "Clock catch-up complete (dropped N
ms)" on stdout around each catch-up of more than a second, and a
guest that sets its own time-of-year clock (SET TIME, an NTP step)
ends a catch-up still under way. Other simulators get the elapsed
clock but keep Open SIMH's rule for long gaps. SHOW CLOCKS prints the
time base and how much is still owed.

The time of day a guest reads from its time-of-year clock (TODR,
watch chip) is still the host's wall clock: a step of the host clock
is ignored by a running guest and seen at its next boot.

Why: guests on laptops and loaded hosts came back hours behind after
a suspend, beyond what their NTP will correct, and a host NTP step
moved guests that keep their own time.

To get Open SIMH's timekeeping behaviour (with this branch's bug
fixes still in effect): SET CLOCK NOELAPSED.
Offered to Open SIMH as pull request #603 (with NOELAPSED as its
default); the default is carried here for good.

This file is updated each time the branch moves. Related Open SIMH issues: #594 (`SET <unit>
FORMAT=` on an attached unit); #597 (`SET CLOCK ASYNCH` with idling enabled crashes the
simulator; not fixed here).
