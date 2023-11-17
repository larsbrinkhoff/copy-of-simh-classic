/*  altairz80_hdsk.c: simulated hard disk device to increase capacity

    Copyright (c) 2002-2007, Peter Schorn

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
    PETER SCHORN BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
    IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
    CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

    Except as contained in this notice, the name of Peter Schorn shall not
    be used in advertising or otherwise to promote the sale, use or other dealings
    in this Software without prior written authorization from Peter Schorn.
    
    Contains code from Howard M. Harte for defining and changing disk geometry.
*/

#include "altairz80_defs.h"

/* the following routines provided courtesy of Howard M. Harte */
t_stat set_geom (UNIT *uptr, int32 val, char *cptr, void *desc);
t_stat show_geom (FILE *st, UNIT *uptr, int32 val, void *desc);
t_stat set_format (UNIT *uptr, int32 val, char *cptr, void *desc);
t_stat show_format (FILE *st, UNIT *uptr, int32 val, void *desc);
t_stat hdsk_attach (UNIT *uptr, char *cptr);

#define UNIT_V_HDSKWLK          (UNIT_V_UF + 0) /* write locked                             */
#define UNIT_HDSKWLK            (1 << UNIT_V_HDSKWLK)
#define UNIT_V_HDSK_VERBOSE     (UNIT_V_UF + 1) /* verbose mode, i.e. show error messages   */
#define UNIT_HDSK_VERBOSE       (1 << UNIT_V_HDSK_VERBOSE)
#define HDSK_MAX_SECTOR_SIZE    1024            /* size of sector                           */
#define HDSK_SECTOR_SIZE        u5              /* size of sector                           */
#define HDSK_SECTORS_PER_TRACK  u4              /* sectors per track                        */
#define HDSK_MAX_TRACKS         u3              /* number of tracks                         */
#define HDSK_FORMAT_TYPE        u6              /* Disk Format Type                         */
#define HDSK_CAPACITY           (2048*32*128)   /* Default Altair HDSK Capacity             */
#define HDSK_NUMBER             8               /* number of hard disks                     */
#define CPM_OK                  0               /* indicates to CP/M everything ok          */
#define CPM_ERROR               1               /* indicates to CP/M an error condition     */
#define CPM_EMPTY               0xe5            /* default value for non-existing bytes     */
#define HDSK_NONE               0
#define HDSK_RESET              1
#define HDSK_READ               2
#define HDSK_WRITE              3
#define HDSK_PARAM              4
#define HDSK_BOOT_ADDRESS       0x5c00

extern char messageBuffer[];
extern int32 PCX;
extern UNIT cpu_unit;
extern uint8 M[MAXMEMSIZE][MAXBANKS];
extern int32 saved_PC;

extern int32 install_bootrom(void);
extern void printMessage(void);
extern void PutBYTEBasic(const uint32 Addr, const uint32 Bank, const uint32 Value);
extern void PutBYTEWrapper(const uint32 Addr, const uint32 Value);
extern void protect(const int32 l, const int32 h);
extern uint8 GetBYTEWrapper(const uint32 Addr);
extern int32 bootrom[BOOTROM_SIZE];

static t_stat hdsk_boot(int32 unitno, DEVICE *dptr);
static int32 hdsk_hasVerbose(void);
int32 hdsk_io(const int32 port, const int32 io, const int32 data);
static int32 hdsk_in(const int32 port);
static int32 hdsk_out(const int32 data);
static int32 checkParameters(void);
static int32 doSeek(void);
static int32 doRead(void);
static int32 doWrite(void);

static int32 hdskLastCommand        = HDSK_NONE;
static int32 hdskCommandPosition    = 0;
static int32 paramcount             = 0;
static int32 selectedDisk;
static int32 selectedSector;
static int32 selectedTrack;
static int32 selectedDMA;
static int32 hdskTrace;

typedef struct {
    char    name[16];
    t_addr  capac;
    uint16  spt;
    uint8   bsh;
    uint8   blm;
    uint8   exm;
    uint16  dsm;
    uint16  drm;
    uint8   al0;
    uint8   al1;
    uint16  cks;
    uint16  off;
    uint8   psh;
    uint8   phm;
} DPB;

