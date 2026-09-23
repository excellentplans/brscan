//#include "build.h"

// TWAIN カラーマッチング処理 ヘッダファイル 
// Create 97.1.8  Brother Systems T.YUGUCHI

#ifndef _H_BRCOLOR_
#define _H_BRCOLOR_

#define HWND void *

//---------------------------------------------------------------------- 定数定義
// TWAIN実行モード(カラーマッチング初期化処理実行時に指定）
#define	TWAIN_SCAN_MODE				0	//TWAIN実行  通常スキャナーモード
#define TWAIN_COPY_MODE				1	//TWAIN実行  ＰＣ ＣＯＰＹ モード

// マッチング対象データ ＲＧＢデータ並び
#define CMATCH_DATALINE_RGB			0	// Red , Green , Blue
#define CMATCH_DATALINE_BGR			1	// Blue , Green, Red

// カラーマッチング種別(カラーマッチング処理実行時に指定）
#define CMATCH_KIND_GAMMA			0	//γカーブ補正
#define CMATCH_KIND_LUT				1	//LUTによるカラーマッチング
#define CMATCH_KIND_MONITOR			2	//モニターキャリブレーション

// マッチング用テーブル領域サイズ
#define	GAMMA_TABLE_NAME			"BrGamma.dat"	
													//γカーブ定義テーブル名
#define GAMMA_TABLE_ID				"BSGT"			//γカーブ定義テーブルID
#define TABLE_GAMMA_SIZE			768 			//γカーブ定義テーブル
													//γデータサイズ 256*3(byte)	

#define MONITOR_GAMMA_TABLE_NAME	"BrMonCal.dat"
													//モニターキャリブレーションγ値テーブル名
#define MONITOR_GAMMA_ID			"BRMC"			//モニターキャリブレーションγ値テーブルID

#define LUT_FILE_NAME				"BrLutCm.dat"
													//LUTデータ定義ファイル名
#define LUT_FILE_ID					"BLCM"			//LUTデータ定義ファイル

#define LUT_KIND_SCAN				"SLUT"			//通常スキャナー処理用ＬＵＴ
#define LUT_KIND_COPY				"CLUT"			//PC COPY用ＬＵＴ

#define LUT_DEFINE_FILE				"BrLutDef.Dat"
													//LUT選択用定義ファイル
#define LUT_DEFINE_FILE_ID			"BRLD"			//LUT選択用定義ファイルID

//メディアタイプ定義
//現在使用していない使い方
#define MEDIA_PLAIN					0				//PLAIN PAPER
#define	MEDIA_COTED					1				//COTED PAPER 320
#define	MEDIA_COTED720				2				//COTED PAPER 720
#define	MEDIA_GROSSY				3				//GROSSY
#define	MEDIA_OHP					4				//OHP
#define MEDIA_OHPMIRROR				5				//OHP MIRROR
#define	MEDIA_PHOTO					6				//PHOTO
//こちらが現在の使い方
#define	MEDIA_STD					0				//standard paper
//#define	MEDIA_PHOTO					6				//photo paper
#define	MEDIA_FB_STD				MEDIA_STD		//FB	standard paper
#define	MEDIA_FB_PHOTO				MEDIA_PHOTO		//FB	photo paper
#define	MEDIA_ADF_STD				7				//ADF	standard paper
#define	MEDIA_ADF_PHOTO				8				//ADF	photo paper

//ドキュメントモード定義
#define	DOCMODE_AUTO				0				//Document Mode AUTO
#define	DOCMODE_GRAPH				1				//Document Mode GRAPH
#define	DOCMODE_PHOTO				2				//Document Mode PHOTO
#define	DOCMODE_CUSTOM				3				//Document Mode Custom

//カラーマットングモード			
#define	COLOR_VIVID					0				//VIVID
#define	COLOR_MATCHSCREEN			1				//MATCH_SCREEN

//ICM(Win95)制御用
//#define ICM_OFF					0				//ICM Off
#define ICM_THROUGH					1				//ICM On
#define	ICM_MANUAL					2				//ICM Manual				

//プリンタードライバー名
#define MFC_PRINTER_NAME			"Brother MFC-7000 Series"
#define MC_PRINTER_NAME				"Brother MC3000"

//INIファイル名
#define MFC_INIFILE_NAME			"Brmfc97c.ini"
#define MC_INIFILE_NAME				"Brmc97c.ini"

//機種ＩＤ
#define	MFC7000						0				//MFC-7000シリーズ
#define MC3000						1				//MC3000 (3in1)

//標準ＬＵＴ ID
#define DEFAULT_LUT					"DLUT"
#define DEFAULT_PHOTO_LUT			"PHTO"
#define ADF_STD_LUT					"DADF"
#define ADF_PHOTO_LUT				"PADF"
//--------------------------------------------------------------------- 構造体定義
#pragma pack(1)

