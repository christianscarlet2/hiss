#include "stdafx.h"
#include "TextImageGen.h"
#include "resource.h"
#include "TrainerDB.h"
#include "TrainerMessages.h"

#include "libpq-fe.h"
#include <vector>
#include <string>

using namespace cv;

// ============================ settings (persisted) ===========================
// Stored in the hiss `settings` table under key 'text2image' (reusing TrainerDB).

struct ST2ISettings {
	CString exe_path;
	CString pt_host, pt_port, pt_user, pt_pass, pt_dbname;
};

static CString GetOrDefault(const char *field, const char *def)
{
	CString v = TrainerDB_GetSetting("text2image", field);
	if (v.IsEmpty()) v = def;
	return v;
}

static ST2ISettings LoadSettings()
{
	ST2ISettings s;
	s.exe_path  = GetOrDefault("path", "C:\\Program Files\\Tesseract-OCR\\text2image.exe");
	s.pt_host   = GetOrDefault("pt_host", "127.0.0.1");
	s.pt_port   = GetOrDefault("pt_port", "5432");
	s.pt_user   = GetOrDefault("pt_user", "postgres");
	s.pt_pass   = GetOrDefault("pt_pass", "dbpass");
	s.pt_dbname = GetOrDefault("pt_dbname", "PT4 DB");
	return s;
}

static void SaveSettings(const ST2ISettings &s)
{
	TrainerDB_SetSetting("text2image", "path", s.exe_path);
	TrainerDB_SetSetting("text2image", "pt_host", s.pt_host);
	TrainerDB_SetSetting("text2image", "pt_port", s.pt_port);
	TrainerDB_SetSetting("text2image", "pt_user", s.pt_user);
	TrainerDB_SetSetting("text2image", "pt_pass", s.pt_pass);
	TrainerDB_SetSetting("text2image", "pt_dbname", s.pt_dbname);
}

// ============================ small helpers ==================================

// Directory of trainer.exe, with a trailing backslash.
static CString ExeDir()
{
	char path[MAX_PATH] = { 0 };
	::GetModuleFileName(NULL, path, MAX_PATH);
	char *last = strrchr(path, '\\');
	if (last != NULL) *(last + 1) = '\0';
	return CString(path);
}

// Run a command line. When `capture` is non-NULL, stdout+stderr are captured into it.
// Returns false only if the process could not be started. Hidden console window.
static bool RunProcess(const CString &cmdline, DWORD timeout_ms, CStringA *capture, DWORD *exit_code)
{
	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE; sa.lpSecurityDescriptor = NULL;

	HANDLE rd = NULL, wr = NULL;
	if (capture != NULL) {
		if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
		SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	}

	STARTUPINFO si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
	if (capture != NULL) {
		si.dwFlags |= STARTF_USESTDHANDLES;
		si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = NULL;
	}
	PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));

	std::vector<char> buf(cmdline.GetString(), cmdline.GetString() + cmdline.GetLength() + 1);
	BOOL ok = CreateProcess(NULL, &buf[0], NULL, NULL, capture != NULL ? TRUE : FALSE,
		CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
	if (capture != NULL && wr != NULL) CloseHandle(wr);   // drop our write end so reads hit EOF
	if (!ok) { if (rd) CloseHandle(rd); return false; }

	if (capture != NULL) {
		char tmp[4096]; DWORD n = 0;
		while (ReadFile(rd, tmp, sizeof(tmp), &n, NULL) && n > 0) capture->Append(tmp, (int)n);
		CloseHandle(rd);
	}
	WaitForSingleObject(pi.hProcess, timeout_ms);
	DWORD code = 0;
	GetExitCodeProcess(pi.hProcess, &code);
	if (code == STILL_ACTIVE) { TerminateProcess(pi.hProcess, 1); code = 1; }
	if (exit_code != NULL) *exit_code = code;
	CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
	return true;
}

