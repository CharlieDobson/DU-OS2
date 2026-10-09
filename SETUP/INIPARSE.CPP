/*===========================================================================
 * INIPARSE.CPP  -  Procedural INI file read/write helpers
 * Target: Open Watcom 1.9   OS/2 2.x / Warp
 *===========================================================================*/
// iniparse.cpp : implementation file for Iniparse

#include "iniparse.hpp"
#include <stdio.h>   	// sprintf, FILE
#include <stdlib.h>  	// atoi, _MAX_PATH
#include <string.h>  	// strlen, strchr, strcpy
#include <io.h>      	// (file helpers)

/*---------------------------------------------------------------------------
 * GetIniKeyInt / GetIniKeyUInt
 * Read an integer value from [Section] Key in FileName into Buffer.
 *---------------------------------------------------------------------------*/
void GetIniKeyInt( int* Buffer, const char* FileName, const char* Section, const char* Key )
{
	if ( !Buffer ) return;

	// initialize buffer
	*Buffer = -1;

    if ( !FileName || !*FileName || !Section || !*Section || !Key || !*Key ) {
        return;
    }

    FILE* fileOpen = fopen( FileName, "r" );
    if ( !fileOpen ) {
        return;
    }

    char temp_buffer[_MAX_PATH] = { '\0' };
    int inSection = 0;

    while ( fgets( temp_buffer, sizeof( temp_buffer ), fileOpen ) != NULL ) {
        // remove newline characters from the end of the line
        size_t len = strlen( temp_buffer );
        if ( len > 0 && ( temp_buffer[len - 1] == '\n' || temp_buffer[len - 1] == '\r' ) ) {
            temp_buffer[len - 1] = '\0';
            len--;
        }

        if ( _stricmp( temp_buffer, Section ) == 0 ) {
            inSection = 1;
            continue;
        }

        // stop if we hit another section
        if ( inSection && temp_buffer[0] == '[' ) {
            break;
        }

        if ( inSection && temp_buffer[0] != '\0' && temp_buffer[0] != ';' ) {
            char* equalsPos = strchr( temp_buffer, '=' );

            if ( NULL != equalsPos ) {
                *equalsPos = '\0'; // split the string
                if ( _stricmp( temp_buffer, Key ) == 0 ) {
                    *Buffer = atoi( equalsPos + 1 );
                    break;
                }
            }
        }
    }

    fclose( fileOpen );
}

void GetIniKeyUInt( unsigned int* Buffer, const char* FileName, const char* Section, const char* Key )
{
	if ( !Buffer ) return;

	// initialize buffer
	*Buffer = 0;

    if ( !FileName || !*FileName || !Section || !*Section || !Key || !*Key ) {
        return;
    }

    FILE* fileOpen = fopen( FileName, "r" );
    if ( !fileOpen ) {
        return;
    }

    char temp_buffer[_MAX_PATH] = { '\0' };
    int inSection = 0;

    while ( fgets( temp_buffer, sizeof( temp_buffer ), fileOpen ) != NULL ) {
        // remove newline characters from the end of the line
        size_t len = strlen( temp_buffer );
        if ( len > 0 && ( temp_buffer[len - 1] == '\n' || temp_buffer[len - 1] == '\r' ) ) {
            temp_buffer[len - 1] = '\0';
            len--;
        }

        if ( _stricmp( temp_buffer, Section ) == 0 ) {
            inSection = 1;
            continue;
        }

        // stop if we hit another section
        if ( inSection && temp_buffer[0] == '[' ) {
            break;
        }

        if ( inSection && temp_buffer[0] != '\0' && temp_buffer[0] != ';' ) {
            char* equalsPos = strchr( temp_buffer, '=' );

            if ( NULL != equalsPos ) {
                *equalsPos = '\0'; // split the string
                if ( _stricmp( temp_buffer, Key ) == 0 ) {
                    *Buffer = (unsigned int)atoi( equalsPos + 1 );
                    break;
                }
            }
        }
    }

    fclose( fileOpen );
}

/*---------------------------------------------------------------------------
 * GetIniKeyString
 * Read a string value from [Section] Key in FileName into Buffer.
 *---------------------------------------------------------------------------*/
