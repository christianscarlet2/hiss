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
#include "DialogEdit.h"
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
	ON_COMMAND(ID_DB_DELETE_TABLEMAP, &COpenScrapeDoc::OnDbDeleteTablemap)
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

// Every tablemap mutation (create/delete an image or hash, edit a region's
// coordinates/colours/transform, nudge or drag a region) routes through
// SetModifiedFlag(true). Image-hash detection is cached per displayed frame and
// only recomputes when the cache is invalidated, so without this the table-view
// overlays + Result field keep showing the OLD (now wrong) hash until the frame
// changes -- "the message has to be recreated on update". Invalidating here and
// requesting a repaint redispatches the detection jobs on the next paint, covering
// ALL edit paths from one place. (Selection changes do NOT mark the doc modified,
// so we don't pay for detection on every click.)
void COpenScrapeDoc::SetModifiedFlag(BOOL bModified)
{
	CDocument::SetModifiedFlag(bModified);
	if (!bModified) return;
	COpenScrapeView *view = COpenScrapeView::GetView();
	if (view != NULL && ::IsWindow(view->GetSafeHwnd())) {
		view->InvalidateCardResults();   // drop stale detection cache
		view->Invalidate(FALSE);         // coalesced WM_PAINT -> recompute jobs
	}
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
	// Flush any value typed into an edit field that hasn't been committed yet (e.g.
	// saving via Ctrl+S while the radius/tolerance field still has focus, so its
	// EN_KILLFOCUS handler never ran). Otherwise that edit is silently lost on save.
	if (theApp.m_TableMapDlg) {
		theApp.m_TableMapDlg->CommitPendingEdits();
	}
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

// Load a tablemap from the hiss DB by name, no picker (startup session restore).
bool COpenScrapeDoc::LoadFromDbByName(const CString name)
{
	if (name.IsEmpty() || p_tablemap_db == NULL) {
		return false;
	}
	if (!p_tablemap_db->TablemapExists(name)) {
		return false;
	}
	if (p_tablemap_db->LoadTablemapFromDB(name, p_tablemap) != SUCCESS) {
		return false;
	}
	m_tm_name = name;
	valid_open = true;
	is_dirty = false;
	SetTitle(name);
	SetModifiedFlag(FALSE);
	RefreshUiAfterLoad();
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
		// AUTOMATION: no s$sitename requirement. Vision refuses to save until that symbol is set,
		// because a poker tablemap is identified by its site. An automation map is a click-through
		// PROCESS (lobby -> tournament row -> register -> confirm); it has no site, and demanding
		// one just to save a half-mapped process would block the normal way of working: screenshot
		// first, map regions, name it later. Ask for a name instead of refusing.
		SaveAsToDb();
		return;
	}
	SaveCurrentToDb(name);
}

// File > Save As to Database: prompt for a new tablemap name and save under it.
// Returns true if the save happened (used by the close-with-changes prompt too).
bool COpenScrapeDoc::SaveAsToDb()
{
	if (p_tablemap_db == NULL) {
		return false;
	}
	// Prompt for the name, defaulting to the current name (or s$sitename for a fresh map).
	CDlgEdit dlg;
	dlg.m_titletext = "Save tablemap as (database name)";
	CString suggested = m_tm_name;
	if (suggested.IsEmpty()) suggested = p_tablemap->sitename();
	dlg.m_result = suggested;
	if (dlg.DoModal() != IDOK) {
		return false;
	}
	CString name = dlg.m_result;
	name.Trim();
	if (name.IsEmpty()) {
		AfxMessageBox("Please enter a name for the tablemap.");
		return false;
	}
	if (name != m_tm_name && p_tablemap_db->TablemapExists(name)) {
		if (AfxMessageBox("A tablemap named '" + name + "' already exists. Overwrite it?",
				MB_YESNO | MB_ICONQUESTION) != IDYES) {
			return false;
		}
	}
	if (SaveCurrentToDb(name)) {
		AfxMessageBox("Saved tablemap as '" + name + "' in the database.");
		return true;
	}
	return false;
}

void COpenScrapeDoc::OnFileSaveAsDb()
{
	SaveAsToDb();
}

