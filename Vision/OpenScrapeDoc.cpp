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


// OpenScrapeDoc.cpp : implementation of the COpenScrapeDoc class
//

#include "stdafx.h"
#include "OpenScrapeDoc.h"

#include <vector>
#include "resource.h"
#include "../CTransform/hash/lookup3.h"
#include "../CTablemap/CTablemapDB.h"
#include "../StructsDefines/structs_defines.h"
#include "DialogSelectTable.h"
#include "DialogSettings.h"
#include "DialogTableMap.h"
#include "global.h"
#include "MainFrm.h"
#include "OpenScrape.h"
#include "OpenScrapeView.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

// COpenScrapeDoc
IMPLEMENT_DYNCREATE(COpenScrapeDoc, CDocument)

BEGIN_MESSAGE_MAP(COpenScrapeDoc, CDocument)
	// Tablemaps are stored in the PostgreSQL "hiss" database. Override the
	// standard File commands (they are routed to the document before the app)
	// and add the .tm import commands.
	ON_COMMAND(ID_FILE_OPEN, &COpenScrapeDoc::OnFileOpenDb)
	ON_COMMAND(ID_FILE_SAVE, &COpenScrapeDoc::OnFileSaveDb)
	ON_COMMAND(ID_FILE_SAVE_AS, &COpenScrapeDoc::OnFileSaveAsDb)
	ON_COMMAND(ID_DB_IMPORT_FILE, &COpenScrapeDoc::OnDbImportFile)
	ON_COMMAND(ID_DB_IMPORT_FOLDER, &COpenScrapeDoc::OnDbImportFolder)
	ON_COMMAND(ID_TOOLS_SETTINGS, &COpenScrapeDoc::OnToolsSettings)
END_MESSAGE_MAP()


// COpenScrapeDoc construction/destruction

COpenScrapeDoc::COpenScrapeDoc()
{
	p_tablemap->ClearTablemap();

	attached_hwnd = NULL;
	ZeroMemory(&attached_rect, sizeof(RECT));
	attached_bitmap = NULL;
	attached_pBits = NULL;
	blink_on = false;
	valid_open = false;
	is_dirty = false;
}

COpenScrapeDoc::~COpenScrapeDoc()
{
}

COpenScrapeDoc * COpenScrapeDoc::GetDocument() 
{
	CFrameWnd * pFrame = (CFrameWnd *)(AfxGetApp()->m_pMainWnd);

	return (COpenScrapeDoc *) pFrame->GetActiveDocument();
}

BOOL COpenScrapeDoc::OnNewDocument()
{
	if (!CDocument::OnNewDocument())
		return FALSE;

	p_tablemap->ClearTablemap();
	m_tm_name = "";

	if (theApp.m_TableMapDlg)
		theApp.m_TableMapDlg->create_tree();

	return TRUE;
}

BOOL COpenScrapeDoc::OnOpenDocument(LPCTSTR lpszPathName)
{
	if (!CDocument::OnOpenDocument(lpszPathName))
		return FALSE;

	if (!valid_open)
	{
		p_tablemap->ClearTablemap();
		SetTitle("");
	}
	
	SetModifiedFlag(is_dirty);

	// Create tree on TableMap dialog
	if (theApp.m_TableMapDlg)  theApp.m_TableMapDlg->create_tree();

	//???ForceRedraw();
	InvalidateRect(theApp.m_pMainWnd->GetSafeHwnd(), NULL, true);
	if (theApp.m_TableMapDlg)  theApp.m_TableMapDlg->Invalidate(true);

	if (valid_open) {
		theApp.RememberTablemap(lpszPathName);
	}

	return valid_open;
}

BOOL COpenScrapeDoc::OnSaveDocument(LPCTSTR lpszPathName)
{
	BOOL saved = CDocument::OnSaveDocument(lpszPathName);
	if (saved) {
		theApp.RememberTablemap(lpszPathName);
	}
	return saved;
}

// COpenScrapeDoc serialization

