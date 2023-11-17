/*  altairz80_sio.c: MITS Altair serial I/O card

    Copyright (c) 2002-2008, Peter Schorn

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
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    PETER SCHORN BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
    IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
    CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

    Except as contained in this notice, the name of Peter Schorn shall not
    be used in advertising or otherwise to promote the sale, use or other dealings
    in this Software without prior written authorization from Peter Schorn.

    Based on work by Charles E Owen (c) 1997

    These functions support a simulated MITS 2SIO interface card.
    The card had two physical I/O ports which could be connected
    to any serial I/O device that would connect to a current loop,
    RS232, or TTY interface. Available baud rates were jumper
    selectable for each port from 110 to 9600.

    All I/O is via programmed I/O. Each device has a status port
    and a data port. A write to the status port can select
    some options for the device (0x03 will reset the port).
    A read of the status port gets the port status:

    +---+---+---+---+---+---+---+---+
    | X | X | X | X | X | X | O | I |
    +---+---+---+---+---+---+---+---+

    I - A 1 in this bit position means a character has been received
        on the data port and is ready to be read.
    O - A 1 in this bit means the port is ready to receive a character
        on the data port and transmit it out over the serial line.

    A read to the data port gets the buffered character, a write
    to the data port writes the character to the device.
*/

#include <ctype.h>

#include "altairz80_defs.h"
#include "sim_sock.h"
#include "sim_tmxr.h"
#include <time.h>
#include <assert.h>
#if UNIX_PLATFORM
#include <glob.h>
#elif defined (_WIN32)
#include <windows.h>
#endif

#define UNIT_V_SIO_ANSI     (UNIT_V_UF + 0)     /* ANSI mode, strip bit 8 on output             */
#define UNIT_SIO_ANSI       (1 << UNIT_V_SIO_ANSI)
#define UNIT_V_SIO_UPPER    (UNIT_V_UF + 1)     /* upper case mode                              */
#define UNIT_SIO_UPPER      (1 << UNIT_V_SIO_UPPER)
#define UNIT_V_SIO_BS       (UNIT_V_UF + 2)     /* map delete to backspace                      */
#define UNIT_SIO_BS         (1 << UNIT_V_SIO_BS)
#define UNIT_V_SIO_VERBOSE  (UNIT_V_UF + 3)     /* verbose mode, i.e. show error messages       */
#define UNIT_SIO_VERBOSE    (1 << UNIT_V_SIO_VERBOSE)
#define UNIT_V_SIO_MAP      (UNIT_V_UF + 4)     /* mapping mode on                              */
#define UNIT_SIO_MAP        (1 << UNIT_V_SIO_MAP)
#define UNIT_V_SIO_BELL     (UNIT_V_UF + 5)     /* ^G (bell character) rings bell               */
#define UNIT_SIO_BELL       (1 << UNIT_V_SIO_BELL)
#define UNIT_V_SIO_INTERRUPT (UNIT_V_UF + 6)     /* create keyboard interrupts                  */
#define UNIT_SIO_INTERRUPT  (1 << UNIT_V_SIO_INTERRUPT)
#define UNIT_V_SIO_SLEEP    (UNIT_V_UF + 7)     /* sleep after keyboard status check            */
#define UNIT_SIO_SLEEP      (1 << UNIT_V_SIO_SLEEP)

#define UNIT_V_SIMH_VERBOSE (UNIT_V_UF + 0)     /* verbose mode for SIMH pseudo device          */
#define UNIT_SIMH_VERBOSE   (1 << UNIT_V_SIMH_VERBOSE)
#define UNIT_V_SIMH_TIMERON (UNIT_V_UF + 1)     /* SIMH pseudo device timer generate interrupts */
#define UNIT_SIMH_TIMERON   (1 << UNIT_V_SIMH_TIMERON)

#define TERMINALS           4                   /* lines per mux                                */
#define SIO_CAN_READ        0x01                /* bit 0 is set iff character available         */
#define SIO_CAN_WRITE       0x02                /* bit 1 is set iff character can be sent       */
#define SIO_RESET           0x03                /* Command to reset SIO                         */
#define VGSIO_CAN_READ      0x02                /* bit 1 is set iff character available         */
#define VGSIO_CAN_WRITE     0x01                /* bit 0 is set iff character can be sent       */
#define KBD_HAS_CHAR        0x40                /* bit 6 is set iff character available         */
#define KBD_HAS_NO_CHAR     0x01                /* bit 0 is set iff no character is available   */

#define BACKSPACE_CHAR      0x08                /* backspace character                          */
#define DELETE_CHAR         0x7f                /* delete character                             */
#define CONTROLC_CHAR       0x03                /* control C character                          */
#define CONTROLG_CHAR       0x07                /* control G char., rings bell when displayed   */
#define CONTROLZ_CHAR       0x1a                /* control Z character                          */

#define PORT_TABLE_SIZE     256                 /* size of port mapping table                   */
#define SLEEP_ALLOWED_START_DEFAULT 100         /* default initial value for sleepAllowedCounter*/

static t_stat sio_set_verbose       (UNIT *uptr, int32 value, char *cptr, void *desc);
static t_stat simh_dev_set_timeron  (UNIT *uptr, int32 value, char *cptr, void *desc);
static t_stat simh_dev_set_timeroff (UNIT *uptr, int32 value, char *cptr, void *desc);
static t_stat sio_reset(DEVICE *dptr);
static t_stat sio_attach(UNIT *uptr, char *cptr);
static t_stat sio_detach(UNIT *uptr);
static t_stat ptr_reset(DEVICE *dptr);
static t_stat ptp_reset(DEVICE *dptr);
static t_stat toBool(char tf, int *result);
static t_stat sio_dev_set_port(UNIT *uptr, int32 value, char *cptr, void *desc);
static t_stat sio_dev_show_port(FILE *st, UNIT *uptr, int32 val, void *desc);
static t_stat sio_dev_set_interrupton(UNIT *uptr, int32 value, char *cptr, void *desc);
static t_stat sio_dev_set_interruptoff(UNIT *uptr, int32 value, char *cptr, void *desc);
static t_stat sio_svc(UNIT *uptr);
static t_stat simh_dev_reset(DEVICE *dptr);
static t_stat simh_svc(UNIT *uptr);
int32 nulldev   (const int32 port, const int32 io, const int32 data);
int32 sr_dev    (const int32 port, const int32 io, const int32 data);
int32 simh_dev  (const int32 port, const int32 io, const int32 data);
int32 sio0d     (const int32 port, const int32 io, const int32 data);
int32 sio0s     (const int32 port, const int32 io, const int32 data);
int32 sio1d     (const int32 port, const int32 io, const int32 data);
int32 sio1s     (const int32 port, const int32 io, const int32 data);
void printMessage(void);
void do_SIMH_sleep(void);
static void pollConnection(void);
static int32 mapCharacter(int32 ch);
static void checkSleep(void);
static void voidSleep(void);

extern int32 getBankSelect(void);
extern void setBankSelect(const int32 b);
extern uint32 getCommon(void);
extern uint8 GetBYTEWrapper(const uint32 Addr);
extern uint32 sim_map_resource(uint32 baseaddr, uint32 size, uint32 resource_type,
        int32 (*routine)(const int32, const int32, const int32), uint8 unmap);

extern int32 chiptype;
extern const t_bool rtc_avail;
extern FILE *sim_log;
extern uint32 PCX;
extern int32 sim_switches;
extern const char *scp_error_messages[];
extern int32 SR;
extern UNIT cpu_unit;
extern volatile int32 stop_cpu;
extern int32 sim_interval;

/* SIMH pseudo device status registers                                                                          */
/* ZSDOS clock definitions                                                                                      */
static time_t ClockZSDOSDelta       = 0;        /* delta between real clock and Altair clock                    */
static int32 setClockZSDOSPos       = 0;        /* determines state for receiving address of parameter block    */
static int32 setClockZSDOSAdr       = 0;        /* address in M of 6 byte parameter block for setting time      */
static int32 getClockZSDOSPos       = 0;        /* determines state for sending clock information               */

/* CPM3 clock definitions                                                                                       */
static time_t ClockCPM3Delta        = 0;        /* delta between real clock and Altair clock                    */
static int32 setClockCPM3Pos        = 0;        /* determines state for receiving address of parameter block    */
static int32 setClockCPM3Adr        = 0;        /* address in M of 5 byte parameter block for setting time      */
static int32 getClockCPM3Pos        = 0;        /* determines state for sending clock information               */
static int32 daysCPM3SinceOrg       = 0;        /* days since 1 Jan 1978                                        */