static DPB dpb[] = {
/*    NAME      CAPAC           SPT BSH   BLM   EXM   DSM     DRM     AL0   AL1   CKS     OFF     PSH   PHM */
    { "HDSK",   HDSK_CAPACITY,  32, 0x05, 0x1F, 0x01, 0x07f9, 0x03FF, 0xFF, 0x00, 0x8000, 0x0006, 0x00, 0x00 }, /* AZ80 HDSK */
    { "EZ80FL", 131072,         32, 0x03, 0x07, 0x00, 127,    0x003E, 0xC0, 0x00, 0x0000, 0x0000, 0x02, 0x03 }, /* 128K FLASH */
    { "P112",   1474560,        72, 0x04, 0x0F, 0x00, 710,    0x00FE, 0xF0, 0x00, 0x0000, 0x0002, 0x02, 0x03 }, /* 1.44M P112 */
    { "SU720",  737280,         36, 0x04, 0x0F, 0x00, 354,    0x007E, 0xC0, 0x00, 0x0020, 0x0002, 0x02, 0x03 }, /* 720K Super I/O */
    { "", 0 }
};

static UNIT hdsk_unit[] = {
    { UDATA (NULL, UNIT_FIX + UNIT_ATTABLE + UNIT_DISABLE + UNIT_ROABLE, HDSK_CAPACITY) },
    { UDATA (NULL, UNIT_FIX + UNIT_ATTABLE + UNIT_DISABLE + UNIT_ROABLE, HDSK_CAPACITY) },
    { UDATA (NULL, UNIT_FIX + UNIT_ATTABLE + UNIT_DISABLE + UNIT_ROABLE, HDSK_CAPACITY) },
    { UDATA (NULL, UNIT_FIX + UNIT_ATTABLE + UNIT_DISABLE + UNIT_ROABLE, HDSK_CAPACITY) },
    { UDATA (NULL, UNIT_FIX + UNIT_ATTABLE + UNIT_DISABLE + UNIT_ROABLE, HDSK_CAPACITY) },
    { UDATA (NULL, UNIT_FIX + UNIT_ATTABLE + UNIT_DISABLE + UNIT_ROABLE, HDSK_CAPACITY) },
    { UDATA (NULL, UNIT_FIX + UNIT_ATTABLE + UNIT_DISABLE + UNIT_ROABLE, HDSK_CAPACITY) },
    { UDATA (NULL, UNIT_FIX + UNIT_ATTABLE + UNIT_DISABLE + UNIT_ROABLE, HDSK_CAPACITY) }
};

static REG hdsk_reg[] = {
    { DRDATA (HDCMD,    hdskLastCommand,        32),    REG_RO  },
    { DRDATA (HDPOS,    hdskCommandPosition,    32),    REG_RO  },
    { DRDATA (HDDSK,    selectedDisk,           32),    REG_RO  },
    { DRDATA (HDSEC,    selectedSector,         32),    REG_RO  },
    { DRDATA (HDTRK,    selectedTrack,          32),    REG_RO  },
    { DRDATA (HDDMA,    selectedDMA,            32),    REG_RO  },
    { DRDATA (HDTRACE,  hdskTrace,              8),             },
    { NULL }
};

static MTAB hdsk_mod[] = {
    { MTAB_XTD|MTAB_VUN|MTAB_VAL,   0,                  "FORMAT",   "FORMAT", &set_format, &show_format, NULL },
    { UNIT_HDSKWLK,                 0,                  "WRTENB",   "WRTENB", NULL    },
    { UNIT_HDSKWLK,                 UNIT_HDSKWLK,       "WRTLCK",   "WRTLCK", NULL    },
    /* quiet, no warning messages       */
    { UNIT_HDSK_VERBOSE,            0,                  "QUIET",    "QUIET", NULL    },
    /* verbose, show warning messages   */
    { UNIT_HDSK_VERBOSE,            UNIT_HDSK_VERBOSE,  "VERBOSE",  "VERBOSE", NULL    },
    { MTAB_XTD|MTAB_VUN|MTAB_VAL,   0,                  "GEOM",     "GEOM", &set_geom, &show_geom, NULL },
    { 0 }
};

