/* vax630_sysdev.c: MicroVAX II system-specific logic

   Copyright (c) 2009-2012, Matt Burke
   This module incorporates code from SimH, Copyright (c) 1998-2008, Robert M Supnik

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
   THE AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Except as contained in this notice, the name(s) of the author(s) shall not be
   used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from the author(s).

   This module contains the MicroVAX II system-specific registers and devices.

   rom          bootstrap ROM (no registers)
   nvr          non-volatile ROM (no registers)
   sysd         system devices

   08-Nov-2012  MB      First version
*/

#include "vax_defs.h"
#include <time.h>

#ifdef DONT_USE_INTERNAL_ROM
#if defined(VAX_620)
#define BOOT_CODE_FILENAME "ka620.bin"
#else
#define BOOT_CODE_FILENAME "ka320.bin"
#endif
#else /* !DONT_USE_INTERNAL_ROM */
#if defined(VAX_620)
#include "vax_ka620_bin.h" /* Defines BOOT_CODE_FILENAME and BOOT_CODE_ARRAY, etc */
#else
#include "vax_ka630_bin.h" /* Defines BOOT_CODE_FILENAME and BOOT_CODE_ARRAY, etc */
#endif
#endif /* DONT_USE_INTERNAL_ROM */


#define UNIT_V_NODELAY  (UNIT_V_UF + 0)                 /* ROM access equal to RAM access */
#define UNIT_NODELAY    (1u << UNIT_V_NODELAY)

t_stat vax630_boot (int32 flag, char *ptr);
int32 sys_model = 0;

/* Special boot command, overrides regular boot */

CTAB vax630_cmd[] = {
    { "BOOT", &vax630_boot, RU_BOOT,
      "bo{ot}                   boot simulator\n", &run_cmd_message },
    { NULL }
    };

/* KA630 boot/diagnostic register */

#define BDR_DISP        0x0000000F                      /* LED display */
#define BDR_V_BDC       8                               /* boot/diag code */
#define BDR_M_BDC       0x3
#define BDR_BDC         (BDR_M_BDC << BDR_V_BDC)
#define BDR_V_CPUC      11                              /* cpu code */
#define BDR_M_CPUC      0x3
#define BDR_CPUC        (BDR_M_CPUC << BDR_V_CPUC)
#define BDR_BRKENB      0x00004000                      /* break enable */
#define BDR_POK         0x00008000                      /* power ok */
#define BDR_RD          (BDR_DISP | BDR_BDC | BDR_CPUC | BDR_BRKENB | BDR_POK)
#define BDR_WR          (BDR_DISP)

/* BDR boot/diagnostic codes */

#define BDC_NORM        0x0                             /* normal startup */
#define BDC_LNGI        0x1                             /* language inquiry */
#define BDC_TSTL        0x2                             /* test loop */
#define BDC_SKPM        0x3                             /* skip mem test */

/* BDR CPU codes */

#define CPUC_ARB        0x0                             /* arbiter */
#define CPUC_AUX1       0x1                             /* auxiliary 1 */
#define CPUC_AUX2       0x2                             /* auxiliary 2 */
#define CPUC_AUX3       0x3                             /* auxiliary 3 */

/* KA630 Memory system error register */

#define MSER_PE         0x00000001                      /* Parity Enable */
#define MSER_WWP        0x00000002                      /* Write Wrong Parity */
#define MSER_LEB        0x00000008                      /* Lost Error Bit */
#define MSER_DQPE       0x00000010                      /* DMA Q22 Parity Err */
#define MSER_CQPE       0x00000020                      /* CPU Q22 Parity Err */
#define MSER_CLPE       0x00000040                      /* CPU Mem Parity Err */
#define MSER_NXM        0x00000080                      /* CPU NXM */
#define MSER_MCD0       0x00000100                      /* Mem Code 0 */
#define MSER_MCD1       0x00000200                      /* Mem Code 1 */
#define MSER_MBZ        0xFFFFFC04
#define MSER_RD         (MSER_PE | MSER_WWP | MSER_LEB | \
	                     MSER_DQPE | MSER_CQPE | MSER_CLPE | \
						 MSER_NXM | MSER_MCD0 | MSER_MCD1)
