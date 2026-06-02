#include "stdafx.h"
#include "TrainerServer.h"
#include "SampleStore.h"
#include "TrainerFonts.h"
#include "FontGlyphStore.h"
#include "TrainerMessages.h"
#include "TablemapRegions.h"

#pragma comment(lib, "ws2_32.lib")

CTrainerServer *p_trainer_server = NULL;

CTrainerServer::CTrainerServer()
{
	_thread = NULL;
	_listen_socket = INVALID_SOCKET;
	_port = 0;
	_stop = false;
}

CTrainerServer::~CTrainerServer()
{
	Stop();
}

bool CTrainerServer::Start(unsigned short port)
{
	if (_thread != NULL) {
		return true;
	}

	WSADATA wsa_data;
	if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
		return false;
	}

	const int kMaxPortAttempts = 20;
	SOCKET listen_socket = INVALID_SOCKET;
	unsigned short bound_port = 0;
	for (int attempt = 0; attempt < kMaxPortAttempts; ++attempt) {
		unsigned short candidate = (unsigned short)(port + attempt);
		listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listen_socket == INVALID_SOCKET) {
			break;
		}
		sockaddr_in address;
		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(candidate);
		if (::bind(listen_socket, (sockaddr *)&address, sizeof(address)) == 0
				&& ::listen(listen_socket, SOMAXCONN) == 0) {
			bound_port = candidate;
			break;
		}
		closesocket(listen_socket);
		listen_socket = INVALID_SOCKET;
	}

	if (bound_port == 0) {
		WSACleanup();
		return false;
	}

	_listen_socket = listen_socket;
	_port = bound_port;
	_stop = false;
	_thread = AfxBeginThread(ServerThread, this, THREAD_PRIORITY_NORMAL, 0, CREATE_SUSPENDED, NULL);
	if (_thread == NULL) {
		closesocket(_listen_socket);
		_listen_socket = INVALID_SOCKET;
		WSACleanup();
		return false;
	}
	_thread->m_bAutoDelete = false;
	_thread->ResumeThread();
	return true;
}

void CTrainerServer::Stop(void)
{
	_stop = true;
	if (_listen_socket != INVALID_SOCKET) {
		closesocket(_listen_socket);
		_listen_socket = INVALID_SOCKET;
	}
	if (_thread != NULL) {
		WaitForSingleObject(_thread->m_hThread, 2000);
		_thread = NULL;
	}
	WSACleanup();
}

UINT CTrainerServer::ServerThread(LPVOID param)
{
	CTrainerServer *server = (CTrainerServer *)param;
	if (server != NULL) {
		server->Run();
	}
	return 0;
}

void CTrainerServer::Run(void)
{
	while (!_stop && _listen_socket != INVALID_SOCKET) {
		SOCKET client = ::accept(_listen_socket, NULL, NULL);
		if (client == INVALID_SOCKET) {
			if (!_stop) {
				Sleep(25);
			}
			continue;
		}
		HandleClient(client);
		closesocket(client);
	}
	if (_listen_socket != INVALID_SOCKET) {
		closesocket(_listen_socket);
		_listen_socket = INVALID_SOCKET;
	}
}