DEVICE hdsk_dev = {
    "HDSK", hdsk_unit, hdsk_reg, hdsk_mod,
    8, 10, 31, 1, 8, 8,
    NULL, NULL, NULL,
    &hdsk_boot, &hdsk_attach, NULL,
    NULL, 0, 0,
    NULL, NULL, NULL
};

/* Attach routine */
t_stat hdsk_attach (UNIT *uptr, char *cptr) {
    uint32 flen;
    t_stat r;
    int32 i;

    r = attach_unit (uptr, cptr);                           /* attach unit */
    if (r != SCPE_OK) return r;                             /* error? */
    if ((flen = sim_fsize (uptr->fileref)) == 0) {          /* new disk image? */
        if (uptr->flags & UNIT_RO) return SCPE_OK;          /* if ro, done */
        return SCPE_OK;
    }

    if(flen>0) {
        uptr->capac = flen;

        uptr->HDSK_FORMAT_TYPE = -1;                        /* Default to unknown format type */

        for(i=0; dpb[i].spt != 0; i++) {
            if(dpb[i].capac == uptr->capac) {
                uptr->HDSK_FORMAT_TYPE = i;
                break;
            }
        }
    }

    if(uptr->HDSK_FORMAT_TYPE == -1) {
        uptr->HDSK_FORMAT_TYPE = 0;
        uptr->capac = dpb[uptr->HDSK_FORMAT_TYPE].capac;
        printf("HDSK: WARNING: Unsupported disk capacity, assuming HDSK type.\n");
        uptr->flags |= UNIT_HDSKWLK;
        printf("HDSK: WARNING: Forcing WRTLCK.\n");
        if(uptr->capac != (uptr->HDSK_MAX_TRACKS * uptr->HDSK_SECTORS_PER_TRACK * uptr->HDSK_SECTOR_SIZE)) {
            printf("HDSK: WARNING: Geometry may be incorrect.\n");
        }
    }

    uptr->HDSK_SECTOR_SIZE = (128 << dpb[uptr->HDSK_FORMAT_TYPE].psh);
    uptr->HDSK_SECTORS_PER_TRACK = dpb[uptr->HDSK_FORMAT_TYPE].spt >> dpb[uptr->HDSK_FORMAT_TYPE].psh;
    uptr->HDSK_MAX_TRACKS = uptr->capac / (uptr->HDSK_SECTORS_PER_TRACK * uptr->HDSK_SECTOR_SIZE);

    return SCPE_OK;
}

/* Set disk geometry routine */
t_stat set_geom (UNIT *uptr, int32 val, char *cptr, void *desc) {
    DEVICE *dptr;

    uint32 ncyl, nsect, ssize;

    if (cptr == NULL) return SCPE_ARG;
    if (uptr == NULL) return SCPE_IERR;
    dptr = find_dev_from_unit (uptr);
    if (dptr == NULL) return SCPE_IERR;

    sscanf(cptr, "%d/%d/%d", &ncyl, &nsect, &ssize);
    uptr->HDSK_MAX_TRACKS = ncyl;
    uptr->HDSK_SECTORS_PER_TRACK = nsect;
    uptr->HDSK_SECTOR_SIZE = ssize;

    return SCPE_OK;
}

/* Show disk geometry routine */
t_stat show_geom (FILE *st, UNIT *uptr, int32 val, void *desc) {
    DEVICE *dptr;
    if (uptr == NULL) return SCPE_IERR;
    dptr = find_dev_from_unit (uptr);
    if (dptr == NULL) return SCPE_IERR;
    fprintf (st, "T:%d/N:%d/S:%d", uptr->HDSK_MAX_TRACKS, uptr->HDSK_SECTORS_PER_TRACK, uptr->u5);

    return SCPE_OK;
}

