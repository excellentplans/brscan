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

#ifndef _H_BROTHER
#define _H_BROTHER

/* ======================================================================

brother.h

SANE backend master module.

Definitions ported from "scantool.h" 5.4.2001.

(C) Marian Matthias Eichholz 2001

Start: 2.4.2001

====================================================================== */

#define DEBUG_SCAN     0x0001
#define DEBUG_COMM     0x0002
#define DEBUG_ORIG     0x0004
#define DEBUG_BASE     0x0011
#define DEBUG_DEVSCAN  0x0012
#define DEBUG_REPLAY   0x0014
#define DEBUG_BUFFER   0x0018
#define DEBUG_SIGNALS  0x0020
#define DEBUG_CALIB    0x0040

#define DEBUG_CRITICAL 1
#define DEBUG_VERBOSE  2
#define DEBUG_INFO     3
#define DEBUG_JUNK     5

#define SCANNER_VENDOR     0x04F9		// Brother Vendor ID

/* ====================================================================== */

#include <sane/sane.h>
#include <usb.h>

#include "brother_dtype.h"

#include "brother_scandec.h"
#include "brcolor.h"

#include "brother_modelinf.h"
#include "brother_mfcinfo.h"

#include "brother_netdev.h"

typedef SANE_Status TState;


typedef struct TScanState {
  BOOL           bEOF;         /* EOF marker for sane_read */
  BOOL           bCanceled;    /* Cancel flag */
  BOOL           bScanning;    /* block is active? */
  int		 iProcessEnd;  /* End marker for ProcessMain */
  BOOL           bReadbufEnd;  /* Read buffer Receive End */
  int            nPageCnt;     /* Page count */
} TScanState;


#ifndef INSANE_VERSION

typedef enum { optCount,		// 0
	       optGroupMode,		// 1
	       optMode, optResolution,  // 2, 3
	       optScanSrc,              // 4
	       optBrightness, optContrast,  // 5, 6
	       optGroupGeometry,optTLX, optTLY, optBRX, optBRY, // 7, 8, 9, 10, 11
	       optLast } TOptionIndex; // 12

#define NUM_OPTIONS optLast

typedef union
  {
    SANE_Word w;
    SANE_Word *wa;              /* word array */
    SANE_String s;
  }
TOptionValue;

typedef struct TDevice {
  struct TDevice        *pNext;
  struct usb_device     *pdev;
  int                   index;
  SANE_Device            sane;
  MODELINF               modelInf;
} TDevice;

extern TDevice *g_pdev;

#endif


//
// UI��Frontend������
//
typedef struct tagUISETTING {
	WORD          wResoType;		// �����٥�����
	WORD          wColorType;		// ���顼������
	int           nBrightness;		// Brightness����
	int           nContrast;		// Contrast����
	RESOLUTION    UserSelect;		// ���򤵤줿������
	AREARECT      ScanAreaMm;		// ��������ϰϻ����0.1mmñ�̡�
	AREARECT      ScanAreaDot;		// ��������ϰϻ����dotñ�̡�
	RESOLIST      ResoList;			// UI��β�����������ܥꥹ��
	SCANMODELIST  ScanModeList;		// UI��Υ������⡼��������ܥꥹ��
	SCANSRCLIST   ScanSrcList;		// UI��Υ�����󥽡���������ܥꥹ��
	int           nSrcType;			// 06/02/28  ���򤵤줿������󥽡���
} UISETTING, *LPUISETTING;

//
// ����������������󡿥������ѥ�᡼��
//
typedef struct tagSCANINFO {
	RESOLUTION  UserSelect;			// ���򤵤줿������
	AREARECT    ScanAreaMm;			// ��������ϰϻ����mmñ�̡�
	AREARECT    ScanAreaDot;		// ��������ϰϻ����dotñ�̡�
	AREASIZE    ScanAreaSize;		// �ɤ߼���ϰϡʥɥåȿ���
	AREASIZE    ScanAreaByte;		// �ɤ߼���ϰϡʥХ��ȿ���
} SCANINFO, *LPSCANINFO;

