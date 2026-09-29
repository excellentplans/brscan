/*

 This file is part of the Brother MFC/DCP backend for SANE.

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the Free
 Software Foundation; either version 2 of the License, or (at your option)
 any later version.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 more details.

 You should have received a copy of the GNU General Public License along with
 this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 Place, Suite 330, Boston, MA  02111-1307  USA

*/
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
//
//	Source filename: brother_log.c
//
//	Copyright(c) 1997-2000 Brother Industries, Ltd.  All Rights Reserved.
//
//
//	Abstract:
//			�����ե���������⥸�塼��
//
//
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdarg.h>

#include "brother_devaccs.h"
#include "brother_misc.h"

#include "brother_log.h"

int g_sane_debug_dll = 0;

//
// �����ե����롦�⥸�塼����������ѿ�
//
static int  nLogFile = 1;
static int  nNewLog  = 1;

static HANDLE  hLogFile = 0;

#ifndef BROTHER_SANE_DIR
#define BROTHER_SANE_DIR "/usr/share/sane/brother/"
#endif


//-----------------------------------------------------------------------------
//
//	Function name:	WriteLogFileString
//
//
//	Abstract:
//		����ʸ���������ե�����˽��Ϥ���
//
//
//	Parameters:
//		lpszLogStr
//			����ʸ����ؤΥݥ���
//
//
//	Return values:
//		�ʤ�
//
//-----------------------------------------------------------------------------
//	WriteLogFileString�ʵ�LogString_file��
void
WriteLogFileString(LPSTR lpszLogStr)
{
    if (hLogFile == 0) {
	char  szLogFile[ MAX_PATH ];

	strcpy( szLogFile, BROTHER_SANE_DIR );

	strcat( szLogFile, LOGFILENAME );
	if (nNewLog == 0){
	    hLogFile = fopen(szLogFile,"a");
	} else {
	    hLogFile = fopen(szLogFile,"w");
	}
    }

    if (hLogFile != NULL) {
	char   szStrBuff[ LOGSTRMAXLEN ];
	DWORD  dwStrLen = 0;

	time_t ltime;
	struct tm *sysTime;
	void *b_sysTime;
	if (NULL != (sysTime = malloc(sizeof(struct tm)))) {
	    b_sysTime = sysTime;
	    if (lpszLogStr != NULL) {
		time(&ltime);
		sysTime = localtime(&ltime);

		dwStrLen = sprintf(szStrBuff, "%02d:%02d:%02d.%03ld  %s\n",
				   sysTime->tm_hour, sysTime->tm_min,
				   sysTime->tm_sec, (ltime%1000), lpszLogStr);
	    } else {
		strcpy( szStrBuff, "\n" );
		dwStrLen = 2;
	    }
	    free(b_sysTime);
	}
	fwrite( szStrBuff, sizeof(char), dwStrLen, hLogFile);
    }

    CloseLogFile();
}


//-----------------------------------------------------------------------------
//
//	Function name:	WriteLog
//
//
//	Abstract:
//		����ʸ�������Ϥ���
//
//
//	Parameters:
//		����ʸ����ʽ��ա�
//
//
//	Return values:
//		�ʤ�
//
//-----------------------------------------------------------------------------
//	OutputLogString�ʵ�LogString��
static int nStderrLog = -1; /* -1 = not checked yet */

void
WriteLog( LPSTR first, ... )
{
    va_list  marker;
    char  szStrBuff[ LOGSTRMAXLEN ];

    va_start(marker, first);
    vsprintf((LPSTR)szStrBuff, first, marker);
    va_end(marker);

    /* Write to stderr if BROTHER_DEBUG env is set (check once) */
    if (nStderrLog < 0)
        nStderrLog = (getenv("BROTHER_DEBUG") != NULL);
    if (nStderrLog)
        fprintf(stderr, "[brother] %s\n", szStrBuff);

    /* Also write to log file if enabled */
    if (nLogFile) {
	WriteLogFileString( (LPSTR)szStrBuff );
    }
}


//-----------------------------------------------------------------------------
//
//	Function name:	WriteLogScanCmd
//
//
//	Abstract:
//		������ʥ��ޥ�ɤ�����˽���
//
//
//	Parameters:
//		lpszId
//			�ղ�ʸ����ؤΥݥ���
//
//		lpszCmd
//			���ޥ��ʸ����ؤΥݥ���
//
//
//	Return values:
//		�ʤ�
//
//-----------------------------------------------------------------------------
//	WriteLogScanCmd�ʵ�WriteLogBidiCmd��
void
WriteLogScanCmd( LPSTR lpszId, LPSTR lpszCmd )
{
    if(nLogFile){
	int   nCmdLen, i;
	char  szStrBuff[ LOGSTRMAXLEN ];
	LPSTR lpCmdStr;

	nCmdLen = strlen(lpszCmd);

	lpCmdStr = szStrBuff;

	strcpy(szStrBuff, "[");
	lpCmdStr++;

	if (nCmdLen > 0 && *lpszCmd == 0x1B) {
	    //
	    // ESC�����ɤ�ʸ������Ѵ�
	    //

	    strcat( szStrBuff, "ESC+" );
	    nCmdLen--;
	    lpszCmd++;
	    lpCmdStr += 4;
	}
	for (i = 0 ; i < nCmdLen ; i++, lpszCmd++) {
	    if (*lpszCmd == '\n') {
		//
		// LF�����ɤ򥹥ڡ������Ѵ�
		//
		*lpCmdStr++ = ' ';
	    }else if ((BYTE)*lpszCmd == (BYTE)0x80){
		//
		// Scanner Command Terminator�ʤ�롼�׽�λ
		//
		break;
	    } else if (' ' <= *lpszCmd && (BYTE)*lpszCmd < 0x80) {
		//
		// Printable������
		//
		*lpCmdStr++ = *lpszCmd;
	    } else {
		//
		// ����¾�Υ�����
		//
		*lpszCmd++ = '.';
	    }
	}
	*lpCmdStr++ = ']';
	*lpCmdStr   = '\0';

	//
	// ����ʸ��������
	//
	WriteLog( "%s %s", lpszId, szStrBuff );
    }
}

//-----------------------------------------------------------------------------
//
//	Function name:	CloseLogFile
//
//
//	Abstract:
//		�����ե�����򥯥�����
//
//
//	Parameters:
//		�ʤ�
//
//
//	Return values:
//		�ʤ�
//
//-----------------------------------------------------------------------------
//
void CloseLogFile( void )
{
    if(hLogFile) {
	fclose(hLogFile);
	hLogFile = NULL;
    }
}


//-----------------------------------------------------------------------------
//
//	Function name:	GetLogSwitch
//
//
//	Abstract:
//		����ͭ����̵�������INI�ե����뤫���������
//
//
//	Parameters:
//		�ʤ�
//
//
//	Return values:
//		�ʤ�
//
//-----------------------------------------------------------------------------
//
void
GetLogSwitch( Brother_Scanner *this )
{
    nLogFile = this->modelConfig.bLogFile;
    nNewLog  = 1;
}


//////// end of brother_log.c ////////
