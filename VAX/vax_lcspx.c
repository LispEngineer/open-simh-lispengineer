/* vax_lcspx.c: DEC LCSPX graphics option

   Copyright (c) 2026, Douglas P. Fields, Jr.

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   THE AUTHOR BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Except as contained in this notice, the name of the author shall not be
   used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from the author.

   lcspx        LCSPX 8-plane graphics option

   The LCSPX is the low-cost member of DEC's SPX family of accelerated
   colour frame buffers, the standard graphics of the VAXstation 4000
   Model 90/90A/96 (VMS device GEA0, driver GEBDRIVER; NetBSD spx).  It is
   built from a frame buffer of 8-bit pixels, a scan processor (a
   microcoded pixel engine that fills, copies and draws), the TBC (the
   timing and bus controller: video timing, interrupts, the command FIFO)
   and a Brooktree Bt459 RAMDAC with its colour map and hardware cursor.
   The option carries a ROM in DEC's option ROM format, which the console
   reads at power-up: a header, the device name, the frame-buffer
   description strings, a self-test of 32 subtests that the console copies
   to memory and runs, and a console driver image that the console keeps
   resident at the top of memory.

   Nothing in DEC's published documentation describes the option at the
   register level.  What is modelled comes from the option ROM's own code
   (the self-test says what every register must do), NetBSD's spx.c
   (addresses, frame-buffer layout, the Bt459 ports) and the real
   machines' console output (hardware/register-map.md and
   learnings/m8-lcspx.md in the project that wrote this model).

   Address map, offsets from the option's base (VAXstation 4000/90:
   3800.0000):

        000.0000 - 03F.FFFF     frame buffer, one byte per pixel
        100.0000 - 12F.FFFF     FIFO 0's command window
        130.0000 - 13F.FFFF     scan processor (13 8000 up: its
                                diagnostic view)
        140.0000 - 16F.FFFF     FIFO 1's command window
        180.0000 - 1AF.FFFF     FIFO 2's command window
        1C0.0000 - 1EF.FFFF     FIFO 3's command window
        1B0.0000 - 1B0.03FF     TBC
        1B1.0000                Bt459 address register, low byte
        1B1.4000                Bt459 address register, high byte
        1B1.8000                Bt459 register port
        1B1.C000                Bt459 colour map port
        1B2.0000 - 1B3.FFFF     TBC FIFO memory
        1BC.0000 - 1BF.FFFF     option ROM: 16 bits wide at a longword
                                stride, bits <31:16> read C000

   Any other reference in the window is absent: a read machine-checks, as
   for any address nothing answers, and a write is dropped.

   The option ROM is DEC's firmware, so it is not part of this simulator;
   ATTACH LCSPX <file> loads it, either as dumped (256 KB of longwords,
   each C000xxxx) or as the 128 KB logical byte stream (lane 0, then lane
   1, of each longword).  Without a file the ROM window reads zero, which
   the console rejects as an invalid option ROM header: the graphics test
   then reports "?? 016 2 UNK 0014" and the console goes on.

   SET LCSPX DISABLED removes the option: its window no longer answers and
   the configuration register reports no LCSPX. */

#include "vax_defs.h"
#include "vax_lcspx.h"

#if !defined (LCSPX_BASE)
#error "the model must define LCSPX_BASE, the option's window in I/O space"
#endif

#define LCSPX_ROMWORDS  (LCSPX_ROMSIZE >> 2)            /* 64K 16-bit words */
/* The configuration and diagnostic register is read in bits <31:16> of
   every option ROM longword (the service manual names the bits; the
   self-test's subtest 7 shows <31>'s behaviour):
        <31>    scan processor test: reads 0 for a while after the scan
                processor is reset (TBC +00C <4>), then 1
        <30>    timeout
        <17>    0: 72 Hz monitor timing, 1: 66 Hz
   The real 4000/90 reads C000 in every longword of the ROM. */

#define LCSPX_ROMHI     0xC0000000                      /* bits <31:16> of every ROM read */
#define CFG_SPTEST      0x80000000
#define SP_RESET_INSTR  1000.0                          /* the scan processor's reset, in instructions */

/* Debugging bitmaps */

#define DBG_ROM         0x0001                          /* option ROM */
#define DBG_REG         0x0002                          /* register access */
#define DBG_FB          0x0004                          /* frame-buffer access */
#define DBG_SP          0x0008                          /* scan processor operations */

static uint16 *lcspx_rom = NULL;                        /* option ROM, one word per longword */
static uint8 *lcspx_fb = NULL;                          /* frame buffer */
static uint32 lcspx_sp[LCSPX_SPSIZE >> 3];              /* scan processor space */
static uint32 sp_lanes[64];                             /* diagnostic view: lanes not written */

/* Scan processor

   The scan processor's space is 512 KB; the second 512 KB is a
   diagnostic view of it.  In the normal view:
        2100-21FC, 2200-22FC    the register file: register i (0-63) is
                                at 2200 + 4i for i < 32, else 2100 + 4i
        2100                    <31:24>: nonzero when the microcode is in
                                the chip, zero when the console must load
                                it (its init routine and several subtests
                                take the one path or the other); the
                                model says 01, the path NetBSD's driver
                                assumes (read only; the real board's
                                value is not known)
        212C                    microcode address <25:16> and the
                                microword's bits <47:32>; reads give the
                                address and bits <47:32> of the word
                                there (subtest 9)
        23F8                    the microword's bits <31:0>: a write
                                stores the 48-bit word at the address
        316C                    plane mask control: <17:16> apply the
                                write mask to the CPU's frame-buffer
                                writes, <25:24> the read mask to its
                                reads (subtest 16; what each of the two
                                bits of a pair selects is not known)
        3170, 3174              write and read plane masks, one bit per
                                bit of a frame-buffer longword
        31A8                    the frame buffer's refresh counter: it
                                changes by itself (subtest 15 waits for
                                a change); the model counts every 16
                                instructions, modulo 1024 (the real
                                counter's width is not known)
   The microcode RAM holds 1024 48-bit words; the console loads it at
   power-up (the option's init routine) and the scan processor runs it.
   In the diagnostic view (+80000) the option ROM's self-test (subtests 7
   and 8) needs:
        2100-22FC               the register file, as in the normal view
        2300 + 4i               register i, one byte lane at a time
        2000 + 4i               reads the complement of the byte lanes
                                that the last write through 2300 + 4i
                                carried; writable
        6000-613F               the same memory as in the normal view
        A000-A13F               reads the complement of what was written
   What these views are for in the hardware (the register file is built
   from byte-sliced chips) is not established; the model gives exactly
   what the self-test checks. */

#define SP_DIAG         0x00080000                      /* the diagnostic view */
#define SP_UCADDR       0x212C                          /* microcode address, bits <47:32> */
#define SP_UCDATA       0x23F8                          /* microcode bits <31:0> */
#define SP_UCSIZE       1024                            /* microcode words */
#define SP_REFRESH      0x31A8                          /* refresh counter */
#define SP_ID           0x2100                          /* <31:24> nonzero: microcode in the chip */
#define SP_ID_INIT      0x01000000
#define SP_XSTART       0x21D0                          /* edges, <31:16> */
#define SP_YSTART       0x21D4
#define SP_XEND         0x21D8
#define SP_YEND         0x21DC
#define SP_DSTPIX       0x21E0                          /* <31:30> depth, <23:0> first pixel */
#define SP_SRCPIX       0x21E4
#define SP_STRIDE       0x21E8                          /* <31:16> source, <15:0> destination */
#define SP_COMMAND      0x21EC                          /* <0> right to left, <1> bottom to top */
#define SP_SRCMASK      0x21F0
#define SP_DSTMASK      0x21F4
#define SP_MPC          0x21FC                          /* a write starts the microcode */
#define SP_FG           0x2260
#define SP_BG           0x2264
#define SP_DESTLOOP     0x2270                          /* <3:0> logical function, <31:16> variant */
#define SP_PLANE        0x219C                          /* copy plane: the source bit(s) */

