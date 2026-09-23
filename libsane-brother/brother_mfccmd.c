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
//	Source filename: brother_mfccmd.c
//
//	Copyright(c) 1997-2000 Brother Industries, Ltd.  All Rights Reserved.
//
//
//	Abstract:
//			MFC������ʡ����ޥ�ɽ����⥸�塼��
//
//
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <string.h>

#include "brother_devinfo.h"
#include "brcolor.h"
#include "brother_devaccs.h"
#include "brother_misc.h"
#include "brother_log.h"

#include "brother_mfccmd.h"

extern BOOL  bTxScanCmd;

//
// Cancel���ޥ�������Ѥߥե饰
//
BOOL  bTxCancelCmd = FALSE;


//-----------------------------------------------------------------------------
//
//	Function name:	SendCancelCommand
//
//
//	Abstract:
//		����󥻥륳�ޥ�ɤ���������
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
SendCancelCommand( usb_dev_handle *hScanner, int seriesNo )
{
    if (bTxScanCmd) {
	//
	// Device ��Open����Ƥ��ơ�Scan���ޥ�������Ѥ�
	//
	if (!bTxCancelCmd) {
	    WriteDeviceCommand(hScanner, MFCMD_CANCELSCAN,
			       strlen(MFCMD_CANCELSCAN), seriesNo);
	    bTxCancelCmd = TRUE;

	    WriteLogScanCmd( "Send CANCEL command", MFCMD_CANCELSCAN );
	} else {
	    WriteLog( "Already sending CANCEL command" );
	}
    } else {
	//
	// Device ��Open����Ƥ��ʤ�����Scan���ޥ����������Ƥ��ʤ�
	//
	WriteLog("Not need to send CANCEL command");
    }
}


//-----------------------------------------------------------------------------
//
//	Function name:	MakeupScanQueryCmd
//
//
//	Abstract:
//		I-Command�Υ��ޥ��ʸ�������������
//
//
//	Parameters:
//		lpszCmdStr
//			���ޥ��ʸ����ؤΥݥ���
//
//
//	Return values:
//		���ޥ��ʸ����Ĺ
//
//-----------------------------------------------------------------------------
//
int
MakeupScanQueryCmd( Brother_Scanner *this, LPSTR lpszCmdStr )
{
	char  szCmdStrTemp[ 16 ];


	//
	// ���ޥ��ʸ����Υ��å�
	//
	strcpy( lpszCmdStr, MFCMD_QUERYSCANINFO );

	//
	// �����٥ѥ�᡼���Υ��å�
	//
	strcat( lpszCmdStr, MFCMD_RESOLUTION );

	// �����������٤Υ��å�
	sprintf( szCmdStrTemp, "%u", this->scanInfo.UserSelect.wResoX );
	strcat( lpszCmdStr, szCmdStrTemp );
	strcat( lpszCmdStr, MFCMD_SEPARATOR );

	// �����������٤Υ��å�
	sprintf( szCmdStrTemp, "%u", this->scanInfo.UserSelect.wResoY );
	strcat( lpszCmdStr, szCmdStrTemp );
	strcat( lpszCmdStr, MFCMD_LF );

	//
	// �ɤ߼��⡼�ɤΥ��å�
	//
	MakeupColorTypeCommand( this->devScanInfo.wColorType, lpszCmdStr );

	//
	// Terminater�Υ��å�
	//
	strcat( lpszCmdStr, (LPSTR)MFCMD_TERMINATOR );
	WriteLog("Resolution Command : %s",lpszCmdStr);
	return strlen( lpszCmdStr );
}