// Parse "  3: Roboto Condensed, Heavy" lines from --list_available_fonts into the font
// description strings text2image accepts for --font (deduplicated).
static void ListFonts(const ST2ISettings &s, const CString &fonts_dir, const CString &tmp_dir,
	std::vector<CStringA> *out)
{
	CString cmd;
	cmd.Format("\"%s\" --fonts_dir=\"%s\" --fontconfig_tmpdir=\"%s\" --list_available_fonts",
		s.exe_path.GetString(), fonts_dir.GetString(), tmp_dir.GetString());
	CStringA cap; DWORD code = 0;
	if (!RunProcess(cmd, 30000, &cap, &code)) return;

	int pos = 0;
	while (pos < cap.GetLength()) {
		int nl = cap.Find('\n', pos);
		CStringA line = (nl < 0) ? cap.Mid(pos) : cap.Mid(pos, nl - pos);
		pos = (nl < 0) ? cap.GetLength() : nl + 1;
		line.Trim();
		int colon = line.Find(':');
		if (colon <= 0) continue;
		CStringA num = line.Left(colon); num.Trim();
		bool all_digit = num.GetLength() > 0;
		for (int i = 0; i < num.GetLength(); ++i)
			if (!isdigit((unsigned char)num[i])) { all_digit = false; break; }
		if (!all_digit) continue;
		CStringA desc = line.Mid(colon + 1); desc.Trim();
		if (desc.IsEmpty()) continue;
		bool dup = false;
		for (size_t k = 0; k < out->size(); ++k) if ((*out)[k] == desc) { dup = true; break; }
		if (!dup) out->push_back(desc);
	}
}

// Escape a value for a single-quoted libpq connection-string field.
static CStringA EscapeConn(const CString &v)
{
	CStringA in(v), out;
	for (int i = 0; i < in.GetLength(); ++i) {
		char c = in[i];
		if (c == '\\' || c == '\'') out += '\\';
		out += c;
	}
	return out;
}

// Pull up to `want` random player names from the PokerTracker-4 database.
static bool FetchNames(const ST2ISettings &s, int want, std::vector<CStringA> *out, CStringA *err)
{
	CStringA host(s.pt_host), port(s.pt_port), user(s.pt_user);
	CStringA conn;
	conn.Format("host=%s port=%s user=%s password='%s' dbname='%s'",
		host.GetString(), port.GetString(), user.GetString(),
		EscapeConn(s.pt_pass).GetString(), EscapeConn(s.pt_dbname).GetString());
	PGconn *c = PQconnectdb(conn);
	if (c == NULL || PQstatus(c) != CONNECTION_OK) {
		if (err) { *err = "Cannot connect to the PT4 database. "; if (c) *err += PQerrorMessage(c); }
		if (c) PQfinish(c);
		return false;
	}
	CStringA sql;
	sql.Format("SELECT player_name FROM player WHERE player_name <> '' ORDER BY random() LIMIT %d",
		want > 0 ? want : 1);
	PGresult *r = PQexec(c, sql);
	bool ok = false;
	if (PQresultStatus(r) == PGRES_TUPLES_OK) {
		int n = PQntuples(r);
		for (int i = 0; i < n; ++i) {
			const char *v = PQgetvalue(r, i, 0);
			if (v && *v) out->push_back(CStringA(v));
		}
		ok = !out->empty();
		if (!ok && err) *err = "PT4 returned no player names.";
	} else if (err) {
		*err = "PT4 query failed. "; *err += PQerrorMessage(c);
	}
	PQclear(r);
	PQfinish(c);
	return ok;
}

// Parse a stored "a,r,g,b,radius" colour; falls back to the given default rgb.
static void ParseColour(const CString &v, int dr, int dg, int db, int *r, int *g, int *b)
{
	int a = 0, rr = dr, gg = dg, bb = db;
	if (sscanf_s(CStringA(v).GetString(), "%d,%d,%d,%d", &a, &rr, &gg, &bb) >= 4) {
		*r = rr; *g = gg; *b = bb;
	} else { *r = dr; *g = dg; *b = db; }
}

// Load the two Create-Fonts colours (foreground text colours) as rgb triples.
static void LoadColours(int fg[2][3])
{
	ParseColour(TrainerDB_GetSetting("font_colours", "c1"), 64, 243, 126, &fg[0][0], &fg[0][1], &fg[0][2]);
	ParseColour(TrainerDB_GetSetting("font_colours", "c2"), 51, 145, 79,  &fg[1][0], &fg[1][1], &fg[1][2]);
}

static int ClampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// True if at least one sample_*.png already exists in training\.
static bool HasTrainingPng(const CString &train_dir)
{
	WIN32_FIND_DATA fd;
	HANDLE h = FindFirstFile(train_dir + "sample_*.png", &fd);
	if (h == INVALID_HANDLE_VALUE) return false;
	FindClose(h);
	return true;
}

