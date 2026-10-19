/* vax_sgec.c: DEC SGEC (DC541) Ethernet controller

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

   sgec         SGEC Ethernet controller

   The SGEC (second generation Ethernet controller, DEC DC541) is the
   Ethernet interface of the NVAX-based VAXstation 4000 Model 90/96 and of
   several VAX 4000 and MicroVAX 3100 systems.  The CPU sees sixteen
   longword CSRs; the chip moves frames to and from memory through rings of
   four-longword descriptors that it owns while their OW bit is set, and
   learns its station and multicast addresses from a "setup frame" queued
   on the transmit ring.

   There is no published register-level description to work from.  What
   this module does comes from NetBSD's driver (dev/ic/sgec.c, sgecreg.h)
   and from the self-test of the KA49 console firmware, which exercises the
   chip in detail; the comments say which.

   This version has no connection to a network: in normal mode every frame
   is sent onto a cable that is not there and completes with "loss of
   carrier" and "no carrier", as on a machine whose Ethernet cable is
   unplugged; nothing is ever received from outside.  The loopback modes
   work: a frame sent in internal or external loopback is received by the
   chip itself, through its address filter.

   CSRs (register n at base + 4n):

   0    vector <15:2>, IPL <31:30> (14 + n), sync mode <29>; <28:16> and
        <1:0> read as one
   1    transmit poll demand (write), reads as ones
   2    receive poll demand (write), reads as ones
   3    receive descriptor list address
   4    transmit descriptor list address
   5    status: <31> ID init done, <30> SF self-test failed, <29:26> SS
        self-test status, <25:24> TS transmit state, <23:22> RS receive
        state, <18:17> OM (mirrors CSR6<9:8>), <16> DN virtual CSR access
        done, <7> BO, <6> TW, <5> RW, <4> ME, <3> RU, <2> RI, <1> TI
        (write one to clear), <0> IS summary
   6    command: <31> RE reset, <30> IE, <28:25> BL burst limit, <20> BE,
        <19> SE, <11> ST start transmission, <10> SR start reception,
        <9:8> OM 0 normal, 1 internal loopback, 2 external loopback, <7> DC
        disable data chaining, <6> FC force collisions, <3> PB pass bad
        frames, <2:1> AF 1 promiscuous, 2 all multicasts
   7    system page table base (physical), for virtual buffer addresses
   8-14 the "virtual" CSRs (the console reaches them with the DN
        handshake; here DN is always set and they are storage)
   15   monitor command

   Descriptors, longwords 0-3 (NetBSD's struct ze_rdes / ze_tdes):

   0    <31> OW; receive: <30:16> frame length, <15:0> RDES0 status;
        transmit: <30:16> TDR count, <15:0> TDES0 status
   1    receive: <31> CA chain, <30> VA virtual, <29> VT virtual PTE
        address; transmit (TDES1 <31:16>): <31> CA, <30> VA, <29:28> DT
        (2 = setup frame), <27> AC no CRC, <26> FS, <25> LS, <24> IC
        interrupt on completion, <23> VT
   2    <31:16> buffer size, <15:0> byte offset in the first page (VA)
   3    buffer address; with CA, the address of the next descriptor; with
        VA, the address of the first PTE of the buffer (system virtual if
        VT, else physical)
*/

#include "vax_defs.h"
#include "vax_sgec.h"

/* CSR0 */

#define CSR0_RW         0xE000FFFC                      /* IPL, SA, vector */
#define CSR0_MBO        0x1FFF0003
#define CSR0_VEC        0x0000FFFC

/* CSR5 */

#define CSR5_ID         0x80000000                      /* init done */
#define CSR5_SF         0x40000000                      /* self-test failed */
#define CSR5_V_TS       24                              /* transmit state */
#define CSR5_V_RS       22                              /* receive state */
#define CSR5_V_OM       17                              /* operating mode */
#define CSR5_DN         0x00010000                      /* virtual CSR access done */
#define CSR5_MBO        0x0038FF00
#define CSR5_BO         0x00000080                      /* boot message */
#define CSR5_TW         0x00000040                      /* transmit watchdog */
#define CSR5_RW         0x00000020                      /* receive watchdog */
#define CSR5_ME         0x00000010                      /* memory error */
#define CSR5_RU         0x00000008                      /* receive buffer unavailable */
#define CSR5_RI         0x00000004                      /* receive interrupt */
#define CSR5_TI         0x00000002                      /* transmit interrupt */
#define CSR5_IS         0x00000001                      /* interrupt summary */
#define CSR5_W1C        (CSR5_BO | CSR5_TW | CSR5_RW | CSR5_ME | CSR5_RU | CSR5_RI | CSR5_TI)

