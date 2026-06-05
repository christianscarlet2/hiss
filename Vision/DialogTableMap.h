//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose:
//
//******************************************************************************


#pragma once
#include "afxcmn.h"
#include "afxwin.h"
#include "resource.h"
#include "StickyButton.h"
#include "ScrollHelper.h"
#include <vector>

// Region grouping types
enum {BY_TYPE = 1, BY_NAME = 2};

class CScrollHelper;    // Forward class declaration.

struct ImageProcessingPreset {
	CString name;
	bool use_default;
	int threshold;
	bool use_cropping;
	int crop_size;
	CString match_mode_label;
};

// CDlgTableMap dialog
class CDlgTableMap : public CDialog
{
protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support
	virtual BOOL PreTranslateMessage(MSG* pMsg);
	virtual BOOL OnInitDialog();
	virtual void OnOK();
	virtual void OnCancel();
	afx_msg void OnPaint();
	afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
	// When the selected region is in a locked group, grey-out its geometry edits
	// (nudge / rectangle / rectangle-2) and show the red "REGION LOCKED" label.
	void ApplyGroupLockState(const CString &region_name);
	afx_msg void OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);
	afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
	afx_msg void OnRegionChange();
	afx_msg void OnOcrRegionChange();
	afx_msg void OnZoomChange();
	afx_msg void OnTimer(UINT_PTR nIDEvent);   // OCR-settings live-reload probe
	afx_msg void OnTvnSelchangedTablemapTree(NMHDR *pNMHDR, LRESULT *pResult);
	afx_msg void OnDeltaposLeftSpin(NMHDR *pNMHDR, LRESULT *pResult);
	afx_msg void OnDeltaposTopSpin(NMHDR *pNMHDR, LRESULT *pResult);
	afx_msg void OnDeltaposBottomSpin(NMHDR *pNMHDR, LRESULT *pResult);
	afx_msg void OnDeltaposRightSpin(NMHDR *pNMHDR, LRESULT *pResult);
	afx_msg void OnDeltaposRadiusSpin(NMHDR *pNMHDR, LRESULT *pResult);
	afx_msg void OnDeltaposThresholdSpin(NMHDR *pNMHDR, LRESULT *pResult);
	afx_msg void OnDeltaposCropSpin(NMHDR *pNMHDR, LRESULT *pResult);
	afx_msg void OnDeltaposSharpenSpin(NMHDR *pNMHDR, LRESULT *pResult);
	afx_msg void OnBnClickedNew();
	afx_msg void OnBnClickedDelete();
	afx_msg void OnBnClickedEdit();
	afx_msg void OnBnClickedCreateGroup();
	afx_msg void OnBnClickedGroupBox();
	afx_msg void OnGroupColorChange();
	afx_msg void OnBnClickedDeletePlayerRegions();
	afx_msg void OnBnClickedDuplicateGroupToPlayer();
	afx_msg void OnBnClickedCreateHash0();
	afx_msg void OnBnClickedCreateHash1();
	afx_msg void OnBnClickedCreateHash2();
	afx_msg void OnBnClickedCreateHash3();
	afx_msg void OnBnClickedUpdateOcrNow();
	afx_msg void OnOperationsCreateHashesImages0();
	afx_msg void OnBnClickedCreateImage();
	afx_msg void OnBnClickedCreateFont();
	afx_msg void OnBnClickedFontplus();
	afx_msg void OnBnClickedFontminus();
	afx_msg void OnBnClickedNudgeTaller();
	afx_msg void OnBnClickedNudgeShorter();
	afx_msg void OnBnClickedNudgeWider();
	afx_msg void OnBnClickedNudgeNarrower();
	afx_msg void OnBnClickedNudgeBigger();
	afx_msg void OnBnClickedNudgeSmaller();
	afx_msg void OnBnClickedNudgeUpleft();
	afx_msg void OnBnClickedNudgeUp();
	afx_msg void OnBnClickedNudgeUpright();
	afx_msg void OnBnClickedNudgeRight();
	afx_msg void OnBnClickedNudgeDownright();
	afx_msg void OnBnClickedNudgeDown();
	afx_msg void OnBnClickedNudgeDownleft();
	afx_msg void OnBnClickedNudgeLeft();
	afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message);
	afx_msg LRESULT OnStickyButtonDown(WPARAM wp, LPARAM lp);
	afx_msg LRESULT OnStickyButtonUp(WPARAM wp, LPARAM lp);
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg BOOL OnToolTipText(UINT id, NMHDR* pTTTStruct, LRESULT* pResult);
	afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
	afx_msg void OnSize(UINT nType, int cx, int cy);
	afx_msg void OnMove(int x, int y);
	afx_msg void OnTvnKeydownTablemapTree(NMHDR *pNMHDR, LRESULT *pResult);
	afx_msg void OnBnClickedUseDefault();
	afx_msg void OnBnClickedUseCrop();
	afx_msg void OnBnClickedImageProcessingPresetAdd();
	afx_msg void OnBnClickedImageProcessingPresetDelete();
	afx_msg void OnBnClickedImageProcessingPresetLoad();
	HTREEITEM GetRecordTypeNode(HTREEITEM item);
	HTREEITEM GetTextSelItemAndRecordType(CString *sel_text, CString *type_text);
	void UpdateDialogTitleForSelection(CString sel_text, CString type_text, HTREEITEM type_node);
	void FocusOpenScrapeViewForRegion(CString sel_text, CString type_text);
	void clear_bitmap_control(void);
	void clear_mat_control(void);
	void draw_region_bitmap(void);
	void draw_image_bitmap(void);
	void draw_template_bitmap(void);
	void disable_and_clear_all(void);;
	void disable_ocr_and_clear_all(void);
	void update_ocr_r$_display(void);
	void update_r$_display(bool dont_update_spinners);
	void update_t$_display();
	void PopulateTemplateMatchModes(void);
	void PopulateTesseractMatchModes(void);
	void SelectTesseractMatchMode(int match_mode_value);
	int SelectedTesseractPageSegMode(void);
	COLORREF get_color_under_mouse(UINT *nFlags, CPoint *point);
	CString GetGroupName(CString regionName);
	HTREEITEM FindRegionGroupItem(HTREEITEM hRegionNode, CString groupName);
	HTREEITEM MoveTreeItem(HTREEITEM hItem, HTREEITEM hNewParent, CString name, bool bSelect);
	void RemoveSingleItemGroups(void);
	void AddNamedGroupsToTree(void);
	void PopulateDeletePlayerCombo(void);
	void PopulateDuplicatePlayerCombo(void);
	void MoveSelectedRegionBy(int dx, int dy);
	void CreateHash(int hash_type);
	void CreateHashesOfAllImages(int hash_type);
	void SaveNodeExpansionState(CArray <bool, bool> *node_state);
	void RestoreNodeExpansionState(CArray <bool, bool> *node_state);
	int GetType(CString selected_text);
	HTREEITEM InsertGroupedRegion(CString itemText);

	CStatic				m_BitmapFrame, m_MatFrame;
	CStickyButton		m_Picker;
	CStatic				m_LockedLabel;   // red "REGION LOCKED" indicator
	// Optional second/third colour cubes for the "Color" transform.
	CStickyButton		m_Picker2, m_Picker3;
	CButton				m_Color2Enable, m_Color3Enable;
	CSpinButtonCtrl		m_LeftSpin, m_TopSpin, m_BottomSpin, m_RightSpin, m_RadiusSpin, m_ThresholdSpin, m_CropSpin, m_SharpenSpin;
	CComboBox			m_Transform, m_Zoom, m_TrackerFontSet, m_TrackerFontNum, m_TrackerCardNum;
	CEdit				m_Alpha, m_Red, m_Green, m_Blue, m_RedAvg, m_GreenAvg, m_BlueAvg, m_Radius, m_Result, m_PixelSeparation, m_ImgProc, m_Threshold, m_CropSize, m_Sharpen;
	CEdit				m_Alpha2, m_Red2, m_Green2, m_Blue2;
	CEdit				m_Alpha3, m_Red3, m_Green3, m_Blue3;
	CButton				m_Rect2Enable;
	CButton				m_New, m_Delete, m_Edit, m_CreateImage, m_CreateFont, m_FontPlus, m_FontMinus;
	CButton				m_CreateGroup, m_GroupBox;
	CComboBox			m_GroupColor;
	CButton				m_DeletePlayerRegions;
	CComboBox			m_DeletePlayer;
	CButton				m_DuplicateGroupToPlayer;
	CComboBox			m_DuplicatePlayer;
	CButton				m_CreateHash0, m_CreateHash1, m_CreateHash2, m_CreateHash3;
	CButton				m_NudgeTaller, m_NudgeShorter, m_NudgeWider, m_NudgeNarrower, m_NudgeBigger, m_NudgeSmaller;
	CButton				m_NudgeUpLeft, m_NudgeUp, m_NudgeUpRight, m_NudgeRight, m_NudgeDownRight, m_NudgeDown;
	CButton				m_NudgeDownLeft, m_NudgeLeft;
	CButton				m_UseCrop, m_UseDefault;
	CButton			m_UpdateOcrNow;
	CComboBox		m_ImageProcessingPreset;
	CButton			m_AddPreset, m_DeletePreset, m_LoadPreset;
	CMenu				m_TableMapMenu;
	CPen				black_pen, green_pen, red_pen, blue_pen, white_dot_pen, null_pen;
	CBrush				white_brush, lt_gray_brush, gray_brush, red_brush, yellow_brush;
	LOGFONT				lf_fixed;
	CFont				separation_font, nudge_font;
	int					separation_font_size;
	CBitmap				picker_bitmap, drawrect_bitmap;
	HBITMAP				h_picker_bitmap, h_drawrect_bitmap;
	bool				picker_cursor;
	bool				picker_cursor2;   // eyedropper active for the 2nd colour
	bool				picker_cursor3;   // eyedropper active for the 3rd colour
	HCURSOR				hCurPicker, hCurStandard;
	bool				ignore_changes;
	CEdit			m_status_cards, m_status_fonts;

