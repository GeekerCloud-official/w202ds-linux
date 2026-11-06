
/************************************************************************
*
* File Name: himax_firmware_config.h
*
*  *   Version: v1.0
*
************************************************************************/
#ifndef _HIMAX_FIRMWARE_CONFIG_H_
#define _HIMAX_FIRMWARE_CONFIG_H_

/********************** Upgrade ***************************

  auto upgrade, please keep enable
*********************************************************/

/*===========Himax Option function=============*/
#define HX_RST_PIN_FUNC
#define HX_EXCP_RECOVERY



/*#define HX_NEW_EVENT_STACK_FORMAT*/
/*#define HX_BOOT_UPGRADE*/
/* W202DS diagnostic: Android policy disabled: HX_SMART_WAKEUP */
/* W202DS diagnostic: Android policy disabled: HX_CONFIG_PM */
/*#define HX_GESTURE_TRACK*/
#define HX_RESUME_SEND_CMD	/*Need to enable on TDDI chipset*/
/*#define HX_HIGH_SENSE*/
/*#define HX_PALM_REPORT*/
/* W202DS diagnostic: Android policy disabled: HX_USB_DETECT_GLOBAL */
/* W202DS diagnostic: Android policy disabled: HX_HEADPHONE */

/*Horizontal and vertical screen switching*/
/* W202DS diagnostic: Android policy disabled: HX_HOR_VER_SWITCH_MODE */

/* for MTK special platform.If turning on,
 * it will report to system by using specific format.
 */
/* W202DS diagnostic: Android policy disabled: HX_REPORT_BY_ZTE_ALGO */
/*#define HX_PROTOCOL_A*/
#define HX_PROTOCOL_B_3PA

#define HX_ZERO_FLASH
/* W202DS diagnostic: Android policy disabled: HX_GET_NOISE */
/* W202DS diagnostic: Android policy disabled: HX_PEN_ONLY_SWITCH */
/*system suspend-chipset power off,
 *oncell chipset need to enable the definition
 */
/*#define HX_RESUME_HW_RESET*/

/* Sample code for TP load before LCM
 */
/*#define HX_TP_TRIGGER_LCM_RST*/

/*#define HX_PARSE_FROM_DT*/

/*Enable this if testing suspend/resume
 *on nitrogen8m
 */
/*#define HX_CONFIG_DRM_PANEL*/

/*used for self test get dsram fail in stress test*/
/*#define HX_STRESS_SELF_TEST*/

/*=============================================*/

/* Enable it if driver go into suspend/resume twice */
/*#undef HX_CONFIG_FB*/

/* Enable it if driver go into suspend/resume twice */
/*#undef HX_CONFIG_DRM*/
/* W202DS diagnostic: Android policy disabled: HXTP_DIFFER_CONFIG */

enum fix_touch_info {
	FIX_HX_RX_NUM = 50,
	FIX_HX_TX_NUM = 32,
#ifdef HXTP_DIFFER_CONFIG
	FIX_HX_RX_NUM_CONFIG = 48,
	FIX_HX_TX_NUM_CONFIG = 32,
	FIX_HX_INT_IS_EDGE_CONFIG = true,
	FIX_HX_STYLUS_FUNC_CONFIG = 0,
#endif
	FIX_HX_BT_NUM = 0,
	FIX_HX_MAX_PT = 10,
	FIX_HX_INT_IS_EDGE = true,
	FIX_HX_STYLUS_FUNC = 0,
	FIX_HX_STYLUS_ID_V2 = 0,
	FIX_HX_STYLUS_RATIO = 10,
#if defined(HX_TP_PROC_2T2R)
	FIX_HX_RX_NUM_2 = 0,
	FIX_HX_TX_NUM_2 = 0,
#endif
};

enum hxtp_vendor_id {
	HX_VENDOR_ID_0	= 0,
	HX_VENDOR_ID_1,
	HX_VENDOR_ID_2,
	HX_VENDOR_ID_3,
	HX_VENDOR_ID_MAX		= 0xFF,
};

/*
 * Numbers of modules support
 */
#define HXTS_VENDOR_0_NAME	"guoxian"
#define HXTS_VENDOR_1_NAME	"lead"
#define HXTS_VENDOR_2_NAME	"lectron"
#define HXTS_VENDOR_3_NAME	"dijing"
#ifdef HX_ZERO_FLASH
/* this macro need be configured refer to module*/
/* W202DS diagnostic: Android policy disabled: HIMAX_DEFAULT_FIRMWARE */
#endif

#endif