/* interrupt related                                                                                            */
static uint32 timeOfNextInterrupt;              /* time when next interrupt is scheduled                        */
       int32 timerInterrupt         = FALSE;    /* timer interrupt pending                                      */
       int32 timerInterruptHandler  = 0x0fc00;  /* default address of interrupt handling routine                */
static int32 setTimerInterruptAdrPos= 0;        /* determines state for receiving timerInterruptHandler         */
static int32 timerDelta             = 100;      /* interrupt every 100 ms                                       */
static int32 setTimerDeltaPos       = 0;        /* determines state for receiving timerDelta                    */

/* stop watch and timer related                                                                                 */
static uint32 stopWatchDelta        = 0;        /* stores elapsed time of stop watch                            */
static int32 getStopWatchDeltaPos   = 0;        /* determines the state for receiving stopWatchDelta            */
static uint32 stopWatchNow          = 0;        /* stores starting time of stop watch                           */
static int32 markTimeSP             = 0;        /* stack pointer for timer stack                                */

                                                /* default time in microseconds to sleep for SIMHSleepCmd       */
#if defined (_WIN32)
static uint32 SIMHSleep             = 1000;     /* Sleep uses milliseconds                                      */
#elif defined (__MWERKS__) && defined (macintosh)
static uint32 SIMHSleep             = 0;        /* no sleep on Macintosh OS9                                    */
#else
static uint32 SIMHSleep             = 100;      /* on other platforms 100 micro seconds is good enough          */
#endif
static uint32 sleepAllowedCounter   = 0;        /* only sleep on no character available when == 0               */
static uint32 sleepAllowedStart     = SLEEP_ALLOWED_START_DEFAULT;  /* default start for above counter          */

/* miscellaneous                                                                                                */
static int32 versionPos             = 0;        /* determines state for sending device identifier               */
static int32 lastCPMStatus          = 0;        /* result of last attachCPM command                             */
static int32 lastCommand            = 0;        /* most recent command processed on port 0xfeh                  */
static int32 getCommonPos           = 0;        /* determines state for sending the 'common' register           */

/* support for wild card expansion                                                                              */
#if UNIX_PLATFORM
static glob_t globS;
static uint32 globPosNameList       = 0;
static int32 globPosName            = 0;
static int32 globValid              = FALSE;
static int32 globError              = 0;
#elif defined (_WIN32)
static WIN32_FIND_DATA FindFileData;
static HANDLE hFind                 = INVALID_HANDLE_VALUE;
static int32 globFinished           = FALSE;
static int32 globValid              = FALSE;
static int32 globPosName            = 0;
#endif

/* SIO status registers                                                                                         */
static int32 warnLevelSIO           = 3;        /* display at most 'warnLevelSIO' times the same warning        */
static int32 warnUnattachedPTP      = 0;        /* display a warning message if < warnLevel and SIO set to
                                                VERBOSE and output to PTP without an attached file              */
static int32 warnUnattachedPTR      = 0;        /* display a warning message if < warnLevel and SIO set to
                                                VERBOSE and attempt to read from PTR without an attached file   */
static int32 warnPTREOF             = 0;        /* display a warning message if < warnLevel and SIO set to
                                                VERBOSE and attempt to read from PTR past EOF                   */
static int32 warnUnassignedPort     = 0;        /* display a warning message if < warnLevel and SIO set to
                                                VERBOSE and attempt to perform IN or OUT on an unassigned PORT  */

       int32 keyboardInterrupt = FALSE;         /* keyboard interrupt pending                                   */
       uint32 keyboardInterruptHandler = 0x0038;/* address of keyboard interrupt handler                        */

static TMLN TerminalLines[TERMINALS] = {        /* four terminals   */
    { 0 }
};

static TMXR altairTMXR = {                      /* mux descriptor   */
    TERMINALS, 0, 0, TerminalLines
};

static UNIT sio_unit = {
    UDATA (&sio_svc, UNIT_ATTABLE | UNIT_SIO_MAP | UNIT_SIO_SLEEP, 0),
    100000, /* wait                                                 */
    FALSE,  /* u3 = FALSE, no character available in buffer         */
    FALSE,  /* u4 = FALSE, terminal input is not attached to a file */
    FALSE,  /* u5 = FALSE, terminal input has not yet reached EOF   */
    0       /* u6 = 0, not used                                     */
};

static REG sio_reg[] = {
    { DRDATA (SIOWLEV,  warnLevelSIO,       32) },
    { DRDATA (WRNUPTP,  warnUnattachedPTP,  32) },
    { DRDATA (WRNUPTR,  warnUnattachedPTR,  32) },
    { DRDATA (WRNPTRE,  warnPTREOF,         32) },
    { DRDATA (WRUPORT,  warnUnassignedPort, 32) },
    { HRDATA (FILEATT,  sio_unit.u4,        8), REG_RO },   /* TRUE iff terminal input is attached to a file    */
    { HRDATA (FILEEOF,  sio_unit.u5,        8), REG_RO },   /* TRUE iff terminal input file has reached EOF     */
    { HRDATA (TSTATUS,  sio_unit.u3,        8) },           /* TRUE iff a character available in sio_unit.buf   */
    { DRDATA (TBUFFER,  sio_unit.buf,       8) },           /* input buffer for one character                   */
    { DRDATA (KEYBDI,   keyboardInterrupt,          3), REG_RO  },
    { HRDATA (KEYBDH,   keyboardInterruptHandler,   16)         },
    { NULL }
};

static MTAB sio_mod[] = {
    { UNIT_SIO_ANSI,        0,                  "TTY",      "TTY",      NULL }, /* keep bit 8 as is for output              */
    { UNIT_SIO_ANSI,        UNIT_SIO_ANSI,      "ANSI",     "ANSI",     NULL }, /* set bit 8 to 0 before output             */
    { UNIT_SIO_UPPER,       0,                  "ALL",      "ALL",      NULL }, /* do not change case of input characters   */
    { UNIT_SIO_UPPER,       UNIT_SIO_UPPER,     "UPPER",    "UPPER",    NULL }, /* change input characters to upper case    */
    { UNIT_SIO_BS,          0,                  "BS",       "BS",       NULL }, /* map delete to backspace                  */
    { UNIT_SIO_BS,          UNIT_SIO_BS,        "DEL",      "DEL",      NULL }, /* map backspace to delete                  */
    { UNIT_SIO_VERBOSE,     0,                  "QUIET",    "QUIET",    NULL }, /* quiet, no error messages                 */
    { UNIT_SIO_VERBOSE,     UNIT_SIO_VERBOSE,   "VERBOSE",  "VERBOSE",  &sio_set_verbose },
                                                                                /* verbose, display warning messages        */
    { UNIT_SIO_MAP,         0,                  "NOMAP",    "NOMAP",    NULL }, /*  disable character mapping               */
    { UNIT_SIO_MAP,         UNIT_SIO_MAP,       "MAP",      "MAP",      NULL }, /*  enable all character mapping            */
    { UNIT_SIO_BELL,        0,                  "BELL",     "BELL",     NULL }, /*  enable bell character                   */
    { UNIT_SIO_BELL,        UNIT_SIO_BELL,      "NOBELL",   "NOBELL",   NULL }, /*  suppress ringing the bell               */
    { UNIT_SIO_SLEEP,       0,                  "NOSLEEP",  "NOSLEEP",  NULL }, /*  no sleep after keyboard status check    */
    { UNIT_SIO_SLEEP,       UNIT_SIO_SLEEP,     "SLEEP",    "SLEEP",    NULL }, /*  sleep after keyboard status check       */
                                                                                /*  no keyboard interrupts                  */
    { UNIT_SIO_INTERRUPT,   0,                  "NOINTERRUPT","NOINTERRUPT",&sio_dev_set_interruptoff },
                                                                                /*  create keyboard interrupts              */
    { UNIT_SIO_INTERRUPT,   UNIT_SIO_INTERRUPT, "INTERRUPT","INTERRUPT",&sio_dev_set_interrupton },
    { MTAB_XTD|MTAB_VDV|MTAB_VAL,   0,          "PORT",     "PORT",     &sio_dev_set_port, &sio_dev_show_port },
    { 0 }
};

