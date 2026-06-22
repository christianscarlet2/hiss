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

// OpenHoldem.cpp : Defines the class behaviors for the application.
//
#include "stdafx.h"
#include "OpenHoldem.h"
#include <psapi.h>
#include <windows.h>
#include "..\CTablemap\CTablemap.h"
#include "..\CTablemap\CTableMapAccess.h"
#include "CAutoConnector.h"
#include "CFormulaParser.h"
#include "CHeartbeatThread.h"
#include "CIteratorThread.h"
#include "COpenHoldemHopperCommunication.h"
#include "COpenHoldemTitle.h"
#include "CSessionCounter.h"
#include "DialogFormulaScintilla.h"
#include "MainFrm.h"
#include "..\DLLs\WindowFunctions_DLL\window_functions.h"
#include "OpenHoldemDoc.h"
#include "OpenHoldemView.h"
#include "Singletons.h"
#include "COcrWorker.h"
#include "CrashHandler.h"
#include "..\CTablemap\CTablemapDB.h"

// DB-backed preferences hooks. The Preferences DLL stays libpq-free; Hiss injects these so
// every setting reads/writes the postgres `settings` table (key "prefs") instead of the INI.
// A DB miss falls back to the legacy INI ONCE and seeds the DB (see CPreferences::ReadReg).
static bool PrefDbReadHook(const char *key, char *out, int out_size) {
  if (p_tablemap_db == NULL || out == NULL || out_size <= 0) return false;
  CString v = p_tablemap_db->GetSettingString("prefs", CString(key));
  if (v.IsEmpty()) return false;                       // miss -> DLL migrates from INI
  strncpy_s(out, out_size, v.GetString(), _TRUNCATE);
  return true;
}
static void PrefDbWriteHook(const char *key, const char *value) {
  if (p_tablemap_db == NULL) return;
  p_tablemap_db->SetSettingString("prefs", CString(key), CString(value == NULL ? "" : value));
}

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

// Supports MRU
AFX_STATIC_DATA const TCHAR _afxFileSection[] = _T("Recent File List");
AFX_STATIC_DATA const TCHAR _afxFileEntry[] = _T("File%d");
AFX_STATIC_DATA const TCHAR _afxPreviewSection[] = _T("Settings");
AFX_STATIC_DATA const TCHAR _afxPreviewEntry[] = _T("PreviewPages");

// COpenHoldemApp
extern bool Scintilla_RegisterClasses(void *hInstance);
extern bool Scintilla_ReleaseResources();

BEGIN_MESSAGE_MAP(COpenHoldemApp, CWinApp)
	ON_COMMAND(ID_APP_ABOUT, &COpenHoldemApp::OnAppAbout)
	// Standard file based document commands
	ON_COMMAND(ID_FILE_NEW, &CWinApp::OnFileNew)
	ON_COMMAND(ID_FILE_OPEN, &CWinApp::OnFileOpen)
	ON_COMMAND(ID_FINISH_INITIALIZATION, &COpenHoldemApp::FinishInitialization)
END_MESSAGE_MAP()

// COpenHoldemApp construction

COpenHoldemApp::COpenHoldemApp() {
}

// COpenHoldemApp destruction
COpenHoldemApp::~COpenHoldemApp() {
}

// The one and only COpenHoldemApp object
COpenHoldemApp theApp;