// Darkest colour (by luminance) across existing training PNGs, used as the generated
// images' border so it matches the real captured table background. Scans up to 64
// samples for speed. Returns false if there are no training PNGs to sample.
static bool FindDarkestInTraining(const CString &train_dir, Vec3b *out)
{
	WIN32_FIND_DATA fd;
	HANDLE h = FindFirstFile(train_dir + "sample_*.png", &fd);
	if (h == INVALID_HANDLE_VALUE) return false;
	double min_lum = 1e9;
	bool found = false;
	int scanned = 0;
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
		CString path = train_dir + fd.cFileName;
		Mat img = imread(std::string(CStringA(path).GetString()), IMREAD_COLOR);
		if (img.empty()) continue;
		for (int y = 0; y < img.rows; ++y) {
			const Vec3b *row = img.ptr<Vec3b>(y);
			for (int x = 0; x < img.cols; ++x) {
				const Vec3b &p = row[x];
				double lum = 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2];
				if (lum < min_lum) { min_lum = lum; *out = p; found = true; }
			}
		}
		if (++scanned >= 64) break;
	} while (FindNextFile(h, &fd));
	FindClose(h);
	return found;
}

// Highest existing sample_NNNN index + 1 (so new files continue after the last one).
static int ComputeStartIndex(const CString &train_dir)
{
	int hi = 0;
	WIN32_FIND_DATA fd;
	HANDLE h = FindFirstFile(train_dir + "sample_*.png", &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			int n = 0;
			if (sscanf_s(fd.cFileName, "sample_%d", &n) == 1 && n > hi) hi = n;
		} while (FindNextFile(h, &fd));
		FindClose(h);
	}
	return hi + 1;
}