#define ST_STOP         0                               /* engine states */
#define ST_RUN          1
#define ST_SUSP         2

/* CSR6 */

#define CSR6_RE         0x80000000                      /* reset */
#define CSR6_IE         0x40000000                      /* interrupt enable */
#define CSR6_RW         0x5E1C0FCE                      /* IE, BL, BE, SE, ST, SR, OM, DC, FC, PB, AF */
#define CSR6_MBO        0x01E7F000
#define CSR6_ST         0x00000800                      /* start transmission */
#define CSR6_SR         0x00000400                      /* start reception */
#define CSR6_V_OM       8
#define CSR6_M_OM       3
#define  OM_NORM        0
#define  OM_ILBK        1
#define  OM_ELBK        2
#define CSR6_DC         0x00000080                      /* disable data chaining */
#define CSR6_FC         0x00000040                      /* force collisions */
#define CSR6_AF_PROM    0x00000002                      /* promiscuous */
#define CSR6_AF_ALLM    0x00000004                      /* all multicasts */

/* Writable bits of the other CSRs (the console's register test) */

#define CSR_ADDR        0x3FFFFFFC                      /* CSR3, 4, 7 */
#define CSR9_RW         0x7FC07FC0                      /* watchdog timers */
#define CSR13_RW        0x000000FF
#define CSR10_REV       0x00040000                      /* revision 4 in <19:16> (see sgec_rd) */

/* Descriptors */

#define DES_OW          0x80000000                      /* owned by the SGEC */
#define DES_V_LEN       16                              /* receive frame length */
#define DES_M_LEN       0x7FFF

#define RDES0_ES        0x8000                          /* error summary */
#define RDES0_V_DT      12                              /* data type */
#define RDES0_BO        0x0400                          /* buffer overflow */
#define RDES0_FS        0x0200                          /* first segment */
#define RDES0_LS        0x0100                          /* last segment */
#define RDES0_FT        0x0020                          /* frame type (Ethernet) */
#define RDES0_TN        0x0008                          /* translation not valid */
#define RDES1_CA        0x80000000                      /* chain address */
#define RDES1_VA        0x40000000                      /* virtual address */
#define RDES1_VT        0x20000000                      /* PTE address is virtual */

#define TDES0_ES        0x8000                          /* error summary */
#define TDES0_LO        0x0800                          /* loss of carrier */
#define TDES0_NC        0x0400                          /* no carrier */
#define TDES0_EC        0x0100                          /* excessive collisions */
#define TDES0_CC        0x0078                          /* collision count */
#define TDES0_TN        0x0004                          /* translation not valid */
#define TDES1_CA        0x80000000                      /* chain address */
#define TDES1_VA        0x40000000                      /* virtual address */
#define TDES1_V_DT      28                              /* data type */
#define TDES1_M_DT      3
#define  DT_SETUP       2
#define TDES1_AC        0x08000000                      /* no CRC */
#define TDES1_FS        0x04000000                      /* first segment */
#define TDES1_LS        0x02000000                      /* last segment */
#define TDES1_HP        0x02000000                      /* setup frame: hash filtering (the LS bit) */
#define TDES1_IC        0x01000000                      /* interrupt on completion */
#define TDES1_VT        0x00800000                      /* PTE address is virtual */

#define SGEC_MAXFR      1518                            /* longest frame, with the FCS */
#define SGEC_MINFR      60                              /* shortest frame, without the FCS */
#define SGEC_SETUPSZ    128                             /* setup frame */
#define SGEC_NFILT      16                              /* perfect filter slots */
#define SGEC_DELAY      50                              /* instructions to a DMA start */
#define SGEC_MAXDES     1024                            /* descriptors walked per pass */