#define MSER_WR         (MSER_PE | MSER_WWP)
#define MSER_RS         (MSER_LEB | MSER_DQPE | MSER_CQPE | MSER_CLPE | MSER_NXM)

/* KA630 CPU error address reg */

#define CEAR_LMADD      0x00007FFF                      /* local mem addr */
#define CEAR_RD         (CEAR_LMADD)

/* KA630 DMA error address reg */

#define DEAR_LMADD      0x00007FFF                      /* local mem addr */
#define DEAR_RD         (DEAR_LMADD)

extern int32 R[16];
extern int32 STK[5];
extern int32 PSL;
extern int32 SISR;
extern int32 SCBB;
extern int32 mapen;
extern int32 pcq[PCQ_SIZE];
extern int32 pcq_p;
extern int32 ibcnt, ppc;
extern int32 in_ie;
extern int32 mchk_va, mchk_ref;
extern int32 fault_PC;
extern int32 int_req[IPL_HLVL];
extern UNIT cpu_unit;
extern UNIT clk_unit;
extern jmp_buf save_env;
extern int32 p1;
extern int32 tmr_poll;

uint32 *rom = NULL;                                     /* boot ROM */
uint32 *nvr = NULL;                                     /* non-volatile mem */
int32 conisp, conpc, conpsl;                            /* console reg */
int32 ka_bdr = BDR_BRKENB;                              /* KA630 boot diag */
int32 ka_mser = 0;                                      /* KA630 mem sys err */
int32 ka_cear = 0;                                      /* KA630 cpu err */
int32 ka_dear = 0;                                      /* KA630 dma err */
static uint32 rom_delay = 0;
t_bool ka_diag_full = FALSE;
t_bool ka_hltenab = TRUE;                               /* Halt Enable / Autoboot flag */

t_stat rom_ex (t_value *vptr, t_addr exta, UNIT *uptr, int32 sw);
t_stat rom_dep (t_value val, t_addr exta, UNIT *uptr, int32 sw);
t_stat rom_reset (DEVICE *dptr);
t_stat rom_set_diag (UNIT *uptr, int32 val, char *cptr, void *desc);
t_stat rom_show_diag (FILE *st, UNIT *uptr, int32 val, void *desc);
char *rom_description (DEVICE *dptr);
t_stat nvr_ex (t_value *vptr, t_addr exta, UNIT *uptr, int32 sw);
t_stat nvr_dep (t_value val, t_addr exta, UNIT *uptr, int32 sw);
t_stat nvr_reset (DEVICE *dptr);
t_stat nvr_attach (UNIT *uptr, char *cptr);
t_stat nvr_detach (UNIT *uptr);
char *nvr_description (DEVICE *dptr);
t_stat sysd_reset (DEVICE *dptr);
char *sysd_description (DEVICE *dptr);

int32 rom_rd (int32 pa);
int32 nvr_rd (int32 pa);
void nvr_wr (int32 pa, int32 val, int32 lnt);
int32 ka_rd (int32 pa);
void ka_wr (int32 pa, int32 val, int32 lnt);
t_stat sysd_powerup (void);
int32 con_halt (int32 code, int32 cc);

