/*===========================================================================
 * INSTTOOL.HPP  -  Installation helper function declarations
 * Target: Open Watcom 1.9   OS/2 2.x / Warp
 *===========================================================================*/
// insttool.hpp : header file
//

#ifndef INSTTOOL_HPP
#define INSTTOOL_HPP

/* CopyOneFile: named to match the Windows helpers, where the plain name
 * collides with the Win32 CopyFile() API. */
int CopyOneFile( const char* Source, const char* Destination );
void RemoveObsoleteFiles( const char* Path );
void FileSearchAndRemove( const char* Path );
int ValidateInstall( const char* Path );
int HasEnoughDiskSpace( char drive );

#endif // INSTTOOL_HPP
