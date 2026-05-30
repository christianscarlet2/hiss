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

#include "../CTransform/hash/lookup3.h"
#include "../StructsDefines/structs_defines.h"
#include "DialogTableMap.h"
#include "global.h"
#include "MainFrm.h"
#include "OpenScrape.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

// COpenScrapeDoc
IMPLEMENT_DYNCREATE(COpenScrapeDoc, CDocument)

BEGIN_MESSAGE_MAP(COpenScrapeDoc, CDocument)
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