extern int32 intexc (int32 vec, int32 cc, int32 ipl, int ei);
extern int32 qbmap_rd (int32 pa);
extern void qbmap_wr (int32 pa, int32 val, int32 lnt);
extern int32 qbmem_rd (int32 pa);
extern void qbmem_wr (int32 pa, int32 val, int32 lnt);
extern int32 wtc_rd (int32 pa);
extern void wtc_wr (int32 pa, int32 val, int32 lnt);
extern void wtc_set_valid (void);
extern void wtc_set_invalid (void);
extern int32 iccs_rd (void);
extern int32 todr_rd (void);
extern int32 rxcs_rd (void);
extern int32 rxdb_rd (void);
extern int32 txcs_rd (void);
extern void iccs_wr (int32 dat);
extern void todr_wr (int32 dat);
extern void rxcs_wr (int32 dat);
extern void txcs_wr (int32 dat);
extern void txdb_wr (int32 dat);
extern void ioreset_wr (int32 dat);

/* ROM data structures

   rom_dev      ROM device descriptor
   rom_unit     ROM units
   rom_reg      ROM register list
*/

UNIT rom_unit = { UDATA (NULL, UNIT_FIX+UNIT_BINK, ROMSIZE) };

REG rom_reg[] = {
    { NULL }
    };

MTAB rom_mod[] = {
    { UNIT_NODELAY, UNIT_NODELAY, "fast access", "NODELAY", NULL },
    { UNIT_NODELAY, 0, "1usec calibrated access", "DELAY", NULL },
    { 0 }
    };

DEVICE rom_dev = {
    "ROM", &rom_unit, rom_reg, rom_mod,
    1, 16, ROMAWIDTH, 4, 16, 32,
    &rom_ex, &rom_dep, &rom_reset,
    NULL, NULL, NULL,
    NULL, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL, 
    &rom_description
    };

/* NVR data structures

   nvr_dev      NVR device descriptor
   nvr_unit     NVR units
   nvr_reg      NVR register list
*/

UNIT nvr_unit =
    { UDATA (NULL, UNIT_FIX+UNIT_BINK, NVRSIZE) };

REG nvr_reg[] = {
    { NULL }
    };

DEVICE nvr_dev = {
    "NVR", &nvr_unit, nvr_reg, NULL,
    1, 16, NVRAWIDTH, 4, 16, 32,
    &nvr_ex, &nvr_dep, &nvr_reset,
    NULL, &nvr_attach, &nvr_detach,
    NULL, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL, 
    &nvr_description
    };

/* SYSD data structures

   sysd_dev     SYSD device descriptor
   sysd_unit    SYSD units
   sysd_reg     SYSD register list
*/

UNIT sysd_unit = { UDATA (NULL, 0, 0) };

REG sysd_reg[] = {
    { HRDATAD (CONISP,         conisp, 32, "console ISP") },
    { HRDATAD (CONPC,           conpc, 32, "console PD") },
    { HRDATAD (CONPSL,         conpsl, 32, "console PSL") },
    { HRDATAD (BDR,            ka_bdr, 16, "KA630 boot diag") },
    { HRDATAD (MSER,          ka_mser,  8, "KA630 mem sys err") },
    { HRDATAD (CEAR,          ka_cear,  8, "KA630 cpu err") },
    { HRDATAD (DEAR,          ka_dear,  8, "KA630 dma err") },
    { HRDATAD (DEAR,          ka_dear,  8, "KA630 dma err") },
    { FLDATAD (DIAG,     ka_diag_full,  0, "KA630 Full Boot diagnostics") },
    { FLDATAD (HLTENAB,    ka_hltenab,  0, "KA630 Autoboot/Halt Enable") },
    { NULL }
    };

DEVICE sysd_dev = {
    "SYSD", &sysd_unit, sysd_reg, NULL,
    1, 16, 16, 1, 16, 8,
    NULL, NULL, &sysd_reset,
    NULL, NULL, NULL,
    NULL, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL, 
    &sysd_description
    };

/* ROM: read only memory - stored in a buffered file
   Register space access routines see ROM twice

   ROM access has been 'regulated' to about 1Mhz to avoid issues
   with testing the interval timers in self-test.  Specifically,
   the VAX boot ROM (ka630.bin) contains code which presumes that
   the VAX runs at a particular slower speed when code is running
   from ROM (which is not cached).  These assumptions are built
   into instruction based timing loops. As the host platform gets
   much faster than the original VAX, the assumptions embedded in
   these code loops are no longer valid.
   
   Code has been added to the ROM implementation to limit CPU speed
   to about 500K instructions per second.  This heads off any future
   issues with the embedded timing loops.  
*/