/* Set disk format routine */
t_stat set_format (UNIT *uptr, int32 val, char *cptr, void *desc) {
    DEVICE *dptr;

    char fmtname[16];
    int32 i;

    if (cptr == NULL) return SCPE_ARG;
    if (uptr == NULL) return SCPE_IERR;
    dptr = find_dev_from_unit (uptr);
    if (dptr == NULL) return SCPE_IERR;

    sscanf(cptr, "%s", fmtname);

    for(i=0;dpb[i].spt != 0;i++) {
        if(!strncmp(fmtname, dpb[i].name, strlen(fmtname))) {
            uptr->HDSK_FORMAT_TYPE = i;
            uptr->capac = dpb[i].capac; /* Set capacity */

            /* Configure physical disk geometry */
            uptr->HDSK_SECTOR_SIZE = (128 << dpb[uptr->HDSK_FORMAT_TYPE].psh);
            uptr->HDSK_SECTORS_PER_TRACK = dpb[uptr->HDSK_FORMAT_TYPE].spt >> dpb[uptr->HDSK_FORMAT_TYPE].psh;
            uptr->HDSK_MAX_TRACKS = uptr->capac / (uptr->HDSK_SECTORS_PER_TRACK * uptr->HDSK_SECTOR_SIZE);

            return SCPE_OK;
        }
    }
    return SCPE_ARG;
}

/* Show disk format routine */
t_stat show_format (FILE *st, UNIT *uptr, int32 val, void *desc) {
    DEVICE *dptr;
    if (uptr == NULL) return SCPE_IERR;
    dptr = find_dev_from_unit (uptr);
    if (dptr == NULL) return SCPE_IERR;
    fprintf (st, "%s", dpb[uptr->HDSK_FORMAT_TYPE].name);

    return SCPE_OK;
}

static const int32 hdskBoot[BOOTROM_SIZE] = {
    0xf3, 0x06, 0x80, 0x3e, 0x0e, 0xd3, 0xfe, 0x05, /* 5c00-5c07 */
    0xc2, 0x05, 0x5c, 0x3e, 0x16, 0xd3, 0xfe, 0x3e, /* 5c08-5c0f */
    0x12, 0xd3, 0xfe, 0xdb, 0xfe, 0xb7, 0xca, 0x20, /* 5c10-5c17 */
    0x5c, 0x3e, 0x0c, 0xd3, 0xfe, 0xaf, 0xd3, 0xfe, /* 5c18-5c1f */
    0x06, 0x20, 0x3e, 0x01, 0xd3, 0xfd, 0x05, 0xc2, /* 5c20-5c27 */
    0x24, 0x5c, 0x11, 0x08, 0x00, 0x21, 0x00, 0x00, /* 5c28-5c2f */
    0x0e, 0xb8, 0x3e, 0x02, 0xd3, 0xfd, 0x3a, 0x37, /* 5c30-5c37 */
    0xff, 0xd6, 0x08, 0xd3, 0xfd, 0x7b, 0xd3, 0xfd, /* 5c38-5c3f */
    0x7a, 0xd3, 0xfd, 0xaf, 0xd3, 0xfd, 0x7d, 0xd3, /* 5c40-5c47 */
    0xfd, 0x7c, 0xd3, 0xfd, 0xdb, 0xfd, 0xb7, 0xca, /* 5c48-5c4f */
    0x53, 0x5c, 0x76, 0x79, 0x0e, 0x80, 0x09, 0x4f, /* 5c50-5c57 */
    0x0d, 0xc2, 0x60, 0x5c, 0xfb, 0xc3, 0x00, 0x00, /* 5c58-5c5f */
    0x1c, 0x1c, 0x7b, 0xfe, 0x20, 0xca, 0x73, 0x5c, /* 5c60-5c67 */
    0xfe, 0x21, 0xc2, 0x32, 0x5c, 0x1e, 0x00, 0x14, /* 5c68-5c6f */
    0xc3, 0x32, 0x5c, 0x1e, 0x01, 0xc3, 0x32, 0x5c, /* 5c70-5c77 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 5c78-5c7f */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 5c80-5c87 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 5c88-5c8f */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 5c90-5c97 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 5c98-5c9f */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 5ca0-5ca7 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 5ca8-5caf */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 5cb0-5cb7 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 5cb8-5cbf */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 5cc0-5cc7 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 5cc8-5ccf */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 5cd0-5cd7 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 5cd8-5cdf */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 5ce0-5ce7 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 5ce8-5cef */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 5cf0-5cf7 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 5cf8-5cff */
};

