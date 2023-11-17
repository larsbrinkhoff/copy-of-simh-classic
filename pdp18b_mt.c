/* 18b PDP magnetic tape simulator

   Copyright (c) 1993-1999, Robert M Supnik

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
   ROBERT M SUPNIK BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Except as contained in this notice, the name of Robert M Supnik shall not
   be used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from Robert M Supnik.

   mt		TC59 magnetic tape for PDP-9
		TC59D magnetic tape for PDP-15

   04-Oct-98	RMS	V2.4 magtape format
   22-Jan-97	RMS	V2.3 magtape format
   29-Jun-96	RMS	Added unit disable support

   Magnetic tapes are represented as a series of variable records
   of the form:

	32b byte count
	byte 0
	byte 1
	:
	byte n-2
	byte n-1
	32 byte count

   If the byte count is odd, the record is padded with an extra byte
   of junk.  File marks are represented by a byte count of 0.
*/

#include "pdp18b_defs.h"

#define MT_NUMDR	8				/* #drives */
#define UNIT_V_WLK	(UNIT_V_UF + 0)			/* write locked */
#define UNIT_WLK	1 << UNIT_V_WLK
#define UNIT_W_UF	2				/* saved flag width */
#define USTAT		u3				/* unit status */
#define UNUM		u4				/* unit number */
#define DBSIZE		(1 << 12)			/* max data record */
#define DBMASK		(DBSIZE - 1)
#define MT_WC		032				/* in core reg */
#define MT_MA		033

/* Command/unit - mt_cu */

#define CU_V_UNIT	15				/* unit */
#define CU_M_UNIT	07
#define CU_PARITY	0040000				/* parity select */
#define CU_DUMP		0020000				/* dump mode */
#define CU_ERASE	0010000				/* ext rec gap */
#define CU_V_CMD	9				/* command */
#define CU_M_CMD	07
#define  FN_NOP		 00
#define  FN_REWIND	 01
#define  FN_READ	 02
#define  FN_CMPARE	 03
#define  FN_WRITE	 04
#define  FN_WREOF	 05
#define  FN_SPACEF	 06
#define  FN_SPACER	 07
#define CU_IE		0000400				/* interrupt enable */
#define CU_V_TYPE	6				/* drive type */
#define CU_M_TYPE	03
#define  TY_9TK		3
#define GET_UNIT(x)	(((x) >> CU_V_UNIT) & CU_M_UNIT)
#define GET_CMD(x)	(((x) >> CU_V_CMD) & CU_M_CMD)
#define GET_TYPE(x)	(((x) >> CU_V_TYPE) & CU_M_TYPE)
#define PACKED(x)	(((x) & CU_DUMP) || (GET_TYPE (x) != TY_9TK))

/* Status - stored in mt_sta or (*) uptr -> USTAT */

#define STA_ERR		0400000				/* error */
#define STA_REW		0200000				/* *rewinding */
#define STA_BOT		0100000				/* *start of tape */
#define STA_ILL		0040000				/* illegal cmd */
#define STA_PAR		0020000				/* parity error */
#define STA_EOF		0010000				/* *end of file */
#define STA_EOT		0004000				/* *end of tape */
#define STA_CPE		0002000				/* compare error */
#define STA_RLE		0001000				/* rec lnt error */
#define STA_DLT		0000400				/* data late */
#define STA_BAD		0000200				/* bad tape */
#define STA_DON		0000100				/* done */

#define STA_CLR		0000077				/* always clear */
#define STA_DYN		(STA_REW | STA_BOT | STA_EOF | STA_EOT)
							/* kept in USTAT */
#define STA_EFLGS	(STA_BOT | STA_ILL | STA_PAR | STA_EOF | \
			 STA_EOT | STA_CPE | STA_RLE | STA_DLT | STA_BAD)
							/* error flags */

extern int32 M[];
extern int32 int_req;
extern UNIT cpu_unit;
int32 mt_cu = 0;					/* command/unit */
int32 mt_sta = 0;					/* status register */
int32 mt_time = 10;					/* record latency */
int32 mt_stopioe = 1;					/* stop on error */
t_stat mt_svc (UNIT *uptr);
t_stat mt_reset (DEVICE *dptr);
t_stat mt_attach (UNIT *uptr, char *cptr);
t_stat mt_detach (UNIT *uptr);
int32 mt_updcsta (UNIT *uptr, int32 val);
UNIT *mt_busy (void);
extern t_stat sim_activate (UNIT *uptr, int32 delay);
extern t_stat sim_cancel (UNIT *uptr);
extern int32 sim_is_active (UNIT *uptr);
extern size_t fxread (void *bptr, size_t size, size_t count, FILE *fptr);
extern size_t fxwrite (void *bptr, size_t size, size_t count, FILE *fptr);