int32 rom_swapb(int32 val)
{
return ((val << 24) & 0xff000000) | (( val << 8) & 0xff0000) |
    ((val >> 8) & 0xff00) | ((val >> 24) & 0xff);
}

int32 rom_read_delay (int32 val)
{
uint32 i, l = rom_delay;
int32 loopval = 0;

if (rom_unit.flags & UNIT_NODELAY)
    return val;

/* Calibrate the loop delay factor when first used.
   Do this 4 times to and use the largest value computed. */

if (rom_delay == 0) {
    uint32 ts, te, c = 10000, samples = 0;
    while (1) {
        c = c * 2;
        te = sim_os_msec();
        while (te == (ts = sim_os_msec ()));            /* align on ms tick */

/* This is merely a busy wait with some "work" that won't get optimized
   away by a good compiler. loopval always is zero.  To avoid smart compilers,
   the loopval variable is referenced in the function arguments so that the
   function expression is not loop invariant.  It also must be referenced
   by subsequent code or to avoid the whole computation being eliminated. */

        for (i = 0; i < c; i++)
            loopval |= (loopval + ts) ^ rom_swapb (rom_swapb (loopval + ts));
        te = sim_os_msec (); 
        if ((te - ts) < 50)                         /* sample big enough? */
            continue;
        if (rom_delay < (loopval + (c / (te - ts) / 1000) + 1))
            rom_delay = loopval + (c / (te - ts) / 1000) + 1;
        if (++samples >= 4)
            break;
        c = c / 2;
        }
    if (rom_delay < 5)
        rom_delay = 5;
    }

for (i = 0; i < l; i++)
    loopval |= (loopval + val) ^ rom_swapb (rom_swapb (loopval + val));
return val + loopval;
}

int32 rom_rd (int32 pa)
{
int32 rg = ((pa - ROMBASE) & ROMAMASK) >> 2;

return rom_read_delay (rom[rg]);
}

void rom_wr_B (int32 pa, int32 val)
{
int32 rg = ((pa - ROMBASE) & ROMAMASK) >> 2;
int32 sc = (pa & 3) << 3;

rom[rg] = ((val & 0xFF) << sc) | (rom[rg] & ~(0xFF << sc));
return;
}

/* ROM examine */

t_stat rom_ex (t_value *vptr, t_addr exta, UNIT *uptr, int32 sw)
{
uint32 addr = (uint32) exta;

if ((vptr == NULL) || (addr & 03))
    return SCPE_ARG;
if (addr >= ROMSIZE)
    return SCPE_NXM;
*vptr = rom[addr >> 2];
return SCPE_OK;
}

/* ROM deposit */

t_stat rom_dep (t_value val, t_addr exta, UNIT *uptr, int32 sw)
{
uint32 addr = (uint32) exta;

if (addr & 03)
    return SCPE_ARG;
if (addr >= ROMSIZE)
    return SCPE_NXM;
rom[addr >> 2] = (uint32) val;
return SCPE_OK;
}

/* ROM reset */

t_stat rom_reset (DEVICE *dptr)
{
if (rom == NULL)
    rom = (uint32 *) calloc (ROMSIZE >> 2, sizeof (uint32));
if (rom == NULL)
    return SCPE_MEM;
return SCPE_OK;
}

char *rom_description (DEVICE *dptr)
{
return "read-only memory";
}

/* NVR: non-volatile RAM - stored in a buffered file */

int32 nvr_rd (int32 pa)
{
int32 rg = (pa - NVRBASE) >> 2;

if (rg < 7)                                             /* watch chip */
    return wtc_rd (pa);
else
    return nvr[rg];
}

