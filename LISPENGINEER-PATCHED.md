# lispengineer-patched: Open SIMH plus reviewed fixes

This branch of `LispEngineer/open-simh-lispengineer` is **Open SIMH master plus a set of
bug fixes that have each passed review**, merged together so that one build carries all of
them. It moves only after a review has passed; each reviewed state is tagged
`lispengineer-patched-YYYY-MM-DD`. Every fix is also offered to Open SIMH as its own pull
request; as those are merged upstream, this branch converges on Open SIMH master.

Build it exactly as Open SIMH, for example:

    git clone -b lispengineer-patched https://github.com/LispEngineer/open-simh-lispengineer.git
    make -C open-simh-lispengineer vaxstation4000m60 microvax3900

## Fixes carried (reviewed 2026-10-26)

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
| TIMER: let an idle sleep reach the clock tick, not stop short of it (an idle guest's sleep stopped about 1 ms short of each tick and its idle loop spun to the tick: about 10 % of a host core for an idle VAXstation 4000/60 or MicroVAX 3900, now under 1 %) | `sim_timer.c` | #622 (on #596, #599 and #613) |
| TIMER: with `SET CLOCK ELAPSED`, a gap during a catch-up restarts its 30-second deadline (a second host suspend while the guest was still repaying the first dropped hours of the first); a clock that cannot keep up with real time says so once, then summarises every 10 minutes, instead of a pair of catch-up lines every 35 seconds | `sim_timer.c`, `scp.c` | #623 (two commits, on #603) |
| LANCE (XS): an INIT whose initialization block cannot be read sets MERR, not IDON (the KA46 power-on NI test failed subtest D) | `VAX/vax_xs.c` | #624 |
| VAX440: report an NI DMA through an invalid map entry in PAR_CTL<NI_MAP> | `VAX/vax440_defs.h`, `VAX/vax440_sysdev.c` | #625 |
| VAX 4xx: the NI interrupt request is latched on the LANCE line's rising edge, as these boards' interrupt controllers do; STOP clears the rest of CSR0 | `VAX/vax_xs.c`, four VAX 4xx `_defs.h` files | #626 (on #587) |
| LANCE (XS): internal loopback carries the CRC, and a frame under 60 bytes is not reported as a buffer error | `VAX/vax_xs.c`, `sim_ether.h` | #627 |
| LANCE (XS): external loopback hands a sent frame back to the receiver (with the four above, the KA46 power-on NI test passes) | `VAX/vax_xs.c`, `sim_ether.h` | #628 |
| LANCE (XS): a transmit frame over 1518 bytes is CSR0<BABL> and is not sent (it was sent truncated and reported as TMD3<BUFF>) | `VAX/vax_xs.c` | #640 |
| LANCE (XS): the transmit descriptor's status words are written whole, not ORed into what the guest left there | `VAX/vax_xs.c` | #641 |
| LANCE (XS): received multicast is filtered with the guest's logical address filter (all multicast was accepted once any filter bit was set; a zero filter dropped broadcast) | `VAX/vax_xs.c` | #642 |
| SCSI: REASSIGN BLOCKS for disks (it was an unknown command) | `sim_scsi.c` | #638 (on #590) |
| SCSI tape: transfers stay inside the bus buffer; I/O status and sense reported, a failed read is no data, an underlength record reports ILI and its residue | `sim_scsi.c` | #639 (on #590) |
| SCSI: a failed host read or write is MEDIUM ERROR, not HARDWARE ERROR (OpenVMS retried 22 times and ran mount verification; now three retries and a read error) | `sim_scsi.c` | #637 (on #589) |
| SCSI: clear the sense information field when a status is set (a tape read's residue reached the next, unrelated CHECK CONDITION) | `sim_scsi.c` | #644 |
| SCSI tape: answer READ POSITION (34h), short form, instead of an unknown command | `sim_scsi.c` | #645 |
| SCSI tape: an I/O error is MEDIUM ERROR, not HARDWARE ERROR | `sim_scsi.c` | #646 (on #639) |
| SCSI tape: refuse the SPACE codes the unit does not implement (they answered GOOD and moved nothing) | `sim_scsi.c` | #647 |
| SCSI tape: truncate an overlength record and leave the tape after it (it read nothing and left the tape in front of the record) | `sim_scsi.c` | #648 (on #639) |
| VAX440: a SCSI DMA through an invalid map entry sets PAR_CTL<SCSI_MAP> and requests the SCSI interrupt, not the NI's | `VAX/vax440_sysdev.c` | #649 (on #625) |
| VAX4XX: the 53C94 stops a DMA transfer the board did not complete (no terminal count, no completion interrupt) | `VAX/vax4xx_rz94.c`, `VAX/vax440_defs.h` | #632 (the KA49 effort's pull request, whose head carries this change) |
| VAX4XX: DZ raises its transmit request when TRDY is set, once, and a TDR write clears TRDY (it was raised at every poll; NetBSD/vax panicked at `init`) | `VAX/vax4xx_dz.c` | #633 (the KA49 effort's, with our d4de843f) |
| VAX440: SCCR keeps MOD, FONFUL and SPECIO as well as CENA | `VAX/vax440_sysdev.c` | #650 |
| SCSI tape: a READ that meets a filemark reports its residue in the sense Information field (the QIC fixed-block loop delivers the blocks before the mark, then the CHECK CONDITION) | `sim_scsi.c` | #653 (on #639) |
| SCSI: a sense qualifier no longer outlives its CHECK CONDITION (a filemark's ASCQ reached the next, unrelated sense) | `sim_scsi.c` | #654 (on #644) |
| VAX4XX: an asynchronous data-in of 16 bytes or fewer that the board does not take goes into the 53C94's FIFO (terminal count, no interrupt) | `VAX/vax4xx_rz94.c` | #632 (the KA49 effort's pull request, whose head e5793a0f carries this change) |
| VAX440: a model of the KA46 secondary cache, `SET SYSD CACHE` (off by default; with it on the console's TEST 4 passes the CACHE test) | `VAX/vax440_sysdev.c`, `VAX/vax440_defs.h`, `VAX/vax_mmu.h` | #655 (on #611, #625) |

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
30 seconds (the rest is dropped; another gap during a catch-up
restarts the 30 seconds, so neither is lost). The simulator prints
"Clock catch-up: N ms owed" and "Clock catch-up complete (dropped N
ms)" on stdout around each catch-up of more than a second (a guest
that cannot keep up with real time says so once instead), and a
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

## Note: a VAXstation 4000/60 with a fresh NVR boots from the network

With the NI fixes above the KA46's console power-on tests all pass, and a
KA46 (and a MicroVAX 3100/80, KA47) whose NVR file is new then does what
the real machine does with its factory settings: it boots `ESA0`, the
network, and retries for ever (`83 BOOT SYS`, `-ESA0`, `?54 RETRY`), with
a disk attached or not, instead of stopping at `>>>`. `^C` at the
console does not stop it. To have the console stop at `>>>` after its
power-on tests, type `SET HALT 3` at `>>>` once (or `SET BOOT DKA0` to
boot the disk); the setting is kept in the attached NVR file. Open SIMH
master still fails the NI test and stops at `>>>`.

This file is updated each time the branch moves. Related Open SIMH issues: #594 (`SET <unit>
FORMAT=` on an attached unit); #597 (`SET CLOCK ASYNCH` with idling enabled crashes the
simulator; not fixed here).