//-----------------------------------------------------------------------------
//
//	Function name:	MakeupScanStartCmd
//
//
//	Abstract:
//		X-Command�Υ��ޥ��ʸ�������������
//
//
//	Parameters:
//		lpszCmdStr
//			���ޥ��ʸ����ؤΥݥ���
//
//
//	Return values:
//		���ޥ��ʸ����Ĺ
//
//-----------------------------------------------------------------------------
//	MakeupScanStartCmd�ʵ�MakeScanCom��
int
MakeupScanStartCmd( Brother_Scanner *this, LPSTR lpszCmdStr )
{
	char  szCmdStrTemp[ 16 ];


	//
	// ���ޥ��ʸ����Υ��å�
	//
	strcpy( lpszCmdStr, MFCMD_STARTSCANNING );

	//
	// �����٥ѥ�᡼���Υ��å�
	//
	strcat( lpszCmdStr, MFCMD_RESOLUTION );

	// �����������٤Υ��å�
	sprintf( szCmdStrTemp, "%u", this->devScanInfo.DeviceScan.wResoX );
	strcat( lpszCmdStr, szCmdStrTemp );
	strcat( lpszCmdStr, MFCMD_SEPARATOR );

	// �����������٤Υ��å�
	sprintf( szCmdStrTemp, "%u", this->devScanInfo.DeviceScan.wResoY );
	strcat( lpszCmdStr, szCmdStrTemp );
	strcat( lpszCmdStr, MFCMD_LF );

	//
	// �ɤ߼��⡼�ɤΥ��å�
	//
	MakeupColorTypeCommand( this->devScanInfo.wColorType, lpszCmdStr );

	//
	// ���̥⡼�ɤΥ��å�
	//
	strcat( lpszCmdStr, MFCMD_COMPRESSION );
	if( this->modelConfig.bCompressEnbale ){
		// PackBits���̤���
		strcat( lpszCmdStr, MFCMD_COMP_PACKBITS );
	}else{
		// ����̵��
		strcat( lpszCmdStr, MFCMD_COMP_NONE );
	}

	//
	// Brightness�ѥ�᡼���Υ��å�
	//
	strcat( lpszCmdStr, MFCMD_BRIGHTNESS );
	MakePercentStr( this->uiSetting.nBrightness, szCmdStrTemp );
	strcat( lpszCmdStr, szCmdStrTemp );

	//
	// Contrast�ѥ�᡼���Υ��å�
	//
	strcat( lpszCmdStr, MFCMD_CONTRAST );
	MakePercentStr( this->uiSetting.nContrast, szCmdStrTemp );
	strcat( lpszCmdStr, szCmdStrTemp );

	//
	// ̾�ɥ⡼�ɤΥ��å�
	//
	strcat( lpszCmdStr, MFCMD_BUSINESS_OFF );

	//
	// Photo�⡼�ɤΥ��å�
	//
	strcat( lpszCmdStr, MFCMD_PHOTOMODE_OFF );

	//
	// �ɤ߼���ϰϤΥ��å�
	//
	strcat( lpszCmdStr, MFCMD_SCANNIGAREA );
	MakeDotStr( this->devScanInfo.ScanAreaDot.left, szCmdStrTemp, TRUE );
	strcat( lpszCmdStr, szCmdStrTemp );
	MakeDotStr( this->devScanInfo.ScanAreaDot.top,  szCmdStrTemp, TRUE);
	strcat( lpszCmdStr, szCmdStrTemp );

	MakeDotStr( this->devScanInfo.ScanAreaDot.right,  szCmdStrTemp, TRUE );
	strcat( lpszCmdStr, szCmdStrTemp );
	MakeDotStr( this->devScanInfo.ScanAreaDot.bottom, szCmdStrTemp, FALSE );
	strcat( lpszCmdStr, szCmdStrTemp );

	//06/02/28
	//Duplex command
	//
	if(this->modelConfig.SupportScanSrc.bit.ADF_DUP){
	  if(this->uiSetting.nSrcType ==  SCANSRC_ADF_DUP)
	    strcat(lpszCmdStr, MFCMD_SCANDUPLEX);
	  else
	    strcat(lpszCmdStr, MFCMD_SCANSINGLE);
	}

	//
	// Terminater�Υ��å�
	//
	strcat( lpszCmdStr, (LPSTR)MFCMD_TERMINATOR );

	return strlen( lpszCmdStr );
}