// Render one username via text2image, recolour (green-on-dark), size to ~`target_h`px,
// and write sample_NNNN.png + sample_NNNN.gt.txt. `next_index` is advanced past any
// names that already exist (so concurrently-created files never clash).
static bool GenerateOne(const ST2ISettings &s, const CString &train_dir, const CString &fonts_dir,
	const CString &tmp_dir, const CStringA &name, const CStringA &font_desc, const int fg[3],
	const Vec3b &border_color, int target_h, int *next_index)
{
	const CString in_txt = tmp_dir + "\\in.txt";
	const CString base   = tmp_dir + "\\s";
	const CString tif    = base + ".tif";
	const CString box    = base + ".box";

	// Write the line of text (UTF-8 bytes as-is, no newline).
	{
		FILE *f = NULL;
		if (fopen_s(&f, CStringA(in_txt).GetString(), "wb") != 0 || f == NULL) return false;
		if (name.GetLength() > 0) fwrite(name.GetString(), 1, name.GetLength(), f);
		fclose(f);
	}
	// Clear stale output so a failed render can't be mistaken for this sample.
	DeleteFile(tif); DeleteFile(box);

	const int pt = 20 + (rand() % 11);   // 20..30
	CString cmd;
	cmd.Format("\"%s\" --text=\"%s\" --outputbase=\"%s\" --font=\"%s\" --fonts_dir=\"%s\" "
		"--fontconfig_tmpdir=\"%s\" --ptsize=%d --xsize=1800 --ysize=200 --margin=12 "
		"--degrade_image=false --rotate_image=false",
		s.exe_path.GetString(), in_txt.GetString(), base.GetString(),
		CString(font_desc).GetString(), fonts_dir.GetString(), tmp_dir.GetString(), pt);
	DWORD code = 0;
	if (!RunProcess(cmd, 20000, NULL, &code)) return false;
	if (GetFileAttributes(tif) == INVALID_FILE_ATTRIBUTES) return false;

	Mat gray = imread(std::string(CStringA(tif).GetString()), IMREAD_GRAYSCALE);
	if (gray.empty()) return false;

	// Ink bounding box (text is dark on white; <200 keeps anti-aliased edges).
	int minx = gray.cols, miny = gray.rows, maxx = -1, maxy = -1;
	for (int y = 0; y < gray.rows; ++y) {
		const uchar *row = gray.ptr<uchar>(y);
		for (int x = 0; x < gray.cols; ++x) {
			if (row[x] < 200) {
				if (x < minx) minx = x; if (x > maxx) maxx = x;
				if (y < miny) miny = y; if (y > maxy) maxy = y;
			}
		}
	}
	if (maxx < 0) return false;   // blank render
	const int pad = 2;
	minx = ClampI(minx - pad, 0, gray.cols - 1);
	miny = ClampI(miny - pad, 0, gray.rows - 1);
	maxx = ClampI(maxx + pad, 0, gray.cols - 1);
	maxy = ClampI(maxy + pad, 0, gray.rows - 1);
	Mat crop = gray(Rect(minx, miny, maxx - minx + 1, maxy - miny + 1));

	// Recolour: dark background, chosen green foreground; ink coverage = (255-gray)/255.
	const int bgR = 18, bgG = 20, bgB = 18;
	Mat colored(crop.rows, crop.cols, CV_8UC3);
	for (int y = 0; y < crop.rows; ++y) {
		const uchar *gp = crop.ptr<uchar>(y);
		Vec3b *op = colored.ptr<Vec3b>(y);
		for (int x = 0; x < crop.cols; ++x) {
			double a = (255.0 - gp[x]) / 255.0;
			int B = ClampI((int)(bgB + (fg[2] - bgB) * a + 0.5), 0, 255);
			int G = ClampI((int)(bgG + (fg[1] - bgG) * a + 0.5), 0, 255);
			int R = ClampI((int)(bgR + (fg[0] - bgR) * a + 0.5), 0, 255);
			op[x] = Vec3b((uchar)B, (uchar)G, (uchar)R);
		}
	}

	// Size to match the existing training samples (line height ~14-15px).
	int new_w = (int)((double)colored.cols * target_h / colored.rows + 0.5);
	if (new_w < 1) new_w = 1;
	Mat sized;
	resize(colored, sized, Size(new_w, target_h), 0, 0, INTER_AREA);

	// Add a 4px border using the darkest colour sampled from an existing training image,
	// extending the width/height a little.
	Mat final_img;
	copyMakeBorder(sized, final_img, 4, 4, 4, 4, BORDER_CONSTANT,
		Scalar(border_color[0], border_color[1], border_color[2]));

	// Next free sample_NNNN (re-checked so concurrently-added files never clash).
	CString png_path, txt_path, base_name;
	for (;;) {
		base_name.Format("sample_%04d", *next_index);
		png_path = train_dir + base_name + ".png";
		txt_path = train_dir + base_name + ".gt.txt";
		if (GetFileAttributes(png_path) == INVALID_FILE_ATTRIBUTES
			&& GetFileAttributes(txt_path) == INVALID_FILE_ATTRIBUTES) break;
		(*next_index)++;
	}
	if (!imwrite(std::string(CStringA(png_path).GetString()), final_img)) return false;
	{
		FILE *f = NULL;
		if (fopen_s(&f, CStringA(txt_path).GetString(), "wb") != 0 || f == NULL) return false;
		if (name.GetLength() > 0) fwrite(name.GetString(), 1, name.GetLength(), f);
		fputc('\r', f); fputc('\n', f);   // match existing gt.txt (UTF-8, CRLF, no BOM)
		fclose(f);
	}
	(*next_index)++;
	return true;
}

// ============================ generation worker ==============================

struct T2IJob {
	HWND notify;
	int count;
	ST2ISettings settings;
	volatile LONG cancel;
	int generated;
};