DEVICE sio_dev = {
    "SIO", &sio_unit, sio_reg, sio_mod,
    1, 10, 31, 1, 8, 8,
    NULL, NULL, &sio_reset,
    NULL, &sio_attach, &sio_detach,
    NULL, 0, 0,
    NULL, NULL, NULL };

static UNIT ptr_unit = {
    UDATA (NULL, UNIT_SEQ | UNIT_ATTABLE | UNIT_ROABLE, 0)
};

static REG ptr_reg[] = {
    { HRDATA (STAT, ptr_unit.u3, 8)  },
    { NULL }
};

DEVICE ptr_dev = {
    "PTR", &ptr_unit, ptr_reg, NULL,
    1, 10, 31, 1, 8, 8,
    NULL, NULL, &ptr_reset,
    NULL, NULL, NULL,
    NULL, 0, 0,
    NULL, NULL, NULL
};

static UNIT ptp_unit = {
    UDATA (NULL, UNIT_SEQ + UNIT_ATTABLE, 0)
};

DEVICE ptp_dev = {
    "PTP", &ptp_unit, NULL, NULL,
    1, 10, 31, 1, 8, 8,
    NULL, NULL, &ptp_reset,
    NULL, NULL, NULL,
    NULL, 0, 0,
    NULL, NULL, NULL
};

/*  Synthetic device SIMH for communication
    between Altair and SIMH environment using port 0xfe */
static UNIT simh_unit = {
    UDATA (&simh_svc, 0, 0), KBD_POLL_WAIT
};

static REG simh_reg[] = {
    { DRDATA (CZD,      ClockZSDOSDelta,        32)             },
    { DRDATA (SCZP,     setClockZSDOSPos,       8),     REG_RO  },
    { HRDATA (SCZA,     setClockZSDOSAdr,       16),    REG_RO  },
    { DRDATA (GCZP,     getClockZSDOSPos,       8),     REG_RO  },

    { DRDATA (CC3D,     ClockCPM3Delta,         32)             },
    { DRDATA (SC3DP,    setClockCPM3Pos,        8),     REG_RO  },
    { HRDATA (SC3DA,    setClockCPM3Adr,        16),    REG_RO  },
    { DRDATA (GC3DP,    getClockCPM3Pos,        8),     REG_RO  },
    { DRDATA (D3DO,     daysCPM3SinceOrg,       32),    REG_RO  },

    { DRDATA (TOFNI,    timeOfNextInterrupt,    32),    REG_RO  },
    { DRDATA (TIMI,     timerInterrupt,         3)              },
    { HRDATA (TIMH,     timerInterruptHandler,  16)             },
    { DRDATA (STIAP,    setTimerInterruptAdrPos,8),     REG_RO  },
    { DRDATA (TIMD,     timerDelta,             32)             },
    { DRDATA (STDP,     setTimerDeltaPos,       8),     REG_RO  },
    { DRDATA (SLEEP,    SIMHSleep,              32)             },
    { DRDATA (VOSLP,    sleepAllowedStart,      32)             },

    { DRDATA (STPDT,    stopWatchDelta,         32),    REG_RO  },
    { DRDATA (STPOS,    getStopWatchDeltaPos,   8),     REG_RO  },
    { DRDATA (STPNW,    stopWatchNow,           32),    REG_RO  },
    { DRDATA (MTSP,     markTimeSP,             8),     REG_RO  },

    { DRDATA (VPOS,     versionPos,             8),     REG_RO  },
    { DRDATA (LCPMS,    lastCPMStatus,          8),     REG_RO  },
    { DRDATA (LCMD,     lastCommand,            8),     REG_RO  },
    { DRDATA (CPOS,     getCommonPos,           8),     REG_RO  },
    { NULL }
};

static MTAB simh_mod[] = {
    /* quiet, no warning messages           */
    { UNIT_SIMH_VERBOSE,    0,                  "QUIET",    "QUIET",    NULL                    },
    /* verbose, display warning messages    */
    { UNIT_SIMH_VERBOSE,    UNIT_SIMH_VERBOSE,  "VERBOSE",  "VERBOSE",  NULL                    },
    /* timer generated interrupts are off   */
    { UNIT_SIMH_TIMERON,    0,                  "TIMEROFF", "TIMEROFF", &simh_dev_set_timeroff  },
    /* timer generated interrupts are on    */
    { UNIT_SIMH_TIMERON,    UNIT_SIMH_TIMERON,  "TIMERON",  "TIMERON",  &simh_dev_set_timeron   },
    { 0 }
};

DEVICE simh_device = {
    "SIMH", &simh_unit, simh_reg, simh_mod,
    1, 10, 31, 1, 16, 4,
    NULL, NULL, &simh_dev_reset,
    NULL, NULL, NULL,
    NULL, 0, 0,
    NULL, NULL, NULL
};

char messageBuffer[256] = { 0 };

void printMessage(void) {
    printf(messageBuffer);
    printf(NLP);
    if (sim_log) {
        fprintf(sim_log, messageBuffer);
        fprintf(sim_log,"\n");
    }
}

static void resetSIOWarningFlags(void) {
    warnUnattachedPTP = warnUnattachedPTR = warnPTREOF = warnUnassignedPort = 0;
}

static t_stat sio_set_verbose(UNIT *uptr, int32 value, char *cptr, void *desc) {
    resetSIOWarningFlags();
    return SCPE_OK;
}

static t_stat sio_attach(UNIT *uptr, char *cptr) {
    t_stat r = SCPE_IERR;
    sio_unit.u3 = FALSE;                                    /* no character in terminal input buffer    */
    get_uint(cptr, 10, 65535, &r);                          /* attempt to get port, discard result      */
    if (r == SCPE_OK) {                                     /* string can be interpreted as port number */
        sio_unit.u4 = FALSE;                                /* terminal input is not attached to a file */
        return tmxr_attach(&altairTMXR, uptr, cptr);        /* attach mux                               */
    }
    sio_unit.u4 = TRUE;                                     /* terminal input is attached to a file     */
    sio_unit.u5 = FALSE;                                    /* EOF not yet reached                      */
    return attach_unit(uptr, cptr);
}

static t_stat sio_detach(UNIT *uptr) {
    sio_unit.u3 = FALSE;                                    /* no character in terminal input buffer    */
    if (sio_unit.u4) {                                      /* is terminal input attached to a file?    */
        sio_unit.u4 = FALSE;                                /* not anymore, detach                      */
        return detach_unit(uptr);
    }
    return tmxr_detach(&altairTMXR, uptr);
}

static void pollConnection(void) {
    if (sio_unit.flags & UNIT_ATT) {
        int32 temp = tmxr_poll_conn(&altairTMXR);           /* poll connection                          */
        if (temp >= 0)
            TerminalLines[temp].rcve = 1;                   /* enable receive                           */
        tmxr_poll_rx(&altairTMXR);                          /* poll input                               */
        tmxr_poll_tx(&altairTMXR);                          /* poll output                              */
    }
}

/* reset routines */
static t_stat sio_reset(DEVICE *dptr) {
    int32 i;
    sio_unit.u3 = FALSE;                                    /* no character in terminal input buffer    */
    resetSIOWarningFlags();
    if (sio_unit.u4) {                                      /* is terminal input attached to a file?    */
        rewind(sio_unit.fileref);                           /* yes, rewind input                        */
        sio_unit.u5 = FALSE;                                /* EOF not yet reached                      */
    }
    else if (sio_unit.flags & UNIT_ATT)
        for (i = 0; i < TERMINALS; i++)
            if (TerminalLines[i].conn)
                tmxr_reset_ln(&TerminalLines[i]);
    return SCPE_OK;
}

static t_stat ptr_reset(DEVICE *dptr) {
    resetSIOWarningFlags();
    ptr_unit.u3 = FALSE;                                    /* End Of File not yet reached              */
    if (ptr_unit.flags & UNIT_ATT)                          /* attached?                                */
        rewind(ptr_unit.fileref);
    return SCPE_OK;
}

static t_stat ptp_reset(DEVICE *dptr) {
    resetSIOWarningFlags();
    return SCPE_OK;
}