void nvr_wr (int32 pa, int32 val, int32 lnt)
{
int32 rg = (pa - NVRBASE) >> 2;

if (rg < 7)                                             /* watch chip */
    wtc_wr (pa, val, lnt);
else {
    int32 sc = (pa & 3) << 3;                           /* merge */
    int32 mask = 0xFF;
    nvr[rg] = ((val & mask) << sc) | (nvr[rg] & ~(mask << sc));
    }
}

/* NVR examine */

t_stat nvr_ex (t_value *vptr, t_addr exta, UNIT *uptr, int32 sw)
{
uint32 addr = (uint32) exta;

if ((vptr == NULL) || (addr & 03))
    return SCPE_ARG;
if (addr >= NVRSIZE)
    return SCPE_NXM;
*vptr = nvr[addr >> 2];
return SCPE_OK;
}

/* NVR deposit */

t_stat nvr_dep (t_value val, t_addr exta, UNIT *uptr, int32 sw)
{
uint32 addr = (uint32) exta;

if (addr & 03)
    return SCPE_ARG;
if (addr >= NVRSIZE)
    return SCPE_NXM;
nvr[addr >> 2] = (uint32) val;
return SCPE_OK;
}

/* NVR reset */

t_stat nvr_reset (DEVICE *dptr)
{
if (nvr == NULL) {
    nvr = (uint32 *) calloc (NVRSIZE >> 2, sizeof (uint32));
    nvr_unit.filebuf = nvr;
    }
if (nvr == NULL)
    return SCPE_MEM;
return SCPE_OK;
}

/* NVR attach */

t_stat nvr_attach (UNIT *uptr, char *cptr)
{
t_stat r;

uptr->flags = uptr->flags | (UNIT_ATTABLE | UNIT_BUFABLE);
r = attach_unit (uptr, cptr);
if (r != SCPE_OK)
    uptr->flags = uptr->flags & ~(UNIT_ATTABLE | UNIT_BUFABLE);
else {
    uptr->hwmark = (uint32) uptr->capac;
    wtc_set_valid ();
    }
return r;
}

/* NVR detach */

t_stat nvr_detach (UNIT *uptr)
{
t_stat r;

r = detach_unit (uptr);
if ((uptr->flags & UNIT_ATT) == 0) {
    uptr->flags = uptr->flags & ~(UNIT_ATTABLE | UNIT_BUFABLE);
    wtc_set_invalid ();
    }
return r;
}

char *nvr_description (DEVICE *dptr)
{
return "non-volatile memory";
}

/* Read KA630 specific IPR's */

int32 ReadIPR (int32 rg)
{
int32 val;

switch (rg) {

    case MT_ICCS:                                       /* ICCS */
        val = iccs_rd ();
        break;

    case MT_RXCS:                                       /* RXCS */
        val = rxcs_rd ();
        break;

    case MT_RXDB:                                       /* RXDB */
        val = rxdb_rd ();
        break;

    case MT_TXCS:                                       /* TXCS */
        val = txcs_rd ();
        break;

    case MT_TXDB:                                       /* TXDB */
        val = 0;
        break;

    case MT_CONISP:                                     /* console ISP */
        val = conisp;
        break;

    case MT_CONPC:                                      /* console PC */
        val = conpc;
        break;

    case MT_CONPSL:                                     /* console PSL */
        val = conpsl;
        break;

    case MT_SID:                                        /* SID */
#if defined(VAX_620)
        val = VAX620_SID;
#else
        val = VAX630_SID;
#endif
        break;

    case MT_NICR:                                       /* NICR */
    case MT_ICR:                                        /* ICR */
    case MT_TODR:                                       /* TODR */
    case MT_CSRS:                                       /* CSRS */
    case MT_CSRD:                                       /* CSRD */
    case MT_CSTS:                                       /* CSTS */
    case MT_CSTD:                                       /* CSTD */
    case MT_TBDR:                                       /* TBDR */
    case MT_CADR:                                       /* CADR */
    case MT_MCESR:                                      /* MCESR */
    case MT_CAER:                                       /* CAER */
    case MT_SBIFS:                                      /* SBIFS */
    case MT_SBIS:                                       /* SBIS */
    case MT_SBISC:                                      /* SBISC */
    case MT_SBIMT:                                      /* SBIMT */
    case MT_SBIER:                                      /* SBIER */
    case MT_SBITA:                                      /* SBITA */
    case MT_SBIQC:                                      /* SBIQC */
    case MT_TBDATA:                                     /* TBDATA */
    case MT_MBRK:                                       /* MBRK */
    case MT_PME:                                        /* PME */
        val = 0;
        break;

    default:
        RSVD_OPND_FAULT;
        }

return val;
}

