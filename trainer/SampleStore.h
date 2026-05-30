#pragma once

#include <vector>

// One captured training sample: a PNG-encoded crop plus an editable label.
// PNG bytes live in memory until the user saves, so unsaved captures never
// litter the training\ folder.
struct STrainerSample {
	int                   id;
	CStringA              region;     // e.g. "p3balance"
	std::vector<unsigned char> png;   // PNG-encoded crop
	CStringA              guess;      // OCR pre-fill / user-edited label
	bool                  saved;
	CStringA              saved_base; // base name once written (sample_NNNN)
};

// Thread-safe store shared between the capture (UI) thread and the HTTP server
// thread. UI appends; server reads JSON/PNG and performs saves.
class CSampleStore {
public:
	CSampleStore();
	~CSampleStore();

	int Add(const CStringA &region, const std::vector<unsigned char> &png, const CStringA &guess);
	CStringA ListJson();                                   // [{id,region,guess,saved}]
	bool GetImage(int id, std::vector<unsigned char> *out);
	bool Save(int id, const CStringA &text);               // write png + .gt.txt
	int SaveAllPending();                                  // returns count saved
	bool Delete(int id);                                   // remove one row
	void ClearAll();                                       // remove all rows
	int ClearUnder(int id);                                // remove every row below this one
	int DeleteDuplicates();                                // remove >=97% identical crops

private:
	CRITICAL_SECTION _cs;
	std::vector<STrainerSample> _samples;
	int _next_id;

	// Writes <base>.png + <base>.gt.txt; returns base name or empty on failure.
	CStringA WriteSampleFiles(const STrainerSample &sample, const CStringA &text);
};

extern CSampleStore *p_sample_store;
