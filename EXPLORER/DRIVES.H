/*===========================================================================
 * DRIVES.H  -  Drive enumeration and DRIVEINFO declarations for Explorer
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *===========================================================================*/

#ifndef DRIVES_H
#define DRIVES_H

/* Drive kinds.  OS/2 has no single call that answers the question, so
   EnumDrives works them out from the attached file system (see DRIVES.CPP). */
#define DRV_UNKNOWN     0
#define DRV_NOEXIST     1
#define DRV_REMOVABLE   2
#define DRV_FIXED       3
#define DRV_REMOTE      4
#define DRV_CDROM       5
#define DRV_RAMDISK     6

/* Maximum number of logical drives (A-Z) */
#define MAX_DRIVES  26

typedef struct _DRIVEINFO {
    char szLabel[6];        /* "A:\", "B:\", ... "Z:\"                     */
    char szFSType[16];      /* attached file system: "FAT", "HPFS", "CDFS" */
    int  nType;             /* one of the DRV_* constants above            */
    int  nBmpSm;            /* small view bitmap resource id  (IDB_SM_*)   */
    int  nBmpMd;            /* medium view bitmap resource id (IDB_MD_*)   */
    int  nBmpLg;            /* large view bitmap resource id  (IDB_LG_*)   */
} DRIVEINFO;

int         EnumDrives(void);
int         GetDriveCount(void);
DRIVEINFO  *GetDriveInfo(int i);
DRIVEINFO  *FindDriveInfo(char cDrive);
void        PopulatePathCombo(HWND hwndCombo);

#endif /* DRIVES_H */
