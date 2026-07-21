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
	// Any tablemap edit (create/delete image|hash, change region values, nudge/drag)
	// funnels through here. Override it to redispatch the card-detection jobs so the
	// table-view overlays + Result field never show a stale/wrong hash on update.
	virtual void SetModifiedFlag(BOOL bModified = TRUE);
	virtual ~COpenScrapeDoc();
	// Tablemaps live in the PostgreSQL "hiss" database. These command handlers
	// replace the file-based File>Open/Save/Save As and add .tm import.
	CString m_tm_name;   // name (DB key) of the currently loaded tablemap
	afx_msg void OnFileOpenDb();
	afx_msg void OnFileSaveDb();
	afx_msg void OnFileSaveAsDb();
	afx_msg void OnDbImportFile();
	afx_msg void OnDbImportFolder();
	// File > Delete Tablemap from Database (with its cascaded child records).
	afx_msg void OnDbDeleteTablemap();
	// Prompt for a name and save the current tablemap to the DB under it.
	bool SaveAsToDb();
	// Tablemaps live in the DB: route the close-with-changes prompt to a DB save
	// (Save / Save As / Don't save / Cancel) instead of the file-based MFC default.
	virtual BOOL SaveModified();
	afx_msg void OnToolsSettings();
	void RefreshUiAfterLoad();
	bool SaveCurrentToDb(const CString name);
	// Load a tablemap from the hiss DB by name (no picker dialog). Used to restore
	// the last-used tablemap on startup. Returns true on success.
	bool LoadFromDbByName(const CString name);
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


