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
        130.0000 - 130.FFFF     scan processor
        1B0.0000 - 1B0.03FF     TBC
        1B1.0000                Bt459 address register, low byte
        1B1.4000                Bt459 address register, high byte
        1B1.8000                Bt459 register port
        1B1.C000                Bt459 colour map port
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
static uint32 lcspx_sp[LCSPX_SPSIZE >> 2];              /* scan processor registers */
static uint32 lcspx_tbc[LCSPX_TBCSIZE >> 2];            /* TBC registers */
static uint32 lcspx_bt[4];                              /* Bt459 ports */

t_stat lcspx_reset (DEVICE *dptr);
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
    { BRDATAD (BT459, lcspx_bt,  16, 32, 4, "Bt459 ports") },
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
    v = lcspx_sp[(off - LCSPX_SP) >> 2];
    sim_debug (DBG_REG, &lcspx_dev, "SP read %04X = %08X, PC %08X\n", off - LCSPX_SP, v, fault_PC);
    return (int32) v;
    }
if ((off - LCSPX_TBC) < LCSPX_TBCSIZE) {                /* TBC */
    v = lcspx_tbc[(off - LCSPX_TBC) >> 2];
    sim_debug (DBG_REG, &lcspx_dev, "TBC read %03X = %08X, PC %08X\n", off - LCSPX_TBC, v, fault_PC);
    return (int32) v;
    }
if ((off - LCSPX_BT) < LCSPX_BTSIZE) {                  /* Bt459: one port per 16 KB */
    v = lcspx_bt[(off - LCSPX_BT) >> 14];
    sim_debug (DBG_REG, &lcspx_dev, "Bt459 read port %d = %08X, PC %08X\n", (off - LCSPX_BT) >> 14, v, fault_PC);
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
    i = (off - LCSPX_SP) >> 2;
    sim_debug (DBG_REG, &lcspx_dev, "SP write %04X = %08X (%d), PC %08X\n", off - LCSPX_SP, val, lnt, fault_PC);
    lcspx_sp[i] = lcspx_merge (lcspx_sp[i], pa, val, lnt);
    return;
    }
if ((off - LCSPX_TBC) < LCSPX_TBCSIZE) {                /* TBC */
    i = (off - LCSPX_TBC) >> 2;
    sim_debug (DBG_REG, &lcspx_dev, "TBC write %03X = %08X (%d), PC %08X\n", off - LCSPX_TBC, val, lnt, fault_PC);
    lcspx_tbc[i] = lcspx_merge (lcspx_tbc[i], pa, val, lnt);
    return;
    }
if ((off - LCSPX_BT) < LCSPX_BTSIZE) {                  /* Bt459 */
    i = (off - LCSPX_BT) >> 14;
    sim_debug (DBG_REG, &lcspx_dev, "Bt459 write port %d = %08X (%d), PC %08X\n", i, val, lnt, fault_PC);
    lcspx_bt[i] = lcspx_merge (lcspx_bt[i], pa, val, lnt);
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
memset (lcspx_tbc, 0, sizeof (lcspx_tbc));
memset (lcspx_bt, 0, sizeof (lcspx_bt));
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