void COpenScrapeDoc::Serialize(CArchive& ar)
{
	CString			s;
	CMainFrame		*pMyMainWnd  = (CMainFrame *) (theApp.m_pMainWnd);
	int				ret;

	if (ar.IsStoring())
	{
		p_tablemap->SaveTablemap(ar, VERSION_TEXT);
	}

	else
	{
		// LoadTableMap will throw a warning on outdated versions.
		// We do no longer do this here.
		// There is also no longer any need to auto-correct old v1-TMs
		// that are older than 3 years old.
		//
		// LoadTablemap opens the file with a raw CFile and reads via CArchive,
		// neither of which is wrapped in a try/catch. If the open/read throws
		// (most commonly the file being locked/open in another program), MFC
		// would otherwise report only the useless generic "Failed to load
		// document". Catch it here and surface the actual reason.
		try
		{
			ret = p_tablemap->LoadTablemap((char *) ar.m_strFileName.GetString());
		}
		catch (CException *e)
		{
			char reason[512] = { 0 };
			e->GetErrorMessage(reason, sizeof(reason));
			s.Format("Could not load the tablemap (file/read exception):\n\n%s\n\nFile: %s\n\n"
				"This usually means the file is open or locked by another program "
				"(a text editor, another OpenScrape/Hiss/trainer instance, antivirus, a sync tool, etc.).\n"
				"Close it elsewhere and try again.",
				reason, ar.m_strFileName.GetString());
			MessageBox(pMyMainWnd->GetSafeHwnd(), s.GetString(), "Table map load error", MB_OK);
			e->Delete();
			is_dirty = false;
			valid_open = false;
			return;
		}
		catch (...)
		{
			s.Format("Could not load the tablemap due to an unexpected error.\n\nFile: %s",
				ar.m_strFileName.GetString());
			MessageBox(pMyMainWnd->GetSafeHwnd(), s.GetString(), "Table map load error", MB_OK);
			is_dirty = false;
			valid_open = false;
			return;
		}
		if (ret == SUCCESS)
		{
			valid_open = true;
		}
		else
		{
			s.Format("Error %d loading table map: %s.", ret, k_tablemap_errors_and_parse_errors_explained[ret]);
			MessageBox(pMyMainWnd->GetSafeHwnd(), s.GetString(), "Table map load error", MB_OK);
			is_dirty = false;
			valid_open = false;
		}
	}
}



// COpenScrapeDoc diagnostics

#ifdef _DEBUG
void COpenScrapeDoc::AssertValid() const
{
	CDocument::AssertValid();
}

void COpenScrapeDoc::Dump(CDumpContext& dc) const
{
	CDocument::Dump(dc);
}
#endif //_DEBUG


// COpenScrapeDoc commands

void COpenScrapeDoc::RefreshUiAfterLoad()
{
	if (theApp.m_TableMapDlg) theApp.m_TableMapDlg->create_tree();
	if (theApp.m_pMainWnd) InvalidateRect(theApp.m_pMainWnd->GetSafeHwnd(), NULL, true);
	if (theApp.m_TableMapDlg) theApp.m_TableMapDlg->Invalidate(true);
}

bool COpenScrapeDoc::SaveCurrentToDb(const CString name)
{
	int ret = p_tablemap_db->SaveTablemapToDB(name, p_tablemap);
	if (ret != SUCCESS) {
		AfxMessageBox("Failed to save tablemap '" + name + "' to the hiss database:\n"
			+ p_tablemap_db->LastError());
		return false;
	}
	m_tm_name = name;
	SetTitle(name);
	SetModifiedFlag(FALSE);
	is_dirty = false;
	return true;
}

