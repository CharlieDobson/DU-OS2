/*===========================================================================
 * INIPARSE.HPP  -  Procedural INI file read/write helpers
 * Target: Open Watcom 1.9   OS/2 2.x / Warp
 *===========================================================================*/
// iniparse.hpp : header file for INIPARSE.CPP

#ifndef INIPARSE_HPP
#define INIPARSE_HPP

void GetIniKeyInt( int* Buffer, const char* FileName, const char* Section, const char* Key );
void GetIniKeyUInt( unsigned int* Buffer, const char* FileName, const char* Section, const char* Key );
void GetIniKeyString( char* Buffer, const char* FileName, const char* Section, const char* Key );
int SetIniKeyInt( const char* FileName, const char* Section, const char* Key, const int Value );
int SetIniKeyString( const char* FileName, const char* Section, const char* Key, const char* Value );

#endif
