#include "stdafx.h"
#include "SampleStore.h"

CSampleStore *p_sample_store = NULL;

// "training\" sub-folder next to trainer.exe, created if needed.
static CString TrainingDirectory()
{
	char path[MAX_PATH] = { 0 };
	::GetModuleFileName(NULL, path, MAX_PATH);
	char *last_backslash = strrchr(path, '\\');
	if (last_backslash != NULL) {
		*(last_backslash + 1) = '\0';
	}
	CString dir = CString(path) + "training\\";
	CreateDirectory(dir, NULL);
	return dir;
}

// Lowest free "sample_NNNN" base (neither .png nor .gt.txt present).
static CString NextTrainingBaseName(const CString &dir)
{
	for (int i = 1; i < 1000000; ++i) {
		CString base;
		base.Format("sample_%04d", i);
		CString png = dir + base + ".png";
		CString txt = dir + base + ".gt.txt";
		if (GetFileAttributes(png) == INVALID_FILE_ATTRIBUTES
			&& GetFileAttributes(txt) == INVALID_FILE_ATTRIBUTES) {
			return base;
		}
	}
	return "sample_overflow";
}

CSampleStore::CSampleStore()
{
	InitializeCriticalSection(&_cs);
	_next_id = 1;
}

CSampleStore::~CSampleStore()
{
	DeleteCriticalSection(&_cs);
}

int CSampleStore::Add(const CStringA &region, const std::vector<unsigned char> &png,
	const std::vector<unsigned char> &png_transformed, const CStringA &guess)
{
	EnterCriticalSection(&_cs);
	STrainerSample sample;
	sample.id = _next_id++;
	sample.region = region;
	sample.png = png;
	sample.png_transformed = png_transformed;
	sample.guess = guess;
	sample.saved = false;
	_samples.push_back(sample);
	int id = sample.id;
	LeaveCriticalSection(&_cs);
	return id;
}

static CStringA JsonEscapeA(const CStringA &value)
{
	CStringA escaped;
	for (int i = 0; i < value.GetLength(); ++i) {
		unsigned char c = (unsigned char)value[i];
		switch (c) {
		case '\\': escaped += "\\\\"; break;
		case '"': escaped += "\\\""; break;
		case '\r': break;
		case '\n': escaped += "\\n"; break;
		case '\t': escaped += "\\t"; break;
		default:
			if (c < 0x20) {
				char buf[8];
				sprintf_s(buf, sizeof(buf), "\\u%04x", c);
				escaped += buf;
			} else {
				escaped += (char)c;
			}
			break;
		}
	}
	return escaped;
}

CStringA CSampleStore::ListJson()
{
	CStringA json = "[";
	EnterCriticalSection(&_cs);
	for (size_t i = 0; i < _samples.size(); ++i) {
		const STrainerSample &s = _samples[i];
		if (i > 0) {
			json += ",";
		}
		CStringA entry;
		entry.Format("{\"id\":%d,\"region\":\"%s\",\"guess\":\"%s\",\"saved\":%s}",
			s.id,
			JsonEscapeA(s.region).GetString(),
			JsonEscapeA(s.guess).GetString(),
			s.saved ? "true" : "false");
		json += entry;
	}
	LeaveCriticalSection(&_cs);
	json += "]";
	return json;
}

bool CSampleStore::GetImage(int id, std::vector<unsigned char> *out)
{
	bool found = false;
	EnterCriticalSection(&_cs);
	for (size_t i = 0; i < _samples.size(); ++i) {
		if (_samples[i].id == id) {
			*out = _samples[i].png;
			found = true;
			break;
		}
	}
	LeaveCriticalSection(&_cs);
	return found;
}

// Writes one <png_path> (raw PNG bytes) + <txt_path> (ground-truth label).
static bool WriteOnePair(const CString &png_path, const CString &txt_path,
	const std::vector<unsigned char> &png, const CStringA &label)
{
	FILE *fp = NULL;
	if (fopen_s(&fp, png_path.GetString(), "wb") != 0 || fp == NULL) {
		return false;
	}
	if (!png.empty()) {
		fwrite(&png[0], 1, png.size(), fp);
	}
	fclose(fp);

	fp = NULL;
	if (fopen_s(&fp, txt_path.GetString(), "w") != 0 || fp == NULL) {
		return false;
	}
	fputs(label.GetString(), fp);
	fputs("\n", fp);
	fclose(fp);
	return true;
}