// File > Open from Database: pick a tablemap by name and load it.
void COpenScrapeDoc::OnFileOpenDb()
{
	std::vector<STablemapDBInfo> maps;
	if (!p_tablemap_db->ListTablemaps(&maps)) {
		AfxMessageBox("Could not read tablemaps from the hiss database:\n"
			+ p_tablemap_db->LastError());
		return;
	}
	if (maps.empty()) {
		AfxMessageBox("The hiss database has no tablemaps yet.\n"
			"Use File > Import .tm File/Folder into Database first.");
		return;
	}
	CDlgSelectTable dlg;
	for (size_t i = 0; i < maps.size(); ++i) {
		STableList e;
		e.hwnd = NULL;
		e.tablemap_index = (int)i;
		e.title = maps[i].name;
		if (!maps[i].sitename.IsEmpty() || !maps[i].titletext.IsEmpty()) {
			e.title.AppendFormat("   [%s / %s]",
				maps[i].sitename.GetString(), maps[i].titletext.GetString());
		}
		dlg.tlist.Add(e);
	}
	if (dlg.DoModal() != IDOK
			|| dlg.selected_item < 0
			|| dlg.selected_item >= (int)maps.size()) {
		return;
	}
	CString name = maps[dlg.selected_item].name;
	int ret = p_tablemap_db->LoadTablemapFromDB(name, p_tablemap);
	if (ret != SUCCESS) {
		AfxMessageBox("Failed to load tablemap '" + name + "':\n"
			+ p_tablemap_db->LastError());
		valid_open = false;
		return;
	}
	m_tm_name = name;
	valid_open = true;
	is_dirty = false;
	SetTitle(name);
	SetModifiedFlag(FALSE);
	RefreshUiAfterLoad();
}

// File > Save to Database: write the current tablemap back under its name.
void COpenScrapeDoc::OnFileSaveDb()
{
	CString name = m_tm_name;
	if (name.IsEmpty()) {
		name = p_tablemap->sitename();
	}
	if (name.IsEmpty()) {
		AfxMessageBox("This tablemap has no name yet.\n"
			"Set the s$sitename symbol (it is used as the tablemap name) and try again.");
		return;
	}
	SaveCurrentToDb(name);
}

// File > Save As to Database: the new name is the s$sitename symbol.
// To rename, change s$sitename then use Save As.
void COpenScrapeDoc::OnFileSaveAsDb()
{
	CString name = p_tablemap->sitename();
	if (name.IsEmpty()) {
		name = m_tm_name;
	}
	if (name.IsEmpty()) {
		AfxMessageBox("Set the s$sitename symbol first; it is used as the tablemap name.");
		return;
	}
	if (name != m_tm_name && p_tablemap_db->TablemapExists(name)) {
		if (AfxMessageBox("A tablemap named '" + name + "' already exists. Overwrite it?",
				MB_YESNO | MB_ICONQUESTION) != IDYES) {
			return;
		}
	}
	if (SaveCurrentToDb(name)) {
		AfxMessageBox("Saved tablemap as '" + name + "'.");
	}
}

// File > Import .tm File into Database
void COpenScrapeDoc::OnDbImportFile()
{
	CFileDialog dlg(TRUE, "tm", NULL, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
		"Tablemaps (*.tm)|*.tm|All files (*.*)|*.*||");
	if (dlg.DoModal() != IDOK) {
		return;
	}
	CString name;
	if (p_tablemap_db->ImportTmFileToDB(dlg.GetPathName(), &name)) {
		AfxMessageBox("Imported\n" + dlg.GetPathName()
			+ "\ninto the hiss database as '" + name + "'.");
	} else {
		AfxMessageBox("Import failed:\n" + p_tablemap_db->LastError());
	}
}

// File > Settings...
void COpenScrapeDoc::OnToolsSettings()
{
	CDlgSettings dlg;
	dlg.DoModal();
	// Settings may have changed which fields use decimal splitting; refresh the
	// scrape view's overlay.
	POSITION pos = GetFirstViewPosition();
	CView *view = GetNextView(pos);
	if (view != NULL) {
		((COpenScrapeView *)view)->ReloadDecimalFields();
		view->Invalidate();
	}
}

// File > Import Folder of .tm into Database (recursive)
void COpenScrapeDoc::OnDbImportFolder()
{
	CFolderPickerDialog dlg(NULL, OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST, NULL);
	if (dlg.DoModal() != IDOK) {
		return;
	}
	CString folder = dlg.GetPathName();
	int imported = 0, failed = 0;
	CString report;
	p_tablemap_db->ImportFolderToDB(folder, &imported, &failed, &report);
	CString summary;
	summary.Format("Folder import complete.\n\nImported: %d\nFailed: %d\n\n%s",
		imported, failed, report.GetString());
	if (summary.GetLength() > 3000) {
		summary = summary.Left(3000) + "\n...(truncated)";
	}
	AfxMessageBox(summary);
}