// COpenHoldemApp initialization
BOOL COpenHoldemApp::InitInstance() {
  // Always-on crash interception: capture a symbolicated stack + minidump to
  // logs\crash_*.log / .dmp for any SEH crash, unhandled C++ exception, or abort.
  // Installed first thing so even early-startup crashes are caught.
  InstallCrashHandler("hiss");
  // InitCommonControlsEx() is required on Windows XP if an application
	// manifest specifies use of ComCtl32.dll version 6 or later to enable
	// visual styles.  Otherwise, any window creation will fail.
  //
  // This code should probably be called at the VERY beginning,
  // especially to support UNICODE-filenames on Win7/8,
  // which might be as early as the ini-file.
  // http://www.maxinmontreal.com/forums/viewtopic.php?f=110&t=17579&p=122399#p122398
  // http://stackoverflow.com/questions/6633515/mfc-app-assert-fail-at-crecentfilelistadd-on-command-line-fileopen
	INITCOMMONCONTROLSEX InitCtrls;
	InitCtrls.dwSize = sizeof(InitCtrls);
	// Set this to include all the common control classes you want to use
	// in your application.
	InitCtrls.dwICC = ICC_WIN95_CLASSES;
	InitCommonControlsEx(&InitCtrls); 
	// Since OH 4.0.0 we always use an ini-file,
	// the one and only in our OH-directory,
	// no matter how it is named.
	// For the technical details please see:
	// http://msdn.microsoft.com/de-de/library/xykfyy20(v=vs.80).aspx

	Scintilla_RegisterClasses(AfxGetInstanceHandle());

	// Initialize richedit2 library
	AfxInitRichEdit2();

	// Change class name of Dialog
	WNDCLASS wc;
	GetClassInfo(AfxGetInstanceHandle(), "#32770", &wc);

	wc.lpszClassName = "OpenHoldemFormula";
	wc.hIcon = AfxGetApp()->LoadIcon(IDI_ICON1);
	RegisterClass(&wc);
  CWinApp::InitInstance();

 	// Initialize OLE libraries
	// Mandatory to call those initialisations. 
	// This will also help win7/8 compatibility 
	// those line are automatically inserted if you create a new MFC project with VS2010
	// http://stackoverflow.com/questions/6633515/mfc-app-assert-fail-at-crecentfilelistadd-on-command-line-fileopen
  // http://www.maxinmontreal.com/forums/viewtopic.php?f=110&t=17579&start=150#p122418
	if (!AfxOleInit())
		return FALSE;
	AfxEnableControlContainer();
  
	// Classes
  // First we have to read the pre4ferences,
  // as start_log() needs to know if the old log has to be deleted...
  free((void*)m_pszProfileName);
  m_pszProfileName = _strdup(IniFilePath().GetString());
  // Settings live in the postgres `settings` table, not the INI. Create the DB handle and
  // wire the preference hooks BEFORE loading, so LoadPreferences() reads from the DB (and
  // one-time-migrates any legacy INI values into it). CTablemapDB self-bootstraps its conn.
  if (p_tablemap_db == NULL) p_tablemap_db = new CTablemapDB;
  Preferences()->SetDbHooks(PrefDbReadHook, PrefDbWriteHook);
  Preferences()->LoadPreferences();

  // OCR-worker mode detection MUST happen before we grab a session slot. Worker
  // processes ("Hiss.exe --ocr-worker") are short-lived helpers; if they grabbed
  // a session id like a real instance they would exhaust the session counter and
  // make every later launch (workers AND the main app) fail with "too many
  // instances on sessioncounter". So workers skip the session counter entirely.
  CString worker_pipe, worker_tm;
  bool is_ocr_worker = ParseOcrWorkerCommandLine(&worker_pipe, &worker_tm);
  g_ocr_worker_mode = is_ocr_worker;   // singletons consult this during construction

	if (!is_ocr_worker) {
		if (!p_sessioncounter) p_sessioncounter = new CSessionCounter();
		// Start logging immediatelly after the loading the preferences
		// and initializing the sessioncounter, which is necessary for
		// the filename of the log (oh_0.log, etc).
		start_log(p_sessioncounter->session_id(), false); //!!!!!
		// ...then re-Load the preferences immediately after creation
		// of the log-file again, as We might want to log the preferences too,
		// which was not yet possible some lines above.
		// http://www.maxinmontreal.com/forums/viewtopic.php?f=124&t=20281&p=142334#p142334
		Preferences()->LoadPreferences();
	} else {
		// Worker: a session counter that grabs NO mutex slot (so it can't exhaust
		// the counter) but still provides a valid session_id(), which the watchdog
		// and other singletons dereference during InstantiateAllSingletons().
		if (!p_sessioncounter) p_sessioncounter = new CSessionCounter(true);
		// Start a log under a fixed pseudo-id so any write_log() during singleton
		// construction has a valid target.
		start_log(9000 + (int)(GetCurrentProcessId() % 1000), false);
	}
	InstantiateAllSingletons();
  // Process-level OCR worker: if launched as "Hiss.exe --ocr-worker", do NOT
  // start the GUI/heartbeat/autoconnector. Just load the tablemap and service
  // recognition requests over the pipe in this isolated process, then exit.
  if (is_ocr_worker) {
    InstallCrashHandler("ocrworker");        // tag worker crashes distinctly
    RunOcrWorker(worker_pipe, worker_tm);     // never returns (ExitProcess)
    return FALSE;
  }
  write_log(Preferences()->debug_openholdem(), "[OpenHoldem] Going to load mouse.DLL\n");
	// mouse.dll - failure in load is fatal
	_mouse_dll = LoadLibrary("mouse.dll");
	if (_mouse_dll == NULL)	{
		CString		t = "";
		t.Format("Unable to load mouse.dll, error: %d\n\nExiting.", GetLastError());
		MessageBox_Error_Warning(t, "OpenHoldem mouse.dll ERROR");
		return false;
	}	else {
		_dll_mouse_process_message = (mouse_process_message_t) GetProcAddress(_mouse_dll, "ProcessMessage");
		_dll_mouse_click = (mouse_click_t) GetProcAddress(_mouse_dll, "MouseClick");
		_dll_mouse_click_drag = (mouse_clickdrag_t) GetProcAddress(_mouse_dll, "MouseClickDrag");
		if (_dll_mouse_process_message==NULL || _dll_mouse_click==NULL || _dll_mouse_click_drag==NULL) {
			CString		t = "";
			t.Format("Unable to find all symbols in mouse.dll");
			MessageBox_Error_Warning(t, "OpenHoldem mouse.dll ERROR");
			FreeLibrary(_mouse_dll);
			_mouse_dll = NULL;
			return false;
		}
	write_log(Preferences()->debug_openholdem(), "[OpenHoldem] Going to load keyboard.DLL\n");}
	// keyboard.dll - failure in load is fatal
	_keyboard_dll = LoadLibrary("keyboard.dll");
	if (_keyboard_dll==NULL) {
		CString		t = "";
		t.Format("Unable to load keyboard.dll, error: %d\n\nExiting.", GetLastError());
		MessageBox_Error_Warning(t, "OpenHoldem keyboard.dll ERROR");
		return false;
	}	else {
		_dll_keyboard_process_message = (keyboard_process_message_t) GetProcAddress(_keyboard_dll, "ProcessMessage");
		_dll_keyboard_sendstring = (keyboard_sendstring_t) GetProcAddress(_keyboard_dll, "SendString");
		_dll_keyboard_sendkey = (keyboard_sendkey_t) GetProcAddress(_keyboard_dll, "SendKey");
		if (_dll_keyboard_process_message==NULL || _dll_keyboard_sendstring==NULL || _dll_keyboard_sendkey==NULL)	{
			CString		t = "";
			t.Format("Unable to find all symbols in keyboard.dll");
			MessageBox_Error_Warning(t, "OpenHoldem keyboard.dll ERROR");
			FreeLibrary(_keyboard_dll);
			_keyboard_dll = NULL;
			return false;
		}
	}
	LoadLastRecentlyUsedFileList();
	// Register the application's document templates.  Document templates
	// serve as the connection between documents, frame windows and views
	CSingleDocTemplate* pDocTemplate;
	// Document template and doc/view
  // https://msdn.microsoft.com/en-us/library/hts9a4xz.aspx
	// https://msdn.microsoft.com/en-us/library/d1e9fe7d.aspx
	write_log(Preferences()->debug_openholdem(), "[OpenHoldem] Going to create CSingleDocTemplate()\n");
	pDocTemplate = new CSingleDocTemplate(
		IDR_MAINFRAME,
		RUNTIME_CLASS(COpenHoldemDoc),
		RUNTIME_CLASS(CMainFrame),	   // main SDI frame window
		RUNTIME_CLASS(COpenHoldemView));
	if (!pDocTemplate) {
		write_log(Preferences()->debug_openholdem(), "[OpenHoldem] Creating CSingleDocTemplate() failed\n");
		return FALSE;
	}
	write_log(Preferences()->debug_openholdem(), "[OpenHoldem] Going to AddDocTemplate()\n");
	AddDocTemplate(pDocTemplate);

	write_log(Preferences()->debug_openholdem(), "[OpenHoldem] Going to EnableShellOpen()\n");
	EnableShellOpen();
	write_log(Preferences()->debug_openholdem(), "[OpenHoldem] Going to RegisterShellFileTypes(false)\n");
	RegisterShellFileTypes(false);
  write_log(Preferences()->debug_openholdem(), "[OpenHoldem] Going to InitializeThreads()\n");
  InitializeThreads();
  write_log(Preferences()->debug_openholdem(), "[OpenHoldem] Going to OpenLastRecentlyUsedFile()\n");
  p_formula_parser->ParseDefaultLibraries(); 
	OpenLastRecentlyUsedFile();
	write_log(Preferences()->debug_openholdem(), "[OpenHoldem] m_pMainWnd = %i\n",
		m_pMainWnd);
	write_log(Preferences()->debug_openholdem(), "[OpenHoldem] Posting message that finishes initialization later\n");
	FinishInitialization();
	write_log(Preferences()->debug_openholdem(), "[OpenHoldem] InitInstance done\n");
	return TRUE;
}