static int32 mapCharacter(int32 ch) {
    ch &= 0xff;
    if (sio_unit.flags & UNIT_SIO_MAP) {
        if (sio_unit.flags & UNIT_SIO_BS) {
            if (ch == BACKSPACE_CHAR)
                return DELETE_CHAR;
        }
        else if (ch == DELETE_CHAR)
            return BACKSPACE_CHAR;
        if (sio_unit.flags & UNIT_SIO_UPPER)
            return toupper(ch);
    }
    return ch;
}

/*  I/O instruction handlers, called from the CPU module when an
    IN or OUT instruction is issued.

    Each function is passed an 'io' flag, where 0 means a read from
    the port, and 1 means a write to the port. On input, the actual
    input is passed as the return value, on output, 'data' is written
    to the device.

    Port 1 controls console I/O. We distinguish three cases:
    1) SIO attached to a file       (i.e. input taken from a file   )
    2) SIO attached to a port       (i.e. Telnet console I/O        )
    3) SIO not attached to a port   (i.e. "regular" console I/O     )
*/

typedef struct {
    int32 port;             /* this information belongs to port number 'port'           */
    int32 terminalLine;     /* map to this 'terminalLine'                               */
    int32 sio_can_read;     /* bit mask to indicate that one can read from this port    */
    int32 sio_cannot_read;  /* bit mask to indicate that one cannot read from this port */
    int32 sio_can_write;    /* bit mask to indicate that one can write to this port     */
    int32 hasReset;         /* TRUE iff SIO has reset command                           */
    int32 sio_reset;        /* reset command                                            */
    int32 hasOUT;           /* TRUE iff port supports OUT command                       */
    int32 isBuiltin;        /* TRUE iff mapping is built in                             */
} SIO_PORT_INFO;

static SIO_PORT_INFO port_table[PORT_TABLE_SIZE] = {
    {0x00, 0, KBD_HAS_CHAR,     KBD_HAS_NO_CHAR, SIO_CAN_WRITE, FALSE, 0, FALSE, TRUE   },
    {0x01, 0, 0,                0,      0, FALSE, 0, FALSE, TRUE                        },
    {0x02, 0, VGSIO_CAN_READ,   0,      VGSIO_CAN_WRITE, FALSE, 0, TRUE, TRUE           },
    {0x03, 0, VGSIO_CAN_READ,   0,      VGSIO_CAN_WRITE, FALSE, 0, FALSE, TRUE          },
    {0x10, 0, SIO_CAN_READ,     0,      SIO_CAN_WRITE, TRUE, SIO_RESET, FALSE, TRUE     },
    {0x14, 1, SIO_CAN_READ,     0,      SIO_CAN_WRITE, TRUE, SIO_RESET, FALSE, TRUE     },
    {0x16, 2, SIO_CAN_READ,     0,      SIO_CAN_WRITE, TRUE, SIO_RESET, FALSE, TRUE     },
    {0x18, 3, SIO_CAN_READ,     0,      SIO_CAN_WRITE, TRUE, SIO_RESET, FALSE, TRUE     },
    {0x11, 0, SIO_CAN_READ,     0,      SIO_CAN_WRITE, TRUE, SIO_RESET, TRUE, TRUE      },
    {0x15, 1, SIO_CAN_READ,     0,      SIO_CAN_WRITE, TRUE, SIO_RESET, TRUE, TRUE      },
    {0x17, 2, SIO_CAN_READ,     0,      SIO_CAN_WRITE, TRUE, SIO_RESET, TRUE, TRUE      },
    {0x19, 3, SIO_CAN_READ,     0,      SIO_CAN_WRITE, TRUE, SIO_RESET, TRUE, TRUE      },
    {-1, 0, 0, 0, 0, 0, 0, 0, 0}   /* must be last */
};

static SIO_PORT_INFO lookupPortInfo(const int32 port, int32 *position) {
    int32 i = 0;
    while ((port_table[i].port != -1) && (port_table[i].port != port)) i++;
    *position = i;
    return port_table[i];
}

/* keyboard idle detection: sleep when feature enabled, no character available
    (duty of caller) and operation not voided (e.g. when output is available) */
static void checkSleep(void) {
    if (sio_unit.flags & UNIT_SIO_SLEEP) {
        if (sleepAllowedCounter) sleepAllowedCounter--;
        else do_SIMH_sleep();
    }
}

/* void sleep for next 'sleepAllowedStart' tests */
static void voidSleep(void) {
    sleepAllowedCounter = sleepAllowedStart;
}

/* generic status port for keyboard input / terminal output */
int32 sio0s(const int32 port, const int32 io, const int32 data) {
    int32 ch, result;
    SIO_PORT_INFO spi = lookupPortInfo(port, &ch);
    assert(spi.port == port);
    pollConnection();
    if (io == 0) { /* IN */
        if (sio_unit.u4)                                    /* attached to a file?                      */
            if (sio_unit.u5)                                /* EOF reached?                             */
                sio_detach(&sio_unit);                      /* detach file and switch to keyboard input */
            else return spi.sio_can_read | spi.sio_can_write;
        if (sio_unit.flags & UNIT_ATT) {                    /* attached to a port?                      */
            if (tmxr_rqln(&TerminalLines[spi.terminalLine]))
                result = spi.sio_can_read;
            else {
                result = spi.sio_cannot_read;
                checkSleep();
            }
            return result |                                 /* read possible if character available     */
                (TerminalLines[spi.terminalLine].conn && TerminalLines[spi.terminalLine].xmte ? spi.sio_can_write : 0x00);
                                                            /* write possible if connected and transmit
                                                                enabled                                 */
        }
        if (sio_unit.u3)                                    /* character available?                     */
            return spi.sio_can_read | spi.sio_can_write;
        ch = sim_poll_kbd();                                /* no, try to get a character               */
        if (ch) {                                           /* character available?                     */
            if (ch == SCPE_STOP) {                          /* stop CPU in case ^E (default) was typed  */
                stop_cpu = TRUE;
                sim_interval = 0;                           /* detect stop condition as soon as possible*/
                return spi.sio_can_write | spi.sio_cannot_read; /* do not consume stop character        */
            }
            sio_unit.u3 = TRUE;                             /* indicate character available             */
            sio_unit.buf = ch;                              /* store character in buffer                */
            return spi.sio_can_read | spi.sio_can_write;
        }
        checkSleep();
        return spi.sio_can_write | spi.sio_cannot_read;
    }                                                       /* OUT follows, no fall-through from IN     */
    if (spi.hasReset && (data == spi.sio_reset))            /* reset command                            */
        sio_unit.u3 = FALSE;                                /* indicate that no character is available  */
    return 0x00;                                            /* ignored since OUT                        */
}

/* generic data port for keyboard input / terminal output */
int32 sio0d(const int32 port, const int32 io, const int32 data) {
    int32 ch;
    SIO_PORT_INFO spi = lookupPortInfo(port, &ch);
    assert(spi.port == port);
    pollConnection();
    if (io == 0) { /* IN */
        if (sio_unit.u4) {                                  /* attached to a file?                      */
            if (sio_unit.u5) {                              /* EOF reached?                             */
                sio_detach(&sio_unit);                      /* detach file and switch to keyboard input */
                return CONTROLC_CHAR;                       /* this time return ^C after all            */
            }
            if ((ch = getc(sio_unit.fileref)) == EOF) {     /* end of file?                             */
                sio_unit.u5 = TRUE;                         /* terminal input file has reached EOF      */
                return CONTROLC_CHAR;                       /* result is ^C (= CP/M interrupt)          */
            }
            return mapCharacter(ch);                        /* return mapped character                  */
        }
        if (sio_unit.flags & UNIT_ATT)
            return mapCharacter(tmxr_getc_ln(&TerminalLines[spi.terminalLine]));
        sio_unit.u3 = FALSE;                                /* no character is available any more       */
        return mapCharacter(sio_unit.buf);                  /* return previous character                */
    }                                                       /* OUT follows, no fall-through from IN     */
    if (spi.hasOUT) {
        ch = sio_unit.flags & UNIT_SIO_ANSI ? data & 0x7f : data;   /* clear highest bit in ANSI mode   */
        if ((ch != CONTROLG_CHAR) || !(sio_unit.flags & UNIT_SIO_BELL)) {
            voidSleep();
            if ((sio_unit.flags & UNIT_ATT) && (!sio_unit.u4))  /* attached to a port and not to a file */
                tmxr_putc_ln(&TerminalLines[spi.terminalLine], ch); /* status ignored                   */
            else
                sim_putchar(ch);
        }
    }
    return 0x00;                                            /* ignored since OUT                        */
}