#define SP_CLIPMIN      0x21C0                          /* clip rectangle: <31:16> y, <15:0> x */
#define SP_CLIPMAX      0x21C4                          /* (exclusive) */

#define CMD_XNEG        0x00000001
#define CMD_YNEG        0x00000002
#define CMD_CLIP        0x00000020                      /* clip to CLIPMIN..CLIPMAX */
#define CMD_TRAP        0x00008000                      /* fill a trapezoid */
#define SP_DXL          0x23C8                          /* trapezoid: XSTART's edge step */
#define SP_DXR          0x23CC                          /* XEND's edge step */
#define CMD_WRMASK      0x00020000                      /* SRCMASK masks the writes */
#define CMD_RDMASK      0x00040000                      /* SRCMASK masks the destination read */
#define CMD_SRCMASK     0x01000000                      /* DSTMASK masks the source read */
#define PIX_INDEX       0x00FFFFFF
#define MPC_FILL        0x2019                          /* fill a rectangle with FG */
#define MPC_COPY        0x201A                          /* copy a rectangle */
#define MPC_VECTOR      0x2099                          /* draw a vector */
#define CMD_NOFIRST     0x00000800                      /* vector: leave out the first point */
#define CMD_NOLAST      0x00000400                      /* vector: leave out the last point */
#define SP_PMCTL        0x316C                          /* plane mask control */
#define SP_WRMASK       0x3170                          /* write plane mask */
#define SP_RDMASK       0x3174                          /* read plane mask */
#define PMCTL_WREN      0x00030000                      /* the write mask applies */
#define PMCTL_RDEN      0x03000000                      /* the read mask applies */
#define SP_LANEDIAG     0x2000                          /* lanes view */
#define SP_BYTEREG      0x2300                          /* register file by byte lane */
#define SP_INVMEM       0xA000                          /* complemented memory */

static uint32 sp_ucaddr = 0;                            /* microcode address */
static uint32 sp_uchi = 0;                              /* microword <47:32>, as written */
static uint32 sp_uclo[SP_UCSIZE];                       /* microcode RAM <31:0> */
static uint16 sp_ucmhi[SP_UCSIZE];                      /* microcode RAM <47:32> */

/* The normal-view offset of register i of the register file */

static uint32 sp_regoff (uint32 i)
{
return (i < 32)? (0x2200 + 4 * i): (0x2100 + 4 * i);
}
static uint32 lcspx_tbc[LCSPX_TBCSIZE >> 2];            /* TBC registers */

/* TBC: timing and bus controller

   The TBC generates the video timing.  A line counter runs while the
   timing CSR's RUN bit is set; it counts the lines of a frame from 0 and
   wraps at the vertical blank finish line.  The registers below are the
   ones the option ROM's self-test gives a meaning (subtests 3 and 4);
   the rest are plain read/write registers.

   Timing CSR (+100):
        <2:0>   requests that the TBC carries out at the next vertical
                blank finish, and then clears, while <3> is set; with
                <3> clear they stay set (subtest 4)
        <3>     REQEN: carry out the requests at vertical blank finish
        <5>     RUN: the line counter runs
        <17>    V0: bit 0 of the line counter (read only)
        <18>    VS: in vertical sync (read only)
   Vertical timing, 11-bit fields, in lines from the start of a frame:
        +110    VSS, vertical sync start
        +112    VSF, vertical sync finish
        +114    VBS, vertical blank start
        +116    VBF, vertical blank finish: the frame's last line
   CVAX interrupt status (+018): <3> VBS reached, <4> VBF reached, both
   latched; writing ones clears them.

   The TBC has four FIFOs of 4096 longwords (subtests 10 and 11).  FIFO k
   (0-3) keeps its entries in the FIFO memory at 1B2.0000 + 8000k, which
   the CPU can also read and write directly, and has four registers at
   +040 + 20k:
        +0      data: a write appends an entry, a read takes the oldest
        +4      write pointer <13:2>: where the next entry goes
        +8      read pointer <13:2>: the oldest entry
        +C      free space <14:2>: 1000 when empty
   Writing a one to bit k of +00C resets FIFO k: both pointers 0, empty;
   a one in bit 4 resets the scan processor (see the configuration and
   diagnostic register).

   A longword write into FIFO k's command window (offset w from the
   option's 100.0000) appends two entries: an address entry, 1000000 | w,
   and the data.  The FIFO keeps a tag with each entry (the data may have
   bit 24 set too).  While the FIFO's routing is enabled in the TBC's
   +004 (bit 3 + 5k; FIFO 3: bit 19; subtest 12) the TBC takes the
   entries and writes each datum to the register its address entry names,
   advancing the address by four for further data.  Within the FIFO's
   window:
        20.0000 - 23.FFFF       the TBC's space: its registers, the
                                Bt459, the FIFO memory (1B0.0000 up;
                                subtests 12 and 13)
   With routing disabled the entries wait, and the CPU can see them in the
   FIFO memory. */

#define TBC_CVINTMSK    (0x014 >> 2)                    /* CVAX interrupt mask */
#define TBC_CVINTSTS    (0x018 >> 2)                    /* CVAX interrupt status */
#define TBC_GRINTMSK    (0x024 >> 2)                    /* graphic interrupt mask */
#define TBC_GRINTSTS    (0x028 >> 2)                    /* graphic interrupt status */
#define TBC_TCSR        (0x100 >> 2)                    /* timing CSR */
#define TBC_VSYNC       (0x110 >> 2)                    /* VSS <10:0>, VSF <26:16> */
#define TBC_VBLANK      (0x114 >> 2)                    /* VBS <10:0>, VBF <26:16> */

#define TCSR_REQ        0x00000007                      /* requests done at VBF */
#define TCSR_REQEN      0x00000008                      /* ... while this is set */
#define TCSR_RUN        0x00000020                      /* timing runs */
#define TCSR_V0         0x00020000                      /* line counter <0> */
#define TCSR_VS         0x00040000                      /* in vertical sync */
#define TCSR_RO         (TCSR_V0 | TCSR_VS)

#define CVI_VBS         0x00000008                      /* vertical blank start */
#define CVI_VBF         0x00000010                      /* vertical blank finish */
#define CVI_W1C         0xFFFFFFFF                      /* every status bit: write one to clear */

#define TBC_VMASK       0x7FF                           /* vertical fields: 11 bits */

#define TBC_FIFORST     (0x00C >> 2)                    /* <3:0> reset FIFO 3-0, <4> the scan processor */
#define RST_SP          0x00000010
#define TBC_FIFO0       (0x040 >> 2)                    /* FIFO 0's data port */
#define TBC_FIFOSTEP    (0x020 >> 2)                    /* to the next FIFO's */
#define FIFO_N          4                               /* FIFOs */
#define FIFO_DEPTH      4096                            /* entries */
#define FIFO_BLOCK      (0x8000 >> 2)                   /* FIFO memory per FIFO, longwords */
#define TBC_ROUTE       (0x004 >> 2)                    /* FIFO routing */
#define FIFO_ADDR       0x01000000                      /* an address entry */
#define CMD_TBC         0x00200000                      /* command address: TBC space */
#define CMD_TBCSIZE     0x00040000
#define TBC_LINE_USEC   13.0                            /* one line, 1280 x 1024 at 72 Hz */

static uint32 lcspx_fifomem[LCSPX_FIFOSIZE >> 2];       /* FIFO memory */
static uint8 lcspx_fifotag[LCSPX_FIFOSIZE >> 2];        /* 1: an address entry */
static uint32 fifo_wp[FIFO_N], fifo_rp[FIFO_N], fifo_cnt[FIFO_N];
static uint32 fifo_dest[FIFO_N];                        /* where the next datum goes */
static uint32 tbc_line = 0;                             /* line counter */
static double sp_treset = -1.0e30;                      /* when the scan processor was reset */
static double tbc_tlast = 0;                            /* sim time of the line's start */