void COpenHoldemApp::FinishInitialization() {
	write_log(Preferences()->debug_openholdem(), "[OpenHoldem] FinishInitialization()\n");
	write_log(Preferences()->debug_openholdem(), "[OpenHoldem] m_pMainWnd = %i\n",
		m_pMainWnd);
	assert(p_openholdem_title != NULL);
	p_openholdem_title->UpdateTitle();
	// Show each instance (no auto-minimize for secondary instances) and restore its
	// own last size/position from the DB, so multiple instances open where they last
	// closed.
	// Hide the main Hiss window COMPLETELY at startup -- a hidden window shows neither on screen NOR in the
	// taskbar. The React table view (its own top-level window with its own taskbar icon) is the primary UI
	// now; its scarlet "restore" toolbar icon un-hides this main window on demand. [Emrald: hide main window
	// from view + taskbar; restore it from the React table view]
	((CMainFrame *)m_pMainWnd)->RestoreWindowPlacementFromDb();   // set saved placement now, while hidden
	m_pMainWnd->ShowWindow(SW_HIDE);
	// Enable drag/drop open (after ProcessShellCommand, per MFC SDI guidance).
	m_pMainWnd->DragAcceptFiles();
}

int COpenHoldemApp::ExitInstance() {
  // timers and threads are already stopped 
  // by CMainFrame::DestroyWindow().
  // Now we cancontinue with singletons.
	DeleteAllSingletons();
	Scintilla_ReleaseResources();
  stop_log();
	return CWinApp::ExitInstance();
}

