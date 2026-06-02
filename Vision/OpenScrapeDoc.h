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

// OpenScrapeDoc.h : interface of the COpenScrapeDoc class
//
#pragma once
#include "../CTransform/CTransform.h"

//!  Container class for Tablemap data to aid with loading/saving
class COpenScrapeDoc : public CDocument
{
protected: // create from serialization only
	COpenScrapeDoc();
	DECLARE_DYNCREATE(COpenScrapeDoc)
	DECLARE_MESSAGE_MAP()
	virtual BOOL OnOpenDocument(LPCTSTR lpszPathName);
	bool		valid_open;
	bool		is_dirty;
	
public:
	static COpenScrapeDoc* GetDocument();
	virtual BOOL OnNewDocument();
	virtual BOOL OnSaveDocument(LPCTSTR lpszPathName);
	virtual void Serialize(CArchive& ar);
	virtual ~COpenScrapeDoc();
	// Tablemaps live in the PostgreSQL "hiss" database. These command handlers
	// replace the file-based File>Open/Save/Save As and add .tm import.
	CString m_tm_name;   // name (DB key) of the currently loaded tablemap
	afx_msg void OnFileOpenDb();
	afx_msg void OnFileSaveDb();
	afx_msg void OnFileSaveAsDb();
	afx_msg void OnDbImportFile();
	afx_msg void OnDbImportFolder();
	afx_msg void OnToolsSettings();
	void RefreshUiAfterLoad();
	bool SaveCurrentToDb(const CString name);
	// Blink state
	bool		blink_on;
	// Window that we are attached to
	HWND		attached_hwnd;
	RECT		attached_rect;
	HBITMAP		attached_bitmap;
	BYTE		*attached_pBits;
#ifdef _DEBUG
	virtual void AssertValid() const;
	virtual void Dump(CDumpContext& dc) const;
#endif
};