/* MT data structures

   mt_dev	MT device descriptor
   mt_unit	MT unit list
   mt_reg	MT register list
   mt_mod	MT modifier list
*/

UNIT mt_unit[] = {
	{ UDATA (&mt_svc, UNIT_ATTABLE + UNIT_DISABLE, 0) },
	{ UDATA (&mt_svc, UNIT_ATTABLE + UNIT_DISABLE, 0) },
	{ UDATA (&mt_svc, UNIT_ATTABLE + UNIT_DISABLE, 0) },
	{ UDATA (&mt_svc, UNIT_ATTABLE + UNIT_DISABLE, 0) },
	{ UDATA (&mt_svc, UNIT_ATTABLE + UNIT_DISABLE, 0) },
	{ UDATA (&mt_svc, UNIT_ATTABLE + UNIT_DISABLE, 0) },
	{ UDATA (&mt_svc, UNIT_ATTABLE + UNIT_DISABLE, 0) },
	{ UDATA (&mt_svc, UNIT_ATTABLE + UNIT_DISABLE, 0) }  };

REG mt_reg[] = {
	{ ORDATA (STA, mt_sta, 18) },
	{ ORDATA (CMD, mt_cu, 18) },
	{ ORDATA (MA, M[MT_MA], 18) },
	{ ORDATA (WC, M[MT_WC], 18) },
	{ FLDATA (INT, int_req, INT_V_MTA) },
	{ FLDATA (STOP_IOE, mt_stopioe, 0) },
	{ DRDATA (TIME, mt_time, 24), PV_LEFT },
	{ ORDATA (UST0, mt_unit[0].USTAT, 18) },
	{ ORDATA (UST1, mt_unit[1].USTAT, 18) },
	{ ORDATA (UST2, mt_unit[2].USTAT, 18) },
	{ ORDATA (UST3, mt_unit[3].USTAT, 18) },
	{ ORDATA (UST4, mt_unit[4].USTAT, 18) },
	{ ORDATA (UST5, mt_unit[5].USTAT, 18) },
	{ ORDATA (UST6, mt_unit[6].USTAT, 18) },
	{ ORDATA (UST7, mt_unit[7].USTAT, 18) },
	{ DRDATA (POS0, mt_unit[0].pos, 31), PV_LEFT + REG_RO },
	{ DRDATA (POS1, mt_unit[1].pos, 31), PV_LEFT + REG_RO },
	{ DRDATA (POS2, mt_unit[2].pos, 31), PV_LEFT + REG_RO },
	{ DRDATA (POS3, mt_unit[3].pos, 31), PV_LEFT + REG_RO },
	{ DRDATA (POS4, mt_unit[4].pos, 31), PV_LEFT + REG_RO },
	{ DRDATA (POS5, mt_unit[5].pos, 31), PV_LEFT + REG_RO },
	{ DRDATA (POS6, mt_unit[6].pos, 31), PV_LEFT + REG_RO },
	{ DRDATA (POS7, mt_unit[7].pos, 31), PV_LEFT + REG_RO },
	{ GRDATA (FLG0, mt_unit[0].flags, 8, UNIT_W_UF, UNIT_V_UF - 1),
		  REG_HRO },
	{ GRDATA (FLG1, mt_unit[1].flags, 8, UNIT_W_UF, UNIT_V_UF - 1),
		  REG_HRO },
	{ GRDATA (FLG2, mt_unit[2].flags, 8, UNIT_W_UF, UNIT_V_UF - 1),
		  REG_HRO },
	{ GRDATA (FLG3, mt_unit[3].flags, 8, UNIT_W_UF, UNIT_V_UF - 1),
		  REG_HRO },
	{ GRDATA (FLG4, mt_unit[4].flags, 8, UNIT_W_UF, UNIT_V_UF - 1),
		  REG_HRO },
	{ GRDATA (FLG5, mt_unit[5].flags, 8, UNIT_W_UF, UNIT_V_UF - 1),
		  REG_HRO },
	{ GRDATA (FLG6, mt_unit[6].flags, 8, UNIT_W_UF, UNIT_V_UF - 1),
		  REG_HRO },
	{ GRDATA (FLG7, mt_unit[7].flags, 8, UNIT_W_UF, UNIT_V_UF - 1),
		  REG_HRO },
	{ NULL }  };