// Small modal "save changes" prompt tailored for the database (no Vista TaskDialog,
// since the project targets XP). Buttons end the dialog with a code that maps to the
// SaveModified() action.  101 = Save, 102 = Save As, 103 = Don't save, IDCANCEL = Cancel.
class CSaveChangesDlg : public CDialog {
 public:
	CString m_message;
	explicit CSaveChangesDlg(CWnd *parent = NULL)
		: CDialog(IDD_SAVECHANGES_DB, parent) {}
 protected:
	virtual BOOL OnInitDialog() {
		CDialog::OnInitDialog();
		SetDlgItemText(IDC_SC_MESSAGE, m_message);
		return TRUE;
	}
	afx_msg void OnSave()    { EndDialog(101); }
	afx_msg void OnSaveAs()  { EndDialog(102); }
	afx_msg void OnDiscard() { EndDialog(103); }
	DECLARE_MESSAGE_MAP()
};
BEGIN_MESSAGE_MAP(CSaveChangesDlg, CDialog)
	ON_BN_CLICKED(IDC_SC_SAVE, &CSaveChangesDlg::OnSave)
	ON_BN_CLICKED(IDC_SC_SAVEAS, &CSaveChangesDlg::OnSaveAs)
	ON_BN_CLICKED(IDC_SC_DISCARD, &CSaveChangesDlg::OnDiscard)
END_MESSAGE_MAP()

// Close-with-changes prompt. Tablemaps live in the hiss database, so the standard MFC
// "save to a .tm file" path is wrong here -- route the prompt to the DB and offer
// Save / Save As / Don't save / Cancel.
//   1 = Save to database, 2 = Save As to database, 3 = Don't save, 0 = Cancel.
static int PromptSaveChangesToDb(const CString &name)
{
	CSaveChangesDlg dlg(AfxGetMainWnd());
	dlg.m_message.Format("Save changes to tablemap '%s' in the database before closing?",
		name.GetString());
	switch (dlg.DoModal()) {
		case 101: return 1;
		case 102: return 2;
		case 103: return 3;
		default:  return 0;   // IDCANCEL / closed
	}
}

BOOL COpenScrapeDoc::SaveModified()
{
	if (!IsModified()) {
		return TRUE;   // nothing changed -> let the close proceed
	}
	CString name = m_tm_name.IsEmpty() ? CString("(unnamed)") : m_tm_name;
	switch (PromptSaveChangesToDb(name)) {
		case 1:   // Save to database (Save As if it has no name yet)
			if (m_tm_name.IsEmpty()) return SaveAsToDb() ? TRUE : FALSE;
			return SaveCurrentToDb(m_tm_name) ? TRUE : FALSE;
		case 2:   // Save As to database (prompts for a name)
			return SaveAsToDb() ? TRUE : FALSE;
		case 3:   // Don't save -> discard and allow close
			return TRUE;
		default:  // Cancel -> abort the close
			return FALSE;
	}
}

// File > Delete Tablemap from Database: pick a tablemap and permanently remove it
// (and all its regions/images/hashes/fonts via ON DELETE CASCADE).
void COpenScrapeDoc::OnDbDeleteTablemap()
{
	if (p_tablemap_db == NULL) return;
	std::vector<STablemapDBInfo> maps;
	if (!p_tablemap_db->ListTablemaps(&maps)) {
		AfxMessageBox("Could not read tablemaps from the hiss database:\n"
			+ p_tablemap_db->LastError());
		return;
	}
	if (maps.empty()) {
		AfxMessageBox("The hiss database has no tablemaps to delete.");
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
	CString warn;
	warn.Format("Permanently delete tablemap '%s' and ALL of its regions, images, "
		"hashes and fonts from the database?\n\nThis cannot be undone.", name.GetString());
	if (AfxMessageBox(warn, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
		return;
	}
	if (!p_tablemap_db->DeleteTablemap(name)) {
		AfxMessageBox("Failed to delete tablemap '" + name + "':\n"
			+ p_tablemap_db->LastError());
		return;
	}
	// If we just deleted the tablemap that's currently open, reset to an empty map.
	if (name.CompareNoCase(m_tm_name) == 0) {
		p_tablemap->ClearTablemap();
		m_tm_name = "";
		valid_open = false;
		SetTitle("");
		CDocument::SetModifiedFlag(FALSE);
		RefreshUiAfterLoad();
	}
	AfxMessageBox("Deleted tablemap '" + name + "' from the database.");
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
	// Modeless so the user can click regions in the scrape view while the
	// AutoOcr live preview updates. Single instance.
	if (theApp.m_SettingsDlg != NULL && ::IsWindow(theApp.m_SettingsDlg->GetSafeHwnd())) {
		theApp.m_SettingsDlg->SetForegroundWindow();
		return;
	}
	CDlgSettings *dlg = new CDlgSettings(theApp.m_pMainWnd);
	if (dlg->Create(CDlgSettings::IDD, theApp.m_pMainWnd)) {
		theApp.m_SettingsDlg = dlg;
		dlg->ShowWindow(SW_SHOW);
	} else {
		delete dlg;
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


