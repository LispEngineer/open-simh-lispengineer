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
        130.0000 - 13F.FFFF     scan processor (13 8000 up: its
                                diagnostic view)
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
#define LCSPX_ROMHI     0xC0000000                      /* bits <31:16> of every ROM read */

/* Debugging bitmaps */

#define DBG_ROM         0x0001                          /* option ROM */
#define DBG_REG         0x0002                          /* register access */
#define DBG_FB          0x0004                          /* frame-buffer access */

static uint16 *lcspx_rom = NULL;                        /* option ROM, one word per longword */
static uint8 *lcspx_fb = NULL;                          /* frame buffer */
static uint32 lcspx_sp[LCSPX_SPSIZE >> 3];              /* scan processor space */
static uint32 sp_lanes[64];                             /* diagnostic view: lanes not written */

/* Scan processor

   The scan processor's space is 512 KB; the second 512 KB is a
   diagnostic view of it.  In the normal view:
        2100-21FC, 2200-22FC    the register file: register i (0-63) is
                                at 2200 + 4i for i < 32, else 2100 + 4i
        212C                    microcode address <25:16> and the
                                microword's bits <47:32>; reads give the
                                address and bits <47:32> of the word
                                there (subtest 9)
        23F8                    the microword's bits <31:0>: a write
                                stores the 48-bit word at the address
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
   Writing a one to bit k of +00C resets FIFO k: both pointers 0, empty. */

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

#define TBC_FIFORST     (0x00C >> 2)                    /* <3:0> reset FIFO 3-0 */
#define TBC_FIFO0       (0x040 >> 2)                    /* FIFO 0's data port */
#define TBC_FIFOSTEP    (0x020 >> 2)                    /* to the next FIFO's */
#define FIFO_N          4                               /* FIFOs */
#define FIFO_DEPTH      4096                            /* entries */
#define FIFO_BLOCK      (0x8000 >> 2)                   /* FIFO memory per FIFO, longwords */
#define TBC_LINE_USEC   13.0                            /* one line, 1280 x 1024 at 72 Hz */

static uint32 lcspx_fifomem[LCSPX_FIFOSIZE >> 2];       /* FIFO memory */
static uint32 fifo_wp[FIFO_N], fifo_rp[FIFO_N], fifo_cnt[FIFO_N];
static uint32 tbc_line = 0;                             /* line counter */
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
static void tbc_advance (void);
static uint32 tbc_rd (int32 i);
t_stat lcspx_attach (UNIT *uptr, CONST char *cptr);
t_stat lcspx_detach (UNIT *uptr);
t_stat lcspx_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr);
const char *lcspx_description (DEVICE *dptr);

/* LCSPX data structures

   lcspx_dev    LCSPX device descriptor
   lcspx_unit   LCSPX unit: the option ROM file
   lcspx_reg    LCSPX register list
*/

UNIT lcspx_unit = { UDATA (NULL, UNIT_ATTABLE + UNIT_RO, 0) };

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

static void fifo_put (int32 k, uint32 v)
{
if (fifo_cnt[k] >= FIFO_DEPTH)                          /* full: lost */
    return;
lcspx_fifomem[k * FIFO_BLOCK + fifo_wp[k]] = v;
fifo_wp[k] = (fifo_wp[k] + 1) & (FIFO_DEPTH - 1);
fifo_cnt[k]++;
}

static uint32 fifo_get (int32 k)
{
uint32 v;

if (fifo_cnt[k] == 0)                                   /* empty */
    return 0;
v = lcspx_fifomem[k * FIFO_BLOCK + fifo_rp[k]];
fifo_rp[k] = (fifo_rp[k] + 1) & (FIFO_DEPTH - 1);
fifo_cnt[k]--;
return v;
}

/* Read a TBC register */

static uint32 tbc_rd (int32 i)
{
int32 k = (i - TBC_FIFO0) / TBC_FIFOSTEP;
uint32 v;

tbc_advance ();
if ((i >= TBC_FIFO0) && (k < FIFO_N) && (((i - TBC_FIFO0) % TBC_FIFOSTEP) < 4)) {
    switch ((i - TBC_FIFO0) % TBC_FIFOSTEP) {

        case 0:                                         /* data: take the oldest */
            return fifo_get (k);

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
            fifo_put (k, nv);
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

    case TBC_FIFORST:                                   /* FIFO resets */
        for (k = 0; k < FIFO_N; k++) {
            if (wv & (1u << k))
                fifo_wp[k] = fifo_rp[k] = fifo_cnt[k] = 0;
            }
        lcspx_tbc[i] = nv;
        break;

    case TBC_TCSR:
        if ((nv & TCSR_RUN) && !(lcspx_tbc[i] & TCSR_RUN)) {    /* starts: line 0 */
            tbc_line = 0;
            tbc_tlast = sim_gtime ();
            }
        lcspx_tbc[i] = nv & ~TCSR_RO;
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
lcspx_sp[n >> 2] = lcspx_merge (lcspx_sp[n >> 2], pa, val, lnt);
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
    sim_debug (DBG_FB, &lcspx_dev, "FB read %06X = %08X\n", off, v);
    return (int32) v;
    }
if ((off - LCSPX_ROM) < LCSPX_ROMSIZE) {                /* option ROM */
    if (lcspx_rom == NULL)                              /* no file: reads zero */
        return 0;
    v = LCSPX_ROMHI | lcspx_rom[(off - LCSPX_ROM) >> 2];
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
uint32 off = ((uint32) pa) - LCSPX_BASE;
uint32 i;

if (lcspx_dev.flags & DEV_DIS)
    return;
if (off < LCSPX_FBSIZE) {                               /* frame buffer */
    sim_debug (DBG_FB, &lcspx_dev, "FB write %06X = %08X (%d)\n", off, val, lnt);
    if (lnt >= L_LONG) {
        off = off & ~3u;
        lcspx_fb[off] = val & 0xFF;
        lcspx_fb[off + 1] = (val >> 8) & 0xFF;
        lcspx_fb[off + 2] = (val >> 16) & 0xFF;
        lcspx_fb[off + 3] = (val >> 24) & 0xFF;
        }
    else if (lnt == L_WORD) {
        off = off & ~1u;
        lcspx_fb[off] = val & 0xFF;
        lcspx_fb[off + 1] = (val >> 8) & 0xFF;
        }
    else lcspx_fb[off] = val & 0xFF;
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
sp_ucaddr = sp_uchi = 0;
memset (lcspx_tbc, 0, sizeof (lcspx_tbc));
tbc_line = 0;
tbc_tlast = sim_gtime ();
memset (lcspx_fifomem, 0, sizeof (lcspx_fifomem));
memset (fifo_wp, 0, sizeof (fifo_wp));
memset (fifo_rp, 0, sizeof (fifo_rp));
memset (fifo_cnt, 0, sizeof (fifo_cnt));
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
