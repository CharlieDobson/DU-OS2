/*===========================================================================
 * LOWMEM.H  -  The adapter and BIOS ROM area, as seen from a PM program
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 * The Windows builds read the ROMs through a selector (Win16) or a flat
 * pointer (Win32s, where the first megabyte is mapped).  An OS/2 process has
 * neither: physical memory below 1 MB belongs to the kernel and to whichever
 * DOS session is in the foreground, and nothing at ring 3 can address it.
 *
 * What OS/2 does offer is TESTCFG.SYS - the "test configuration" driver
 * every install since 2.1 loads from CONFIG.SYS, and which the hardware
 * detection in Selective Install uses itself.  Opened as the character
 * device TESTCFG$, its IOCtl category 80h function 40h copies physical memory
 * to the caller - but only inside the adapter/BIOS window C0000h-FFFFFh,
 * which is exactly the part Reveal wants.  The interrupt vectors and the BIOS
 * data area are out of its reach, and so out of this program's.
 *
 * The whole area is copied once at start-up, 32 KB per call, and every
 * search reads the copy.  A chunk the driver refuses is marked missing and
 * LowRom returns NULL for any span that touches it; if TESTCFG$ is not there
 * at all, or the top of the ROM does not look like a BIOS, the facility
 * switches itself off and the pages say "Unknown" rather than guess.
 *
 * UNVERIFIED: the parameter packet of IOCtl 80h/40h is taken from the
 * published uses of it (dmidecode's OS/2 port among them), not from an IBM
 * document, and has not been run here - there is no OS/2 machine on this
 * side.  If it is wrong the driver fails the call and Reveal degrades as
 * above; nothing is written anywhere.
 *===========================================================================*/

#ifndef LOWMEM_H
#define LOWMEM_H 1

#define LOWMEM_ROM_BASE     0x000C0000UL
#define LOWMEM_ROM_SIZE     0x00040000UL    /* C0000h..FFFFFh, 256 KB       */

BOOL        LowMemInit( void );             /* FALSE: ROM area unavailable  */
void        LowMemTerm( void );
BOOL        LowMemAvailable( void );

/* The start-up copy of the ROM area: a pointer to the byte that was at
   physical address 'linear', valid for 'len' bytes, or NULL if that span is
   outside C0000h-FFFFFh, was refused by the driver, or was never copied. */
const BYTE* LowRom( ULONG linear, ULONG len );

#endif /* LOWMEM_H */
