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
//	Source filename: brother_misc.c
//
//	Copyright(c) 1997-2000 Brother Industries, Ltd.  All Rights Reserved.
//
//
//	Abstract:
//			�Ƽ�ؿ���
//
//
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

#include <string.h>

#include "brother_misc.h"

//-----------------------------------------------------------------------------
//
//	Function name:	GetToken
//
//
//	Abstract:
//		����ʸ���󤫤�Token����Ф�
//
//
//	Parameters:
//		lppszData
//			in:  ʸ����ؤΥݥ��󥿤ؤΥݥ���
//			out: Token���Ф����ʸ����ؤΥݥ��󥿤���Ǽ�����
//
//
//	Return values:
//		���Ф���Token�ؤΥݥ���
//
//-----------------------------------------------------------------------------
//
LPSTR
GetToken( LPSTR *lppszData )
{
	LPSTR  lpszToken;
	LPSTR  lpszNextTop;

	lpszToken = lpszNextTop = *lppszData;

	if( lpszNextTop != NULL && *lpszNextTop ){
		while( *lpszNextTop ){
			if( *lpszNextTop == ',' ){
				*lpszNextTop++ = '\0';
				*lppszData = lpszNextTop;
				break;
			}else{
				lpszNextTop++;
			}
		}
	}else{
		lpszToken = NULL;
	}
	return lpszToken;
}


//-----------------------------------------------------------------------------
//
//	Function name:	StrToWord
//
//
//	Abstract:
//		ʸ��������(WORD)���Ѵ�
//
//
//	Parameters:
//		lpszText
//			ʸ����ؤΥݥ���
//
//
//	Return values:
//		�Ѵ����줿����
//
//-----------------------------------------------------------------------------
//
WORD
StrToWord( LPSTR lpszText )
{
	WORD   wResult = 0;
	char   chData = *lpszText;

	if( lpszText != NULL ){
		while( chData ){
			if( '0' <= chData && chData <= '9' ){
				wResult = wResult * 10 + ( chData - '0' );
			}else{
				wResult = 0;
				break;
			}
			lpszText++;
			chData = *lpszText;
		}
	}
	return wResult;
}

//////// end of brother_misc.c ////////