static t_stat hdsk_boot(int32 unitno, DEVICE *dptr) {
    int32 i;
    if (MEMSIZE < 24*KB) {
        printf("Need at least 24KB RAM to boot from hard disk.\n");
        return SCPE_ARG;
    }
    if (cpu_unit.flags & (UNIT_ALTAIRROM | UNIT_BANKED)) {
        if (install_bootrom()) {
            printf("ALTAIR boot ROM installed.\n");
        }
        /* check whether we are really modifying an LD A,<> instruction */
        if (bootrom[UNIT_NO_OFFSET_1 - 1] == LDA_INSTRUCTION) {
            bootrom[UNIT_NO_OFFSET_1] = (unitno + NUM_OF_DSK) & 0xff;  /* LD A,<unitno> */
        }
        else { /* Attempt to modify non LD A,<> instructions is refused. */
            printf("Incorrect boot ROM offset detected.\n");
            return SCPE_IERR;
        }
    }
    for (i = 0; i < BOOTROM_SIZE; i++) {
        PutBYTEBasic(i + HDSK_BOOT_ADDRESS, 0, hdskBoot[i] & 0xff);
    }
    saved_PC = HDSK_BOOT_ADDRESS;
    protect(HDSK_BOOT_ADDRESS, HDSK_BOOT_ADDRESS + BOOTROM_SIZE - 1);
    return SCPE_OK;
}

/* returns TRUE iff there exists a disk with VERBOSE */
static int32 hdsk_hasVerbose(void) {
    int32 i;
    for (i = 0; i < HDSK_NUMBER; i++) {
        if (((hdsk_dev.units + i) -> flags) & UNIT_HDSK_VERBOSE) {
            return TRUE;
        }
    }
    return FALSE;
}

/*  The hard disk port is 0xfd. It understands the following commands.

    1.  Reset
        ld  b,32
        ld  a,HDSK_RESET
    l:  out (0fdh),a
        dec b
        jp  nz,l

    2.  Read / write
        ; parameter block
        cmd:        db  HDSK_READ or HDSK_WRITE
        hd:         db  0   ; 0 .. 7, defines hard disk to be used
        sector: db  0       ; 0 .. 31, defines sector
        track:  dw  0       ; 0 .. 2047, defines track
        dma:        dw  0   ; defines where result is placed in memory

        ; routine to execute
        ld  b,7             ; size of parameter block
        ld  hl,cmd          ; start address of parameter block
    l:  ld  a,(hl)          ; get byte of parameter block
        out (0fdh),a        ; send it to port
        inc hl              ; point to next byte
        dec b               ; decrement counter
        jp  nz,l            ; again, if not done
        in  a,(0fdh)        ; get result code

    3.  Retrieve Disk Parameters from controller (Howard M. Harte)
        Reads a 19-byte parameter block from the disk controller.
        This parameter block is in CP/M DPB format for the first 17 bytes,
        and the last two bytes are the lsb/msb of the disk's physical
        sector size.

        ; routine to execute
        ld   a,hdskParam    ; hdskParam = 4
        out  (hdskPort),a   ; Send 'get parameters' command, hdskPort = 0fdh
        ld   a,(diskno)
        out  (hdskPort),a   ; Send selected HDSK number
        ld   b,17
    1:  in   a,(hdskPort)   ; Read 17-bytes of DPB
        ld   (hl), a
        inc  hl
        djnz 1
        in   a,(hdskPort)   ; Read LSB of disk's physical sector size.
        ld   (hsecsiz), a
        in   a,(hdskPort)   ; Read MSB of disk's physical sector size.
        ld   (hsecsiz+1), a

*/

