#pragma once

#include <vector>

// One captured training sample: a PNG-encoded crop plus an editable label.
// PNG bytes live in memory until the user saves, so unsaved captures never
// litter the training\ folder.
struct STrainerSample {
	int                   id;
	CStringA              region;     // e.g. "p3balance"
	std::vector<unsigned char> png;   // PNG-encoded raw crop (the "regular" view)
	std::vector<unsigned char> png_transformed; // PNG of the transformed (mask) image
	CStringA              guess;      // recognized / user-edited label
	bool                  saved;
	CStringA              saved_base; // base name once written (sample_NNNN)

	// Raw top-down BGRA crop + colour cube, kept so the row can be re-recognized
	// with a different Text0..Text9 font group on the regular image.
	std::vector<unsigned char> bgra;
	int                   bw, bh;
	unsigned long         color;      // COLORREF
	int                   radius;
};

// Thread-safe store shared between the capture (UI) thread and the HTTP server
// thread. UI appends; server reads JSON/PNG and performs saves.
class CSampleStore {
public:
	CSampleStore();
	~CSampleStore();

	int Add(const CStringA &region, const std::vector<unsigned char> &png,
		const std::vector<unsigned char> &png_transformed, const CStringA &guess,
		const std::vector<unsigned char> &bgra, int bw, int bh, unsigned long color, int radius);
	CStringA ListJson();                                   // [{id,region,guess,saved}]

	// Which engine recognizes the table: TRAINER_MODE_AUTOOCR/TRAINER_MODE_TEXT + index.
	void SetTransform(int mode, int index);
	int  TransformMode();
	int  TransformIndex();

	// Snapshot every row's crop for re-recognition on the UI thread, and write
	// recognized text back by id.
	struct SRecogItem {
		int id; CStringA region;
		std::vector<unsigned char> bgra; int bw, bh; unsigned long color; int radius;
	};
	void SnapshotForRecognition(std::vector<SRecogItem> *out);
	void SetGuess(int id, const CStringA &text);
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
	int _transform_mode;    // TRAINER_MODE_AUTOOCR / TRAINER_MODE_TEXT
	int _transform_index;   // 0/1 for AutoOcr, 0..9 for Text

	// Writes <base>.png + <base>.gt.txt (regular) and, when a transformed image
	// is present, <base>-transformed.png + <base>-transformed.gt.txt (same label).
	// Returns base name or empty on failure.
	CStringA WriteSampleFiles(const STrainerSample &sample, const CStringA &text);
};

extern CSampleStore *p_sample_store;