MTAB mt_mod[] = {
	{ UNIT_WLK, 0, "write enabled", "ENABLED", NULL },
	{ UNIT_WLK, UNIT_WLK, "write locked", "LOCKED", NULL }, 
	{ 0 }  };

DEVICE mt_dev = {
	"MT", mt_unit, mt_reg, mt_mod,
	MT_NUMDR, 10, 31, 1, 8, 8,
	NULL, NULL, &mt_reset,
	NULL, &mt_attach, &mt_detach };

/* IOT routine */

int32 mt (int32 pulse, int32 AC)
{
int32 f;
UNIT *uptr;

uptr = mt_dev.units + GET_UNIT (mt_cu);			/* get unit */
mt_updcsta (uptr, 0);					/* update status */
if (pulse == 001)					/* MTTR */
	return (!sim_is_active (uptr))? IOT_SKP + AC: AC;
if (pulse == 021)					/* MTCR */
	return (!mt_busy ())? IOT_SKP + AC: AC;
if (pulse == 041)					/* MTSF */
	return (mt_sta & (STA_ERR | STA_DON))? IOT_SKP + AC: AC;
if (pulse == 002) return (mt_cu & 0777700);		/* MTRC */
if (pulse == 042) return mt_sta;			/* MTRS */
if ((pulse & 062) == 022) {				/* MTAF, MTLC */
	if (!mt_busy ()) mt_cu = mt_sta = 0;		/* if not busy, clr */
	mt_sta = mt_sta & ~(STA_ERR | STA_DON);  }	/* clear flags */
if ((pulse & 064) == 024)				/* MTCM, MTLC  */
	mt_cu = (mt_cu & 0770700) | (AC & 0777700);	/* load status */
if (pulse == 004) {					/* MTGO */
	f = GET_CMD (mt_cu);				/* get function */
	if (mt_busy () || (sim_is_active (uptr)) ||
	   (((f == FN_SPACER) || (f == FN_REWIND)) & (uptr -> pos == 0)) ||
	   (((f == FN_WRITE) || (f == FN_WREOF)) && (uptr -> flags & UNIT_WLK))
	   || ((uptr -> flags & UNIT_ATT) == 0) || (f == FN_NOP))
		mt_sta = mt_sta | STA_ILL;		/* illegal op flag */
	else {	if (f == FN_REWIND) uptr -> USTAT = STA_REW;	/* rewind? */
		else mt_sta = uptr -> USTAT = 0;	/* no, clear status */
		sim_activate (uptr, mt_time);  }  }	/* start io */
mt_updcsta (mt_dev.units + GET_UNIT (mt_cu), 0);	/* update status */
return AC;
}

/* Unit service

   If rewind done, reposition to start of tape, set status
   else, do operation, set done, interrupt
*/