typedef struct TScanDec {
  void              *hScanDec;               // ���̥ǡ���Ÿ���饤�֥��Υϥ�ɥ�
  SCANDECOPEN        lpfnScanDecOpen;        // ���̥ǡ���Ÿ���饤�֥��Υ����ץ�ؿ�
  SCANDECSETTBL      lpfnScanDecSetTbl;      // ���̥ǡ���Ÿ���饤�֥��Υơ��֥륻�åȴؿ�
  SCANDECPAGESTART   lpfnScanDecPageStart;   // ���̥ǡ���Ÿ���饤�֥��γ��Ͻ����ؿ�
  SCANDECWRITE       lpfnScanDecWrite;       // ���̥ǡ���Ÿ���饤�֥��ν񤭹��ߴؿ�
  SCANDECPAGEEND     lpfnScanDecPageEnd;     // ���̥ǡ���Ÿ���饤�֥��ν�λ�����ؿ�
  SCANDECCLOSE       lpfnScanDecClose;       // ���̥ǡ���Ÿ���饤�֥��Υ��������ؿ�
} TScanDec;


// DSCMATCH.C���������Ƥ��볰���ѿ���¤�ΤȤ������
typedef struct TCorlorMATCH {
  void              *hColorMatch;           // ���顼�ޥå��󥰥饤�֥��Υϥ�ɥ�
  COLORINIT          lpfnColorMatchingInit; // ���顼�ޥå��󥰥饤�֥��ν�����ؿ�
  COLOREND           lpfnColorMatchingEnd;  // ���顼�ޥå��󥰥饤�֥��ν�λ�ؿ�
  COLORMATCHING      lpfnColorMatchingFnc;  // ���顼�ޥå��󥰥饤�֥��μ¹Դؿ�
  int                nColorMatchStatus;     // ���顼�ޥå��󥰥��ơ�����
  char               szTwdsColorMatch[ MAX_PATH ]; // ���顼�ޥå��󥰥饤�֥��Υե�ѥ�̾
  char               szLutFilePathName[ MAX_PATH ];// ���顼�ޥå��󥰤�LUT�ե�����ե�ѥ�̾
  void              *hGrayTbl;              // GrayTable�Υϥ�ɥ�
} TCorlorMATCH;

typedef struct tagDEVHANDLE {
  br_net_dev_handle      net;
  usb_dev_handle   *usb;
  int              device;
  int              net_device_index;
  int              usb_w_ep;
  int              usb_r_ep;
} dev_handle;


struct brscan5_ops;	/* ops dispatch handle — see brother_brscan5.h */
typedef struct brscan5_session brscan5_session_t;	/* brscan5 session — see brother_brscan5.h */
typedef struct Brother_Scanner {
#ifndef INSANE_VERSION
  struct Brother_Scanner    *pNext;
  SANE_Option_Descriptor     aoptDesc[NUM_OPTIONS];
  TOptionValue               aoptVal[NUM_OPTIONS];
#endif
  const struct brscan5_ops  *ops;		/* dispatch handle: brscan5 vs. 3/4 path */
  brscan5_session_t         *br5;		/* brscan5 session state (brscan5 models only) */
  MODELINF                   modelInf;		// �����ץ󤵤줿�ǥХ�������
  MODELCONFIG                modelConfig;       // �����ץ󤵤줿�ǥХ����γƼ��������

  dev_handle                 *hScanner;		// USB������ʤΥϥ�ɥ�

  UISETTING                  uiSetting;         //
  TCorlorMATCH               cmatch;            // ���顼�ޥå����ѹ�¤��
  TScanDec                   scanDec;           // ���̥ǡ���Ÿ���ѹ�¤��
  SCANINFO                   scanInfo;          // ���������ξ���
  DEVSCANINFO                devScanInfo;       // �ǥХ����Υ���������
  MFCMODELINFO               mfcModelInfo;      //
  MFCDEVICEHEAD              mfcDevInfoHeader;  //
  MFCDEVICEINFO              mfcDeviceInfo;     //
  TScanState                 scanState;         // ���������Υ��ơ���������
} Brother_Scanner;

#define  usb_dev_handle   dev_handle

// from DS_INFO.H
//
// �����٥������ѥ���ܥ����
//
#define RESOTYPECNT   13				// �����٥���������
#define RES100X100     0				//  100 x  100 dpi
#define RES150X150     1				//  150 x  150 dpi
#define RES200X100     2				//  200 x  100 dpi
#define RES200X200     3				//  200 x  200 dpi
#define RES200X400     4				//  200 x  400 dpi
#define RES300X300     5				//  300 x  300 dpi
#define RES400X400     6				//  400 x  400 dpi
#define RES600X600     7				//  600 x  600 dpi
#define RES800X800     8				//  800 x  800 dpi
#define RES1200X1200   9				// 1200 x 1200 dpi
#define RES2400X2400  10				// 2400 x 2400 dpi
#define RES4800X4800  11				// 4800 x 4800 dpi
#define RES9600X9600  12				// 9600 x 9600 dpi
#define DEF_RESOTYPE  RES200X200		// �����٥����׽����

//
// ���顼�������ѥ���ܥ����
//
#define COLORTYPECNT   7				// ���顼����������
#define COLOR_BW       0				// Black & White
#define COLOR_ED       1				// Error Diffusion Gray
#define COLOR_DTH      2				// Dithered Gray
#define COLOR_TG       3				// True Gray
#define COLOR_256      4				// 256 Color
#define COLOR_FUL      5				// 24bit Full Color
#define COLOR_FUL_NOCM 6				// 24bit Full Color(ColorMatch�ʤ�)
#define DEF_COLORTYPE  COLOR_BW			// ���顼�����׽����

//
// ������󥽡����ѥ���ܥ����
//
//06/02/28 Duplex��������Ѥ�SCANSRC_ADF_DUP�ɲ�
#define SCANSRCCNT      3				// ������󥽡�������
#define SCANSRC_FB      0				// Flatbed
#define SCANSRC_ADF     1				// Automatic document feeder
#define SCANSRC_ADF_DUP 2				// ADF Duplex
#define DEF_SCANSRC    SCANSRC_ADF			// ������󥽡��������

//
// Brightness/Contrast�ѥ���ܥ����
//
#define DEF_BRIGHTNESS     0			// Brightness�����
#define MIN_BRIGHTNESS   -50			// Brightness�Ǿ���
#define MAX_BRIGHTNESS    50			// Brightness������
#define DEF_CONTRAST       0			// Contrast�����
#define MIN_CONTRAST     -50			// Contrast�Ǿ���
#define MAX_CONTRAST      50			// Contrast������

//
// ���ƥ������������ѥ���ܥ����
//
#define PAPERTYPECNT   8				// ���ƥ���������������
#define PAPER_A4       0				// A4
#define PAPER_B5       1				// B5
#define PAPER_LETTER   2				// US-Letter
#define PAPER_LEGAL    3				// Legal
#define PAPER_A5       4				// A5
#define PAPER_EXEC     5				// Executive
#define PAPER_BCARD    6				// ̾��
#define PAPER_USER     7				// �桼��������

//
// ���ƥ�����(0.1mmñ��)�ѥ���ܥ����
//
#define PSIZE_A4_X      2100			// A4��(0.1mmñ��)
#define PSIZE_A4_Y      2970			// A4Ĺ��(0.1mmñ��)
#define PSIZE_B5_X      1820			// B5��(0.1mmñ��)
#define PSIZE_B5_Y      2570			// B5Ĺ��(0.1mmñ��)
#define PSIZE_LETTER_X  2159			// Letter��(0.1mmñ��)
#define PSIZE_LETTER_Y  2794			// LetterĹ��(0.1mmñ��)
#define PSIZE_LEGAL_X   2159			// Legal��(0.1mmñ��)
#define PSIZE_LEGAL_Y   3556			// LegalĹ��(0.1mmñ��)
#define PSIZE_A5_X      1480			// A5��(0.1mmñ��)
#define PSIZE_A5_Y      2100			// A5Ĺ��(0.1mmñ��)
#define PSIZE_EXEC_X    1842			// Executive��(0.1mmñ��)
#define PSIZE_EXEC_Y    2667			// ExecutiveĹ��(0.1mmñ��)
#define PSIZE_BCARD_X    900			// ̾����(0.1mmñ��)
#define PSIZE_BCARD_Y    600			// ̾��Ĺ��(0.1mmñ��)
#define PSIZE_12INCH    3048			// 12inch(0.1mmñ��)

#define PSIZE_MIN_X       89			// �Ǿ���(0.1mmñ��)
#define PSIZE_MIN_Y       89			// �Ǿ�Ĺ(0.1mmñ��)
#define PSIZE_MAX_X     PSIZE_LEGAL_X	// ������(0.1mmñ��)
#define PSIZE_MAX_Y     PSIZE_LEGAL_Y	// ����Ĺ(0.1mmñ��)
#define PSIZE_BCARDWID  1200			// ̾�ɥ⡼�ɺ�����

#define PSIZE_VMARGIN     60			// �岼�ޡ�����(0.1mmñ��)
#define PSIZE_HMARGIN     20			// �����ޡ�����(0.1mmñ��)

#define BROTHER_SANE_DIR "/usr/share/sane/brother/"
#define BROTHER_GRAYCMDATA_DIR "GrayCmData/"

#endif