/* Debug flags */

#define DBG_REG         0x0001                          /* CSR accesses */
#define DBG_TX          0x0002                          /* transmit ring */
#define DBG_RX          0x0004                          /* receive ring */
#define DBG_INT         0x0008                          /* interrupts */
#define DBG_SET         0x0010                          /* setup frames */

DEBTAB sgec_debug[] = {
    { "REG",  DBG_REG,  "CSR reads and writes" },
    { "TX",   DBG_TX,   "transmit descriptors" },
    { "RX",   DBG_RX,   "receive descriptors" },
    { "INT",  DBG_INT,  "interrupt requests" },
    { "SET",  DBG_SET,  "setup frames and the address filter" },
    { NULL, 0 }
    };

uint32 sgec_csr[SGEC_NCSR] = { 0 };                     /* CSRs as stored */
uint32 sgec_rxcur = 0;                                  /* current receive descriptor */
uint32 sgec_txcur = 0;                                  /* current transmit descriptor */
int32 sgec_txst = ST_STOP;                              /* transmit state */
int32 sgec_rxst = ST_STOP;                              /* receive state */
int32 sgec_irq = 0;                                     /* request asserted */
uint8 sgec_filt[SGEC_NFILT][6];                         /* perfect filter */
uint8 sgec_hash[64];                                    /* hash filter (512 bits) */
int32 sgec_hashmode = 0;                                /* hash filtering */
uint32 sgec_crctab[256];
uint8 sgec_frame[SGEC_MAXFR + 4];                       /* frame being sent */
uint32 sgec_txframes = 0;                               /* statistics */
uint32 sgec_rxframes = 0;

t_stat sgec_svc (UNIT *uptr);
t_stat sgec_reset (DEVICE *dptr);
t_stat sgec_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr);
const char *sgec_description (DEVICE *dptr);
static void sgec_update_int (void);
static void sgec_chip_reset (void);

UNIT sgec_unit = { UDATA (&sgec_svc, 0, 0) };

REG sgec_reg[] = {
    { BRDATAD (CSR,   sgec_csr,   16, 32, SGEC_NCSR, "CSRs as stored") },
    { HRDATAD (RXCUR, sgec_rxcur, 32, "current receive descriptor") },
    { HRDATAD (TXCUR, sgec_txcur, 32, "current transmit descriptor") },
    { DRDATAD (TXST,  sgec_txst,   2, "transmit state (0 stopped, 1 running, 2 suspended)") },
    { DRDATAD (RXST,  sgec_rxst,   2, "receive state (0 stopped, 1 running, 2 suspended)") },
    { FLDATAD (INT,   sgec_irq,    0, "interrupt request") },
    { BRDATAD (HASH,  sgec_hash,  16,  8, 64, "hash filter") },
    { DRDATAD (HASHMODE, sgec_hashmode, 1, "hash filtering") },
    { DRDATAD (TXFRAMES, sgec_txframes, 32, "frames transmitted") },
    { DRDATAD (RXFRAMES, sgec_rxframes, 32, "frames received") },
    { NULL }
    };

DEVICE sgec_dev = {
    "SGEC", &sgec_unit, sgec_reg, NULL,
    1, 16, 32, 4, 16, 32,
    NULL, NULL, &sgec_reset,
    NULL, NULL, NULL,
    NULL, DEV_DEBUG, 0, sgec_debug, NULL, NULL, &sgec_help, NULL, NULL,
    &sgec_description
    };

/* Physical memory, for descriptors, buffers and page tables */

static t_bool sgec_rdl (uint32 pa, uint32 *val)
{
if (((pa & 3) != 0) || !ADDR_IS_MEM (pa))
    return FALSE;
*val = (uint32) ReadL (pa);
return TRUE;
}

static t_bool sgec_wrl (uint32 pa, uint32 val)
{
if (((pa & 3) != 0) || !ADDR_IS_MEM (pa))
    return FALSE;
WriteL (pa, (int32) val);
return TRUE;
}

/* Ethernet CRC-32: the frame check sequence (complemented, sent low byte
   first) and, without the complement, the hash filter's index */