/* Write KA630 specific IPR's */

void WriteIPR (int32 rg, int32 val)
{
switch (rg) {

    case MT_ICCS:                                       /* ICCS */
        iccs_wr (val);
        break;

    case MT_RXCS:                                       /* RXCS */
        rxcs_wr (val);
        break;

    case MT_RXDB:                                       /* RXDB */
        break;

    case MT_TXCS:                                       /* TXCS */
        txcs_wr (val);
        break;

    case MT_TXDB:                                       /* TXDB */
        txdb_wr (val);
        break;

    case MT_IORESET:                                    /* IORESET */
        ioreset_wr (val);
        break;

    case MT_SID:
    case MT_CONISP:
    case MT_CONPC:
    case MT_CONPSL:                                     /* halt reg */
        RSVD_OPND_FAULT;

    case MT_NICR:                                       /* NICR */
    case MT_ICR:                                        /* ICR */
    case MT_TODR:                                       /* TODR */
    case MT_CSRS:                                       /* CSRS */
    case MT_CSRD:                                       /* CSRD */
    case MT_CSTS:                                       /* CSTS */
    case MT_CSTD:                                       /* CSTD */
    case MT_TBDR:                                       /* TBDR */
    case MT_CADR:                                       /* CADR */
    case MT_MCESR:                                      /* MCESR */
    case MT_CAER:                                       /* CAER */
    case MT_SBIFS:                                      /* SBIFS */
    case MT_SBIS:                                       /* SBIS */
    case MT_SBISC:                                      /* SBISC */
    case MT_SBIMT:                                      /* SBIMT */
    case MT_SBIER:                                      /* SBIER */
    case MT_SBITA:                                      /* SBITA */
    case MT_SBIQC:                                      /* SBIQC */
    case MT_TBDATA:                                     /* TBDATA */
    case MT_MBRK:                                       /* MBRK */
    case MT_PME:                                        /* PME */
        break;

    default:
        RSVD_OPND_FAULT;
        }

return;
}

/* Read/write I/O register space

   These routines are the 'catch all' for address space map.  Any
   address that doesn't explicitly belong to memory, I/O, or ROM
   is given to these routines for processing.
*/

struct reglink {                                        /* register linkage */
    uint32      low;                                    /* low addr */
    uint32      high;                                   /* high addr */
    int32       (*read)(int32 pa);                      /* read routine */
    void        (*write)(int32 pa, int32 val, int32 lnt); /* write routine */
    };

struct reglink regtable[] = {
    { QBMAPBASE, QBMAPBASE+QBMAPSIZE, &qbmap_rd, &qbmap_wr },
    { ROMBASE, ROMBASE+ROMSIZE+ROMSIZE, &rom_rd, NULL },
    { NVRBASE, NVRBASE+NVRSIZE, &nvr_rd, &nvr_wr },
    { KABASE, KABASE+KASIZE, &ka_rd, &ka_wr },
/*    { QVMBASE, QVMBASE+QVMSIZE, &qv_mem_rd, &qv_mem_wr }, */
    { QBMBASE, QBMBASE+QBMSIZE, &qbmem_rd, &qbmem_wr },
    { 0, 0, NULL, NULL }
    };

