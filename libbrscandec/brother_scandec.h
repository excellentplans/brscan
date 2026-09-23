/*******************************************************************************
 *
 *	Scan Decode �إå����ե�����
 *
 *	Copyright: 2000 brother Industries , Ltd.
 *
 *	ver 1.0.0 : 2000.04.13 : ���� : ��������
 ******************************************************************************/
#ifndef	__BROTHER_SCAN_DECODE_H
#define	__BROTHER_SCAN_DECODE_H

#include "../libsane-brother/brother_dtype.h"

#include <stdint.h>

typedef	struct {
	INT		nInResoX ;		/* ���ϲ����� */
	INT		nInResoY ;		/* ���ϲ����� */
	INT		nOutResoX ;		/* ���ϲ����� */
	INT		nOutResoY ;		/* ���ϲ����� */
	INT		nColorType ;	/* ���顼���� */
	DWORD	dwInLinePixCnt ;	/* 1�饤������Pixel�� */
	INT		nOutDataKind ;	/* 24BitColor����RGB���Ϸ��� */
	BOOL	bLongBoundary ;	/* Long Boundary���� */

/* ScanDecOpen������ꤵ��� */
	DWORD	dwOutLinePixCnt ;	/* 1�饤�����Pixel�� */
	DWORD	dwOutLineByte ;		/* 1�饤�����Byte�� */
	DWORD	dwOutWriteMaxSize ;	/* ����񤭹��ߥХåե������� */

} SCANDEC_OPEN ;

typedef	struct {
	INT		nInDataComp ;		/* ���ϥǡ������̼��� */
#define	SCIDC_WHITE		1	/* ����饤�� */
#define	SCIDC_NONCOMP	2	/* �󰵽� */
#define	SCIDC_PACK		3	/* �ѥå��ӥåİ��� */

	INT		nInDataKind ;		/* ���ϥǡ�����Ǽ���� */
	CHAR	*pLineData ;	/* ���ϣ��饤��ǡ�����Ǽ�� */
	DWORD	dwLineDataSize ;	/* ���ϣ��饤��ǡ��������� */
	CHAR	*pWriteBuff ;	/* ���ϥǡ�����Ǽ�� */
	DWORD	dwWriteBuffSize ;	/* ���ϥǡ����Хåե������� */
	BOOL	bReverWrite ;	/* ���ϥǡ�����Ǽ��ˡ */

} SCANDEC_WRITE ;

BOOL	ScanDecOpen( SCANDEC_OPEN * );
void	ScanDecSetTblHandle( HANDLE, HANDLE );
BOOL	ScanDecPageStart( void );
DWORD	ScanDecWrite( SCANDEC_WRITE *, INT * );
DWORD	ScanDecPageEnd( SCANDEC_WRITE *, INT * );
BOOL	ScanDecClose( void );

typedef BOOL  (*SCANDECOPEN) (SCANDEC_OPEN * );
typedef void  (*SCANDECSETTBL) ( HANDLE, HANDLE );
typedef BOOL  (*SCANDECPAGESTART) ( void );
typedef DWORD (*SCANDECWRITE) ( SCANDEC_WRITE *, INT * );
typedef DWORD (*SCANDECPAGEEND) ( SCANDEC_WRITE *, INT * );
typedef BOOL  (*SCANDECCLOSE) ( void );

typedef unsigned char byte;
typedef unsigned int uint;
typedef unsigned short ushort;
typedef unsigned long ulong;
typedef unsigned char undefined;
typedef unsigned short undefined2;
typedef uint32_t undefined4;
typedef uint64_t undefined8;

/*
 * Ghidra decompiler helper macros (portable, no __int128 needed).
 *
 * The pattern SUB168(ZEXT816(0xaaaaaaaaaaaaaaab) * ZEXT816(x) >> shift, 0)
 * is the compiler's idiom for unsigned division by 3:
 *   >> 0x41 (65): x / 3
 *   >> 0x40 (64): x * 2 / 3
 *   >> 0x43 (67): x / 12
 */
#define DIV3(x)        ((uint64_t)(x) / 3)
#define TWO_THIRDS(x)  ((uint64_t)(x) * 2 / 3)

#endif	/* ! __BROTHER_SCAN_DECODE_H */