static DWORD WINAPI T2IWorker(LPVOID param)
{
	T2IJob *job = (T2IJob *)param;
	srand((unsigned)GetTickCount());

	const CString exe_dir   = ExeDir();
	const CString train_dir = exe_dir + "training\\";
	const CString fonts_dir = train_dir + "fonts";          // no trailing slash (t2i arg)
	const CString tmp_dir   = exe_dir + "_t2i_tmp";         // beside exe, keeps font cache
	CreateDirectory(train_dir, NULL);
	CreateDirectory(tmp_dir, NULL);

	std::vector<CStringA> fonts;
	ListFonts(job->settings, fonts_dir, tmp_dir, &fonts);
	if (fonts.empty()) {
		PostMessage(job->notify, WM_TRAINER_T2I_DONE, 0,
			(LPARAM) new CStringA("No usable fonts found in training\\fonts. "
				"Check the folder and that text2image.exe can read it."));
		return 0;
	}

	std::vector<CStringA> names; CStringA nerr;
	if (!FetchNames(job->settings, job->count, &names, &nerr)) {
		PostMessage(job->notify, WM_TRAINER_T2I_DONE, 0, (LPARAM) new CStringA(nerr));
		return 0;
	}

	// Border colour = darkest pixel from an existing training PNG (the real captured
	// table background). If there are none, there is nothing to sample, so stop.
	Vec3b border_color;
	if (!FindDarkestInTraining(train_dir, &border_color)) {
		PostMessage(job->notify, WM_TRAINER_T2I_DONE, 0,
			(LPARAM) new CStringA("No training images found in training\\ to sample a border colour from. "
				"Generate at least one training image first, then run this tool."));
		return 0;
	}

	int fg[2][3]; LoadColours(fg);
	int next_index = ComputeStartIndex(train_dir);
	int made = 0;

	for (int i = 0; i < job->count; ++i) {
		if (job->cancel) break;
		const CStringA &name = names[i % names.size()];
		const CStringA &font = fonts[rand() % (int)fonts.size()];
		const int *fgc = fg[rand() % 2];
		const int th = 14 + (rand() % 2);   // 14 or 15 px tall
		if (GenerateOne(job->settings, train_dir, fonts_dir, tmp_dir, name, font, fgc, border_color, th, &next_index))
			++made;
		PostMessage(job->notify, WM_TRAINER_T2I_PROGRESS, (WPARAM)(i + 1), (LPARAM)job->count);
	}

	job->generated = made;
	PostMessage(job->notify, WM_TRAINER_T2I_DONE, (WPARAM)made, (LPARAM)NULL);
	return 0;
}

// ============================ dialogs ========================================

// ---- settings ----
class CT2ISettingsDlg : public CDialog {
public:
	CT2ISettingsDlg(CWnd *p) : CDialog(IDD_T2I_SETTINGS, p) {}
	enum { IDD = IDD_T2I_SETTINGS };
protected:
	virtual BOOL OnInitDialog();
	virtual void OnOK();
	afx_msg void OnBrowse();
	afx_msg void OnBrowseA0();
	afx_msg void OnBrowseA1();
	DECLARE_MESSAGE_MAP()
};
BEGIN_MESSAGE_MAP(CT2ISettingsDlg, CDialog)
	ON_BN_CLICKED(IDC_T2I_BROWSE, &CT2ISettingsDlg::OnBrowse)
	ON_BN_CLICKED(IDC_T2I_A0_BROWSE, &CT2ISettingsDlg::OnBrowseA0)
	ON_BN_CLICKED(IDC_T2I_A1_BROWSE, &CT2ISettingsDlg::OnBrowseA1)
END_MESSAGE_MAP()

// Decimal-split field types shown in the Settings multi-select (matches Vision).
static const char *kDecimalFieldTypes[] = {
	"balance", "pot", "bet", "stack", "call", "raise", "blinds", "ante"
};

// Pick a Tesseract model (.traineddata or .checkpoint) into the given control.
static void BrowseModelInto(CWnd *parent, int edit_id)
{
	CString cur; parent->GetDlgItemText(edit_id, cur);
	CFileDialog dlg(TRUE, "traineddata", cur, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
		"Tesseract models (*.traineddata;*.checkpoint)|*.traineddata;*.checkpoint|All files (*.*)|*.*||", parent);
	if (dlg.DoModal() == IDOK) parent->SetDlgItemText(edit_id, dlg.GetPathName());
}

BOOL CT2ISettingsDlg::OnInitDialog()
{
	CDialog::OnInitDialog();
	ST2ISettings s = LoadSettings();
	SetDlgItemText(IDC_T2I_PATH, s.exe_path);
	SetDlgItemText(IDC_T2I_PT_HOST, s.pt_host);
	SetDlgItemText(IDC_T2I_PT_PORT, s.pt_port);
	SetDlgItemText(IDC_T2I_PT_USER, s.pt_user);
	SetDlgItemText(IDC_T2I_PT_PASS, s.pt_pass);
	SetDlgItemText(IDC_T2I_PT_DB, s.pt_dbname);
	// AutoOcr0/AutoOcr1 model paths come from the shared DB, the same place Hiss + Vision
	// read them. A region's transform field (A0/A1) decides which model OCRs it.
	SetDlgItemText(IDC_T2I_A0_MODEL, TrainerDB_GetSetting("autoocr0", "model"));
	SetDlgItemText(IDC_T2I_A1_MODEL, TrainerDB_GetSetting("autoocr1", "model"));

	// Decimal-splitting field-type multi-select, bound to the shared decimal_split_fields
	// list (same control as Vision).
	CString dsraw = TrainerDB_GetSetting("decimal_split_fields", "fields");
	for (int i = 0; i < (int)(sizeof(kDecimalFieldTypes) / sizeof(kDecimalFieldTypes[0])); ++i) {
		SendDlgItemMessage(IDC_T2I_DECIMAL_LIST, LB_ADDSTRING, 0, (LPARAM)kDecimalFieldTypes[i]);
		// Select it when the field type appears in the stored JSON array.
		CString tok; tok.Format("\"%s\"", kDecimalFieldTypes[i]);
		if (dsraw.Find(tok) >= 0) SendDlgItemMessage(IDC_T2I_DECIMAL_LIST, LB_SETSEL, TRUE, i);
	}
	return TRUE;
}