static void sgec_crc_init (void)
{
uint32 c, i, k;

for (i = 0; i < 256; i++) {
    c = i;
    for (k = 0; k < 8; k++)
        c = (c & 1)? (c >> 1) ^ 0xEDB88320: (c >> 1);
    sgec_crctab[i] = c;
    }
}

static uint32 sgec_crc (const uint8 *p, uint32 n)
{
uint32 c = 0xFFFFFFFF;

while (n--)
    c = sgec_crctab[(c ^ *p++) & 0xFF] ^ (c >> 8);
return c;
}

/* A buffer: physical, or (VA) described by a list of PTEs whose address
   is physical or (VT) system virtual, translated through the system page
   table at CSR7.  The PTE form follows the VAX's (valid <31>, page frame
   <20:0>, 512-byte pages); that the SGEC reads it so is an inference from
   the descriptor fields and from VMS's I/O structures (guess). */

typedef struct {
    uint32      addr;                                   /* DES3 */
    uint32      offset;                                 /* byte offset (VA) */
    t_bool      va;                                     /* virtual */
    t_bool      vt;                                     /* PTE address virtual */
    } SGEC_BUF;

static t_bool sgec_pte_pa (uint32 ptea, t_bool vt, uint32 *pa)
{
uint32 spte;

if (!vt) {
    *pa = ptea;
    return TRUE;
    }
if ((ptea & 0xC0000000) != 0x80000000)                  /* system space only */
    return FALSE;
if (!sgec_rdl ((sgec_csr[7] & CSR_ADDR) + (((ptea & 0x3FFFFFFF) >> 9) << 2), &spte))
    return FALSE;
if ((spte & 0x80000000) == 0)
    return FALSE;
*pa = ((spte & 0x1FFFFF) << 9) | (ptea & 0x1FF);
return TRUE;
}

/* Physical address of byte n of a buffer; FALSE if it cannot be mapped */

static t_bool sgec_buf_pa (SGEC_BUF *b, uint32 n, uint32 *pa)
{
uint32 off, ptepa, pte;

if (!b->va) {
    *pa = b->addr + n;
    return ADDR_IS_MEM (*pa);
    }
off = (b->offset & 0x1FF) + n;
if (!sgec_pte_pa (b->addr + ((off >> 9) << 2), b->vt, &ptepa) ||
    !sgec_rdl (ptepa, &pte) || ((pte & 0x80000000) == 0))
    return FALSE;
*pa = ((pte & 0x1FFFFF) << 9) | (off & 0x1FF);
return ADDR_IS_MEM (*pa);
}

static t_bool sgec_buf_read (SGEC_BUF *b, uint8 *dst, uint32 len)
{
uint32 i, pa = 0;

for (i = 0; i < len; i++) {
    if ((i == 0) || (((pa + 1) & 0x1FF) == 0)) {        /* first byte, new page */
        if (!sgec_buf_pa (b, i, &pa))
            return FALSE;
        }
    else pa = pa + 1;
    dst[i] = (uint8) ReadB (pa);
    }
return TRUE;
}

static t_bool sgec_buf_write (SGEC_BUF *b, const uint8 *src, uint32 len)
{
uint32 i, pa = 0;

for (i = 0; i < len; i++) {
    if ((i == 0) || (((pa + 1) & 0x1FF) == 0)) {
        if (!sgec_buf_pa (b, i, &pa))
            return FALSE;
        }
    else pa = pa + 1;
    WriteB (pa, src[i]);
    }
return TRUE;
}

/* Memory error: the chip stops both engines and reports ME */

static void sgec_memerr (uint32 pa)
{
sim_debug (DBG_TX | DBG_RX, &sgec_dev, "memory error at %08X\n", pa);
sgec_csr[5] |= CSR5_ME;
sgec_txst = sgec_rxst = ST_STOP;
}

/* Setup frame: 16 slots of 8 bytes, an address in the first 6 of each
   (NetBSD's layout: the station address in slot 0, broadcast in slot 1,
   multicast addresses after).  With the setup descriptor's HP bit (the LS
   position, TDES1<25>), the first 64 bytes are a 512-bit hash table of
   multicast addresses and bytes 64-69 the station address (the KA49
   console's address filter test). */