private:
        void AddImageProcessingPreset();
        void DeleteSelectedImageProcessingPreset();
        void LoadSelectedImageProcessingPreset();
        void RefreshImageProcessingPresetList();
        void SaveImageProcessingPresetsToRegistry();
        void LoadImageProcessingPresetsFromRegistry();
        ImageProcessingPreset GetCurrentImageProcessingPreset();
        void ApplyImageProcessingPreset(const ImageProcessingPreset& preset);
	RECT detectTemplate(Mat area, Mat tpl, int match_mode);
	void DetectAndShowTemplate(string name);
	CString GetDetectTemplateResult(CString area_name, CString tpl_name, RECT* rect_result);
	CString GetDetectTemplatesResult(CString area_name);
	CString get_ocr_result(Mat img_orig, CString transform, bool fast = false, CString region_name = "");
	void process_ocr(Mat img_orig, bool fast = false, bool second_pass = false);
	Mat prepareImage(Mat img_orig, bool binarize = true, int threshold = 100, bool second_pass = false);
	Mat ScaleOcrViewImage(Mat img_bounded);
	Mat binarize_array_opencv(Mat image, int threshold);
	CScrollHelper* m_scrollHelper;
	int threshold, match_mode;
	CString m_ocr_char_whitelist;	// scrub set for the region being OCR'd
	bool m_ocr_auto_threshold;		// true => Otsu auto-threshold (non-balance/text regions)
	CString m_current_ocr_model;	// model spec currently loaded into api/api2
	CString m_last_settings_revision;	// last DB settings revision seen by the probe timer
	// (Re)load the configured AutoOcr0/AutoOcr1 model for `transform` into api/api2,
	// re-Initialising only when the model actually changes. Returns false on failure.
	bool EnsureOcrModel(const CString &transform);
	Mat m_last_ocr_input;			// exact image last handed to Tesseract (for debug dump)
 public:
	// Write the most recent Tesseract input image to a file (for "Debug this field").
	void SaveLastOcrInput(const CString &path);
 protected:
	bool proceed_scroll = true;
	std::vector<ImageProcessingPreset> m_ImageProcessingPresets;

	string trim(string str) {
		return regex_replace(str, regex("\\s"), "");
	}

	float convertTofloat(const string& str) {
		float result;
		istringstream iss(str);
		iss >> result;
		return result;
	}