void CT2ISettingsDlg::OnOK()
{
	ST2ISettings s;
	GetDlgItemText(IDC_T2I_PATH, s.exe_path);
	GetDlgItemText(IDC_T2I_PT_HOST, s.pt_host);
	GetDlgItemText(IDC_T2I_PT_PORT, s.pt_port);
	GetDlgItemText(IDC_T2I_PT_USER, s.pt_user);
	GetDlgItemText(IDC_T2I_PT_PASS, s.pt_pass);
	GetDlgItemText(IDC_T2I_PT_DB, s.pt_dbname);
	SaveSettings(s);
	// Persist the AutoOcr models to the shared DB (Hiss + Vision pick them up).
	CString a0, a1;
	GetDlgItemText(IDC_T2I_A0_MODEL, a0); a0.Trim();
	GetDlgItemText(IDC_T2I_A1_MODEL, a1); a1.Trim();
	TrainerDB_SetSetting("autoocr0", "model", a0);
	TrainerDB_SetSetting("autoocr1", "model", a1);

	// Persist the decimal-splitting field list from the multi-select.
	std::vector<CString> decimal;
	for (int i = 0; i < (int)(sizeof(kDecimalFieldTypes) / sizeof(kDecimalFieldTypes[0])); ++i) {
		if (SendDlgItemMessage(IDC_T2I_DECIMAL_LIST, LB_GETSEL, i, 0) > 0)
			decimal.push_back(CString(kDecimalFieldTypes[i]));
	}
	TrainerDB_SetSettingArray("decimal_split_fields", "fields", decimal);

	CDialog::OnOK();
}

void CT2ISettingsDlg::OnBrowse()
{
	CFileDialog dlg(TRUE, "exe", NULL, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
		"Executables (*.exe)|*.exe|All files (*.*)|*.*||", this);
	if (dlg.DoModal() == IDOK) SetDlgItemText(IDC_T2I_PATH, dlg.GetPathName());
}

void CT2ISettingsDlg::OnBrowseA0() { BrowseModelInto(this, IDC_T2I_A0_MODEL); }
void CT2ISettingsDlg::OnBrowseA1() { BrowseModelInto(this, IDC_T2I_A1_MODEL); }

// ---- "how many?" ----
class CGenCountDlg : public CDialog {
public:
	CGenCountDlg(CWnd *p) : CDialog(IDD_GEN_COUNT, p), m_count(0) {}
	enum { IDD = IDD_GEN_COUNT };
	int m_count;
protected:
	virtual BOOL OnInitDialog() { CDialog::OnInitDialog(); SetDlgItemInt(IDC_GEN_COUNT_EDIT, 50, FALSE); return TRUE; }
	virtual void OnOK() {
		BOOL ok = FALSE;
		int n = GetDlgItemInt(IDC_GEN_COUNT_EDIT, &ok, FALSE);
		if (!ok || n <= 0) { MessageBox("Enter a positive number of samples.", "Generate", MB_OK | MB_ICONWARNING); return; }
		m_count = n;
		CDialog::OnOK();
	}
};