void GetIniKeyString( char* Buffer, const char* FileName, const char* Section, const char* Key )
{
    if ( !Buffer ) return;
    if ( !FileName || !*FileName || !Section || !*Section || !Key || !*Key ) {
        Buffer[0] = '\0';
        return;
    }

    // initialize return buffer to empty string
    Buffer[0] = '\0';

    FILE* fileOpen = fopen( FileName, "r" );
    if ( !fileOpen ) {
        return;
    }

    char temp_buffer[_MAX_PATH] = { '\0' };
    int inSection = 0;

    while ( fgets( temp_buffer, sizeof( temp_buffer ), fileOpen ) != NULL ) {
        // remove newline characters from the end of the line
        size_t len = strlen( temp_buffer );
        if ( len > 0 && ( temp_buffer[len - 1] == '\n' || temp_buffer[len - 1] == '\r' ) ) {
            temp_buffer[len - 1] = '\0';
            len--;
        }

        if ( _stricmp( temp_buffer, Section ) == 0 ) {
            inSection = 1;
            continue;
        }

        // stop if we hit another section
        if ( inSection && temp_buffer[0] == '[' ) {
            break;
        }

        if ( inSection && temp_buffer[0] != '\0' && temp_buffer[0] != ';' ) {
            char* equalsPos = strchr( temp_buffer, '=' );
            if ( NULL != equalsPos ) {
                *equalsPos = '\0'; // split the string
                if ( _stricmp( temp_buffer, Key ) == 0 ) {
                    strcpy( Buffer, equalsPos + 1 );
                    break;
                }
            }
        }
    }

    fclose( fileOpen );
}

/*---------------------------------------------------------------------------
 * SetIniKeyInt / SetIniKeyString
 * Overwrite an existing Key in [Section] with the given Value.
 * Returns 1 on success, 0 if the key was not found.
 *---------------------------------------------------------------------------*/
int SetIniKeyInt( const char* FileName, const char* Section, const char* Key, const int Value )
{
    if ( !FileName || !*FileName || !Section || !*Section || !Key || !*Key ) {
        return 0;
    }

    FILE* filenopen = fopen( FileName, "r+" );
    // if file cannot be opened, return 0 for FALSE
    if ( !filenopen ) {
        return 0;
    }

    char temp_buffer[_MAX_PATH] = { '\0' };
    int inSection = 0;

    while ( fgets( temp_buffer, sizeof( temp_buffer ), filenopen ) != NULL ) {
        // remove newline characters from the end of the line
        size_t len = strlen( temp_buffer );
        if ( len > 0 && ( temp_buffer[len - 1] == '\n' || temp_buffer[len - 1] == '\r' ) ) {
            temp_buffer[len - 1] = '\0';
            len--;
        }

        if ( _stricmp( temp_buffer, Section ) == 0 ) {
            inSection = 1;
            continue;
        }

        // stop if we hit another section
        if ( inSection && temp_buffer[0] == '[' ) {
            break;
        }

        if ( inSection && temp_buffer[0] != '\0' && temp_buffer[0] != ';' ) {
            char* equalsPos = strchr( temp_buffer, '=' );
            if ( NULL != equalsPos ) {
                *equalsPos = '\0'; // split the string
                if ( _stricmp( temp_buffer, Key ) == 0 ) {
                    // move file pointer back to start of this line
                    fseek( filenopen, -((long)len + 2), SEEK_CUR );
                    // write new value
                    fprintf( filenopen, "%s=%d\n", Key, Value );
                    fclose( filenopen );
                    return 1; // TRUE
                }
            }
        }
    }

    fclose( filenopen );

    return 0; // FALSE
}

int SetIniKeyString( const char* FileName, const char* Section, const char* Key, const char* Value )
{
    if ( !FileName || !*FileName || !Section || !*Section || !Key || !*Key || !Value ) {
        return 0;
    }

    FILE* filenopen = fopen( FileName, "r+" );
    // if file cannot be opened, return 0 for FALSE
    if ( !filenopen ) {
        return 0;
    }

    char temp_buffer[_MAX_PATH] = { '\0' };
    int inSection = 0;

    while ( fgets( temp_buffer, sizeof( temp_buffer ), filenopen ) != NULL ) {
        // remove newline characters from the end of the line
        size_t len = strlen( temp_buffer );
        if ( len > 0 && ( temp_buffer[len - 1] == '\n' || temp_buffer[len - 1] == '\r' ) ) {
            temp_buffer[len - 1] = '\0';
            len--;
        }

        if ( _stricmp( temp_buffer, Section ) == 0 ) {
            inSection = 1;
            continue;
        }

        // stop if we hit another section
        if ( inSection && temp_buffer[0] == '[' ) {
            break;
        }

        if ( inSection && temp_buffer[0] != '\0' && temp_buffer[0] != ';' ) {
            char* equalsPos = strchr( temp_buffer, '=' );
            if ( NULL != equalsPos ) {
                *equalsPos = '\0'; // split the string
                if ( _stricmp( temp_buffer, Key ) == 0 ) {
                    // move file pointer back to start of this line
                    fseek( filenopen, -((long)len + 2), SEEK_CUR );
                    // write new value
                    fprintf( filenopen, "%s=%s\n", Key, Value );
                    fclose( filenopen );
                    return 1; // TRUE
                }
            }
        }
    }

    fclose( filenopen );
    return 0; // FALSE
}