//-----------------------------------------------------------------------------
//
//	Function name:	MakeupColorTypeCommand
//
//
//	Abstract:
//		���顼�����ס����ޥ��ʸ�������������
//
//
//	Parameters:
//		nColorType
//			���顼�������ֹ�
//
//		lpszColorCmd
//			���顼�����ס����ޥ��ʸ����ؤΥݥ���
//
//
//	Return values:
//		�ʤ�
//
//-----------------------------------------------------------------------------
//
void
MakeupColorTypeCommand( WORD nColorType, LPSTR lpszColorCmd )
{
	strcat( lpszColorCmd, MFCMD_COLORTYPE );

	switch( nColorType ){
		case COLOR_BW:
			strcat( lpszColorCmd, MFCMD_CTYPE_TEXT );
			break;

		case COLOR_ED:
			strcat( lpszColorCmd, MFCMD_CTYPE_ERRDIF );
			break;

		case COLOR_TG:
			strcat( lpszColorCmd, MFCMD_CTYPE_GRAY64 );
			break;

		case COLOR_256:
			strcat( lpszColorCmd, MFCMD_CTYPE_8BITC );
			break;

		case COLOR_FUL:
			strcat( lpszColorCmd, MFCMD_CTYPE_24BITC );
			break;

		case COLOR_FUL_NOCM:
			strcat( lpszColorCmd, MFCMD_CTYPE_24BITC );
			break;
	}
}


//-----------------------------------------------------------------------------
//
//	Function name:	MakePercentStr
//
//
//	Abstract:
//		Brightness/Contrast�Υѥ�᡼��ʸ�������������
//
//
//	Parameters:
//		nPercent
//			Brightness/Contrast�ѥ�᡼��
//
//		lpszStr
//			�ѥ�᡼��ʸ����Хåե��ؤΥݥ���
//
//
//	Return values:
//		�ʤ�
//
//-----------------------------------------------------------------------------
//	MakePercentStr�ʵ�MakeContrastStr��
void
MakePercentStr( int nPercent, LPSTR lpszStr )
{
	int  nLength;


	//
	// -50��+50��0%��100%���Ѵ�
	//
	nPercent += 50;
	if( nPercent < 0 ){
		nPercent = 0;
	}else if( nPercent > 100 ){
		nPercent = 100;
	}
	//
	//���ޥ��ʸ������Ѵ�
	//
	sprintf( lpszStr, "%u", (WORD)nPercent );
	nLength = strlen( lpszStr );
	lpszStr += nLength;
	*lpszStr++ = MFCMD_LFCHR;
	*lpszStr   = '\0';
}


//-----------------------------------------------------------------------------
//
//	Function name:	MakeDotStr
//
//
//	Abstract:
//		��������ϰϥѥ�᡼��ʸ�������������
//
//
//	Parameters:
//		nPosition
//			��������ϰϤκ�ɸ��
//
//		lpszStr
//			�ѥ�᡼��ʸ����Хåե��ؤΥݥ���
//
//		bSeparator
//			���ѥ졼���ղåե饰
//
//
//	Return values:
//		�ʤ�
//
//-----------------------------------------------------------------------------
//
void
MakeDotStr( int nPosition, LPSTR lpszStr, BOOL bSeparator )
{
	int  nLength;


	sprintf( lpszStr, "%u", (WORD)nPosition );
	nLength = strlen( lpszStr );
	lpszStr += nLength;
	if( bSeparator ){
		*lpszStr++ = MFCMD_SEPARATORCHR;
	}else{
		*lpszStr++ = MFCMD_LFCHR;
	}
	*lpszStr   = '\0';
}

//////// end of brother_mfccmd.c ////////
