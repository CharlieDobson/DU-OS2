/*===========================================================================
 * INSTTOOL.CPP  -  Installation helper: file copy, validation, cleanup
 * Target: Open Watcom 1.9   OS/2 2.x / Warp
 *
 * The OS/2 copy of the Windows installers' helper.  Same functions, same
 * callers; the Win32 file calls underneath are replaced by their Dos*
 * equivalents:
 *
 *   CopyFile          ->  DosCopy        keeps dates, attributes and EAs
 *   FindFirstFile     ->  DosFindFirst / DosFindNext / DosFindClose
 *   SetFileAttributes ->  DosSetPathInfo (FILESTATUS3.attrFile)
 *   GetDiskFreeSpace  ->  DosQueryFSInfo (FSIL_ALLOC)
 *   _access           ->  DosQueryPathInfo
 *===========================================================================*/

#define INCL_DOSFILEMGR
#define INCL_DOSERRORS
#include <os2.h>

#include "insttool.hpp"
#include <stdlib.h>  	// _MAX_PATH, _MAX_DRIVE, _splitpath
#include <stdio.h>   	// sprintf, fopen
#include <string.h>  	// strlen, strcmp, memset
#include <ctype.h>   	// toupper
#include "iniparse.hpp"
#include "unzip.hpp"

/* DosFindFirst's attribute argument names the attributes a match MAY have.
 * Directories are left out so a wildcard never matches one. */
#define FIND_FILES  ( FILE_READONLY | FILE_HIDDEN | FILE_SYSTEM | FILE_ARCHIVED )

/* Does the path exist (file or directory)? */
static int PathExists( const char* path )
{
    FILESTATUS3 fs;

    return DosQueryPathInfo( (PCSZ)path, FIL_STANDARD, &fs, sizeof( fs ) ) == NO_ERROR;
}

/* Clear every attribute bit so a read-only, hidden or system file can be
 * deleted.  The dates go back as zero, which DosSetPathInfo reads as "leave
 * this one as it is". */
static void ClearAttributes( const char* path )
{
    FILESTATUS3 fs;

    if ( DosQueryPathInfo( (PCSZ)path, FIL_STANDARD, &fs, sizeof( fs ) ) != NO_ERROR ) {
        return;
    }
    if ( ( fs.attrFile & ( FILE_READONLY | FILE_HIDDEN | FILE_SYSTEM ) ) == 0 ) {
        return;
    }

    memset( &fs.fdateCreation,   0, sizeof( FDATE ) );
    memset( &fs.ftimeCreation,   0, sizeof( FTIME ) );
    memset( &fs.fdateLastAccess, 0, sizeof( FDATE ) );
    memset( &fs.ftimeLastAccess, 0, sizeof( FTIME ) );
    memset( &fs.fdateLastWrite,  0, sizeof( FDATE ) );
    memset( &fs.ftimeLastWrite,  0, sizeof( FTIME ) );
    fs.attrFile = 0;
    DosSetPathInfo( (PCSZ)path, FIL_STANDARD, &fs, sizeof( fs ), 0 );
}

/* Delete every file matching the wildcard pattern (directories are skipped).
 * The names come back bare, so they are rejoined to the pattern's directory
 * before deleting. */
static void DeleteMatching( const char* pattern )
{
    FILEFINDBUF3 found;
    HDIR         hdir = HDIR_CREATE;
    ULONG        count = 1;
    char         drive[_MAX_DRIVE] = { '\0' };
    char         directory[_MAX_PATH] = { '\0' };
    char         temp_buffer[_MAX_PATH] = { '\0' };

    // break the pattern up into parts so found names can be rejoined to their dir
    _splitpath( pattern, drive, directory, NULL, NULL );

    if ( DosFindFirst( (PCSZ)pattern, &hdir, FIND_FILES, &found, sizeof( found ),
                       &count, FIL_STANDARD ) != NO_ERROR ) {
        return;
    }

    do {
        if ( count == 0 ) break;

        sprintf( temp_buffer, "%s%s%s", drive, directory, found.achName );

        // reset the attributes before deleting so a read-only or hidden
        // file does not refuse
        ClearAttributes( temp_buffer );
        DosDelete( (PCSZ)temp_buffer );

        count = 1;
    } while ( DosFindNext( hdir, &found, sizeof( found ), &count ) == NO_ERROR );

    DosFindClose( hdir );
}