/* check the parameters and return TRUE iff parameters are correct or have been repaired */
static int32 checkParameters(void) {
    UNIT *uptr = hdsk_dev.units + selectedDisk;
    int32 currentFlag;
    if ((selectedDisk < 0) || (selectedDisk >= HDSK_NUMBER)) {
        if (hdsk_hasVerbose()) {
            MESSAGE_2("HDSK%d does not exist, will use HDSK0 instead.", selectedDisk);
        }
        selectedDisk = 0;
    }
    currentFlag = (hdsk_dev.units + selectedDisk) -> flags;
    if ((currentFlag & UNIT_ATT) == 0) {
        if (currentFlag & UNIT_HDSK_VERBOSE) {
            MESSAGE_2("HDSK%d is not attached.", selectedDisk);
        }
        return FALSE; /* cannot read or write */
    }
    if ((selectedSector < 0) || (selectedSector >= uptr->HDSK_SECTORS_PER_TRACK)) {
        if (currentFlag & UNIT_HDSK_VERBOSE) {
            MESSAGE_4("HDSK%d: 0 <= Sector=%02d < %d violated, will use 0 instead.",
                selectedDisk, selectedSector, uptr->HDSK_SECTORS_PER_TRACK);
        }
        selectedSector = 0;
    }
    if ((selectedTrack < 0) || (selectedTrack >= uptr->HDSK_MAX_TRACKS)) {
        if (currentFlag & UNIT_HDSK_VERBOSE) {
            MESSAGE_4("HDSK%d: 0 <= Track=%04d < %04d violated, will use 0 instead.",
                selectedDisk, selectedTrack, uptr->HDSK_MAX_TRACKS);
        }
        selectedTrack = 0;
    }
    selectedDMA &= ADDRMASK;
    if (hdskTrace) {
        MESSAGE_7("%s HDSK%d Track=%04d Sector=%02d Len=%04d DMA=%04x\n",
            (hdskLastCommand == HDSK_READ) ? "Read" : "Write",
            selectedDisk, selectedTrack, selectedSector, uptr->HDSK_SECTOR_SIZE, selectedDMA);
    }
    return TRUE;
}

static int32 doSeek(void) {
    UNIT *uptr = hdsk_dev.units + selectedDisk;
    if (fseek(uptr -> fileref,
        (uptr->HDSK_SECTORS_PER_TRACK * uptr->HDSK_SECTOR_SIZE) * selectedTrack + (uptr->HDSK_SECTOR_SIZE * selectedSector), SEEK_SET)) {
        if ((uptr -> flags) & UNIT_HDSK_VERBOSE) {
            MESSAGE_4("Could not access HDSK%d Sector=%02d Track=%04d.",
                selectedDisk, selectedSector, selectedTrack);
        }
        return CPM_ERROR;
    }
    else {
        return CPM_OK;
    }
}

uint8 hdskbuf[HDSK_MAX_SECTOR_SIZE];    /* data buffer  */

static int32 doRead(void) {
    int32 i;
    UNIT *uptr = hdsk_dev.units + selectedDisk;
    if (doSeek()) {
        return CPM_ERROR;
    }

    if (fread(hdskbuf, uptr->HDSK_SECTOR_SIZE, 1, uptr -> fileref) != 1) {
        for (i = 0; i < uptr->HDSK_SECTOR_SIZE; i++) {
            hdskbuf[i] = CPM_EMPTY;
        }
        if ((uptr -> flags) & UNIT_HDSK_VERBOSE) {
            MESSAGE_4("Could not read HDSK%d Sector=%02d Track=%04d.",
                selectedDisk, selectedSector, selectedTrack);
        }
        return CPM_OK; /* allows the creation of empty hard disks */
    }
    for (i = 0; i < uptr->HDSK_SECTOR_SIZE; i++) {
        PutBYTEWrapper(selectedDMA + i, hdskbuf[i]);
    }
    return CPM_OK;
}