t_stat mt_svc (UNIT *uptr)
{
int32 c, c1, c2, c3, f, i, p, u, err;
int32 wc, xma;
t_stat rval;
t_mtrlnt tbc, cbc;
unsigned int8 dbuf[(3 * DBSIZE)];
static t_mtrlnt bceof = { 0 };

u = uptr -> UNUM;					/* get unit number */
if (uptr -> USTAT & STA_REW) {				/* rewind? */
	uptr -> pos = 0;				/* update position */
	if (uptr -> flags & UNIT_ATT) uptr -> USTAT = STA_BOT;
	else uptr -> USTAT = 0;
	if (u == GET_UNIT (mt_cu)) mt_updcsta (uptr, STA_DON);
	return SCPE_OK;  }

f = GET_CMD (mt_cu);					/* get command */
if ((uptr -> flags & UNIT_ATT) == 0) {			/* if not attached */
	mt_updcsta (uptr, STA_ILL);			/* illegal operation */
	return IORETURN (mt_stopioe, SCPE_UNATT);  }

if ((f == FN_WRITE) || (f == FN_WREOF)) {		/* write? */
	if (uptr -> flags & UNIT_WLK) {			/* write locked? */
		mt_updcsta (uptr, STA_ILL);		/* illegal operation */
		return SCPE_OK;  }
	mt_cu = mt_cu & ~CU_ERASE;  }			/* clear erase flag */

err = 0;
rval = SCPE_OK;
switch (f) {						/* case on function */

/* Unit service, continued */

case FN_READ:						/* read */
case FN_CMPARE:						/* read/compare */
	fseek (uptr -> fileref, uptr -> pos, SEEK_SET);
	fxread (&tbc, sizeof (t_mtrlnt), 1, uptr -> fileref);
	if ((err = ferror (uptr -> fileref)) ||		/* error or eof? */
	    (feof (uptr -> fileref))) {
		uptr -> USTAT = STA_EOT;
		mt_updcsta (uptr, STA_RLE);
		break;  }
	if (tbc == 0) {					/* tape mark? */
		uptr -> USTAT = STA_EOF;
		mt_updcsta (uptr, STA_RLE);
		uptr -> pos = uptr -> pos + sizeof (t_mtrlnt);
		break;  }
	tbc = MTRL (tbc);				/* ignore error flag */
	wc = DBSIZE - (M[MT_WC] & DBMASK);		/* get word count */
	cbc = PACKED (mt_cu)? wc * 3: wc * 2;		/* expected bc */
	if (tbc != cbc) mt_sta = mt_sta | STA_RLE;	/* wrong size? */
	if (tbc < cbc) {				/* record small? */
		cbc = tbc;				/* use smaller */
		wc = PACKED (mt_cu)? ((tbc + 2) / 3): ((tbc + 1) / 2);  }
	i = fxread (dbuf, sizeof (int8), cbc, uptr -> fileref);
	for ( ; i < cbc; i++) dbuf[i] = 0;		/* fill with 0's */
	err = ferror (uptr -> fileref);
	for (i = p = 0; i < wc; i++) {			/* copy buffer */
		M[MT_MA] = (M[MT_MA] + 1) & 0777777;
		xma = M[MT_MA] & ADDRMASK;
		if (PACKED (mt_cu)) {			/* packed? */
			c1 = dbuf[p++] & 077;
			c2 = dbuf[p++] & 077;
			c3 = dbuf[p++] & 077;
			c = (c1 << 12) | (c2 << 6) | c3;  }
	    	else {	c1 = dbuf[p++];
			c2 = dbuf[p++];
			c = (c1 << 8) | c2;  }
		if ((f == FN_READ) && MEM_ADDR_OK (xma)) M[xma] = c;
		else if ((f == FN_CMPARE) && (c != (M[xma] &
			(PACKED (mt_cu)? 0777777: 0177777)))) {
			mt_updcsta (uptr, STA_CPE);
			break;  }
		M[MT_WC] = (M[MT_WC] + 1) & 0777777;  }
	uptr -> pos = uptr -> pos + ((tbc + 1) & ~1) +
		(2 * sizeof (t_mtrlnt));
	break;
case FN_WRITE:						/* write */
	fseek (uptr -> fileref, uptr -> pos, SEEK_SET);
	wc = DBSIZE - (M[MT_WC] & DBMASK);		/* get word count */
	tbc = PACKED (mt_cu)? wc * 3: wc * 2;
	fxwrite (&tbc, sizeof (t_mtrlnt), 1, uptr -> fileref);
	for (i = p = 0; i < wc; i++) {		/* copy buf to tape */
		M[MT_MA] = (M[MT_MA] + 1) & 0777777;
		xma = M[MT_MA] & ADDRMASK;
		if (PACKED (mt_cu)) {			/* packed? */
			dbuf[p++] = (M[xma] >> 12) & 077;
			dbuf[p++] = (M[xma] >> 6) & 077;
			dbuf[p++] = M[xma] & 077;  }
		else {	dbuf[p++] = (M[xma] >> 8) & 0377;
			dbuf[p++] = M[xma] & 0377;  }
		M[MT_WC] = (M[MT_WC] + 1) & 0777777;  }
	fxwrite (dbuf, sizeof (char), (tbc + 1) & ~1, uptr -> fileref);
	fxwrite (&tbc, sizeof (t_mtrlnt), 1, uptr -> fileref);
	err = ferror (uptr -> fileref);
	uptr -> pos = uptr -> pos + ((tbc + 1) & ~1) +
		(2 * sizeof (t_mtrlnt));
	break;

/* Unit service, continued */

case FN_WREOF:
	fseek (uptr -> fileref, uptr -> pos, SEEK_SET);
	fxwrite (&bceof, sizeof (t_mtrlnt), 1, uptr -> fileref);
	err = ferror (uptr -> fileref);
	uptr -> pos = uptr -> pos + sizeof (t_mtrlnt);
	uptr -> USTAT = STA_EOF;
	break;
case FN_SPACEF:						/* space forward */
	wc = 01000000 - M[MT_WC];			/* get word count */
	do {	fseek (uptr -> fileref, uptr -> pos, SEEK_SET);
		fxread (&tbc, sizeof (t_mtrlnt), 1, uptr -> fileref); /* read bc */
		if ((err = ferror (uptr -> fileref)) ||	/* error or eof? */
		     feof (uptr -> fileref)) {
			uptr -> USTAT = STA_EOT;
			break;  }
		if (tbc == 0) {				/* zero bc? */
			uptr -> USTAT = STA_EOF;
			uptr -> pos = uptr -> pos + sizeof (t_mtrlnt);
			break;  }
		uptr -> pos = uptr -> pos + ((MTRL (tbc) + 1) & ~1) +
			(2 * sizeof (t_mtrlnt));  }
	while ((M[MT_WC] = (M[MT_WC] + 1) & 0777777) != 0);
	break;
case FN_SPACER:						/* space reverse */
	wc = 01000000 - M[MT_WC];			/* get word count */
	if (uptr -> pos == 0) {				/* at BOT? */
		uptr -> USTAT = STA_BOT;
		break;  }
	do {	fseek (uptr -> fileref, uptr -> pos - sizeof (t_mtrlnt),
		SEEK_SET);
		fxread (&tbc, sizeof (t_mtrlnt), 1, uptr -> fileref);
		tbc = MTRL (tbc);			/* ignore error flag */
		if ((err = ferror (uptr -> fileref)) ||	/* error or eof? */
		     feof (uptr -> fileref)) {
			uptr -> USTAT = STA_BOT;
			uptr -> pos = 0;
			break;  }
		if (tbc == 0) {				/* end of file? */
			uptr -> USTAT = STA_EOF;
			uptr -> pos = uptr -> pos - sizeof (t_mtrlnt);
			break;  }
		uptr -> pos = uptr -> pos - ((tbc + 1) & ~1) -
			(2 * sizeof (t_mtrlnt));
		if (uptr -> pos == 0) {			/* at BOT? */
			uptr -> USTAT = STA_BOT;
			break;  }  }
	while ((M[MT_WC] = (M[MT_WC] + 1) & 0777777) != 0);
	break;  }					/* end case */

/* Unit service, continued */

if (err != 0) {						/* I/O error */
	mt_updcsta (uptr, STA_PAR);			/* flag error */
	perror ("MT I/O error");
	rval = SCPE_IOERR;
	clearerr (uptr -> fileref);  }
mt_updcsta (uptr, STA_DON);				/* set done */
return IORETURN (mt_stopioe, rval);
}