/* Bt459 RAMDAC

   Four ports, one per 16 KB of the option's window, data on bits <7:0>:
        0       address register <7:0>
        1       address register <11:8>
        2       the register the address selects
        3       the colour map entry the address selects
   The 12-bit address register increments after each access to a single
   register, and after the third access (red, green, blue: the modulo-3
   counter) to a colour map entry, an overlay colour (100-10F) or a
   cursor colour (181-183); loading either half of it resets the counter.
   Brooktree's Bt459 data sheet gives the register set:
        000-0FF colour map (port 3)     200     ID, reads 4A
        100-10F overlay colours         201-203 command registers 0-2
        181-183 cursor colours          204     pixel read mask
        300-30C cursor and window       206     pixel blink mask
                command and position    208-209 overlay read, blink masks
        400-7FF cursor RAM              20A     interleave
                                        20B     test
                                        20C-20E signature (red, green, blue) */

#define BT_ID           0x4A                            /* Bt459 identification */
#define BT_AMASK        0xFFF                           /* address register */

static uint32 bt_addr = 0;                              /* address register */
static uint32 bt_m3 = 0;                                /* modulo-3 counter */
static uint8 bt_cmap[256][3];                           /* colour map */
static uint8 bt_ovl[16][3];                             /* overlay colours */
static uint8 bt_curcol[3][3];                           /* cursor colours */
static uint8 bt_reg[16];                                /* 200-20F */
static uint8 bt_cur[16];                                /* 300-30F */
static uint8 bt_curram[1024];                           /* 400-7FF */

t_stat lcspx_reset (DEVICE *dptr);
t_stat lcspx_svc (UNIT *uptr);
static void lcspx_wroff (uint32 off, int32 pa, int32 val, int32 lnt);
static void tbc_advance (void);
static uint32 tbc_rd (int32 i);
static void tbc_wr (int32 i, uint32 nv, uint32 wv);
t_stat lcspx_attach (UNIT *uptr, CONST char *cptr);
t_stat lcspx_detach (UNIT *uptr);
t_stat lcspx_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr);
const char *lcspx_description (DEVICE *dptr);

/* LCSPX data structures

   lcspx_dev    LCSPX device descriptor
   lcspx_unit   LCSPX unit: the option ROM file
   lcspx_reg    LCSPX register list
*/

UNIT lcspx_unit = { UDATA (&lcspx_svc, UNIT_ATTABLE + UNIT_RO, 0) };

REG lcspx_reg[] = {
    { BRDATAD (TBC,   lcspx_tbc, 16, 32, LCSPX_TBCSIZE >> 2, "TBC registers") },
    { HRDATAD (BTADDR, bt_addr, 12, "Bt459 address register") },
    { BRDATAD (BTREG, bt_reg, 16, 8, 16, "Bt459 registers 200-20F") },
    { BRDATAD (BTCUR, bt_cur, 16, 8, 16, "Bt459 cursor registers 300-30F") },
    { NULL }
    };

DEBTAB lcspx_debug[] = {
    { "ROM", DBG_ROM, "option ROM" },
    { "REG", DBG_REG, "register access" },
    { "FB",  DBG_FB,  "frame-buffer access" },
    { "SP",  DBG_SP,  "scan processor operations" },
    { NULL, 0 }
    };

DEVICE lcspx_dev = {
    "LCSPX", &lcspx_unit, lcspx_reg, NULL,
    1, 16, 32, 4, 16, 32,
    NULL, NULL, &lcspx_reset,
    NULL, &lcspx_attach, &lcspx_detach,
    NULL, DEV_DISABLE | DEV_DEBUG, 0, lcspx_debug, NULL, NULL, &lcspx_help,
    NULL, NULL, &lcspx_description
    };

/* The option is present unless the device is disabled */

t_bool lcspx_present (void)
{
return (lcspx_dev.flags & DEV_DIS)? FALSE: TRUE;
}

/* Merge a byte or word write into a longword */

static uint32 lcspx_merge (uint32 old, int32 pa, int32 val, int32 lnt)
{
int32 sc = (pa & 3) << 3;
uint32 mask;

if (lnt >= L_LONG)
    return (uint32) val;
mask = ((lnt == L_BYTE)? 0xFFu: 0xFFFFu) << sc;
return (old & ~mask) | (((uint32) val << sc) & mask);
}

/* TBC video timing */

static uint32 tbc_field (int32 reg, int32 hi)
{
return (lcspx_tbc[reg] >> (hi? 16: 0)) & TBC_VMASK;
}

static t_bool tbc_in_vsync (void)
{
return (lcspx_tbc[TBC_TCSR] & TCSR_RUN) &&
    (tbc_line >= tbc_field (TBC_VSYNC, 0)) && (tbc_line < tbc_field (TBC_VSYNC, 1));
}

/* One line period in instructions, from the calibrated instruction rate
   (at least a few instructions, so that a polling loop sees every line) */

static double tbc_line_instr (void)
{
double ipl = sim_timer_inst_per_sec () * TBC_LINE_USEC / 1000000.0;

return (ipl < 16.0)? 16.0: ipl;
}

/* At vertical blank finish the TBC carries out the requests in the
   timing CSR */

static void tbc_vbf_reqs (void)
{
if (lcspx_tbc[TBC_TCSR] & TCSR_REQEN)
    lcspx_tbc[TBC_TCSR] &= ~TCSR_REQ;
}

/* Bring the line counter up to the present, latching the vertical blank
   events it passes */

static void tbc_advance (void)
{
double now = sim_gtime ();
double ipl;
uint32 n, vbs, vbf;

if ((lcspx_tbc[TBC_TCSR] & TCSR_RUN) == 0) {
    tbc_tlast = now;
    return;
    }
ipl = tbc_line_instr ();
if (now - tbc_tlast < ipl)
    return;
vbs = tbc_field (TBC_VBLANK, 0);
vbf = tbc_field (TBC_VBLANK, 1);
if (vbf == 0)                                           /* a frame is at least one line */
    vbf = 1;
if ((now - tbc_tlast) >= (ipl * (vbf + 1))) {           /* a whole frame or more: every event */
    n = (uint32) ((now - tbc_tlast) / ipl);
    lcspx_tbc[TBC_CVINTSTS] |= CVI_VBS | CVI_VBF;
    tbc_vbf_reqs ();
    tbc_line = (tbc_line + n) % (vbf + 1);
    tbc_tlast = tbc_tlast + n * ipl;
    return;
    }
while (now - tbc_tlast >= ipl) {
    tbc_tlast = tbc_tlast + ipl;
    if (tbc_line >= vbf) {                              /* past the last line: a new frame */
        tbc_line = 0;
        lcspx_tbc[TBC_CVINTSTS] |= CVI_VBF;
        tbc_vbf_reqs ();
        }
    else tbc_line++;
    if (tbc_line == vbs)
        lcspx_tbc[TBC_CVINTSTS] |= CVI_VBS;
    }
}

/* TBC FIFOs */

static void fifo_put (int32 k, uint32 v, t_bool addr)
{
if (fifo_cnt[k] >= FIFO_DEPTH)                          /* full: lost */
    return;
lcspx_fifomem[k * FIFO_BLOCK + fifo_wp[k]] = v;
lcspx_fifotag[k * FIFO_BLOCK + fifo_wp[k]] = addr? 1: 0;
fifo_wp[k] = (fifo_wp[k] + 1) & (FIFO_DEPTH - 1);
fifo_cnt[k]++;
}

static uint32 fifo_get (int32 k, t_bool *addr)
{
uint32 v;

if (fifo_cnt[k] == 0) {                                 /* empty */
    *addr = FALSE;
    return 0;
    }
v = lcspx_fifomem[k * FIFO_BLOCK + fifo_rp[k]];
*addr = lcspx_fifotag[k * FIFO_BLOCK + fifo_rp[k]]? TRUE: FALSE;
fifo_rp[k] = (fifo_rp[k] + 1) & (FIFO_DEPTH - 1);
fifo_cnt[k]--;
return v;
}

/* Is FIFO k's routing enabled? */