static void sgec_load_setup (const uint8 *sf, t_bool hash)
{
int32 i;

sgec_hashmode = hash;
memset (sgec_filt, 0, sizeof (sgec_filt));
memset (sgec_hash, 0, sizeof (sgec_hash));
if (hash) {
    memcpy (sgec_hash, sf, 64);
    memcpy (sgec_filt[0], sf + 64, 6);
    }
else {
    for (i = 0; i < SGEC_NFILT; i++)
        memcpy (sgec_filt[i], sf + (i << 3), 6);
    }
sim_debug (DBG_SET, &sgec_dev, "setup frame (%s): station %02X-%02X-%02X-%02X-%02X-%02X\n",
           hash? "hash": "perfect", sgec_filt[0][0], sgec_filt[0][1], sgec_filt[0][2],
           sgec_filt[0][3], sgec_filt[0][4], sgec_filt[0][5]);
}

/* Address filter.  A destination of all zeros matches no slot, so it is
   rejected unless the chip is promiscuous (the console's test). */

static t_bool sgec_accept (const uint8 *dst)
{
static const uint8 zero[6] = { 0 };
int32 i, mc = dst[0] & 1;
uint32 h;

if (sgec_csr[6] & CSR6_AF_PROM)
    return TRUE;
if (mc && (sgec_csr[6] & CSR6_AF_ALLM))
    return TRUE;
if (memcmp (dst, zero, 6) == 0)
    return FALSE;
if (sgec_hashmode) {
    if (memcmp (dst, sgec_filt[0], 6) == 0)
        return TRUE;
    if (!mc)
        return FALSE;
    h = sgec_crc (dst, 6) & 0x1FF;                      /* CRC without the final complement */
    return (sgec_hash[h >> 3] >> (h & 7)) & 1;
    }
for (i = 0; i < SGEC_NFILT; i++) {
    if (memcmp (dst, sgec_filt[i], 6) == 0)
        return TRUE;
    }
return FALSE;
}

/* Receive a frame (len bytes including the FCS) into the receive ring,
   chaining over several descriptors unless CSR6<DC> */

static void sgec_receive (const uint8 *fr, uint32 len, uint32 dtype)
{
uint32 d0, d1, d2, d3, first = 0, n, done = 0, cnt;
uint32 rdes0, cur;
SGEC_BUF b;

if (((sgec_csr[6] & CSR6_SR) == 0) || (sgec_rxst == ST_STOP))
    return;
if (!sgec_accept (fr)) {
    sim_debug (DBG_RX, &sgec_dev, "frame to %02X-%02X-%02X-%02X-%02X-%02X rejected by the filter\n",
               fr[0], fr[1], fr[2], fr[3], fr[4], fr[5]);
    return;
    }
for (cnt = 0; done < len; cnt++) {
    cur = sgec_rxcur;
    if (!sgec_rdl (cur, &d0) || !sgec_rdl (cur + 4, &d1) ||
        !sgec_rdl (cur + 8, &d2) || !sgec_rdl (cur + 12, &d3)) {
        sgec_memerr (cur);
        return;
        }
    if ((d0 & DES_OW) == 0) {                           /* no buffer */
        sim_debug (DBG_RX, &sgec_dev, "descriptor %08X not owned: receive suspended\n", cur);
        sgec_csr[5] |= CSR5_RU;
        sgec_rxst = ST_SUSP;
        sgec_csr[10] = (sgec_csr[10] & ~0xFFFFu) | ((sgec_csr[10] + 1) & 0xFFFF);
        return;
        }
    if (d1 & RDES1_CA) {                                /* chain */
        sgec_rxcur = d3 & ~3u;
        if (cnt > SGEC_MAXDES)
            return;
        continue;
        }
    b.addr = d3;
    b.offset = d2 & 0xFFFF;
    b.va = (d1 & RDES1_VA) != 0;
    b.vt = (d1 & RDES1_VT) != 0;
    n = (d2 >> 16) & 0xFFFF;
    rdes0 = (dtype << RDES0_V_DT);
    if (done == 0) {
        rdes0 |= RDES0_FS;
        first = cur;
        }
    if (n > len - done)
        n = len - done;
    if (!sgec_buf_write (&b, fr + done, n))
        rdes0 |= RDES0_ES | RDES0_TN;
    done = done + n;
    if ((done < len) && (sgec_csr[6] & CSR6_DC)) {      /* one buffer per frame */
        rdes0 |= RDES0_ES | RDES0_BO;
        done = len;
        }
    if (done >= len) {
        rdes0 |= RDES0_LS;
        if ((((uint32) fr[12] << 8) | fr[13]) >= 0x600)
            rdes0 |= RDES0_FT;
        }
    sgec_rxcur = cur + 16;
    sgec_wrl (cur, (((done >= len)? len: 0) << DES_V_LEN) | rdes0);  /* OW cleared last */
    sim_debug (DBG_RX, &sgec_dev, "descriptor %08X: %d bytes, RDES0 %04X (frame from %08X)\n",
               cur, n, rdes0, first);
    }
sgec_csr[5] |= CSR5_RI;
sgec_rxframes++;
}