/* Copy Source to Destination.  DosCopy carries the dates, the attributes and
 * the extended attributes across in one call, which is more than the Windows
 * CopyFile did and far more than a read/write loop would. */
int CopyOneFile( const char* Source, const char* Destination )
{
    if ( !Source || !*Source || !Destination || !*Destination ) return 0;

    return DosCopy( (PCSZ)Source, (PCSZ)Destination, DCPY_EXISTING ) == NO_ERROR ? 1 : 0;
}

/* Remove files and directories left behind by earlier releases.  The OS/2
 * utilities have had no earlier release, so both lists are empty for now;
 * the hook is kept so the Windows installers and this one keep the same
 * shape.  Add a path or wildcard (relative to the install directory) to the
 * first list, or a directory name to the second, when something is retired. */
void RemoveObsoleteFiles( const char* Path )
{
    if ( !Path || !*Path ) return;

    static const char* deprecated_files[] = {
        NULL
    };

    static const char* deprecated_folders[] = {
        NULL
    };

    char file_buffer[_MAX_PATH] = { '\0' };

    for ( int remove_deprecated = 0; deprecated_files[remove_deprecated] != NULL; ++remove_deprecated ) {
        // construct a full path to the file or wildcard pattern
        sprintf( file_buffer, "%s\\%s", Path, deprecated_files[remove_deprecated] );
        DeleteMatching( file_buffer );
    }

    // remove deprecated folders (only an empty one goes)
    for ( int remove_folder = 0; deprecated_folders[remove_folder] != NULL; ++remove_folder ) {
        sprintf( file_buffer, "%s\\%s", Path, deprecated_folders[remove_folder] );
        DosDeleteDir( (PCSZ)file_buffer );
    }
}

void FileSearchAndRemove( const char* Path )
{
    if ( !Path || !*Path ) return;

    DeleteMatching( Path );
}

int ValidateInstall( const char* Path )
{
    if ( !Path || !*Path ) return 0;

    FILE* fileOpen = fopen( "SETUP.INF", "r" );
    if ( !fileOpen ) return 0;

    char temp_buffer[_MAX_PATH] = { '\0' };
    int inSection = 0;

    while ( fgets( temp_buffer, sizeof( temp_buffer ), fileOpen ) != NULL ) {
        // remove newline characters from the end of the line
        size_t len = strlen( temp_buffer );
        while ( len > 0 && ( temp_buffer[len - 1] == '\n' || temp_buffer[len - 1] == '\r' ) ) {
            temp_buffer[len - 1] = '\0';
            len--;
        }

        if ( strcmp( temp_buffer, "[Files]" ) == 0 ) {
            inSection = 1;
            continue;
        }

        // stop if we hit another section
        if ( inSection && temp_buffer[0] == '[' ) {
            break;
        }

        if ( inSection && temp_buffer[0] != '\0' && temp_buffer[0] != ';' ) {
        	char check_path[_MAX_PATH] = { '\0' };
            sprintf( check_path, "%s\\%s", Path, temp_buffer );
            // if file does not exist, return failure
            if ( !PathExists( check_path ) ) {
                fclose( fileOpen );
                return 0;
            }
        }
    }

    fclose( fileOpen );

    return 1;
}

int HasEnoughDiskSpace( char drive )
{
    unsigned int requiredKB = 0;
    unsigned long required = 0L;
    unsigned long freeBytes = 0L;
    FSALLOCATE fsa;

    GetIniKeyUInt( &requiredKB, "SETUP.INF", "[DiskSpace]", "required" );

    // widen to 32-bit before scaling so the byte count cannot overflow
    required = (unsigned long)requiredKB * 1024L;

    if ( DosQueryFSInfo( (ULONG)( toupper( drive ) - 'A' + 1 ), FSIL_ALLOC,
                         &fsa, sizeof( fsa ) ) == NO_ERROR ) {
        freeBytes = (unsigned long)fsa.cUnitAvail
                  * (unsigned long)fsa.cSectorUnit
                  * (unsigned long)fsa.cbSector;
    }

    return freeBytes > required;
}