static t_bool fifo_routed (int32 k)
{
return (lcspx_tbc[TBC_ROUTE] & (1u << ((k < 3)? (3 + 5 * k): 19)))? TRUE: FALSE;
}

/* Carry out one datum taken from a FIFO */

static void cmd_write (uint32 a, uint32 v)
{
a = a % LCSPX_CMDSTEP;                                  /* the address within the FIFO's window */
if ((a - CMD_TBC) < CMD_TBCSIZE)                        /* TBC registers, Bt459, FIFO memory */
    lcspx_wroff (LCSPX_TBC + (a - CMD_TBC), (int32) a, (int32) v, L_LONG);
else sim_debug (DBG_REG, &lcspx_dev, "command to %06X = %08X not modelled\n", a, v);
}

/* Let the TBC take the entries of every routed FIFO */

static void fifo_drain (void)
{
static t_bool busy = FALSE;                             /* a routed write can route more */
t_bool addr;
int32 k;
uint32 v;

if (busy)
    return;
busy = TRUE;
for (k = 0; k < FIFO_N; k++) {
    while (fifo_routed (k) && (fifo_cnt[k] > 0)) {
        v = fifo_get (k, &addr);
        if (addr)
            fifo_dest[k] = v & 0x00FFFFFC;
        else {
            cmd_write (fifo_dest[k], v);
            fifo_dest[k] = (fifo_dest[k] + 4) & 0x00FFFFFC;
            }
        }
    }
busy = FALSE;
}

/* Read a TBC register */

static uint32 tbc_rd (int32 i)
{
int32 k = (i - TBC_FIFO0) / TBC_FIFOSTEP;
uint32 v;
t_bool addr;

tbc_advance ();
if ((i >= TBC_FIFO0) && (k < FIFO_N) && (((i - TBC_FIFO0) % TBC_FIFOSTEP) < 4)) {
    switch ((i - TBC_FIFO0) % TBC_FIFOSTEP) {

        case 0:                                         /* data: take the oldest */
            return fifo_get (k, &addr);

        case 1:                                         /* write pointer */
            return fifo_wp[k] << 2;

        case 2:                                         /* read pointer */
            return fifo_rp[k] << 2;

        default:                                        /* free space */
            return (FIFO_DEPTH - fifo_cnt[k]) << 2;
            }
    }
v = lcspx_tbc[i];
if (i == TBC_TCSR)
    v = (v & ~TCSR_RO) | ((tbc_line & 1)? TCSR_V0: 0) | (tbc_in_vsync ()? TCSR_VS: 0);
return v;
}

/* Write a TBC register: nv is the register's new value, wv the bits the
   write carried (for the write-one-to-clear status registers) */

static void tbc_wr (int32 i, uint32 nv, uint32 wv)
{
int32 k = (i - TBC_FIFO0) / TBC_FIFOSTEP;

tbc_advance ();
if ((i >= TBC_FIFO0) && (k < FIFO_N) && (((i - TBC_FIFO0) % TBC_FIFOSTEP) < 4)) {
    switch ((i - TBC_FIFO0) % TBC_FIFOSTEP) {

        case 0:                                         /* data: append */
            fifo_put (k, nv, FALSE);
            fifo_drain ();
            break;

        default:                                        /* pointers: read only */
            break;
            }
    return;
    }
switch (i) {

    case TBC_CVINTSTS:                                  /* status: write one to clear */
        lcspx_tbc[i] &= ~(wv & CVI_W1C);
        break;

    case TBC_FIFORST:                                   /* FIFO and scan processor resets */
        for (k = 0; k < FIFO_N; k++) {
            if (wv & (1u << k))
                fifo_wp[k] = fifo_rp[k] = fifo_cnt[k] = 0;
            }
        if (wv & RST_SP)
            sp_treset = sim_gtime ();
        lcspx_tbc[i] = nv;
        break;

    case TBC_TCSR:
        if ((nv & TCSR_RUN) && !(lcspx_tbc[i] & TCSR_RUN)) {    /* starts: line 0 */
            tbc_line = 0;
            tbc_tlast = sim_gtime ();
            }
        lcspx_tbc[i] = nv & ~TCSR_RO;
        break;

    case TBC_ROUTE:                                     /* routing: may start a FIFO */
        lcspx_tbc[i] = nv;
        fifo_drain ();
        break;

    default:
        lcspx_tbc[i] = nv;
        break;
        }
}

/* Bt459: the colour triple at an address, or NULL for a single register */

static uint8 *bt_triple (uint32 a, t_bool cmap)
{
if (cmap)
    return bt_cmap[a & 0xFF];
if ((a >= 0x100) && (a <= 0x10F))
    return bt_ovl[a - 0x100];
if ((a >= 0x181) && (a <= 0x183))
    return bt_curcol[a - 0x181];
return NULL;
}

/* The single register at an address */

static uint8 *bt_single (uint32 a)
{
static uint8 none;

if ((a >= 0x200) && (a <= 0x20F))
    return &bt_reg[a - 0x200];
if ((a >= 0x300) && (a <= 0x30F))
    return &bt_cur[a - 0x300];
if ((a >= 0x400) && (a <= 0x7FF))
    return &bt_curram[a - 0x400];
none = 0;                                               /* reserved: reads zero */
return &none;
}

/* Step the address after an access to a triple or a single register */

static void bt_step (t_bool triple)
{
if (triple) {
    if (++bt_m3 < 3)
        return;
    bt_m3 = 0;
    }
bt_addr = (bt_addr + 1) & BT_AMASK;
}

static uint32 bt_rd (uint32 port)
{
uint8 *t;
uint32 v;

switch (port) {

    case 0:                                             /* address <7:0> */
        return bt_addr & 0xFF;

    case 1:                                             /* address <11:8> */
        return (bt_addr >> 8) & 0xF;

    case 2:                                             /* register */
    case 3:                                             /* colour map */
        t = bt_triple (bt_addr, port == 3);
        if (t != NULL) {
            v = t[bt_m3];
            bt_step (TRUE);
            return v;
            }
        if (bt_addr == 0x200)                           /* ID */
            v = BT_ID;
        else v = *bt_single (bt_addr);
        bt_step (FALSE);
        return v;
        }
return 0;
}

static void bt_wr (uint32 port, uint32 v)
{
uint8 *t;

switch (port) {

    case 0:                                             /* address <7:0> */
        bt_addr = (bt_addr & 0xF00) | v;
        bt_m3 = 0;
        break;

    case 1:                                             /* address <11:8> */
        bt_addr = (bt_addr & 0x0FF) | ((v & 0xF) << 8);
        bt_m3 = 0;
        break;

    case 2:                                             /* register */
    case 3:                                             /* colour map */
        t = bt_triple (bt_addr, port == 3);
        if (t != NULL) {
            t[bt_m3] = (uint8) v;
            bt_step (TRUE);
            break;
            }
        if (bt_addr != 0x200)                           /* the ID is read only */
            *bt_single (bt_addr) = (uint8) v;
        bt_step (FALSE);
        break;
        }
}