/* Transmit engine: runs the transmit ring until a descriptor the chip does
   not own, then suspends.  TI is set by a completed frame with IC and
   whenever the engine suspends; that rule gives exactly the interrupt
   counts the KA49 console's interrupt test demands (two during
   initialisation, one for a looped-back frame). */

static void sgec_transmit (void)
{
uint32 d0, d1, d2, d3, cur, n, len = 0, tdes0, cnt, fcs;
uint32 mode = (sgec_csr[6] >> CSR6_V_OM) & CSR6_M_OM;
uint8 sf[SGEC_SETUPSZ];
SGEC_BUF b;
t_bool err = FALSE;

for (cnt = 0; (sgec_csr[6] & CSR6_ST) && (sgec_txst == ST_RUN); cnt++) {
    cur = sgec_txcur;
    if (!sgec_rdl (cur, &d0) || !sgec_rdl (cur + 4, &d1) ||
        !sgec_rdl (cur + 8, &d2) || !sgec_rdl (cur + 12, &d3)) {
        sgec_memerr (cur);
        break;
        }
    if (((d0 & DES_OW) == 0) || (cnt > SGEC_MAXDES)) {  /* nothing to send */
        sim_debug (DBG_TX, &sgec_dev, "descriptor %08X not owned: transmit suspended\n", cur);
        sgec_txst = ST_SUSP;
        sgec_csr[5] |= CSR5_TI;
        break;
        }
    if (d1 & TDES1_CA) {                                /* chain */
        sgec_txcur = d3 & ~3u;
        continue;
        }
    b.addr = d3;
    b.offset = d2 & 0xFFFF;
    b.va = (d1 & TDES1_VA) != 0;
    b.vt = (d1 & TDES1_VT) != 0;
    n = (d2 >> 16) & 0xFFFF;
    tdes0 = 0;
    sgec_txcur = cur + 16;
    if (((d1 >> TDES1_V_DT) & TDES1_M_DT) == DT_SETUP) {
        if (n > SGEC_SETUPSZ)
            n = SGEC_SETUPSZ;
        memset (sf, 0xFF, sizeof (sf));
        if (sgec_buf_read (&b, sf, n))
            sgec_load_setup (sf, (d1 & TDES1_HP) != 0);
        else tdes0 = TDES0_ES | TDES0_TN;
        sgec_wrl (cur, tdes0);
        if (d1 & TDES1_IC)
            sgec_csr[5] |= CSR5_TI;
        continue;
        }
    if (d1 & TDES1_FS) {                                /* a new frame */
        len = 0;
        err = FALSE;
        }
    if (len + n > SGEC_MAXFR)                           /* too long: keep what fits */
        n = (len < SGEC_MAXFR)? SGEC_MAXFR - len: 0;
    if (!sgec_buf_read (&b, sgec_frame + len, n))
        err = TRUE;
    len = len + n;
    if ((d1 & TDES1_LS) == 0) {                         /* more to come */
        sgec_wrl (cur, 0);
        continue;
        }
    if (err)                                            /* the frame: */
        tdes0 = TDES0_ES | TDES0_TN;
    else if (mode == OM_NORM)                           /* no cable */
        tdes0 = TDES0_ES | TDES0_LO | TDES0_NC;
    else {                                              /* loopback */
        tdes0 = TDES0_LO | TDES0_NC;                    /* (the console expects 0C00) */
        if (sgec_csr[6] & CSR6_FC)
            tdes0 |= TDES0_ES | TDES0_EC;
        }
    if (!err && (mode != OM_NORM) && !(sgec_csr[6] & CSR6_FC)) {
        if (len < SGEC_MINFR) {                         /* pad */
            memset (sgec_frame + len, 0, SGEC_MINFR - len);
            len = SGEC_MINFR;
            }
        if ((d1 & TDES1_AC) == 0) {                     /* append the FCS */
            fcs = sgec_crc (sgec_frame, len) ^ 0xFFFFFFFF;
            sgec_frame[len++] = fcs & 0xFF;
            sgec_frame[len++] = (fcs >> 8) & 0xFF;
            sgec_frame[len++] = (fcs >> 16) & 0xFF;
            sgec_frame[len++] = (fcs >> 24) & 0xFF;
            }
        sgec_receive (sgec_frame, len, mode);
        }
    sim_debug (DBG_TX, &sgec_dev, "descriptor %08X: frame of %d bytes, TDES0 %04X\n", cur, len, tdes0);
    sgec_wrl (cur, tdes0);
    sgec_txframes++;
    if (d1 & TDES1_IC)
        sgec_csr[5] |= CSR5_TI;
    }
}