static int32 doWrite(void) {
    int32 i;

    UNIT *uptr = hdsk_dev.units + selectedDisk;
    if (((uptr -> flags) & UNIT_HDSKWLK) == 0) { /* write enabled */
        if (doSeek()) {
            return CPM_ERROR;
        }
        for (i = 0; i < uptr->HDSK_SECTOR_SIZE; i++) {
            hdskbuf[i] = GetBYTEWrapper(selectedDMA + i);
        }
        if (fwrite(hdskbuf, uptr->HDSK_SECTOR_SIZE, 1, uptr -> fileref) != 1) {
            if ((uptr -> flags) & UNIT_HDSK_VERBOSE) {
                MESSAGE_4("Could not write HDSK%d Sector=%02d Track=%04d.",
                    selectedDisk, selectedSector, selectedTrack);
            }
            return CPM_ERROR;
        }
    }
    else {
        if ((uptr -> flags) & UNIT_HDSK_VERBOSE) {
            MESSAGE_4("Could not write to locked HDSK%d Sector=%02d Track=%04d.",
                selectedDisk, selectedSector, selectedTrack);
        }
        return CPM_ERROR;
    }
    return CPM_OK;
}

static int32 hdsk_in(const int32 port) {
    UNIT *uptr = hdsk_dev.units + selectedDisk;

    int32 result;
    if ((hdskCommandPosition == 6) && ((hdskLastCommand == HDSK_READ) || (hdskLastCommand == HDSK_WRITE))) {
        result = checkParameters() ? ((hdskLastCommand == HDSK_READ) ? doRead() : doWrite()) : CPM_ERROR;
        hdskLastCommand = HDSK_NONE;
        hdskCommandPosition = 0;
        return result;
    } else if (hdskLastCommand == HDSK_PARAM) {
        DPB current = dpb[uptr->HDSK_FORMAT_TYPE];
        uint8 params[17];
        params[ 0] =  current.spt & 0xff; params[ 1] = (current.spt >> 8) & 0xff;
        params[ 2] = current.bsh;
        params[ 3] = current.blm;
        params[ 4] = current.exm;
        params[ 5] = current.dsm & 0xff; params[ 6] = (current.dsm >> 8) & 0xff;
        params[ 7] = current.drm & 0xff; params[ 8] = (current.drm >> 8) & 0xff;
        params[ 9] = current.al0;
        params[10] = current.al1;
        params[11] = current.cks & 0xff; params[12] = (current.cks >> 8) & 0xff;
        params[13] = current.off & 0xff; params[14] = (current.off >> 8) & 0xff;
        params[15] = current.psh;
        params[16] = current.phm;
        if (++paramcount >= 19) {
            hdskLastCommand = HDSK_NONE;
        }
        if (paramcount <= 17)
            return params[paramcount - 1];
        else if (paramcount == 18)
            return (uptr->HDSK_SECTOR_SIZE & 0xff);
        else if (paramcount == 19)
            return (uptr->HDSK_SECTOR_SIZE >> 8);
        else
            MESSAGE_2("HDSK%d Get parameter error.", selectedDisk);

    }
    else if (hdsk_hasVerbose()) {
        MESSAGE_4("Illegal IN command detected (port=%03xh, cmd=%d, pos=%d).",
            port, hdskLastCommand, hdskCommandPosition);
    }
    return CPM_OK;
}

static int32 hdsk_out(const int32 data) {
    switch(hdskLastCommand) {

        case HDSK_PARAM:
            paramcount = 0;
            selectedDisk = data;
            break;

        case HDSK_READ:

        case HDSK_WRITE:
            switch(hdskCommandPosition) {

                case 0:
                    selectedDisk = data;
                    hdskCommandPosition++;
                    break;

                case 1:
                    selectedSector = data;
                    hdskCommandPosition++;
                    break;

                case 2:
                    selectedTrack = data;
                    hdskCommandPosition++;
                    break;

                case 3:
                    selectedTrack += (data << 8);
                    hdskCommandPosition++;
                    break;

                case 4:
                    selectedDMA = data;
                    hdskCommandPosition++;
                    break;

                case 5:
                    selectedDMA += (data << 8);
                    hdskCommandPosition++;
                    break;

                default:
                    hdskLastCommand = HDSK_NONE;
                    hdskCommandPosition = 0;
            }
            break;

        default:
            hdskLastCommand = data;
            hdskCommandPosition = 0;
    }
    return 0; /* ignored, since OUT */
}

int32 hdsk_io(const int32 port, const int32 io, const int32 data) {
    return io == 0 ? hdsk_in(port) : hdsk_out(data);
}