/* The scan processor's drawing operations

   The scan processor runs microcode; the model carries out what the
   microcode does at the entry points the option ROM and NetBSD's spx
   driver start (the microprogram counter, 21FC), from the registers they
   set up first:
        2019    fill the rectangle with FG (with COMMAND<15>: a trapezoid)
        201A    copy a rectangle from SRCPIX
        2099    draw a vector
   The rectangle's edges are XSTART..XEND and YSTART..YEND (pixels, in
   <31:16>); COMMAND<0> and <1> draw right to left and bottom to top, the
   columns from XEND to XSTART - 1 and the rows from YEND to YSTART - 1.
   DSTPIX (and SRCPIX) give the pixel at (XSTART, YSTART) as an index
   into the frame buffer, and the pixel depth in <31:30> (0: 8 bits, 1:
   16, 2: 32); STRIDE gives the line lengths in pixels.  DESTLOOP<3:0> is
   the logical function of source (FG, or the copied pixel) and
   destination, numbered as X11's: 3 copies.  SRCMASK (21F0) is a mask
   over each longword of the frame buffer (four 8-bit pixels): with
   COMMAND<17> it masks what is written, with COMMAND<18> it masks the
   destination before the logical function (subtest 20); with
   COMMAND<24> DSTMASK (21F4) masks the copied source pixels (subtest 23).
   (The names are NetBSD's.)  DESTLOOP<31:16> selects a variant of the
   copy: 0011 copies pixels; 2012 (the option ROM, subtest 24) and 2014
   (NetBSD's character drawing) copy a plane: a source pixel with any of
   the bits of 219C (its part of the longword mask) set gives FG, else BG.
   With COMMAND<5>
   only the
   pixels inside the clip rectangle are drawn: CLIPMIN (21C0) inclusive
   to CLIPMAX (21C4) exclusive, y in <31:16> and x in <15:0> (subtest
   18).  When the operation is done
   the TBC sets its CVAX interrupt status <1>; the model draws at once
   and reports done after a time that grows with the area (the software
   clears the status after it starts the operation, then waits). */

#define CVI_SPDONE      0x00000002                      /* scan processor done */

static uint32 sp_reg (uint32 o)
{
return lcspx_sp[o >> 2];
}

static uint32 sp_logic (uint32 fn, uint32 src, uint32 dst)
{
switch (fn & 0xF) {
    case 0x0:   return 0;                               /* clear */
    case 0x1:   return src & dst;                       /* and */
    case 0x2:   return src & ~dst;                      /* and reverse */
    case 0x3:   return src;                             /* copy */
    case 0x4:   return ~src & dst;                      /* and inverted */
    case 0x5:   return dst;                             /* no-op */
    case 0x6:   return src ^ dst;                       /* xor */
    case 0x7:   return src | dst;                       /* or */
    case 0x8:   return ~(src | dst);                    /* nor */
    case 0x9:   return ~(src ^ dst);                    /* equivalence */
    case 0xA:   return ~dst;                            /* invert */
    case 0xB:   return src | ~dst;                      /* or reverse */
    case 0xC:   return ~src;                            /* copy inverted */
    case 0xD:   return ~src | dst;                      /* or inverted */
    case 0xE:   return ~(src & dst);                    /* nand */
    default:    return 0xFFFFFFFF;                      /* set */
    }
}

/* A pixel of the frame buffer, by index, at a depth (0, 1, 2) */

static uint32 fb_getpix (uint32 idx, uint32 depth)
{
uint32 b = (idx << depth) & (LCSPX_FBSIZE - 1);

switch (depth) {
    case 0:     return lcspx_fb[b];
    case 1:     return lcspx_fb[b & ~1u] | (lcspx_fb[(b & ~1u) + 1] << 8);
    default:    b = b & ~3u;
                return lcspx_fb[b] | (lcspx_fb[b + 1] << 8) |
                    (lcspx_fb[b + 2] << 16) | ((uint32) lcspx_fb[b + 3] << 24);
    }
}

static void fb_putpix (uint32 idx, uint32 depth, uint32 v)
{
uint32 b = (idx << depth) & (LCSPX_FBSIZE - 1);

switch (depth) {
    case 0:     lcspx_fb[b] = v & 0xFF;
                break;
    case 1:     b = b & ~1u;
                lcspx_fb[b] = v & 0xFF;
                lcspx_fb[b + 1] = (v >> 8) & 0xFF;
                break;
    default:    b = b & ~3u;
                lcspx_fb[b] = v & 0xFF;
                lcspx_fb[b + 1] = (v >> 8) & 0xFF;
                lcspx_fb[b + 2] = (v >> 16) & 0xFF;
                lcspx_fb[b + 3] = (v >> 24) & 0xFF;
                break;
    }
}

static int32 sp_rect (t_bool copy)
{
uint32 cmd = sp_reg (SP_COMMAND);
int32 xs = (int32) (sp_reg (SP_XSTART) >> 16), xe = (int32) (sp_reg (SP_XEND) >> 16);
int32 ys = (int32) (sp_reg (SP_YSTART) >> 16), ye = (int32) (sp_reg (SP_YEND) >> 16);
uint32 depth = sp_reg (SP_DSTPIX) >> 30;
uint32 pmask = (depth == 0)? 0xFF: ((depth == 1)? 0xFFFF: 0xFFFFFFFF);
int32 dstride = (int32) (sp_reg (SP_STRIDE) & 0xFFFF);
int32 sstride = (int32) (sp_reg (SP_STRIDE) >> 16);
int32 dx = (cmd & CMD_XNEG)? -1: 1, dy = (cmd & CMD_YNEG)? -1: 1;
int32 w = (xe - xs) * dx, h = (ye - ys) * dy;           /* sizes, when the direction agrees */
int32 x0 = (dx < 0)? -1: 0, y0 = (dy < 0)? -1: 0;       /* first column and row from the start */
uint32 fn = sp_reg (SP_DESTLOOP) & 0xF;
uint32 lmask = sp_reg (SP_SRCMASK);                     /* over a frame-buffer longword */
uint32 smask = sp_reg (SP_DSTMASK);                     /* the same, for the source */
uint32 dbase = sp_reg (SP_DSTPIX) & PIX_INDEX, sbase = sp_reg (SP_SRCPIX) & PIX_INDEX;
uint32 fg = sp_reg (SP_FG) & pmask;
uint32 bg = sp_reg (SP_BG) & pmask;
uint32 variant = sp_reg (SP_DESTLOOP) >> 16;
t_bool plane = copy && (((variant & 0xF) == 2) || ((variant & 0xF) == 4));
t_bool clip = (cmd & CMD_CLIP)? TRUE: FALSE;
int32 cx0 = sp_reg (SP_CLIPMIN) & 0xFFFF, cy0 = sp_reg (SP_CLIPMIN) >> 16;
int32 cx1 = sp_reg (SP_CLIPMAX) & 0xFFFF, cy1 = sp_reg (SP_CLIPMAX) >> 16;
int32 r, c, x, y;
uint32 d, src, res, m, idx;

sim_debug (DBG_SP, &lcspx_dev, "%s x %d..%d y %d..%d cmd %08X fn %X dst %06X src %06X stride %d/%d fg %X mask %X\n",
    copy? "copy": "fill", xs, xe, ys, ye, cmd, fn, dbase, sbase, dstride, sstride, fg, lmask);
if ((w <= 0) || (h <= 0))
    return 0;
for (r = 0; r < h; r++) {
    y = y0 + r * dy;
    for (c = 0; c < w; c++) {
        x = x0 + c * dx;
        if (clip && (((xs + x) < cx0) || ((xs + x) >= cx1) ||
            ((ys + y) < cy0) || ((ys + y) >= cy1)))
            continue;
        if (copy) {
            idx = sbase + x + y * sstride;
            src = fb_getpix (idx, depth);
            if (cmd & CMD_SRCMASK)
                src = src & (smask >> (((idx << depth) & 3) << 3));
            if (plane)                                  /* expand the plane */
                src = (src & (sp_reg (SP_PLANE) >> (((idx << depth) & 3) << 3)) & pmask)? fg: bg;
            }
        else src = fg;
        idx = dbase + x + y * dstride;
        m = (lmask >> (((idx << depth) & 3) << 3)) & pmask; /* this pixel's part of the mask */
        d = fb_getpix (idx, depth);
        res = sp_logic (fn, src, (cmd & CMD_RDMASK)? (d & m): d) & pmask;
        if (cmd & CMD_WRMASK)
            res = (d & ~m) | (res & m);
        fb_putpix (idx, depth, res);
        }
    }
return w * h;
}