/* Interrupt request: interrupt status in CSR5 and IE */

static void sgec_update_int (void)
{
int32 irq = ((sgec_csr[5] & CSR5_W1C) != 0) && ((sgec_csr[6] & CSR6_IE) != 0);

if (irq != sgec_irq)
    sim_debug (DBG_INT, &sgec_dev, "request %s (CSR5 %08X)\n", irq? "set": "cleared", sgec_csr[5]);
sgec_irq = irq;
if (irq)
    SGEC_SET_INT;
else
    SGEC_CLR_INT;
}

int32 sgec_vector (void)
{
return (int32) (sgec_csr[0] & CSR0_VEC);
}

t_stat sgec_svc (UNIT *uptr)
{
sgec_transmit ();
sgec_update_int ();
return SCPE_OK;
}

/* CSR read */

int32 sgec_rd (int32 pa)
{
int32 rg = (pa >> 2) & (SGEC_NCSR - 1);
uint32 val;

switch (rg) {

    case 0:
        val = (sgec_csr[0] & CSR0_RW) | CSR0_MBO;
        break;

    case 1: case 2:                                     /* poll demands */
        val = 0xFFFFFFFF;
        break;

    case 5:
        val = (sgec_csr[5] & (CSR5_ID | CSR5_SF | 0x3C000000 | CSR5_W1C)) |
              ((uint32) sgec_txst << CSR5_V_TS) | ((uint32) sgec_rxst << CSR5_V_RS) |
              (((sgec_csr[6] >> CSR6_V_OM) & CSR6_M_OM) << CSR5_V_OM) |
              CSR5_DN | CSR5_MBO;
        if (val & CSR5_W1C)
            val |= CSR5_IS;
        break;

    case 6:
        val = (sgec_csr[6] & CSR6_RW) | CSR6_MBO;
        break;

    case 10:                                            /* revision, missed frames */
        val = CSR10_REV | (sgec_csr[10] & 0xFFFF);      /* VMS's EZDRIVER accepts revision 3 or 4 */
        break;                                          /* only (and loads extra data into a 3) */

    default:
        val = sgec_csr[rg];
        break;
        }
sim_debug (DBG_REG, &sgec_dev, "CSR%d read %08X, PC %08X\n", rg, val, fault_PC);
return (int32) val;
}

/* CSR write */