/* PTR/PTP status port */
int32 sio1s(const int32 port, const int32 io, const int32 data) {
    if (io == 0) {                                          /* IN                                       */
                                                            /* reset I bit iff PTR unit not attached or
                                                                no more data available. O bit is always
                                                                set since write always possible.        */
        if ((ptr_unit.flags & UNIT_ATT) == 0) {             /* PTR is not attached                      */
            if ((sio_unit.flags & UNIT_SIO_VERBOSE) && (warnUnattachedPTR < warnLevelSIO)) {
                warnUnattachedPTR++;
/*06*/          MESSAGE_2("Attempt to test status of unattached PTR[0x%02x]. 0x02 returned.", port);
            }
            return SIO_CAN_WRITE;
        }
                                                            /* if EOF then SIO_CAN_WRITE else
                                                                (SIO_CAN_WRITE and SIO_CAN_READ)        */
        return ptr_unit.u3 ? SIO_CAN_WRITE : (SIO_CAN_READ | SIO_CAN_WRITE);
    }                                                       /* OUT follows                              */
    if (data == SIO_RESET)
        ptr_unit.u3 = FALSE;                                /* reset EOF indicator                      */
    return 0x00;                                            /* ignored since OUT                        */
}

/* PTR/PTP data port */
int32 sio1d(const int32 port, const int32 io, const int32 data) {
    int32 ch;
    if (io == 0) {                                          /* IN                                       */
        if (ptr_unit.u3) {                                  /* EOF reached, no more data available      */
            if ((sio_unit.flags & UNIT_SIO_VERBOSE) && (warnPTREOF < warnLevelSIO)) {
                warnPTREOF++;
/*07*/          MESSAGE_2("PTR[0x%02x] attempted to read past EOF. 0x00 returned.", port);
            }
            return 0x00;
        }
        if ((ptr_unit.flags & UNIT_ATT) == 0) {             /* not attached                             */
            if ((sio_unit.flags & UNIT_SIO_VERBOSE) && (warnUnattachedPTR < warnLevelSIO)) {
                warnUnattachedPTR++;
/*08*/          MESSAGE_2("Attempt to read from unattached PTR[0x%02x]. 0x00 returned.", port);
            }
            return 0x00;
        }
        if ((ch = getc(ptr_unit.fileref)) == EOF) {         /* end of file?                             */
            ptr_unit.u3 = TRUE;                             /* remember EOF reached                     */
            return CONTROLZ_CHAR;                           /* ^Z denotes end of text file in CP/M      */
        }
        return ch & 0xff;
    }                                                       /* OUT follows                              */
    if (ptp_unit.flags & UNIT_ATT)                          /* unit must be attached                    */
        putc(data, ptp_unit.fileref);
                                                            /* else ignore data                         */
    else if ((sio_unit.flags & UNIT_SIO_VERBOSE) && (warnUnattachedPTP < warnLevelSIO)) {
        warnUnattachedPTP++;
/*09*/  MESSAGE_3("Attempt to output '0x%02x' to unattached PTP[0x%02x] - ignored.", data, port);
    }
    return 0x00;                                            /* ignored since OUT                        */
}

static t_stat toBool(char tf, int *result) {
    if (tf == 'T') {
        *result = TRUE;
        return SCPE_OK;
    }
    if (tf == 'F') {
        *result = FALSE;
        return SCPE_OK;
    }
    return SCPE_ARG;
}

static void show_sio_port_info(FILE *st, SIO_PORT_INFO sip) {
    if (sio_unit.flags & UNIT_SIO_VERBOSE)
        fprintf(st, "(Port=%02x/Terminal=%1i/Read=0x%02x/NotRead=0x%02x/"
            "Write=0x%02x/Reset?=%s/Reset=0x%02x/Data?=%s)",
            sip.port, sip.terminalLine, sip.sio_can_read, sip.sio_cannot_read,
            sip.sio_can_write, sip.hasReset ? "True" : "False", sip.sio_reset,
            sip.hasOUT ? "True" : "False");
    else
        fprintf(st, "(%02x/%1i/%02x/%02x/%02x/%s/%02x/%s)",
            sip.port, sip.terminalLine, sip.sio_can_read, sip.sio_cannot_read,
            sip.sio_can_write, sip.hasReset ? "T" : "F", sip.sio_reset,
            sip.hasOUT ? "T" : "F");
}

static uint32 equalSIP(SIO_PORT_INFO x, SIO_PORT_INFO y) {
    /* isBuiltin is not relevant for equality, only for display */
    return (x.port == y.port) && (x.terminalLine == y.terminalLine) &&
    (x.sio_can_read == y.sio_can_read) && (x.sio_cannot_read == y.sio_cannot_read) &&
    (x.sio_can_write == y.sio_can_write) && (x.hasReset == y.hasReset) &&
    (x.sio_reset == y.sio_reset) && (x.hasOUT == y.hasOUT);
}

static t_stat sio_dev_set_port(UNIT *uptr, int32 value, char *cptr, void *desc) {
    int32 result, n, position;
    SIO_PORT_INFO sip = { 0 }, old;
    char hasReset, hasOUT;
    if (cptr == NULL) return SCPE_ARG;
    result = sscanf(cptr, "%x%n", &sip.port, &n);
    if ((result == 1) && (cptr[n] == 0)) {
        old = lookupPortInfo(sip.port, &position);
        if (old.port == -1) {
            printf("No mapping for port 0x%02x exists - cannot remove.\n", sip.port);
            return SCPE_ARG;
        }
        do {
            port_table[position] = port_table[position + 1];
            position++;
        }
        while (port_table[position].port != -1);
        sim_map_resource(sip.port, 1, RESOURCE_TYPE_IO, &nulldev, FALSE);
        if (sio_unit.flags & UNIT_SIO_VERBOSE) {
            printf("Removing mapping for port 0x%02x.\n\t", sip.port);
            show_sio_port_info(stdout, old);
        }
        return SCPE_OK;
    }
    result = sscanf(cptr, "%x/%d/%x/%x/%x/%1c/%x/%1c%n", &sip.port,
        &sip.terminalLine, &sip.sio_can_read, &sip.sio_cannot_read,
        &sip.sio_can_write, &hasReset, &sip.sio_reset, &hasOUT, &n);
    if ((result != 8) || (result == EOF) || (cptr[n] != 0)) return SCPE_ARG;
    result = toBool(hasReset, &sip.hasReset);
    if (result != SCPE_OK) return result;
    result = toBool(hasOUT, &sip.hasOUT);
    if (result != SCPE_OK) return result;
    if (sip.port != (sip.port & 0xff)) {
        printf("Truncating port 0x%x to 0x%02x.\n", sip.port, sip.port & 0xff);
        sip.port &= 0xff;
    }
    old = lookupPortInfo(sip.port, &position);
    if (old.port == sip.port) {
        if (sio_unit.flags & UNIT_SIO_VERBOSE) {
            printf("Replacing mapping for port 0x%02x.\n\t", sip.port);
            show_sio_port_info(stdout, old);
            printf("-> ");
            show_sio_port_info(stdout, sip);
            if (equalSIP(sip, old)) printf("[identical]");
        }
    }
    else {
        port_table[position + 1] = old;
        if (sio_unit.flags & UNIT_SIO_VERBOSE) {
            printf("Adding mapping for port 0x%02x.\n\t", sip.port);
            show_sio_port_info(stdout, sip);
        }
    }
    if (sio_unit.flags & UNIT_SIO_VERBOSE) printf("\n");
    port_table[position] = sip;
    sim_map_resource(sip.port, 1, RESOURCE_TYPE_IO, (sip.hasOUT ||
        (sip.sio_can_read == 0) && (sip.sio_cannot_read == 0) &&
        (sip.sio_can_write == 0)) ? &sio0d : &sio0s, FALSE);
    return SCPE_OK;
}

static t_stat sio_dev_show_port(FILE *st, UNIT *uptr, int32 val, void *desc) {
    int32 i, first = TRUE;
    for (i = 0; port_table[i].port != -1; i++)
        if (!port_table[i].isBuiltin) {
            if (first) first = FALSE;
            else fprintf(st, " ");
            show_sio_port_info(st, port_table[i]);
        }
    if (first) fprintf(st, "no extra port");
    return SCPE_OK;
}