/* Fill a trapezoid (COMMAND<15> with the fill entry; subtest 26)

   The trapezoid lies between two edges, XSTART and XEND, from row
   YSTART to row YEND, all 16.16 fixed point; 23C8 and 23CC are the
   edges' steps per row.  Each row is filled from the left edge's integer
   part up to, not including, the right edge's.  COMMAND<0> says XEND's
   edge is the left one, COMMAND<1> that the rows go upwards (and the
   first row is then the one above YSTART).

   An edge steps as the option ROM's own check of the drawing computes
   it, which is not a plain fixed-point addition: the step's fraction is
   added to the edge's, and the carry goes into the integer part; but
   after a step that carried, the next step subtracts instead a fraction
   taken from YSTART<15:0> (the left edge) or YEND<15:0> (the right
   edge), and adds one more to the integer part unless that borrows.
   The fractions in YSTART and YEND are thus the edges' error terms.
   The real board passes the check, so this is taken to be what its
   microcode does; it is the least certain part of the model. */

typedef struct {
    uint32      f;                                      /* fraction */
    int32       i;                                      /* integer part */
    int32       si;                                     /* step: integer part */
    uint32      sf;                                     /* step: fraction */
    uint32      adj;                                    /* the fraction after a carry */
    t_bool      carried;
    } SP_EDGE;

static void sp_edge_init (SP_EDGE *e, uint32 x, uint32 step, uint32 adj)
{
e->f = x & 0xFFFF;
e->i = (int16) (x >> 16);
e->sf = step & 0xFFFF;
e->si = (int16) (step >> 16);
e->adj = adj & 0xFFFF;
e->carried = FALSE;
}

static void sp_edge_step (SP_EDGE *e)
{
uint32 t;

if (!e->carried) {
    t = e->f + e->sf;
    e->carried = (t & 0xFFFF0000)? TRUE: FALSE;
    }
else {
    t = e->f - e->adj;
    e->carried = (t & 0xFFFF0000)? FALSE: TRUE;
    }
e->f = t & 0xFFFF;
e->i = e->i + e->si + (e->carried? 1: 0);
}

static int32 sp_trap (void)
{
uint32 cmd = sp_reg (SP_COMMAND);
uint32 depth = sp_reg (SP_DSTPIX) >> 30;
uint32 pmask = (depth == 0)? 0xFF: ((depth == 1)? 0xFFFF: 0xFFFFFFFF);
int32 stride = (int32) (sp_reg (SP_STRIDE) & 0xFFFF);
uint32 fn = sp_reg (SP_DESTLOOP) & 0xF;
uint32 fg = sp_reg (SP_FG) & pmask;
uint32 base = sp_reg (SP_DSTPIX) & PIX_INDEX;
int32 x0 = (int16) (sp_reg (SP_XSTART) >> 16), y0 = (int16) (sp_reg (SP_YSTART) >> 16);
int32 y1 = (int16) (sp_reg (SP_YEND) >> 16);
int32 h = y1 - y0, dy = 1, row = y0, r, x, n = 0;
uint32 idx;
SP_EDGE le, re;

if (cmd & CMD_XNEG) {
    sp_edge_init (&le, sp_reg (SP_XEND), sp_reg (SP_DXR), sp_reg (SP_YSTART));
    sp_edge_init (&re, sp_reg (SP_XSTART), sp_reg (SP_DXL), sp_reg (SP_YEND));
    }
else {
    sp_edge_init (&le, sp_reg (SP_XSTART), sp_reg (SP_DXL), sp_reg (SP_YSTART));
    sp_edge_init (&re, sp_reg (SP_XEND), sp_reg (SP_DXR), sp_reg (SP_YEND));
    }
sim_debug (DBG_SP, &lcspx_dev, "trapezoid edges %d/%d rows %d..%d cmd %08X fn %X\n", le.i, re.i, y0, y1, cmd, fn);
if (h < 0) {                                            /* upwards */
    h = -h;
    dy = -1;
    sp_edge_step (&le);
    sp_edge_step (&re);
    row = y0 - 1;
    }
for (r = 0; r < h; r++, row = row + dy) {
    for (x = le.i; x < re.i; x++, n++) {
        idx = base + (x - x0) + (row - y0) * stride;    /* DSTPIX is the pixel at (XSTART, YSTART) */
        fb_putpix (idx, depth, sp_logic (fn, fg, fb_getpix (idx, depth)) & pmask);
        }
    sp_edge_step (&le);
    sp_edge_step (&re);
    }
return n;
}

/* Draw a vector (entry 2099; subtest 27)

   From (XSTART, YSTART) to (XEND, YEND), integer parts, the points that
   Bresenham's algorithm gives, as the option ROM's check computes them:
   the line is taken left to right (the end points, and with them
   COMMAND<11> and <10>, exchanged if need be), mirrored to go downwards,
   and with x and y exchanged when it is steep; the error term starts at
   2 dy - dx - 1, a point moves the minor axis while the term is not
   negative.  COMMAND<11> leaves out the first point, COMMAND<10> the
   last.  Each point takes the logical function of FG and the pixel. */

static int32 sp_vector (void)
{
uint32 cmd = sp_reg (SP_COMMAND);
uint32 depth = sp_reg (SP_DSTPIX) >> 30;
uint32 pmask = (depth == 0)? 0xFF: ((depth == 1)? 0xFFFF: 0xFFFFFFFF);
int32 stride = (int32) (sp_reg (SP_STRIDE) & 0xFFFF);
uint32 fn = sp_reg (SP_DESTLOOP) & 0xF;
uint32 fg = sp_reg (SP_FG) & pmask;
uint32 base = sp_reg (SP_DSTPIX) & PIX_INDEX;
int32 ox = (int16) (sp_reg (SP_XSTART) >> 16), oy = (int16) (sp_reg (SP_YSTART) >> 16);
int32 x0 = ox, y0 = oy;
int32 x1 = (int16) (sp_reg (SP_XEND) >> 16), y1 = (int16) (sp_reg (SP_YEND) >> 16);
t_bool nofirst = (cmd & CMD_NOFIRST)? TRUE: FALSE, nolast = (cmd & CMD_NOLAST)? TRUE: FALSE;
t_bool yneg = FALSE, steep = FALSE, tb;
int32 dx, dy, e, maj, min, last, t, n = 0, px, py;
uint32 idx;

dx = x1 - x0;
if (dx < 0) {                                           /* left to right */
    dx = -dx;
    t = x0; x0 = x1; x1 = t;
    t = y0; y0 = y1; y1 = t;
    tb = nofirst; nofirst = nolast; nolast = tb;
    }
dy = y1 - y0;
if (dy < 0) {                                           /* downwards */
    dy = -dy;
    y0 = -y0;
    y1 = -y1;
    yneg = TRUE;
    }
if (dy > dx) {                                          /* steep: exchange the axes */
    t = dx; dx = dy; dy = t;
    t = x0; x0 = y0; y0 = t;
    t = x1; x1 = y1; y1 = t;
    steep = TRUE;
    }
e = 2 * dy - dx - 1;
sim_debug (DBG_SP, &lcspx_dev, "vector (%d,%d) to (%d,%d) cmd %08X fn %X\n",
    ox, oy, (int16) (sp_reg (SP_XEND) >> 16), (int16) (sp_reg (SP_YEND) >> 16), cmd, fn);
last = nolast? x1 - 1: x1;
for (maj = x0, min = y0; maj <= last; ) {
    if ((maj != x0) || !nofirst) {
        if (steep) {
            px = min;
            py = yneg? -maj: maj;
            }
        else {
            px = maj;
            py = yneg? -min: min;
            }
        idx = base + (px - ox) + (py - oy) * stride;
        fb_putpix (idx, depth, sp_logic (fn, fg, fb_getpix (idx, depth)) & pmask);
        n++;
        }
    maj++;
    if (e < 0)
        e = e + 2 * dy;
    else {
        min++;
        e = e + 2 * dy - 2 * dx;
        }
    }
return n;
}

/* The microprogram counter was written: run the operation */

#define SP_DONE_MIN     50                              /* instructions to "done", at least */
#define SP_PIX_PER_INSTR 8                              /* and per pixel drawn */