public:
	virtual BOOL DestroyWindow();
	CDlgTableMap(CWnd* pParent = NULL);   // standard constructor
	virtual ~CDlgTableMap();
	enum { IDD = IDD_TABLEMAP };
	void create_tree(void);
	// Flush the currently-focused edit field (coords / colour / radius-tolerance /
	// transform) into the selected region's in-memory record. Call this right before
	// saving to the DB so a value typed but not yet "committed" (e.g. saving with
	// Ctrl+S while the radius field still has focus, so its EN_KILLFOCUS never fired)
	// is not silently dropped.
	void CommitPendingEdits() { OnRegionChange(); }
	// Name of the region currently selected in the tree, or "" if the selection
	// is not a region. Used by the Settings live OCR preview.
	CString SelectedRegionName();
	void update_ocr_display(void);
	void update_display(void);
	void UpdateStatus(void);
	HTREEITEM update_tree(CString node_text);
	// Number of stored images whose hash isn't in hash-group 0 yet (i.e. how many new
	// hash records "Create hashes of all Images" would add right now).
	int CountCreatableHashes(int hash_type);
	// Refresh the "Create hashes of all Images (N)" menu caption to that live count.
	void UpdateHashMenuCount();
	void GroupRegions(void);
	void UngroupRegions(void);
	void UpdateDisplayOfAllRegions();
	// Current text shown in the Result field (for the "Debug this field" dump).
	CString GetResultText() { CString s; if (::IsWindow(m_Result.GetSafeHwnd())) m_Result.GetWindowText(s); return s; }
	HTREEITEM GetTypeNode(CString type);
	HTREEITEM FindItem(CString s, HTREEITEM start);
	void CaptureColorPresetPoint(CPoint point);
	void CaptureColorPresetColor(CPoint point);

	CTreeCtrl			m_TableMapTree;
	CEdit				m_Left, m_Top, m_Bottom, m_Right, m_xy;
	CStickyButton		m_DrawRect;
	// Optional second rectangle for the "Color" transform (public: the view writes
	// to these while drag-drawing, mirroring the rect-1 controls above).
	CEdit				m_Left2, m_Top2, m_Right2, m_Bottom2;
	CStickyButton		m_DrawRect2;
	CComboBox			m_MatchMode;
	vector<pair<Rect, CString>> ResultBoxes, ResultBoxes2;
	CString ResultString, ResultString2;
	Rect	bestRect, bestRect2;
	int		region_grouping;
	int		crop_size;
	int		sharpen;

	DECLARE_MESSAGE_MAP()
};