static t_stat sio_dev_set_interrupton(UNIT *uptr, int32 value, char *cptr, void *desc) {
    keyboardInterrupt = FALSE;
    return sim_activate(&sio_unit, sio_unit.wait);          /* activate unit */
}

static t_stat sio_dev_set_interruptoff(UNIT *uptr, int32 value, char *cptr, void *desc) {
    keyboardInterrupt = FALSE;
    sim_cancel(&sio_unit);
    return SCPE_OK;
}

static t_stat sio_svc(UNIT *uptr) {
    if (sio0s(0, 0, 0) & KBD_HAS_CHAR) {
        keyboardInterrupt = TRUE;
    }
    if (sio_unit.flags & UNIT_SIO_INTERRUPT)
        sim_activate(&sio_unit, sio_unit.wait);             /* activate unit    */
    return SCPE_OK;
}

int32 nulldev(const int32 port, const int32 io, const int32 data) {
    if ((sio_unit.flags & UNIT_SIO_VERBOSE) && (warnUnassignedPort < warnLevelSIO)) {
        warnUnassignedPort++;
        if (io == 0) {
            MESSAGE_2("Attempt to input from unassigned port 0x%04x - ignored.", port);
        }
        else {
            MESSAGE_3("Attempt to output 0x%02x to unassigned port 0x%04x - ignored.", data, port);
        }
    }
    return io == 0 ? 0xff : 0;
}

int32 sr_dev(const int32 port, const int32 io, const int32 data) {
    return io == 0 ? SR : 0;
}

static int32 toBCD(const int32 x) {
    return (x / 10) * 16 + (x % 10);
}

static int32 fromBCD(const int32 x) {
    return 10 * ((0xf0 & x) >> 4) + (0x0f & x);
}

/*  Z80 or 8080 programs communicate with the SIMH pseudo device via port 0xfe.
        The following principles apply:

    1)  For commands that do not require parameters and do not return results
        ld  a,<cmd>
        out (0feh),a
        Special case is the reset command which needs to be send 128 times to make
        sure that the internal state is properly reset.

    2)  For commands that require parameters and do not return results
        ld  a,<cmd>
        out (0feh),a
        ld  a,<p1>
        out (0feh),a
        ld  a,<p2>
        out (0feh),a
        ...
        Note: The calling program must send all parameter bytes. Otherwise
        the pseudo device is left in an undefined state.

    3)  For commands that do not require parameters and return results
        ld  a,<cmd>
        out (0feh),a
        in  a,(0feh)    ; <A> contains first byte of result
        in  a,(0feh)    ; <A> contains second byte of result
        ...
        Note: The calling program must request all bytes of the result. Otherwise
        the pseudo device is left in an undefined state.

    4)  Commands requiring parameters and returning results do not exist currently.

*/

enum simhPseudoDeviceCommands { /* do not change order or remove commands, add only at the end          */
    printTimeCmd,               /*  0 print the current time in milliseconds                            */
    startTimerCmd,              /*  1 start a new timer on the top of the timer stack                   */
    stopTimerCmd,               /*  2 stop timer on top of timer stack and show time difference         */
    resetPTRCmd,                /*  3 reset the PTR device                                              */
    attachPTRCmd,               /*  4 attach the PTR device                                             */
    detachPTRCmd,               /*  5 detach the PTR device                                             */
    getSIMHVersionCmd,          /*  6 get the current version of the SIMH pseudo device                 */
    getClockZSDOSCmd,           /*  7 get the current time in ZSDOS format                              */
    setClockZSDOSCmd,           /*  8 set the current time in ZSDOS format                              */
    getClockCPM3Cmd,            /*  9 get the current time in CP/M 3 format                             */
    setClockCPM3Cmd,            /* 10 set the current time in CP/M 3 format                             */
    getBankSelectCmd,           /* 11 get the selected bank                                             */
    setBankSelectCmd,           /* 12 set the selected bank                                             */
    getCommonCmd,               /* 13 get the base address of the common memory segment                 */
    resetSIMHInterfaceCmd,      /* 14 reset the SIMH pseudo device                                      */
    showTimerCmd,               /* 15 show time difference to timer on top of stack                     */
    attachPTPCmd,               /* 16 attach PTP to the file with name at beginning of CP/M command line*/
    detachPTPCmd,               /* 17 detach PTP                                                        */
    hasBankedMemoryCmd,         /* 18 determines whether machine has banked memory                      */
    setZ80CPUCmd,               /* 19 set the CPU to a Z80                                              */
    set8080CPUCmd,              /* 20 set the CPU to an 8080                                            */
    startTimerInterruptsCmd,    /* 21 start timer interrupts                                            */
    stopTimerInterruptsCmd,     /* 22 stop timer interrupts                                             */
    setTimerDeltaCmd,           /* 23 set the timer interval in which interrupts occur                  */
    setTimerInterruptAdrCmd,    /* 24 set the address to call by timer interrupts                       */
    resetStopWatchCmd,          /* 25 reset the millisecond stop watch                                  */
    readStopWatchCmd,           /* 26 read the millisecond stop watch                                   */
    SIMHSleepCmd,               /* 27 let SIMH sleep for SIMHSleep microseconds                         */
    getHostOSPathSeparator,     /* 28 obtain the file path separator of the OS under which SIMH runs    */
    getHostFilenames            /* 29 perform wildcard expansion and obtain list of file names          */
};

#define CPM_COMMAND_LINE_LENGTH    128
#define TIMER_STACK_LIMIT          10       /* stack depth of timer stack   */
static uint32 markTime[TIMER_STACK_LIMIT];  /* timer stack                  */
static struct tm currentTime;
static int32 currentTimeValid = FALSE;
static char version[] = "SIMH003";

static t_stat simh_dev_reset(DEVICE *dptr) {
    currentTimeValid        = FALSE;
    ClockZSDOSDelta         = 0;
    setClockZSDOSPos        = 0;
    getClockZSDOSPos        = 0;
    ClockCPM3Delta          = 0;
    setClockCPM3Pos         = 0;
    getClockCPM3Pos         = 0;
    getStopWatchDeltaPos    = 0;
    getCommonPos            = 0;
    setTimerDeltaPos        = 0;
    setTimerInterruptAdrPos = 0;
    markTimeSP              = 0;
    versionPos              = 0;
    lastCommand             = 0;
    lastCPMStatus           = SCPE_OK;
    timerInterrupt          = FALSE;
    if (simh_unit.flags & UNIT_SIMH_TIMERON)
        simh_dev_set_timeron(NULL, 0, NULL, NULL);
    return SCPE_OK;
}

static void warnNoRealTimeClock(void) {
    if (simh_unit.flags & UNIT_SIMH_VERBOSE) {
        MESSAGE_1("Sorry - no real time clock available.");
    }
}

static t_stat simh_dev_set_timeron(UNIT *uptr, int32 value, char *cptr, void *desc) {
    if (rtc_avail) {
        timeOfNextInterrupt = sim_os_msec() + timerDelta;
        return sim_activate(&simh_unit, simh_unit.wait);    /* activate unit */
    }
    warnNoRealTimeClock();
    return SCPE_ARG;
}

static t_stat simh_dev_set_timeroff(UNIT *uptr, int32 value, char *cptr, void *desc) {
    timerInterrupt = FALSE;
    sim_cancel(&simh_unit);
    return SCPE_OK;
}

static t_stat simh_svc(UNIT *uptr) {
    uint32 n = sim_os_msec();
    if (n >= timeOfNextInterrupt) {
        timerInterrupt = TRUE;
        timeOfNextInterrupt += timerDelta;
        if (n >= timeOfNextInterrupt)               /* time of next interrupt is not in the future  */
            timeOfNextInterrupt = n + timerDelta;   /* make sure it is in the future!               */
    }
    if (simh_unit.flags & UNIT_SIMH_TIMERON)
        sim_activate(&simh_unit, simh_unit.wait);   /* activate unit                                */
    return SCPE_OK;
}

static char cpmCommandLine[CPM_COMMAND_LINE_LENGTH];
static void createCPMCommandLine(void) {
    int32 i, len = (GetBYTEWrapper(0x80) & 0x7f); /* 0x80 contains length of command line, discard first char */
    for (i = 0; i < len - 1; i++)
        cpmCommandLine[i] = (char)GetBYTEWrapper(0x82 + i); /* the first char, typically ' ', is discarded */
    cpmCommandLine[i] = 0; /* make C string */
}