// CDlgAbout dialog used for App About
class CDlgAbout : public CDialog 
{
public:
	CDlgAbout();

// Dialog Data
	enum { IDD = IDD_ABOUTBOX };

protected:
	virtual void DoDataExchange(CDataExchange* pDX);	// DDX/DDV support

// Implementation
protected:
	DECLARE_MESSAGE_MAP()
};


CDlgAbout::CDlgAbout() : CDialog(CDlgAbout::IDD) {
}


void CDlgAbout::DoDataExchange(CDataExchange* pDX) {
	CDialog::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CDlgAbout, CDialog)
END_MESSAGE_MAP()

// App command to run the dialog
void COpenHoldemApp::OnAppAbout() {
	CDlgAbout aboutDlg;
	aboutDlg.DoModal();
}

void COpenHoldemApp::LoadLastRecentlyUsedFileList() {
	// Added due to inability to get standard LoadStdProfileSettings working properly
	ASSERT_VALID(this);
	ASSERT(m_pRecentFileList == NULL);

	write_log(Preferences()->debug_openholdem(), "[OpenHoldem] Going to load file history\n");
	if (kNumberOfLastRecentlyUsedFilesInFileMenu > 0) 
	{
		// create file MRU since nMaxMRU not zero
		m_pRecentFileList = new CRecentFileList(0, _afxFileSection, _afxFileEntry, 
			kNumberOfLastRecentlyUsedFilesInFileMenu);
		m_pRecentFileList->ReadList();
	}
	// 0 by default means not set
	m_nNumPreviewPages = GetProfileInt(_afxPreviewSection, _afxPreviewEntry, 0);
}

void COpenHoldemApp::StoreLastRecentlyUsedFileList() {
	m_pRecentFileList->WriteList();
}

void COpenHoldemApp::OpenLastRecentlyUsedFile() {
	// Parse command line for standard shell commands, DDE, file open
	CCommandLineInfo cmdInfo;
	ParseCommandLine(cmdInfo);
	write_log(Preferences()->debug_openholdem(), "[OpenHoldem] Going to open last recently used file\n");
	// Open the most recently used file. (First on the MRU list.) Get the last
	// file from the registry. We need not account for cmdInfo.m_bRunAutomated and
	// cmdInfo.m_bRunEmbedded as they are processed before we get here.
	if (cmdInfo.m_nShellCommand == CCommandLineInfo::FileNew)	{
		CString sLastPath(GetProfileString(_afxFileSection, "File1"));
    if (!sLastPath.IsEmpty())	{
			write_log(Preferences()->debug_openholdem(), "[OpenHoldem] Last path: %s\n", 
				sLastPath);
			CFile f;
			// If file is there, set to open!
			if (f.Open(sLastPath, CFile::modeRead | CFile::shareDenyWrite))	{
				cmdInfo.m_nShellCommand = CCommandLineInfo::FileOpen;
				cmdInfo.m_strFileName = sLastPath;
				f.Close();
			}
		}
	}
	write_log(Preferences()->debug_openholdem(), "[OpenHoldem] Going to dispatch command-line\n");
	// Dispatch commands specified on the command line.  Will fail if
	// app was launched with /RegServer, /Register, /Unregserver or /Unregister.	
	if (!ProcessShellCommand(cmdInfo)) {
		write_log(Preferences()->debug_openholdem(), "[OpenHoldem] Dispatching command-line failed\n");
	}
}

void COpenHoldemApp::InitializeThreads() {
  // Heartbeat thread cares about everything: connecting, scraping, playing
  write_log(Preferences()->debug_openholdem(), "[OpenHoldem] Going to start heartbeat thread\n");
  assert(p_heartbeat_thread == NULL);
  p_heartbeat_thread = new CHeartbeatThread();
  assert(p_heartbeat_thread != NULL);
  p_heartbeat_thread->StartThread();
  // Iterator thread
  p_iterator_thread = new CIteratorThread();
  assert(p_iterator_thread != NULL);
}