static void sp_start (uint32 mpc)
{
int32 n = 0;

switch (mpc & 0xFFFF) {

    case MPC_FILL:
        n = (sp_reg (SP_COMMAND) & CMD_TRAP)? sp_trap (): sp_rect (FALSE);
        break;

    case MPC_COPY:
        n = sp_rect (TRUE);
        break;

    case MPC_VECTOR:
        n = sp_vector ();
        break;

    default:
        sim_debug (DBG_SP, &lcspx_dev, "microcode entry %04X not modelled\n", mpc);
        break;
        }
sim_cancel (&lcspx_unit);
sim_activate (&lcspx_unit, SP_DONE_MIN + n / SP_PIX_PER_INSTR);
}

/* The scan processor has finished */

t_stat lcspx_svc (UNIT *uptr)
{
lcspx_tbc[TBC_CVINTSTS] |= CVI_SPDONE;
return SCPE_OK;
}

/* Scan processor space: o is the longword's offset in the space */

static uint32 sp_rd (uint32 o)
{
uint32 n = o & (SP_DIAG - 1);

if (o & SP_DIAG) {                                      /* diagnostic view */
    if ((n >= SP_LANEDIAG) && (n < SP_LANEDIAG + 0x100))
        return sp_lanes[(n - SP_LANEDIAG) >> 2];
    if ((n >= SP_BYTEREG) && (n < SP_BYTEREG + 0x100))
        n = sp_regoff ((n - SP_BYTEREG) >> 2);
    else if ((n >= SP_INVMEM) && (n < SP_INVMEM + 0x140))
        return ~lcspx_sp[n >> 2];
    }
else if (n == SP_UCADDR)
    return (sp_ucaddr << 16) | sp_ucmhi[sp_ucaddr];
else if (n == SP_UCDATA)
    return sp_uclo[sp_ucaddr];
else if (n == SP_REFRESH)
    return ((uint32) (sim_gtime () / 16.0)) & 0x3FF;
return lcspx_sp[n >> 2];
}

static void sp_wr (uint32 o, int32 pa, int32 val, int32 lnt)
{
uint32 n = o & (SP_DIAG - 1);
uint32 i, lanes;

if (o & SP_DIAG) {                                      /* diagnostic view */
    if ((n >= SP_LANEDIAG) && (n < SP_LANEDIAG + 0x100)) {
        i = (n - SP_LANEDIAG) >> 2;
        sp_lanes[i] = lcspx_merge (sp_lanes[i], pa, val, lnt);
        return;
        }
    if ((n >= SP_BYTEREG) && (n < SP_BYTEREG + 0x100)) {
        i = (n - SP_BYTEREG) >> 2;
        lanes = lcspx_merge (0, pa, -1, lnt);           /* the lanes this write carries */
        sp_lanes[i] = ~lanes;
        n = sp_regoff (i);
        }
    }
else if (n == SP_UCADDR) {
    i = lcspx_merge ((sp_ucaddr << 16) | sp_uchi, pa, val, lnt);
    sp_ucaddr = (i >> 16) & (SP_UCSIZE - 1);
    sp_uchi = i & 0xFFFF;
    return;
    }
else if (n == SP_UCDATA) {
    sp_uclo[sp_ucaddr] = lcspx_merge (sp_uclo[sp_ucaddr], pa, val, lnt);
    sp_ucmhi[sp_ucaddr] = (uint16) sp_uchi;
    return;
    }
if (n == SP_ID)                                         /* read only */
    return;
lcspx_sp[n >> 2] = lcspx_merge (lcspx_sp[n >> 2], pa, val, lnt);
if (n == SP_MPC)
    sp_start (lcspx_sp[n >> 2]);
}

/* Read the option's window

   Inputs:
        pa      =       physical address
   Output:
        longword of data (the caller selects the byte or word it wants)
*/

int32 lcspx_rd (int32 pa)
{
uint32 off = ((uint32) pa) - LCSPX_BASE;
uint32 v;

if (lcspx_dev.flags & DEV_DIS)
    MACH_CHECK (MCHK_READ);
if (off < LCSPX_FBSIZE) {                               /* frame buffer */
    off = off & ~3u;
    v = lcspx_fb[off] | (lcspx_fb[off + 1] << 8) |
        (lcspx_fb[off + 2] << 16) | ((uint32) lcspx_fb[off + 3] << 24);
    if (lcspx_sp[SP_PMCTL >> 2] & PMCTL_RDEN)           /* read plane mask */
        v = v & lcspx_sp[SP_RDMASK >> 2];
    sim_debug (DBG_FB, &lcspx_dev, "FB read %06X = %08X\n", off, v);
    return (int32) v;
    }
if ((off - LCSPX_ROM) < LCSPX_ROMSIZE) {                /* option ROM */
    if (lcspx_rom == NULL)                              /* no file: reads zero */
        return 0;
    v = LCSPX_ROMHI | lcspx_rom[(off - LCSPX_ROM) >> 2];
    if ((sim_gtime () - sp_treset) < SP_RESET_INSTR)  /* the scan processor is still resetting */
        v = v & ~CFG_SPTEST;
    sim_debug (DBG_ROM, &lcspx_dev, "ROM read %05X = %08X\n", off - LCSPX_ROM, v);
    return (int32) v;
    }
if ((off - LCSPX_SP) < LCSPX_SPSIZE) {                  /* scan processor */
    v = sp_rd ((off - LCSPX_SP) & ~3u);
    sim_debug (DBG_REG, &lcspx_dev, "SP read %05X = %08X, PC %08X\n", off - LCSPX_SP, v, fault_PC);
    return (int32) v;
    }
if ((off - LCSPX_TBC) < LCSPX_TBCSIZE) {                /* TBC */
    v = tbc_rd ((off - LCSPX_TBC) >> 2);
    sim_debug (DBG_REG, &lcspx_dev, "TBC read %03X = %08X, PC %08X\n", off - LCSPX_TBC, v, fault_PC);
    return (int32) v;
    }
if ((off - LCSPX_FIFO) < LCSPX_FIFOSIZE) {              /* FIFO memory */
    v = lcspx_fifomem[(off - LCSPX_FIFO) >> 2];
    sim_debug (DBG_REG, &lcspx_dev, "FIFO memory read %05X = %08X, PC %08X\n", off - LCSPX_FIFO, v, fault_PC);
    return (int32) v;
    }
if ((off - LCSPX_BT) < LCSPX_BTSIZE) {                  /* Bt459: one port per 16 KB */
    v = bt_rd ((off - LCSPX_BT) >> 14);
    sim_debug (DBG_REG, &lcspx_dev, "Bt459 read port %d = %02X, PC %08X\n", (off - LCSPX_BT) >> 14, v, fault_PC);
    return (int32) v;
    }
MACH_CHECK (MCHK_READ);                                 /* nothing answers */
return 0;
}

/* Write the option's window

   Inputs:
        pa      =       physical address
        val     =       data, right justified
        lnt     =       length (L_BYTE, L_WORD, L_LONG)
   Outputs:
        none
*/

void lcspx_wr (int32 pa, int32 val, int32 lnt)
{
if (lcspx_dev.flags & DEV_DIS)
    return;
lcspx_wroff (((uint32) pa) - LCSPX_BASE, pa, val, lnt);
}

/* Write at an offset in the option's window, for the CPU or for the
   TBC's command path (pa gives the byte lanes of a short write) */