CStringA CSampleStore::WriteSampleFiles(const STrainerSample &sample, const CStringA &text)
{
	CString dir = TrainingDirectory();
	CString base = NextTrainingBaseName(dir);

	// Ground-truth text. These are balance fields shown in big blinds, so the
	// label always carries the " BB" unit even though the user only types the
	// numeric value once in the React view (which shows "+ BB"). The SAME label
	// is written for both the regular and the transformed image.
	CStringA label = text;
	label.Trim();
	CStringA lower = label; lower.MakeLower();
	if (lower.GetLength() < 2 || lower.Right(2) != "bb") {
		label += " BB";
	}

	// Regular (raw) crop -> sample_NNNN.png + sample_NNNN.gt.txt
	if (!WriteOnePair(dir + base + ".png", dir + base + ".gt.txt", sample.png, label)) {
		return "";
	}

	// Transformed (OCR-preprocessed) image, when present ->
	// sample_NNNN-transformed.png + sample_NNNN-transformed.gt.txt
	if (!sample.png_transformed.empty()) {
		WriteOnePair(dir + base + "-transformed.png", dir + base + "-transformed.gt.txt",
			sample.png_transformed, label);
	}

	return CStringA(base);
}

bool CSampleStore::Save(int id, const CStringA &text)
{
	bool ok = false;
	EnterCriticalSection(&_cs);
	for (size_t i = 0; i < _samples.size(); ++i) {
		if (_samples[i].id == id) {
			_samples[i].guess = text;
			CStringA base = WriteSampleFiles(_samples[i], text);
			if (!base.IsEmpty()) {
				_samples[i].saved = true;
				_samples[i].saved_base = base;
				ok = true;
			}
			break;
		}
	}
	LeaveCriticalSection(&_cs);
	return ok;
}

int CSampleStore::SaveAllPending()
{
	int count = 0;
	EnterCriticalSection(&_cs);
	for (size_t i = 0; i < _samples.size(); ++i) {
		if (!_samples[i].saved && !_samples[i].guess.IsEmpty()) {
			CStringA base = WriteSampleFiles(_samples[i], _samples[i].guess);
			if (!base.IsEmpty()) {
				_samples[i].saved = true;
				_samples[i].saved_base = base;
				++count;
			}
		}
	}
	LeaveCriticalSection(&_cs);
	return count;
}

bool CSampleStore::Delete(int id)
{
	bool found = false;
	EnterCriticalSection(&_cs);
	for (size_t i = 0; i < _samples.size(); ++i) {
		if (_samples[i].id == id) {
			_samples.erase(_samples.begin() + i);
			found = true;
			break;
		}
	}
	LeaveCriticalSection(&_cs);
	return found;
}

void CSampleStore::ClearAll()
{
	EnterCriticalSection(&_cs);
	_samples.clear();
	LeaveCriticalSection(&_cs);
}

int CSampleStore::ClearUnder(int id)
{
	int removed = 0;
	EnterCriticalSection(&_cs);
	for (size_t i = 0; i < _samples.size(); ++i) {
		if (_samples[i].id == id) {
			removed = (int)(_samples.size() - (i + 1));
			_samples.erase(_samples.begin() + (i + 1), _samples.end());
			break;
		}
	}
	LeaveCriticalSection(&_cs);
	return removed;
}

int CSampleStore::DeleteDuplicates()
{
	int removed = 0;
	EnterCriticalSection(&_cs);
	size_t n = _samples.size();
	// Build a normalized grayscale signature per sample (decoded once).
	std::vector<cv::Mat> sig(n);
	for (size_t i = 0; i < n; ++i) {
		if (_samples[i].png.empty()) continue;
		cv::Mat raw(1, (int)_samples[i].png.size(), CV_8UC1, (void *)&_samples[i].png[0]);
		cv::Mat img = cv::imdecode(raw, cv::IMREAD_GRAYSCALE);
		if (!img.empty()) {
			cv::resize(img, sig[i], cv::Size(64, 32), 0, 0, cv::INTER_AREA);
		}
	}
	std::vector<bool> drop(n, false);
	for (size_t i = 0; i < n; ++i) {
		if (drop[i] || sig[i].empty()) continue;
		for (size_t j = i + 1; j < n; ++j) {
			if (drop[j] || sig[j].empty()) continue;
			cv::Mat diff;
			cv::absdiff(sig[i], sig[j], diff);
			double similarity = 1.0 - (cv::mean(diff)[0] / 255.0);
			if (similarity >= 0.97) {   // same or >=97% identical
				drop[j] = true;
			}
		}
	}
	for (size_t i = n; i-- > 0; ) {
		if (drop[i]) {
			_samples.erase(_samples.begin() + i);
			++removed;
		}
	}
	LeaveCriticalSection(&_cs);
	return removed;
}