/* The CP/M command line is used as the name of a file and UNIT* uptr is attached to it. */
static void attachCPM(UNIT *uptr) {
    createCPMCommandLine();
    if (uptr == &ptr_unit)
        sim_switches = SWMASK('R');
    else if (uptr == &ptp_unit)
        sim_switches = SWMASK('W') | SWMASK('C');   /* 'C' option makes sure that file is properly truncated
                                                        if it had existed before                                */
    lastCPMStatus = attach_unit(uptr, cpmCommandLine);
    if ((lastCPMStatus != SCPE_OK) && (simh_unit.flags & UNIT_SIMH_VERBOSE)) {
        MESSAGE_3("Cannot open '%s' (%s).", cpmCommandLine, scp_error_messages[lastCPMStatus - SCPE_BASE]);
        /* must keep curly braces as MESSAGE_N is a macro with two statements */
    }
}

/* setClockZSDOSAdr points to 6 byte block in M: YY MM DD HH MM SS in BCD notation */
static void setClockZSDOS(void) {
    struct tm newTime;
    int32 year = fromBCD(GetBYTEWrapper(setClockZSDOSAdr));
    newTime.tm_year = year < 50 ? year + 100 : year;
    newTime.tm_mon  = fromBCD(GetBYTEWrapper(setClockZSDOSAdr + 1)) - 1;
    newTime.tm_mday = fromBCD(GetBYTEWrapper(setClockZSDOSAdr + 2));
    newTime.tm_hour = fromBCD(GetBYTEWrapper(setClockZSDOSAdr + 3));
    newTime.tm_min  = fromBCD(GetBYTEWrapper(setClockZSDOSAdr + 4));
    newTime.tm_sec  = fromBCD(GetBYTEWrapper(setClockZSDOSAdr + 5));
    ClockZSDOSDelta = mktime(&newTime) - time(NULL);
}

#define SECONDS_PER_MINUTE  60
#define SECONDS_PER_HOUR    (60 * SECONDS_PER_MINUTE)
#define SECONDS_PER_DAY     (24 * SECONDS_PER_HOUR)
static time_t mkCPM3Origin(void) {
    struct tm date;
    date.tm_year    = 77;
    date.tm_mon     = 11;
    date.tm_mday    = 31;
    date.tm_hour    = 0;
    date.tm_min     = 0;
    date.tm_sec     = 0;
    return mktime(&date);
}

/* setClockCPM3Adr points to 5 byte block in M:
    0 - 1 int16:    days since 31 Dec 77
        2 BCD byte: HH
        3 BCD byte: MM
        4 BCD byte: SS                              */
static void setClockCPM3(void) {
    ClockCPM3Delta = mkCPM3Origin()                                                                 +
    (GetBYTEWrapper(setClockCPM3Adr) + GetBYTEWrapper(setClockCPM3Adr + 1) * 256) * SECONDS_PER_DAY +
    fromBCD(GetBYTEWrapper(setClockCPM3Adr + 2)) * SECONDS_PER_HOUR                                 +
    fromBCD(GetBYTEWrapper(setClockCPM3Adr + 3)) * SECONDS_PER_MINUTE                               +
    fromBCD(GetBYTEWrapper(setClockCPM3Adr + 4)) - time(NULL);
}

static int32 simh_in(const int32 port) {
    int32 result = 0;
    switch(lastCommand) {

        case getHostFilenames:
#if UNIX_PLATFORM
            if (globValid)
                if (globPosNameList < globS.gl_pathc) {
                    if (!(result = globS.gl_pathv[globPosNameList][globPosName++])) {
                        globPosNameList++;
                        globPosName = 0;
                    }
                }
                else {
                    globValid = FALSE;
                    lastCommand = 0;
                    globfree(&globS);
                }
#elif defined (_WIN32)
            if (globValid)
                if (globFinished)
                    globValid = FALSE;
                else if (!(result = FindFileData.cFileName[globPosName++])) {
                    globPosName = 0;
                    if (!FindNextFile(hFind, &FindFileData)) {
                        globFinished = TRUE;
                        FindClose(hFind);
                        hFind = INVALID_HANDLE_VALUE;
                    }
                }
#else
            lastCommand = 0;
#endif
            break;

        case attachPTRCmd:

        case attachPTPCmd:
            result = lastCPMStatus;
            lastCommand = 0;
            break;

        case getClockZSDOSCmd:
            if (currentTimeValid)
                switch(getClockZSDOSPos) {

                    case 0:
                        result = toBCD(currentTime.tm_year > 99 ?
                            currentTime.tm_year - 100 : currentTime.tm_year);
                        getClockZSDOSPos = 1;
                        break;

                    case 1:
                        result = toBCD(currentTime.tm_mon + 1);
                        getClockZSDOSPos = 2;
                        break;

                    case 2:
                        result = toBCD(currentTime.tm_mday);
                        getClockZSDOSPos = 3;
                        break;

                    case 3:
                        result = toBCD(currentTime.tm_hour);
                        getClockZSDOSPos = 4;
                        break;

                    case 4:
                        result = toBCD(currentTime.tm_min);
                        getClockZSDOSPos = 5;
                        break;

                    case 5:
                        result = toBCD(currentTime.tm_sec);
                        getClockZSDOSPos = lastCommand = 0;
                        break;
                }
            else
                result = getClockZSDOSPos = lastCommand = 0;
            break;

        case getClockCPM3Cmd:
            if (currentTimeValid)
                switch(getClockCPM3Pos) {
                    case 0:
                        result = daysCPM3SinceOrg & 0xff;
                        getClockCPM3Pos = 1;
                        break;

                    case 1:
                        result = (daysCPM3SinceOrg >> 8) & 0xff;
                        getClockCPM3Pos = 2;
                        break;

                    case 2:
                        result = toBCD(currentTime.tm_hour);
                        getClockCPM3Pos = 3;
                        break;

                    case 3:
                        result = toBCD(currentTime.tm_min);
                        getClockCPM3Pos = 4;
                        break;

                    case 4:
                        result = toBCD(currentTime.tm_sec);
                        getClockCPM3Pos = lastCommand = 0;
                        break;
                }
            else
                result = getClockCPM3Pos = lastCommand = 0;
            break;

        case getSIMHVersionCmd:
            result = version[versionPos++];
            if (result == 0)
                versionPos = lastCommand = 0;
            break;

        case getBankSelectCmd:
            if (cpu_unit.flags & UNIT_CPU_BANKED)
                result = getBankSelect();
            else {
                result = 0;
                if (simh_unit.flags & UNIT_SIMH_VERBOSE) {
                    MESSAGE_1("Get selected bank ignored for non-banked memory.");
                }
            }
            lastCommand = 0;
            break;

        case getCommonCmd:
            if (getCommonPos == 0) {
                result = getCommon() & 0xff;
                getCommonPos = 1;
            }
            else {
                result = (getCommon() >> 8) & 0xff;
                getCommonPos = lastCommand = 0;
            }
            break;

        case hasBankedMemoryCmd:
            result = cpu_unit.flags & UNIT_CPU_BANKED ? MAXBANKS : 0;
            lastCommand = 0;
            break;

        case readStopWatchCmd:
            if (getStopWatchDeltaPos == 0) {
                result = stopWatchDelta & 0xff;
                getStopWatchDeltaPos = 1;
            }
            else {
                result = (stopWatchDelta >> 8) & 0xff;
                getStopWatchDeltaPos = lastCommand = 0;
            }
            break;

        case getHostOSPathSeparator:
#if defined (__MWERKS__) && defined (macintosh)
            result = ':';   /* colon on Macintosh OS 9  */
#elif defined (_WIN32)
            result = '\\';  /* back slash in Windows    */
#else
            result = '/';   /* slash in UNIX            */
#endif
            break;

        default:
            if (simh_unit.flags & UNIT_SIMH_VERBOSE) {
                MESSAGE_2("Undefined IN from SIMH pseudo device on port %03xh ignored.",
                    port);
            }
            result = lastCommand = 0;
    }
    return result;
}

void do_SIMH_sleep(void) {
#if defined (_WIN32)
    if ((SIMHSleep / 1000) && !sio_unit.u4) /* time to sleep and SIO not attached to a file */
        Sleep(SIMHSleep / 1000);
#else
    if (SIMHSleep && !sio_unit.u4)          /* time to sleep and SIO not attached to a file */
        usleep(SIMHSleep);
#endif
}