// ---- progress (runs the worker thread) ----
class CT2IProgressDlg : public CDialog {
public:
	CT2IProgressDlg(int count, const ST2ISettings &s, CWnd *p)
		: CDialog(IDD_T2I_PROGRESS, p), m_thread(NULL), m_had_error(false) {
		m_job.notify = NULL; m_job.count = count; m_job.settings = s; m_job.cancel = 0; m_job.generated = 0;
	}
	enum { IDD = IDD_T2I_PROGRESS };
	int Generated() const { return m_job.generated; }
	bool HadError() const { return m_had_error; }
protected:
	virtual BOOL OnInitDialog();
	virtual void OnCancel();
	afx_msg LRESULT OnProg(WPARAM, LPARAM);
	afx_msg LRESULT OnDone(WPARAM, LPARAM);
	DECLARE_MESSAGE_MAP()
private:
	T2IJob m_job;
	HANDLE m_thread;
	bool m_had_error;
};
BEGIN_MESSAGE_MAP(CT2IProgressDlg, CDialog)
	ON_MESSAGE(WM_TRAINER_T2I_PROGRESS, &CT2IProgressDlg::OnProg)
	ON_MESSAGE(WM_TRAINER_T2I_DONE, &CT2IProgressDlg::OnDone)
END_MESSAGE_MAP()

BOOL CT2IProgressDlg::OnInitDialog()
{
	CDialog::OnInitDialog();
	m_job.notify = GetSafeHwnd();
	SendDlgItemMessage(IDC_T2I_PROG_BAR, PBM_SETRANGE32, 0, (LPARAM)m_job.count);
	SendDlgItemMessage(IDC_T2I_PROG_BAR, PBM_SETPOS, 0, 0);
	CString t; t.Format("Preparing to generate %d sample(s)...", m_job.count);
	SetDlgItemText(IDC_T2I_PROG_TEXT, t);
	m_thread = CreateThread(NULL, 0, T2IWorker, &m_job, 0, NULL);
	if (m_thread == NULL) { m_had_error = true; EndDialog(IDCANCEL); }
	return TRUE;
}

LRESULT CT2IProgressDlg::OnProg(WPARAM w, LPARAM l)
{
	SendDlgItemMessage(IDC_T2I_PROG_BAR, PBM_SETPOS, (WPARAM)w, 0);
	CString t; t.Format("Generating sample %d of %d...", (int)w, (int)l);
	SetDlgItemText(IDC_T2I_PROG_TEXT, t);
	return 0;
}

LRESULT CT2IProgressDlg::OnDone(WPARAM w, LPARAM l)
{
	if (m_thread != NULL) { WaitForSingleObject(m_thread, 5000); CloseHandle(m_thread); m_thread = NULL; }
	CStringA *err = (CStringA *)l;
	if (err != NULL) {
		m_had_error = true;
		MessageBox(CString(*err), "Sample Generation", MB_OK | MB_ICONERROR);
		delete err;
	}
	m_job.generated = (int)w;
	EndDialog(err != NULL ? IDCANCEL : IDOK);
	return 0;
}

void CT2IProgressDlg::OnCancel()
{
	InterlockedExchange(&m_job.cancel, 1);   // signal; the worker posts DONE when it stops
	CWnd *btn = GetDlgItem(IDCANCEL);
	if (btn) btn->EnableWindow(FALSE);
	SetDlgItemText(IDC_T2I_PROG_TEXT, "Cancelling...");
}

// ============================ public entry points ============================

void T2I_OpenSettings(CWnd *parent)
{
	CT2ISettingsDlg dlg(parent);
	dlg.DoModal();
}

void T2I_GenerateInteractive(CWnd *parent)
{
	ST2ISettings s = LoadSettings();
	if (GetFileAttributes(s.exe_path) == INVALID_FILE_ATTRIBUTES) {
		AfxMessageBox("text2image.exe was not found.\nSet its path in Tools > Settings...",
			MB_OK | MB_ICONWARNING);
		return;
	}
	// The generated images' border colour is sampled from an existing training PNG, so
	// require at least one before generating.
	if (!HasTrainingPng(ExeDir() + "training\\")) {
		AfxMessageBox("No training images found in the training\\ folder.\n"
			"Generate at least one training image first (capture a sample), then use this tool.",
			MB_OK | MB_ICONWARNING);
		return;
	}
	CGenCountDlg cd(parent);
	if (cd.DoModal() != IDOK) return;

	CT2IProgressDlg pd(cd.m_count, s, parent);
	pd.DoModal();
	if (!pd.HadError()) {
		CString msg;
		msg.Format("Generated %d username sample(s) into training\\.", pd.Generated());
		AfxMessageBox(msg, MB_OK | MB_ICONINFORMATION);
	}
}