static void lcspx_wroff (uint32 off, int32 pa, int32 val, int32 lnt)
{
uint32 i;

if (off < LCSPX_FBSIZE) {                               /* frame buffer */
    uint32 old, nv, m;

    sim_debug (DBG_FB, &lcspx_dev, "FB write %06X = %08X (%d)\n", off, val, lnt);
    off = off & ~3u;
    old = lcspx_fb[off] | (lcspx_fb[off + 1] << 8) |
        (lcspx_fb[off + 2] << 16) | ((uint32) lcspx_fb[off + 3] << 24);
    nv = lcspx_merge (old, pa, val, lnt);
    if (lcspx_sp[SP_PMCTL >> 2] & PMCTL_WREN) {         /* write plane mask */
        m = lcspx_sp[SP_WRMASK >> 2];
        nv = (old & ~m) | (nv & m);
        }
    lcspx_fb[off] = nv & 0xFF;
    lcspx_fb[off + 1] = (nv >> 8) & 0xFF;
    lcspx_fb[off + 2] = (nv >> 16) & 0xFF;
    lcspx_fb[off + 3] = (nv >> 24) & 0xFF;
    return;
    }
if (((off - LCSPX_CMD) < (FIFO_N * LCSPX_CMDSTEP)) &&   /* a FIFO's command window */
    (((off - LCSPX_CMD) % LCSPX_CMDSTEP) < LCSPX_CMDSIZE)) {
    i = (off - LCSPX_CMD) / LCSPX_CMDSTEP;
    sim_debug (DBG_REG, &lcspx_dev, "FIFO %d command %06X = %08X (%d), PC %08X\n", i, off - LCSPX_CMD, val, lnt, fault_PC);
    if (lnt >= L_LONG) {
        fifo_put (i, FIFO_ADDR | ((off - LCSPX_CMD) & 0x00FFFFFC), TRUE);
        fifo_put (i, val, FALSE);
        fifo_drain ();
        }
    return;
    }
if ((off - LCSPX_SP) < LCSPX_SPSIZE) {                  /* scan processor */
    sim_debug (DBG_REG, &lcspx_dev, "SP write %05X = %08X (%d), PC %08X\n", off - LCSPX_SP, val, lnt, fault_PC);
    sp_wr ((off - LCSPX_SP) & ~3u, pa, val, lnt);
    return;
    }
if ((off - LCSPX_TBC) < LCSPX_TBCSIZE) {                /* TBC */
    i = (off - LCSPX_TBC) >> 2;
    sim_debug (DBG_REG, &lcspx_dev, "TBC write %03X = %08X (%d), PC %08X\n", off - LCSPX_TBC, val, lnt, fault_PC);
    tbc_wr (i, lcspx_merge (lcspx_tbc[i], pa, val, lnt), lcspx_merge (0, pa, val, lnt));
    return;
    }
if ((off - LCSPX_FIFO) < LCSPX_FIFOSIZE) {              /* FIFO memory */
    i = (off - LCSPX_FIFO) >> 2;
    sim_debug (DBG_REG, &lcspx_dev, "FIFO memory write %05X = %08X (%d), PC %08X\n", off - LCSPX_FIFO, val, lnt, fault_PC);
    lcspx_fifomem[i] = lcspx_merge (lcspx_fifomem[i], pa, val, lnt);
    lcspx_fifotag[i] = 0;
    return;
    }
if ((off - LCSPX_BT) < LCSPX_BTSIZE) {                  /* Bt459 */
    i = (off - LCSPX_BT) >> 14;
    if (lnt < L_LONG)                                   /* the data lane is <7:0> */
        val = val << ((pa & 3) << 3);
    sim_debug (DBG_REG, &lcspx_dev, "Bt459 write port %d = %02X, PC %08X\n", i, val & 0xFF, fault_PC);
    bt_wr (i, val & 0xFF);
    return;
    }
return;                                                 /* the ROM and the holes ignore writes */
}

/* Reset */

t_stat lcspx_reset (DEVICE *dptr)
{
if (lcspx_fb == NULL) {
    lcspx_fb = (uint8 *) calloc (LCSPX_FBSIZE, sizeof (uint8));
    if (lcspx_fb == NULL)
        return SCPE_MEM;
    }
memset (lcspx_sp, 0, sizeof (lcspx_sp));
memset (sp_lanes, 0, sizeof (sp_lanes));
lcspx_sp[SP_ID >> 2] = SP_ID_INIT;
sim_cancel (&lcspx_unit);
sp_ucaddr = sp_uchi = 0;
memset (lcspx_tbc, 0, sizeof (lcspx_tbc));
tbc_line = 0;
tbc_tlast = sim_gtime ();
memset (lcspx_fifomem, 0, sizeof (lcspx_fifomem));
memset (fifo_wp, 0, sizeof (fifo_wp));
memset (fifo_rp, 0, sizeof (fifo_rp));
memset (fifo_cnt, 0, sizeof (fifo_cnt));
memset (fifo_dest, 0, sizeof (fifo_dest));
memset (lcspx_fifotag, 0, sizeof (lcspx_fifotag));
bt_addr = bt_m3 = 0;
return SCPE_OK;
}

/* Attach the option ROM file

   Two forms are accepted:
        262144 bytes    the ROM as the CPU reads it (a dump of
                        39BC.0000-39BF.FFFF): longwords, bits <15:0> data;
                        bits <31:16> should read C000 and are not kept
        131072 bytes    the logical ROM: a byte stream, two bytes per
                        longword, lane 0 (bits <7:0>) first
*/

t_stat lcspx_attach (UNIT *uptr, CONST char *cptr)
{
FILE *f;
uint8 *buf;
long len;
t_stat r;
uint32 i, odd = 0;

f = sim_fopen (cptr, "rb");
if (f == NULL)
    return sim_messagef (SCPE_OPENERR, "%s: cannot open %s\n", sim_dname (&lcspx_dev), cptr);
len = (long) sim_fsize (f);
if ((len != LCSPX_ROMSIZE) && (len != (LCSPX_ROMSIZE >> 1))) {
    fclose (f);
    return sim_messagef (SCPE_ARG, "%s: %s is %ld bytes; the option ROM is %d (as read) or %d (logical)\n",
        sim_dname (&lcspx_dev), cptr, len, LCSPX_ROMSIZE, LCSPX_ROMSIZE >> 1);
    }
buf = (uint8 *) malloc ((size_t) len);
if ((buf == NULL) || (fread (buf, 1, (size_t) len, f) != (size_t) len)) {
    free (buf);
    fclose (f);
    return SCPE_IOERR;
    }
fclose (f);
r = attach_unit (uptr, cptr);                           /* record the file name */
if (r != SCPE_OK) {
    free (buf);
    return r;
    }
if (lcspx_rom == NULL)
    lcspx_rom = (uint16 *) calloc (LCSPX_ROMWORDS, sizeof (uint16));
for (i = 0; i < LCSPX_ROMWORDS; i++) {
    if (len == LCSPX_ROMSIZE) {                         /* as read: little-endian longwords */
        lcspx_rom[i] = buf[4 * i] | (buf[4 * i + 1] << 8);
        if ((buf[4 * i + 2] != 0x00) || (buf[4 * i + 3] != 0xC0))
            odd++;
        }
    else lcspx_rom[i] = buf[2 * i] | (buf[2 * i + 1] << 8);
    }
free (buf);
if (odd)
    sim_messagef (SCPE_OK, "%s: %u longwords of %s do not read C000 in bits <31:16>\n",
        sim_dname (&lcspx_dev), odd, cptr);
return SCPE_OK;
}

t_stat lcspx_detach (UNIT *uptr)
{
free (lcspx_rom);
lcspx_rom = NULL;
return detach_unit (uptr);
}

t_stat lcspx_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr)
{
fprintf (st, "LCSPX 8-plane graphics option (%s)\n\n", dptr->name);
fprintf (st, "The LCSPX is the colour graphics option of the VAXstation 4000 Model 90.\n");
fprintf (st, "Its option ROM, DEC firmware that the console reads and runs at power-up,\n");
fprintf (st, "is not part of the simulator; load a dump of it with\n\n");
fprintf (st, "   sim> ATTACH LCSPX <file>\n\n");
fprintf (st, "The file is either the ROM as the CPU reads it (262144 bytes, one longword\n");
fprintf (st, "C000xxxx per 16-bit word) or the logical ROM (131072 bytes).  Without it\n");
fprintf (st, "the ROM reads as zero and the console reports the graphics as UNK.\n\n");
fprintf (st, "SET LCSPX DISABLED removes the option altogether.\n");
fprint_set_help (st, dptr);
fprint_show_help (st, dptr);
fprint_reg_help (st, dptr);
return SCPE_OK;
}

const char *lcspx_description (DEVICE *dptr)
{
return "LCSPX graphics option";
}