static int32 simh_out(const int32 port, const int32 data) {
    time_t now;
    switch(lastCommand) {

        case setClockZSDOSCmd:
            if (setClockZSDOSPos == 0) {
                setClockZSDOSAdr = data;
                setClockZSDOSPos = 1;
            }
            else {
                setClockZSDOSAdr |= (data << 8);
                setClockZSDOS();
                setClockZSDOSPos = lastCommand = 0;
            }
            break;

        case setClockCPM3Cmd:
            if (setClockCPM3Pos == 0) {
                setClockCPM3Adr = data;
                setClockCPM3Pos = 1;
            }
            else {
                setClockCPM3Adr |= (data << 8);
                setClockCPM3();
                setClockCPM3Pos = lastCommand = 0;
            }
            break;

        case setBankSelectCmd:
            if (cpu_unit.flags & UNIT_CPU_BANKED)
                setBankSelect(data & BANKMASK);
            else if (simh_unit.flags & UNIT_SIMH_VERBOSE) {
                MESSAGE_2("Set selected bank to %i ignored for non-banked memory.", data & 3);
            }
            lastCommand = 0;
            break;

        case setTimerDeltaCmd:
            if (setTimerDeltaPos == 0) {
                timerDelta          = data;
                setTimerDeltaPos    = 1;
            }
            else {
                timerDelta |= (data << 8);
                setTimerDeltaPos = lastCommand = 0;
            }
            break;

        case setTimerInterruptAdrCmd:
            if (setTimerInterruptAdrPos == 0) {
                timerInterruptHandler       = data;
                setTimerInterruptAdrPos     = 1;
            }
            else {
                timerInterruptHandler |= (data << 8);
                setTimerInterruptAdrPos = lastCommand = 0;
            }
            break;

        default:
            lastCommand = data;
            switch(data) {

                case getHostFilenames:
#if UNIX_PLATFORM
                    if (!globValid) {
                        globValid = TRUE;
                        globPosNameList = globPosName = 0;
                        createCPMCommandLine();
                        globError = glob(cpmCommandLine, GLOB_ERR, NULL, &globS);
                        if (globError) {
                            if (simh_unit.flags & UNIT_SIMH_VERBOSE) {
                                MESSAGE_3("Cannot expand '%s'. Error is %i.", cpmCommandLine, globError);
                            }
                            globfree(&globS);
                            globValid = FALSE;
                        }
                    }
#elif defined (_WIN32)
                    if (!globValid) {
                        globValid = TRUE;
                        globPosName = 0;
                        globFinished = FALSE;
                        createCPMCommandLine();
                        hFind = FindFirstFile(cpmCommandLine, &FindFileData);
                        if (hFind == INVALID_HANDLE_VALUE) {
                            if (simh_unit.flags & UNIT_SIMH_VERBOSE) {
                                MESSAGE_3("Cannot expand '%s'. Error is %lu.", cpmCommandLine, GetLastError());
                            }
                            globValid = FALSE;
                        }
                    }
#endif
                    break;

                case SIMHSleepCmd:
                    do_SIMH_sleep();
                    break;

                case printTimeCmd:  /* print time */
                    if (rtc_avail) {
                        MESSAGE_2("Current time in milliseconds = %d.", sim_os_msec());
                    }
                    else {
                        warnNoRealTimeClock();
                    }
                    break;

                case startTimerCmd: /* create a new timer on top of stack */
                    if (rtc_avail)
                        if (markTimeSP < TIMER_STACK_LIMIT)
                            markTime[markTimeSP++] = sim_os_msec();
                        else {
                            MESSAGE_1("Timer stack overflow.");
                        }
                    else warnNoRealTimeClock();
                    break;

                case stopTimerCmd:  /* stop timer on top of stack and show time difference */
                    if (rtc_avail)
                        if (markTimeSP > 0) {
                            uint32 delta = sim_os_msec() - markTime[--markTimeSP];
                            MESSAGE_2("Timer stopped. Elapsed time in milliseconds = %d.", delta);
                        }
                        else {
                            MESSAGE_1("No timer active.");
                        }
                    else warnNoRealTimeClock();
                    break;

                case resetPTRCmd:   /* reset ptr device */
                    ptr_reset(NULL);
                    break;

                case attachPTRCmd:  /* attach ptr to the file with name at beginning of CP/M command line */
                    attachCPM(&ptr_unit);
                    break;

                case detachPTRCmd:  /* detach ptr */
                    detach_unit(&ptr_unit);
                    break;

                case getSIMHVersionCmd:
                    versionPos = 0;
                    break;

                case getClockZSDOSCmd:
                    time(&now);
                    now += ClockZSDOSDelta;
                    currentTime = *localtime(&now);
                    currentTimeValid = TRUE;
                    getClockZSDOSPos = 0;
                    break;

                case setClockZSDOSCmd:
                    setClockZSDOSPos = 0;
                    break;

                case getClockCPM3Cmd:
                    time(&now);
                    now += ClockCPM3Delta;
                    currentTime = *localtime(&now);
                    currentTimeValid = TRUE;
                    daysCPM3SinceOrg = (int32) ((now - mkCPM3Origin()) / SECONDS_PER_DAY);
                    getClockCPM3Pos = 0;
                    break;

                case setClockCPM3Cmd:
                    setClockCPM3Pos = 0;
                    break;

                case getBankSelectCmd:
                case setBankSelectCmd:
                case getCommonCmd:
                case hasBankedMemoryCmd:
                case getHostOSPathSeparator:
                    break;

                case resetSIMHInterfaceCmd:
                    markTimeSP  = 0;
                    lastCommand = 0;
#if UNIX_PLATFORM
                    if (globValid) {
                        globValid = FALSE;
                        globfree(&globS);
                    }
#elif defined (_WIN32)
                    if (globValid) {
                        globValid = FALSE;
                        if (hFind != INVALID_HANDLE_VALUE) {
                            FindClose(hFind);
                        }
                    }
#endif
                    break;

                case showTimerCmd:  /* show time difference to timer on top of stack */
                    if (rtc_avail)
                        if (markTimeSP > 0) {
                            uint32 delta = sim_os_msec() - markTime[markTimeSP - 1];
                            MESSAGE_2("Timer running. Elapsed in milliseconds = %d.", delta);
                        }
                        else {
                            MESSAGE_1("No timer active.");
                        }
                    else warnNoRealTimeClock();
                    break;

                case attachPTPCmd:  /* attach ptp to the file with name at beginning of CP/M command line */
                    attachCPM(&ptp_unit);
                    break;

                case detachPTPCmd:  /* detach ptp */
                    detach_unit(&ptp_unit);
                    break;

                case setZ80CPUCmd:
                    chiptype = CHIP_TYPE_Z80;
                    break;

                case set8080CPUCmd:
                    chiptype = CHIP_TYPE_8080;
                    break;

                case startTimerInterruptsCmd:
                    if (simh_dev_set_timeron(NULL, 0, NULL, NULL) == SCPE_OK) {
                        timerInterrupt = FALSE;
                        simh_unit.flags |= UNIT_SIMH_TIMERON;
                    }
                    break;

                case stopTimerInterruptsCmd:
                    simh_unit.flags &= ~UNIT_SIMH_TIMERON;
                    simh_dev_set_timeroff(NULL, 0, NULL, NULL);
                    break;

                case setTimerDeltaCmd:
                    setTimerDeltaPos = 0;
                    break;

                case setTimerInterruptAdrCmd:
                    setTimerInterruptAdrPos = 0;
                    break;

                case resetStopWatchCmd:
                    stopWatchNow = rtc_avail ? sim_os_msec() : 0;
                    break;

                case readStopWatchCmd:
                    getStopWatchDeltaPos = 0;
                    stopWatchDelta = rtc_avail ? sim_os_msec() - stopWatchNow : 0;
                    break;

                default:
                    if (simh_unit.flags & UNIT_SIMH_VERBOSE) {
                        MESSAGE_3("Unknown command (%i) to SIMH pseudo device on port %03xh ignored.",
                            data, port);
                    }
                }
    }
    return 0x00; /* ignored, since OUT */
}

/* port 0xfe is a device for communication SIMH <--> Altair machine */
int32 simh_dev(const int32 port, const int32 io, const int32 data) {
    return io == 0 ? simh_in(port) : simh_out(port, data);
}