void sgec_wr (int32 pa, int32 val, int32 lnt)
{
int32 rg = (pa >> 2) & (SGEC_NCSR - 1);
uint32 v = (uint32) val;

if (lnt < L_LONG) {                                     /* merge a partial write */
    int32 sc = (pa & 3) << 3;
    uint32 mask = ((lnt == L_WORD)? 0xFFFFu: 0xFFu) << sc;
    v = ((v << sc) & mask) | ((uint32) sgec_rd (pa & ~3) & ~mask);
    if (rg == 5)                                        /* do not clear what was not written */
        v = v & mask;
    }
sim_debug (DBG_REG, &sgec_dev, "CSR%d write %08X, PC %08X\n", rg, v, fault_PC);
switch (rg) {

    case 0:
        sgec_csr[0] = v & CSR0_RW;
        break;

    case 1:                                             /* transmit poll demand */
        if (sgec_csr[6] & CSR6_ST) {
            sgec_txst = ST_RUN;
            sim_activate_abs (&sgec_unit, SGEC_DELAY);
            }
        break;

    case 2:                                             /* receive poll demand */
        if ((sgec_csr[6] & CSR6_SR) && (sgec_rxst == ST_SUSP))
            sgec_rxst = ST_RUN;
        break;

    case 3:                                             /* receive list */
        sgec_csr[3] = v & CSR_ADDR;
        sgec_rxcur = sgec_csr[3];
        break;

    case 4:                                             /* transmit list */
        sgec_csr[4] = v & CSR_ADDR;
        sgec_txcur = sgec_csr[4];
        break;

    case 5:                                             /* status: write one to clear */
        sgec_csr[5] = sgec_csr[5] & ~(v & CSR5_W1C);
        break;

    case 6:
        if (v & CSR6_RE) {                              /* reset */
            sgec_chip_reset ();
            break;
            }
        sgec_csr[6] = v & CSR6_RW;
        if (v & CSR6_ST) {                              /* (re)start transmission */
            sgec_txst = ST_RUN;
            sim_activate_abs (&sgec_unit, SGEC_DELAY);
            }
        else sgec_txst = ST_STOP;
        if (v & CSR6_SR) {                              /* start reception */
            if (sgec_rxst == ST_STOP)
                sgec_rxst = ST_RUN;
            }
        else sgec_rxst = ST_STOP;
        break;

    case 7:                                             /* system page table */
        sgec_csr[7] = v & CSR_ADDR;
        break;

    case 9:                                             /* watchdog timers */
        sgec_csr[9] = v & CSR9_RW;
        break;

    case 10:                                            /* revision, missed frames: read only */
        break;

    case 13:
        sgec_csr[13] = v & CSR13_RW;
        break;

    default:                                            /* 8, 11, 12, 14, 15 */
        sgec_csr[rg] = v;
        break;
        }
sgec_update_int ();
}

/* Chip reset (CSR6<RE>): the self-test passes at once, CSR6 clears, both
   engines stop, the filter is empty */

static void sgec_chip_reset (void)
{
sim_cancel (&sgec_unit);
sgec_csr[5] = CSR5_ID;
sgec_csr[6] = 0;
sgec_csr[10] = 0;
sgec_txst = sgec_rxst = ST_STOP;
sgec_rxcur = sgec_csr[3];
sgec_txcur = sgec_csr[4];
sgec_hashmode = 0;
memset (sgec_filt, 0, sizeof (sgec_filt));
memset (sgec_hash, 0, sizeof (sgec_hash));
}

t_stat sgec_reset (DEVICE *dptr)
{
sgec_crc_init ();
memset (sgec_csr, 0, sizeof (sgec_csr));
sgec_chip_reset ();
sgec_update_int ();
return SCPE_OK;
}

t_stat sgec_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr)
{
fprintf (st, "SGEC (DC541) Ethernet controller (%s)\n\n", dptr->name);
fprintf (st, "The SGEC is the Ethernet controller of the NVAX-based VAXstation 4000\n");
fprintf (st, "Model 90 and 96.  This version is not connected to a network: frames sent\n");
fprintf (st, "in normal mode complete with loss of carrier and no carrier, as on a\n");
fprintf (st, "machine whose Ethernet cable is unplugged, and nothing is received from\n");
fprintf (st, "outside.  Internal and external loopback work, through the chip's own\n");
fprintf (st, "address filter.  The station address is the network address ROM's (NAR).\n\n");
fprint_reg_help (st, dptr);
return SCPE_OK;
}

const char *sgec_description (DEVICE *dptr)
{
return "SGEC Ethernet controller";
}