/* ReadReg - read register space

   Inputs:
        pa      =       physical address
        lnt     =       length (BWLQ) - ignored
   Output:
        longword of data
*/

int32 ReadReg (uint32 pa, int32 lnt)
{
struct reglink *p;

for (p = &regtable[0]; p->low != 0; p++) {
    if ((pa >= p->low) && (pa < p->high) && p->read)
        return p->read (pa);
    }

MACH_CHECK (MCHK_READ);
}

/* WriteReg - write register space

   Inputs:
        pa      =       physical address
        val     =       data to write, right justified in 32b longword
        lnt     =       length (BWLQ)
   Outputs:
        none
*/

void WriteReg (uint32 pa, int32 val, int32 lnt)
{
struct reglink *p;

for (p = &regtable[0]; p->low != 0; p++) {
    if ((pa >= p->low) && (pa < p->high) && p->write) {
        p->write (pa, val, lnt);  
        return;
        }
    }

MACH_CHECK (MCHK_WRITE);
}

/* KA630 registers */

int32 ka_rd (int32 pa)
{
int32 rg = (pa - KABASE) >> 2;

switch (rg) {

    case 0:                                             /* BDR */
        return ka_bdr & BDR_RD;

    case 1:                                             /* MSER */
        return ka_mser & MSER_RD;

    case 2:                                             /* CEAR */
        return ka_cear & CEAR_RD;

    case 3:                                             /* DEAR */
        return ka_dear & DEAR_RD;
        }

return 0;
}

void ka_wr (int32 pa, int32 val, int32 lnt)
{
int32 rg = (pa - KABASE) >> 2;

switch (rg) {

    case 0:                                             /* BDR */
        ka_bdr = (ka_bdr & ~BDR_WR) | (val & BDR_WR);
        break;

    case 1:                                             /* MSER */
        ka_mser = (ka_mser & ~MSER_WR) | (val & MSER_WR);
        ka_mser = ka_mser & ~(val & MSER_RS);
        break;

    case 2:                                             /* CEAR */
    case 3:                                             /* DEAR */
        break;
        }
return;
}

int32 sysd_hlt_enb (void)
{
return ka_bdr & BDR_BRKENB;
}

/* Machine check */

int32 machine_check (int32 p1, int32 opc, int32 cc, int32 delta)
{
int32 st, p2, acc;

if (in_ie) {
    in_ie = 0;
    return con_halt(CON_DBLMCK, cc);                    /* double machine check */
    }
if (p1 & 0x80)                                          /* mref? set v/p */
    p1 = p1 + mchk_ref;
p2 = mchk_va + 4;                                       /* save vap */
st = 0;
if (p1 & 0x80) {                                        /* mref? */
    cc = intexc (SCB_MCHK, cc, 0, IE_EXC);              /* take normal exception */
	if (!(ka_mser & MSER_CQPE) && !(ka_mser & MSER_CLPE))
		ka_mser |= MSER_NXM;
}
else cc = intexc (SCB_MCHK, cc, 0, IE_SVE);             /* take severe exception */
acc = ACC_MASK (KERN);                                  /* in kernel mode */
in_ie = 1;
SP = SP - 16;                                           /* push 4 words */
Write (SP, 12, L_LONG, WA);                             /* # bytes */
Write (SP + 4, p1, L_LONG, WA);                         /* mcheck type */
Write (SP + 8, p2, L_LONG, WA);                         /* address */
Write (SP + 12, st, L_LONG, WA);                        /* state */
in_ie = 0;
return cc;
}

/* Console entry */