void CTrainerServer::HandleClient(SOCKET client)
{
	char buffer[8192];
	int received = recv(client, buffer, sizeof(buffer) - 1, 0);
	if (received <= 0) {
		return;
	}
	buffer[received] = 0;
	CStringA request(buffer);

	int line_end = request.Find("\r\n");
	CStringA first_line = line_end >= 0 ? request.Left(line_end) : request;
	int first_space = first_line.Find(' ');
	int second_space = first_line.Find(' ', first_space + 1);
	if (first_space < 0 || second_space < 0) {
		CStringA response = Response("bad request\r\n", "text/plain; charset=utf-8", "400 Bad Request");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	CStringA target = first_line.Mid(first_space + 1, second_space - first_space - 1);
	int question = target.Find('?');
	CStringA path = question >= 0 ? target.Left(question) : target;
	CStringA query = question >= 0 ? target.Mid(question + 1) : "";

	// API: list of samples.
	if (path.CompareNoCase("/api/samples") == 0) {
		CStringA body = (p_sample_store != NULL) ? p_sample_store->ListJson() : CStringA("[]");
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// API: PNG bytes for one sample.
	if (path.CompareNoCase("/api/sample/image") == 0) {
		int id = atoi(QueryValue(query, "id"));
		std::vector<unsigned char> png;
		if (p_sample_store != NULL && id > 0 && p_sample_store->GetImage(id, &png) && !png.empty()) {
			SendBinary(client, &png[0], (int)png.size(), "image/png");
		} else {
			CStringA response = Response("not found\r\n", "text/plain; charset=utf-8", "404 Not Found");
			send(client, response.GetString(), response.GetLength(), 0);
		}
		return;
	}

	// API: save one sample (id + text).
	if (path.CompareNoCase("/api/sample/save") == 0) {
		int id = atoi(QueryValue(query, "id"));
		CStringA text = UrlDecode(QueryValue(query, "text"));
		bool ok = (p_sample_store != NULL && id > 0 && p_sample_store->Save(id, text));
		CStringA body;
		body.Format("{\"ok\":%s}", ok ? "true" : "false");
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// API: save all pending samples (uses their current guesses).
	if (path.CompareNoCase("/api/sample/saveall") == 0) {
		int count = (p_sample_store != NULL) ? p_sample_store->SaveAllPending() : 0;
		CStringA body;
		body.Format("{\"saved\":%d}", count);
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// API: delete one sample.
	if (path.CompareNoCase("/api/sample/delete") == 0) {
		int id = atoi(QueryValue(query, "id"));
		bool ok = (p_sample_store != NULL && id > 0 && p_sample_store->Delete(id));
		CStringA body;
		body.Format("{\"ok\":%s}", ok ? "true" : "false");
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// API: clear every sample.
	if (path.CompareNoCase("/api/samples/clear") == 0) {
		if (p_sample_store != NULL) p_sample_store->ClearAll();
		CStringA response = Response("{\"ok\":true}", "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// API: clear every sample below the given one.
	if (path.CompareNoCase("/api/sample/clearunder") == 0) {
		int id = atoi(QueryValue(query, "id"));
		int removed = (p_sample_store != NULL && id > 0) ? p_sample_store->ClearUnder(id) : 0;
		CStringA body;
		body.Format("{\"removed\":%d}", removed);
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// API: set the active transform (AutoOcr0/1 or Text0..Text9) and re-recognize
	// all rows. Recognition runs on the UI thread (AutoOcr uses Tesseract).
	if (path.CompareNoCase("/api/transform") == 0) {
		CStringA t = UrlDecode(QueryValue(query, "t"));
		int mode = TRAINER_MODE_AUTOOCR, index = 0;
		if (t.Left(4).CompareNoCase("Text") == 0) { mode = TRAINER_MODE_TEXT; index = atoi(t.Mid(4)); }
		else if (t.Left(7).CompareNoCase("AutoOcr") == 0) { mode = TRAINER_MODE_AUTOOCR; index = atoi(t.Mid(7)); }
		if (p_sample_store != NULL) p_sample_store->SetTransform(mode, index);
		LRESULT recognized = (g_trainer_main_hwnd != NULL)
			? ::SendMessage(g_trainer_main_hwnd, WM_TRAINER_RECOGNIZE_ALL, 0, 0) : 0;
		int fonts = (mode == TRAINER_MODE_TEXT && p_trainer_fonts != NULL) ? p_trainer_fonts->group_count(index) : 0;
		CStringA body;
		body.Format("{\"ok\":true,\"transform\":\"%s\",\"recognized\":%d,\"fonts\":%d}",
			t.GetString(), (int)recognized, fonts);
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// API: start/stop sample capture (the Start/Stop button moved to the web UI).
	// Optional ?set=start|stop|toggle; with no `set`, just reports current state.
	if (path.CompareNoCase("/api/capture") == 0) {
		CStringA set = UrlDecode(QueryValue(query, "set"));
		int action = 0;   // 0 = status only
		if (set.CompareNoCase("start") == 0) action = 1;
		else if (set.CompareNoCase("stop") == 0) action = 2;
		else if (set.CompareNoCase("toggle") == 0) action = 3;
		LRESULT capturing = (g_trainer_main_hwnd != NULL)
			? ::SendMessage(g_trainer_main_hwnd, WM_TRAINER_SET_CAPTURE, (WPARAM)action, 0) : 0;
		CStringA body;
		body.Format("{\"ok\":true,\"capturing\":%s}", capturing ? "true" : "false");
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// API: delete every file in the training\ folder (moved here from the dialog).
	if (path.CompareNoCase("/api/training/clear") == 0) {
		LRESULT deleted = (g_trainer_main_hwnd != NULL)
			? ::SendMessage(g_trainer_main_hwnd, WM_TRAINER_CLEAR_TRAINING, 0, 0) : 0;
		CStringA body; body.Format("{\"ok\":true,\"deleted\":%d}", (int)deleted);
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// API: which characters already have a font in a group (for the coverage footer).
	if (path.CompareNoCase("/api/fonts/coverage") == 0) {
		int group = atoi(QueryValue(query, "group"));
		CStringA chars = (p_trainer_fonts != NULL) ? p_trainer_fonts->GroupChars(group) : CStringA("");
		CStringA esc;
		for (int i = 0; i < chars.GetLength(); ++i) {
			char c = chars[i];
			if (c == '"' || c == '\\') { esc += '\\'; esc += c; }
			else esc += c;
		}
		CStringA body; body.Format("{\"ok\":true,\"group\":%d,\"chars\":\"%s\"}", group, esc.GetString());
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// API: transform info - current transform + per-group font counts (for the dropdown).
	if (path.CompareNoCase("/api/fonts/info") == 0) {
		int mode = (p_sample_store != NULL) ? p_sample_store->TransformMode() : TRAINER_MODE_AUTOOCR;
		int idx = (p_sample_store != NULL) ? p_sample_store->TransformIndex() : 0;
		CStringA cur;
		cur.Format("%s%d", (mode == TRAINER_MODE_TEXT) ? "Text" : "AutoOcr", idx);
		CStringA counts = "[";
		for (int g = 0; g < TFE_NUM_FONT_GROUPS; ++g) {
			if (g > 0) counts += ",";
			CStringA c; c.Format("%d", (p_trainer_fonts != NULL) ? p_trainer_fonts->group_count(g) : 0);
			counts += c;
		}
		counts += "]";
		CStringA body;
		body.Format("{\"current\":\"%s\",\"counts\":%s}", cur.GetString(), counts.GetString());
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// ---- Font-creation editor -------------------------------------------------
	// Open the font-creation window (handled on the UI thread).
	if (path.CompareNoCase("/api/fonts/open") == 0) {
		if (g_trainer_main_hwnd != NULL) ::PostMessage(g_trainer_main_hwnd, WM_TRAINER_OPEN_FONTS, 0, 0);
		CStringA response = Response("{\"ok\":true}", "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}
	// Set the editor's working font group.
	if (path.CompareNoCase("/api/fonts/group") == 0) {
		int group = atoi(QueryValue(query, "group"));
		if (p_font_glyph_store != NULL) p_font_glyph_store->SetEditGroup(group);
		CStringA response = Response("{\"ok\":true}", "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}
	// Capture+segment the live scrapes into the glyph store (UI thread). Uses the
	// sticky edit-group unless an explicit group is provided.
	if (path.CompareNoCase("/api/fonts/capture") == 0) {
		CStringA g = QueryValue(query, "group");
		if (!g.IsEmpty() && p_font_glyph_store != NULL) p_font_glyph_store->SetEditGroup(atoi(g));
		if (g_trainer_main_hwnd != NULL) ::SendMessage(g_trainer_main_hwnd, WM_TRAINER_CAPTURE_FONTS, 0, 0);
		CStringA response = Response("{\"ok\":true}", "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}
	// List pending glyphs.
	if (path.CompareNoCase("/api/fonts/list") == 0) {
		CStringA body = (p_font_glyph_store != NULL) ? p_font_glyph_store->ListJson() : CStringA("[]");
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}
	// PNG mask image for one pending glyph.
	if (path.CompareNoCase("/api/fonts/glyph/image") == 0) {
		int gid = atoi(QueryValue(query, "gid"));
		std::vector<unsigned char> png;
		if (p_font_glyph_store != NULL && gid > 0 && p_font_glyph_store->GetImage(gid, &png) && !png.empty()) {
			SendBinary(client, &png[0], (int)png.size(), "image/png");
		} else {
			CStringA response = Response("not found\r\n", "text/plain; charset=utf-8", "404 Not Found");
			send(client, response.GetString(), response.GetLength(), 0);
		}
		return;
	}
	// PNG of the glyph's actual (regular) pixels, for reference.
	if (path.CompareNoCase("/api/fonts/glyph/regular") == 0) {
		int gid = atoi(QueryValue(query, "gid"));
		std::vector<unsigned char> png;
		if (p_font_glyph_store != NULL && gid > 0 && p_font_glyph_store->GetRegularImage(gid, &png) && !png.empty()) {
			SendBinary(client, &png[0], (int)png.size(), "image/png");
		} else {
			CStringA response = Response("not found\r\n", "text/plain; charset=utf-8", "404 Not Found");
			send(client, response.GetString(), response.GetLength(), 0);
		}
		return;
	}
	// PNG of the entire original region scrape this glyph came from.
	if (path.CompareNoCase("/api/fonts/glyph/full") == 0) {
		int gid = atoi(QueryValue(query, "gid"));
		std::vector<unsigned char> png;
		if (p_font_glyph_store != NULL && gid > 0 && p_font_glyph_store->GetFullImage(gid, &png) && !png.empty()) {
			SendBinary(client, &png[0], (int)png.size(), "image/png");
		} else {
			CStringA response = Response("not found\r\n", "text/plain; charset=utf-8", "404 Not Found");
			send(client, response.GetString(), response.GetLength(), 0);
		}
		return;
	}
	// Assign / clear the character for one glyph. A non-empty char creates the font
	// now and purges pending glyphs whose font now exists; returns how many removed.
	// After creating a font we rescan every live region against the updated groups
	// (same as Capture/Undo) so all glyphs — not just the edited one — are re-evaluated.
	if (path.CompareNoCase("/api/fonts/setchar") == 0) {
		int gid = atoi(QueryValue(query, "gid"));
		CStringA ch = UrlDecode(QueryValue(query, "ch"));
		int removed = (p_font_glyph_store != NULL && gid > 0) ? p_font_glyph_store->AssignChar(gid, ch) : 0;
		if (!ch.IsEmpty() && g_trainer_main_hwnd != NULL) {
			::SendMessage(g_trainer_main_hwnd, WM_TRAINER_CAPTURE_FONTS, 0, 0);
		}
		CStringA body; body.Format("{\"ok\":true,\"removed\":%d}", removed);
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}
	// OCR a glyph's REFERENCE image with the chosen transform (AutoOcrN / TextN) to
	// suggest its character. The UI thread does the recognition (Tesseract).
	if (path.CompareNoCase("/api/fonts/glyph/ocr") == 0) {
		int gid = atoi(QueryValue(query, "gid"));
		CStringA t = UrlDecode(QueryValue(query, "t"));
		int mode = TRAINER_MODE_AUTOOCR, index = 0;
		if (t.Left(4).CompareNoCase("Text") == 0) { mode = TRAINER_MODE_TEXT; index = atoi(t.Mid(4)); }
		else if (t.Left(7).CompareNoCase("AutoOcr") == 0) { mode = TRAINER_MODE_AUTOOCR; index = atoi(t.Mid(7)); }
		LRESULT code = (g_trainer_main_hwnd != NULL && gid > 0)
			? ::SendMessage(g_trainer_main_hwnd, WM_TRAINER_OCR_GLYPH, (WPARAM)gid,
				(LPARAM)((mode << 8) | (index & 0xFF))) : 0;
		// The char comes from the OCR whitelist (alphanumeric / . / _); escape the two
		// JSON-special bytes just in case.
		CStringA ch;
		if (code > 0) {
			char c = (char)code;
			if (c == '"' || c == '\\') ch.Format("\\%c", c);
			else ch.Format("%c", c);
		}
		CStringA body; body.Format("{\"ok\":true,\"ch\":\"%s\"}", ch.GetString());
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}
	// Read the source ARGB under a pixel of a glyph's "regular" or "full" image
	// (used by the colour eyedropper). px/py are natural-PNG pixel coordinates.
	if (path.CompareNoCase("/api/fonts/glyph/pixel") == 0) {
		int gid = atoi(QueryValue(query, "gid"));
		CStringA img = QueryValue(query, "img");
		int px = atoi(QueryValue(query, "px"));
		int py = atoi(QueryValue(query, "py"));
		int a = 0, r = 0, g = 0, b = 0;
		bool ok = (p_font_glyph_store != NULL && gid > 0
			&& p_font_glyph_store->GetPixel(gid, img, px, py, &a, &r, &g, &b));
		CStringA body;
		if (ok) body.Format("{\"ok\":true,\"a\":%d,\"r\":%d,\"g\":%d,\"b\":%d}", a, r, g, b);
		else    body = "{\"ok\":false}";
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}
	// Re-segment a glyph with a new colour/radius and persist colour/radius to r$.
	if (path.CompareNoCase("/api/fonts/glyph/regen") == 0) {
		int gid = atoi(QueryValue(query, "gid"));
		int a = atoi(QueryValue(query, "a"));
		int r = atoi(QueryValue(query, "r"));
		int g = atoi(QueryValue(query, "g"));
		int b = atoi(QueryValue(query, "b"));
		int radius = atoi(QueryValue(query, "radius"));
		bool ok = (p_font_glyph_store != NULL && gid > 0
			&& p_font_glyph_store->Regen(gid, a, r, g, b, radius));
		CStringA body; body.Format("{\"ok\":%s}", ok ? "true" : "false");
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}
	// Set a glyph's save group AND pull the colour/radius default from a region whose
	// transform is "Text<group>", regenerate, and return the applied colour/radius.
	if (path.CompareNoCase("/api/fonts/glyph/group") == 0) {
		int gid = atoi(QueryValue(query, "gid"));
		int group = atoi(QueryValue(query, "group"));
		int a = 0, r = 0, g = 0, b = 0, radius = 0;
		bool ok = (p_font_glyph_store != NULL && gid > 0
			&& p_font_glyph_store->SetGroupDefaults(gid, group, &a, &r, &g, &b, &radius));
		CStringA body;
		if (ok) body.Format("{\"ok\":true,\"a\":%d,\"r\":%d,\"g\":%d,\"b\":%d,\"radius\":%d}", a, r, g, b, radius);
		else    body = "{\"ok\":false}";
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}
	// Colour/radius default for a transform (Text<group>), used to pre-fill the
	// global per-group picker when its dropdown changes.
	if (path.CompareNoCase("/api/fonts/groupdefaults") == 0) {
		int group = atoi(QueryValue(query, "group"));
		CString transform; transform.Format("Text%d", group);
		COLORREF color = 0; int radius = 0;
		bool ok = RegionColors_GetByTransform(transform, &color, &radius);
		CStringA body;
		if (ok) body.Format("{\"ok\":true,\"a\":%d,\"r\":%d,\"g\":%d,\"b\":%d,\"radius\":%d}",
			(int)((color >> 24) & 0xff), (int)GetRValue(color), (int)GetGValue(color), (int)GetBValue(color), radius);
		else    body = "{\"ok\":false}";
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}
	// Global per-group picker: apply colour + radius to every glyph in a group.
	if (path.CompareNoCase("/api/fonts/applygroup") == 0) {
		int group = atoi(QueryValue(query, "group"));
		int a = atoi(QueryValue(query, "a"));
		int r = atoi(QueryValue(query, "r"));
		int g = atoi(QueryValue(query, "g"));
		int b = atoi(QueryValue(query, "b"));
		int radius = atoi(QueryValue(query, "radius"));
		int count = (p_font_glyph_store != NULL)
			? p_font_glyph_store->ApplyToGroup(group, a, r, g, b, radius) : 0;
		CStringA body; body.Format("{\"ok\":true,\"count\":%d}", count);
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}
	// Global colour: write colour + radius to EVERY balance region's r$ (repairs
	// regions that currently capture nothing) and regenerate all pending glyphs.
	if (path.CompareNoCase("/api/fonts/applyall") == 0) {
		int a = atoi(QueryValue(query, "a"));
		int r = atoi(QueryValue(query, "r"));
		int g = atoi(QueryValue(query, "g"));
		int b = atoi(QueryValue(query, "b"));
		int radius = atoi(QueryValue(query, "radius"));
		int regions = 0, glyphs = 0;
		if (p_font_glyph_store != NULL) glyphs = p_font_glyph_store->ApplyToAll(a, r, g, b, radius, &regions);
		else regions = RegionColors_UpdateAll(RGB(r, g, b) | ((COLORREF)(a & 0xff) << 24), radius);
		CStringA body; body.Format("{\"ok\":true,\"glyphs\":%d,\"regions\":%d}", glyphs, regions);
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}
	// Apply group + colour + radius to every row strictly below this one, regenerating each.
	if (path.CompareNoCase("/api/fonts/applybelow") == 0) {
		int gid = atoi(QueryValue(query, "gid"));
		int group = atoi(QueryValue(query, "group"));
		int a = atoi(QueryValue(query, "a"));
		int r = atoi(QueryValue(query, "r"));
		int g = atoi(QueryValue(query, "g"));
		int b = atoi(QueryValue(query, "b"));
		int radius = atoi(QueryValue(query, "radius"));
		int count = (p_font_glyph_store != NULL && gid > 0)
			? p_font_glyph_store->ApplyBelow(gid, group, a, r, g, b, radius) : 0;
		CStringA body; body.Format("{\"ok\":true,\"count\":%d}", count);
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}
	// Set which font group a glyph saves to (also the sticky default for captures).
	if (path.CompareNoCase("/api/fonts/setgroup") == 0) {
		int gid = atoi(QueryValue(query, "gid"));
		int group = atoi(QueryValue(query, "group"));
		bool ok = (p_font_glyph_store != NULL && gid > 0 && p_font_glyph_store->SetGroupFor(gid, group));
		CStringA body; body.Format("{\"ok\":%s}", ok ? "true" : "false");
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}
	// Delete one pending glyph.
	if (path.CompareNoCase("/api/fonts/delete") == 0) {
		int gid = atoi(QueryValue(query, "gid"));
		bool ok = (p_font_glyph_store != NULL && gid > 0 && p_font_glyph_store->Delete(gid));
		CStringA body; body.Format("{\"ok\":%s}", ok ? "true" : "false");
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}
	// Undo the last delete or label/create. Undo() removes the created font from the
	// tablemap first (so the glyph is no longer detected) and restores the stored
	// row(s); we then rescan the live glyphs so the now-unknown glyph re-surfaces
	// naturally (deduped against what was just restored).
	if (path.CompareNoCase("/api/fonts/undo") == 0) {
		int restored = (p_font_glyph_store != NULL) ? p_font_glyph_store->Undo() : 0;
		if (g_trainer_main_hwnd != NULL) ::SendMessage(g_trainer_main_hwnd, WM_TRAINER_CAPTURE_FONTS, 0, 0);
		CStringA body; body.Format("{\"ok\":true,\"restored\":%d}", restored);
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}
	// Clear all pending glyphs.
	if (path.CompareNoCase("/api/fonts/clear") == 0) {
		if (p_font_glyph_store != NULL) p_font_glyph_store->Clear();
		CStringA response = Response("{\"ok\":true}", "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}
	// Delete every font record (t$) for the loaded tablemap from the hiss database.
	if (path.CompareNoCase("/api/fonts/deletealltm") == 0) {
		int removed = (p_trainer_fonts != NULL) ? p_trainer_fonts->DeleteAllFonts() : -1;
		CStringA body;
		if (removed < 0) {
			body = "{\"ok\":false,\"error\":\"No tablemap loaded, or the database write failed.\"}";
		} else {
			body.Format("{\"ok\":true,\"removed\":%d}", removed);
		}
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}
	// Save all labeled glyphs into the tablemap's t$ groups, then persist the .tm.
	if (path.CompareNoCase("/api/fonts/save") == 0) {
		int written = (p_font_glyph_store != NULL) ? p_font_glyph_store->SaveAll() : 0;
		CStringA body; body.Format("{\"saved\":%d}", written);
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// API: delete near-duplicate (>=97% identical) crops.
	if (path.CompareNoCase("/api/samples/dedup") == 0) {
		int removed = (p_sample_store != NULL) ? p_sample_store->DeleteDuplicates() : 0;
		CStringA body;
		body.Format("{\"removed\":%d}", removed);
		CStringA response = Response(body, "application/json; charset=utf-8");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// Static page.
	if (path.CompareNoCase("/") == 0 || path.CompareNoCase("/trainer") == 0 || path.CompareNoCase("/trainer/") == 0) {
		ServeFile(client, "trainer.html");
		return;
	}
	if (path.Left(9).CompareNoCase("/trainer/") == 0) {
		CStringA relative = path.Mid(9);
		if (relative.IsEmpty()) {
			relative = "trainer.html";
		}
		ServeFile(client, CString(relative));
		return;
	}

	CStringA response = Response("not found\r\n", "text/plain; charset=utf-8", "404 Not Found");
	send(client, response.GetString(), response.GetLength(), 0);
}

CStringA CTrainerServer::Response(CStringA body, CStringA content_type, CStringA status)
{
	CStringA response;
	response.Format(
		"HTTP/1.1 %s\r\n"
		"Content-Type: %s\r\n"
		"Cache-Control: no-store\r\n"
		"Content-Length: %d\r\n"
		"Connection: close\r\n"
		"\r\n"
		"%s",
		status.GetString(), content_type.GetString(), body.GetLength(), body.GetString());
	return response;
}

void CTrainerServer::SendBinary(SOCKET client, const unsigned char *data, int len, CStringA content_type)
{
	CStringA header;
	header.Format(
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: %s\r\n"
		"Cache-Control: no-store\r\n"
		"Content-Length: %d\r\n"
		"Connection: close\r\n"
		"\r\n",
		content_type.GetString(), len);
	send(client, header.GetString(), header.GetLength(), 0);
	if (len > 0) {
		send(client, (const char *)data, len, 0);
	}
}

bool CTrainerServer::ServeFile(SOCKET client, CString relative_path)
{
	relative_path.Replace("/", "\\");
	if (relative_path.Find("..") >= 0) {
		CStringA response = Response("bad path\r\n", "text/plain; charset=utf-8", "400 Bad Request");
		send(client, response.GetString(), response.GetLength(), 0);
		return false;
	}

	// Resolve against trainer\web\ relative to the cwd and to the exe folder.
	CString path;
	path.Format("trainer\\web\\%s", relative_path.GetString());
	if (GetFileAttributes(path) == INVALID_FILE_ATTRIBUTES) {
		TCHAR module_path[MAX_PATH] = { 0 };
		GetModuleFileName(NULL, module_path, MAX_PATH);
		CString module_dir(module_path);
		int slash = module_dir.ReverseFind('\\');
		if (slash >= 0) {
			module_dir = module_dir.Left(slash);
			CString candidate;
			candidate.Format("%s\\trainer\\web\\%s", module_dir.GetString(), relative_path.GetString());
			if (GetFileAttributes(candidate) != INVALID_FILE_ATTRIBUTES) {
				path = candidate;
			} else {
				candidate.Format("%s\\..\\trainer\\web\\%s", module_dir.GetString(), relative_path.GetString());
				if (GetFileAttributes(candidate) != INVALID_FILE_ATTRIBUTES) {
					path = candidate;
				}
			}
		}
		if (GetFileAttributes(path) == INVALID_FILE_ATTRIBUTES) {
			CStringA response = Response("not found\r\n", "text/plain; charset=utf-8", "404 Not Found");
			send(client, response.GetString(), response.GetLength(), 0);
			return false;
		}
	}

	CFile file;
	if (!file.Open(path, CFile::modeRead | CFile::typeBinary)) {
		CStringA response = Response("cannot read file\r\n", "text/plain; charset=utf-8", "500 Internal Server Error");
		send(client, response.GetString(), response.GetLength(), 0);
		return false;
	}
	ULONGLONG length = file.GetLength();
	std::vector<unsigned char> body((size_t)length);
	if (length > 0) {
		file.Read(&body[0], (UINT)length);
	}
	file.Close();

	SendBinary(client, body.empty() ? NULL : &body[0], (int)body.size(), ContentType(path));
	return true;
}

CStringA CTrainerServer::ContentType(CString path)
{
	path.MakeLower();
	if (path.Right(5) == ".html") return "text/html; charset=utf-8";
	if (path.Right(3) == ".js") return "application/javascript; charset=utf-8";
	if (path.Right(4) == ".css") return "text/css; charset=utf-8";
	if (path.Right(4) == ".png") return "image/png";
	if (path.Right(4) == ".svg") return "image/svg+xml";
	return "application/octet-stream";
}

CStringA CTrainerServer::QueryValue(CStringA query, CStringA name)
{
	int start = 0;
	while (start <= query.GetLength()) {
		int end = query.Find('&', start);
		CStringA pair = end >= 0 ? query.Mid(start, end - start) : query.Mid(start);
		int equals = pair.Find('=');
		CStringA key = equals >= 0 ? pair.Left(equals) : pair;
		if (key.CompareNoCase(name) == 0) {
			return equals >= 0 ? pair.Mid(equals + 1) : "";
		}
		if (end < 0) {
			break;
		}
		start = end + 1;
	}
	return "";
}

CStringA CTrainerServer::UrlDecode(CStringA value)
{
	CStringA decoded;
	for (int i = 0; i < value.GetLength(); ++i) {
		char c = value[i];
		if (c == '+') {
			decoded += ' ';
		} else if (c == '%' && i + 2 < value.GetLength()) {
			char hex[3] = { value[i + 1], value[i + 2], 0 };
			char *end = NULL;
			long parsed = strtol(hex, &end, 16);
			if (end != hex) {
				decoded += (char)parsed;
				i += 2;
			}
		} else {
			decoded += c;
		}
	}
	return decoded;
}
