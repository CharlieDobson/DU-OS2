/*===========================================================================
 * ROMSRCHR.HPP  -  ROM BIOS string, date and version searches
 *
 * The OS/2 copy of DUSOURCE\REVEAL\ROMSRCHR.HPP.  Same interface; the
 * searches read LOWMEM.CPP's start-up copy of the ROM area instead of a far
 * pointer, and return empty-handed when there is no copy (no TESTCFG$).
 *
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *===========================================================================*/

#ifndef ROMSEARCHER_H
#define ROMSEARCHER_H

enum rom_search_area_t { C000 = 0, D000, E000, F800 };

void RomStringSearch( const rom_search_area_t search, const char* strings[], char* buffer, const unsigned int BufferSize );
void RomDateSearch( const rom_search_area_t search, char* buffer, const unsigned int BufferSize );
void RomVersionSearch( const rom_search_area_t search, char* buffer, const unsigned int BufferSize );

#endif // ROMSEARCHER_H