int32 con_halt (int32 code, int32 cc)
{
int32 temp;

conisp = IS;                                            /* save ISP */
conpc = PC;                                             /* save PC */
conpsl = ((PSL | cc) & 0xFFFF00FF) | code;              /* PSL, param */
temp = (PSL >> PSL_V_CUR) & 0x7;                        /* get is'cur */
if (temp > 4)                                           /* invalid? */
    conpsl = conpsl | CON_BADPSL;
else STK[temp] = SP;                                    /* save stack */
if (mapen)                                              /* mapping on? */
    conpsl = conpsl | CON_MAPON;
mapen = 0;                                              /* turn off map */
SP = IS;                                                /* set SP from IS */
PSL = PSL_IS | PSL_IPL1F;                               /* PSL = 41F0000 */
JUMP (ROMBASE);                                         /* PC = 20040000 */
return 0;                                               /* new cc = 0 */
}


/* Special boot command - linked into SCP by initial reset

   Syntax: BOOT {CPU}

*/

t_stat vax630_boot (int32 flag, char *ptr)
{
char gbuf[CBUFSIZE];

get_glyph (ptr, gbuf, 0);                           /* get glyph */
if (gbuf[0] && strcmp (gbuf, "CPU"))
    return SCPE_ARG;                                /* Only can specify CPU device */
return run_cmd (flag, "CPU");
}


/* Bootstrap */

t_stat cpu_boot (int32 unitno, DEVICE *dptr)
{
t_stat r;

PC = ROMBASE;
PSL = PSL_IS | PSL_IPL1F;
conisp = 0;
conpc = 0;
conpsl = PSL_IS | PSL_IPL1F | CON_PWRUP;
if (rom == NULL)
    return SCPE_IERR;
if (*rom == 0) {                                        /* no boot? */
    r = cpu_load_bootcode (BOOT_CODE_FILENAME, BOOT_CODE_ARRAY, BOOT_CODE_SIZE, TRUE, 0);
    if (r != SCPE_OK)
        return r;
    }
return SCPE_OK;
}

t_stat sysd_set_diag (UNIT *uptr, int32 val, char *cptr, void *desc)
{
if (cptr != NULL) ka_diag_full = strcmp(cptr, "MIN");
return SCPE_OK;
}

t_stat sysd_show_diag (FILE *st, UNIT *uptr, int32 val, void *desc)
{
fprintf(st, "DIAG=%s", (ka_diag_full ? "full" :"min"));
return SCPE_OK;
}

t_stat sysd_set_halt (UNIT *uptr, int32 val, char *cptr, void *desc)
{
ka_hltenab = val;
return SCPE_OK;
}

t_stat sysd_show_halt (FILE *st, UNIT *uptr, int32 val, void *desc)
{
fprintf(st, "%s", ka_hltenab ? "NOAUTOBOOT" : "AUTOBOOT");
return SCPE_OK;
}

/* SYSD reset */

t_stat sysd_reset (DEVICE *dptr)
{
if (sim_switches & SWMASK ('P')) sysd_powerup ();       /* powerup? */
ka_bdr = (BDR_POK | \
    ((ka_diag_full ? BDC_NORM : BDC_SKPM) << BDR_V_BDC) | \
    (CPUC_ARB << BDR_V_CPUC) | \
    (ka_hltenab ? BDR_BRKENB : 0) | \
    0xF);
ka_mser = 0;
ka_cear = 0;
ka_dear = 0;

sim_vm_cmd = vax630_cmd;

return SCPE_OK;
}

char *sysd_description (DEVICE *dptr)
{
return "system devices";
}

/* SYSD powerup */

t_stat sysd_powerup (void)
{
ka_diag_full = 0;
return SCPE_OK;
}

t_stat cpu_print_model (FILE *st)
{
#if defined(VAX_620)
fprintf (st, "rtVAX 1000");
#else
fprintf (st, "MicroVAX II");
#endif
return SCPE_OK;
}

t_stat cpu_model_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, char *cptr)
{
fprintf (st, "Initial memory size is 16MB.\n\n");
fprintf (st, "The simulator is booted with the BOOT command:\n\n");
fprintf (st, "   sim> BOOT\n\n");
return SCPE_OK;
}