typedef struct{					//カラーマッチング初期化モード指定構造体：ファイル名付き
	int		nRgbLine;			//RGBデータ並び	  (BGR  or RGB)
	int		nPaperType;			//原稿 紙種別
	int		nMachineId;			//機種ID
	LPSTR	lpLutName;			//Lut Name
}CMATCH_INIT;	

typedef struct{										//モニターキャリブレーションγ値構造体
	char	FileID[4];			//'BRMC'
	short	BlackPoint[4];		//index[0,1,2,3]=[r,g,b,reserved]
	float	Gamma[4];			//index[0,1,2,3]=[r,g,b,reserved]
	short	Flag;				//default Flag
}BRMONCALDAT; 

typedef struct{										//LUTデータ構造体
	short	sLutVer;			//lut version
}BRLUT_HEAD_DATA_VER;

typedef struct{										//LUTデータ構造体

	float	s3D_Xmin;			//3D DATA X最小値
	float	s3D_Xmax;			//3D DATA X最大値
	float	s3D_Ymin;			//3D DATA Y最小値
	float	s3D_Ymax;			//3D DATA Y最大値
	float	s3D_Zmin;			//3D DATA Z最小値
	float	s3D_Zmax;			//3D DATA Z最大値

	short	s3D_Xsplit;			//3D DATA X方向分割数
	short	s3D_Ysplit;			//3D DATA Y方向分割数
	short	s3D_Zsplit;			//3D DATA Z方向分割数

}BRLUT_HEAD_DATA_00;

typedef struct{										//LUTデータ定義ファイルテーブルヘッダ情報構造体
	char	TableID[4];			//テーブル識別コード
	long	lTableOffset;		//LUTデータ開始オフセット値
}BRLUT_FILE_HEAD;

typedef struct{										//LUTカラーマッチング対象データ情報

	short	sIndex_X;			//LUT参照インデックス Ｘ
	short	sIndex_Y;			//LUT参照インデックス Ｙ
	short	sIndex_Z;			//LUT参照インデックス Ｚ
}BRLUT_INPUT_DATA;

typedef struct{										//LUT代表点(測色データ）格納構造体
	float	fExpPoint_X[8];		//代表点 Ｐ Ｘ０〜Ｘ７
	float	fExpPoint_Y[8];		//代表点 Ｐ Ｙ０〜Ｙ７
	float	fExpPoint_Z[8];		//代表点 Ｐ Ｚ０〜Ｚ７
}BRLUT_EXP_POINT;

typedef struct{										//LUT代表点（論理データ）格納構造体
	float	fLogicPoint_X[8];
	float	fLogicPoint_Y[8];
	float	fLogicPoint_Z[8];
}BRLUT_LOGIC_POINT;

typedef struct{										//LUT代表点データ格納構造体
	float	fLutXData;			//代表点 Ｘ方向データ
	float	fLutYData;			//代表点 Ｙ方向データ
	float	fLutZData;			//代表点 Ｚ方向データ
}BRLUT_DATA;

typedef struct{										//LUT選択用データ構造体
	short	nScanMode;			//スキャナーモードﾞ（SCAN or COPY)
	short	nInputMedia;		//入力メディア（紙タイプ）
	short	nOutputMedia;		//出力メディア
	short	nDocMode;			//ドキュメントモード
	short	nColorMatch;		//VIVID or MATCHSCREEN
	short	nIcmControl;		//ICM モード
}BRLUT_SELECT;	

typedef struct{										//Hash Table Data用構造体
	BYTE	yColorX;
	BYTE	yColorY;
	BYTE	yColorZ;
}COLOR_HASH_DAT;

typedef struct{										//Hash Table キー用構造体
	long			nHashKey;	//Hash Key
	COLOR_HASH_DAT	*pColorDat;	//Hash Data Pointer
}COLOR_HASH_KEY;
#pragma pack()

//--------------------------------------------------------------------- 関数宣言
//---------------------------------------------------------------外部公開 関数宣言
//カラーマッチング初期化処理
BOOL ColorMatchingInit(CMATCH_INIT matchingInitDat);
//カラーマッチング終了処理
void ColorMatchingEnd(void);
//カラーマッチング処理
BOOL ColorMatching(BYTE *pRgbData, long lRgbDataLength, long lLineCount);

//カラーマッチング初期化処理
typedef BOOL (*COLORINIT)(CMATCH_INIT);
//カラーマッチング終了処理
typedef void (*COLOREND)(void);
//カラーマッチング処理
typedef BOOL (*COLORMATCHING)(BYTE *, long , long );

#endif // _H_BRCOLOR_
