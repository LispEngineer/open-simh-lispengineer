# lispengineer-patched: Open SIMH plus reviewed fixes

This branch of `LispEngineer/open-simh-lispengineer` is **Open SIMH master plus a set of
bug fixes that have each passed review**, merged together so that one build carries all of
them. It moves only after a review has passed; each reviewed state is tagged
`lispengineer-patched-YYYY-MM-DD`. Every fix is also offered to Open SIMH as its own pull
request; as those are merged upstream, this branch converges on Open SIMH master.

Build it exactly as Open SIMH, for example:

    git clone -b lispengineer-patched https://github.com/LispEngineer/open-simh-lispengineer.git
    make -C open-simh-lispengineer vaxstation4000m60 microvax3900

## Fixes carried (reviewed 2026-09-30)

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
| TIMER: bound what `sim_idle()` credits for a long or negative sleep (with idling enabled, a host pause of 10 s or more, or a step of the host clock, left the simulator sleeping seconds to minutes per clock tick) | `sim_timer.c` | not yet offered |

This file is updated each time the branch moves. Related Open SIMH issue: #594 (`SET <unit>
FORMAT=` on an attached unit).