/* Update controller status */

int32 mt_updcsta (UNIT *uptr, int32 new)
{
mt_sta = (mt_sta & ~(STA_DYN | STA_ERR | STA_CLR)) |
	(uptr -> USTAT & STA_DYN) | new;
if (mt_sta & STA_EFLGS) mt_sta = mt_sta | STA_ERR;	/* error flag */
if ((mt_sta & (STA_ERR | STA_DON)) && ((mt_cu & CU_IE) == 0))
	int_req = int_req | INT_MTA;
else int_req = int_req & ~INT_MTA;			/* int request */
return mt_sta;
}

/* Test if controller busy */

UNIT *mt_busy (void)
{
int32 u;
UNIT *uptr;

for (u = 0; u < MT_NUMDR; u++) {			/* loop thru units */
	uptr = mt_dev.units + u;
	if (sim_is_active (uptr) && ((uptr -> USTAT & STA_REW) == 0))
		return uptr;  }
return NULL;
}

/* Reset routine */

t_stat mt_reset (DEVICE *dptr)
{
int32 u;
UNIT *uptr;

mt_cu = mt_sta = 0;
for (u = 0; u < MT_NUMDR; u++) {			/* loop thru units */
	uptr = mt_dev.units + u;
	uptr -> UNUM = u;				/* init drive number */
	sim_cancel (uptr);				/* cancel activity */
	if (uptr -> flags & UNIT_ATT) uptr -> USTAT = STA_BOT;
	else uptr -> USTAT = 0;  }
mt_updcsta (&mt_unit[0], 0);				/* update status */
return SCPE_OK;
}

/* IORS routine */

int32 mt_iors (void)
{
return (mt_sta & (STA_ERR | STA_DON))? IOS_MTA: 0;
}

/* Attach routine */

t_stat mt_attach (UNIT *uptr, char *cptr)
{
t_stat r;

r = attach_unit (uptr, cptr);
if (r != SCPE_OK) return r;
uptr -> USTAT = STA_BOT;
mt_updcsta (mt_dev.units + GET_UNIT (mt_cu), 0);	/* update status */
return r;
}

/* Detach routine */

t_stat mt_detach (UNIT* uptr)
{
if (!sim_is_active (uptr)) uptr -> USTAT = 0;
mt_updcsta (mt_dev.units + GET_UNIT (mt_cu), 0);	/* update status */
return detach_unit (uptr);
}
