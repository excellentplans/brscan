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
//	Source filename: brother_devaccs.h
//
//		Copyright(c) 1997-2000 Brother Industries, Ltd.  All Rights Reserved.
//
//
//	Abstract:
//			Device�������������⥸�塼�롦�إå���
//
//
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
#ifndef _BROTHER_DEVACCS_H_
#define _BROTHER_DEVACCS_H_

#include "brother.h"

//
// �������Хåե�������
//
extern WORD  gwInBuffSize;

//
// Device�����������Υ����ॢ���Ȼ���
//
extern UINT  gnQueryTimeout;	// Query�ϥ��ޥ�ɤΥ쥹�ݥ󥹼������Υ����ॢ���Ȼ���
extern UINT  gnScanTimeout;		// ������󳫻ϡ����������Υ����ॢ���Ȼ���

//
// Time out���֤Υǥե����
//
#define TIMEOUT_QUERYRES  3000	// Query�ϥ��ޥ�ɤΥ쥹�ݥ󥹼������ѡ�msecñ�̡�
#define TIMEOUT_SCANNING  60	// ������󳫻ϡ�����������ѡ�secñ�̡�

#define NETTIMEOUT        3000
#define NETTIMEOUTST      {NETTIMEOUT/1000,(NETTIMEOUT%1000)*1000}


#define RETRY_CNT 5

// �٥����ͭ���ޥ�����
#define BREQ_TYPE 0xC0
#define BREQ_GET_OPEN 0x01
#define BREQ_GET_CLOSE 0x02

#define BREQ_GET_LENGTH 5

#define BDESC_TYPE 0x10

#define BCOMMAND_RETURN 0x8000

#define BCOMMAND_SCANNER 0x02

//
// �ؿ��Υץ��ȥ��������
//
void    GetDeviceAccessParam( Brother_Scanner *this );
int     OpenDevice( usb_dev_handle *hScanner, int seriesNo );
void    CloseDevice( usb_dev_handle *hScanner );
int     ReadDeviceData( usb_dev_handle *hScanner, LPSTR lpRxBuffer, int nReadSize, int seriesNo );
int     ReadNonFixedData( usb_dev_handle *hScanner, LPSTR lpBuffer, WORD wReadSize, DWORD dwTimeOut, int seriesNo );
int     WriteDeviceData( usb_dev_handle *hScanner, LPSTR lpTxBuffer, int nWriteSize, int seriesNo );
int     WriteDeviceCommand( usb_dev_handle *hScanner, LPSTR lpTxBuffer, int nWriteSize, int seriesNo );
HANDLE  AllocReceiveBuffer( DWORD  dwBuffSize );
void    FreeReceiveBuffer( void );


//
//    Try to set configuration
//
//    if false, send the CLEAR_FEATURE request
//       in order to reset the data toggle statement.
//
//    M-LNX-24   2006/04/12 kado

int  usb_set_configuration_or_reset_toggle(
		   Brother_Scanner *this,
		   int configuration);


#endif //_BROTHER_DEVACCS_H_


//////// end of brother_devaccs.h ////////
