# OCR Name Mapping Implementation Guide

## Overview
This implementation adds a database-backed name mapping system for OCR-detected player names in OpenHoldem. Instead of relying solely on fuzzy matching with Levenshtein distance, the system now uses actual player names from PokerTracker 4's hand history data to verify OCR results.

## Database Schema

The implementation requires one new table in the PokerTracker 4 PostgreSQL database:

```sql
CREATE TABLE ocr_name_mappings (
    id SERIAL PRIMARY KEY,
    actual_username VARCHAR(255) NOT NULL,
    ocr_detected_name VARCHAR(255) NOT NULL,
    id_site INTEGER NOT NULL,
    verified BOOLEAN DEFAULT FALSE,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    last_updated TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(ocr_detected_name, id_site),
    FOREIGN KEY(id_site) REFERENCES site(id_site) ON DELETE CASCADE
);

CREATE INDEX idx_ocr_name_mappings_ocr_name ON ocr_name_mappings(ocr_detected_name, id_site);
CREATE INDEX idx_ocr_name_mappings_actual_name ON ocr_name_mappings(actual_username, id_site);
```

## Files Added

1. **Hiss/COCRNameMapping.h** - Header file for the OCR name mapping class
2. **Hiss/COCRNameMapping.cpp** - Implementation of OCR name mapping with PostgreSQL queries
3. **Hiss/PT4_OCRNameMapping_Migration.sql** - SQL migration script to create the table

## Files Modified

1. **Hiss/CPokerTrackerThread.h** - Updated struct and added OCR mapping member
2. **Hiss/CPokerTrackerThread.cpp** - Added name mapping lookup and verification

## Setup Instructions

### Step 1: Create the Database Table
Connect to your PokerTracker 4 PostgreSQL database and run:

```bash
psql -h <PT4_HOST> -U <PT4_USER> -d <PT4_DATABASE> -f Hiss/PT4_OCRNameMapping_Migration.sql
```

Or manually execute the SQL from the migration file in your PostgreSQL client.

### Step 2: Populate Initial Mappings (Optional)
The migration script automatically populates the table with all existing player names from the `player` table, marking them as `verified = TRUE`. This ensures immediate accuracy for known players.

To verify the table was created and populated:

```sql
SELECT COUNT(*) as mapping_count FROM ocr_name_mappings;
SELECT * FROM ocr_name_mappings LIMIT 10;
```

### Step 3: Rebuild OpenHoldem
After the code changes are in place:
1. Add the new files to your Visual Studio project
2. Ensure `COCRNameMapping.h` and `COCRNameMapping.cpp` are included
3. Verify the #include directive in CPokerTrackerThread.h references COCRNameMapping
4. Rebuild the solution

## How It Works

### Name Lookup Process

1. **OCR Scrapes Name**: User sees a name on screen - "JohnDoe123"
2. **Database Lookup**: System queries ocr_name_mappings table for mapping
3. **Verified Match Found**: Name is marked as verified and displayed in bright green
4. **No Match Found**: Name falls back to OCR result, displayed in white

### Color Coding

- **Bright Green (0x00FF00)**: Name is verified from hand history mapping
- **White (0xFFFFFF)**: Name is from OCR detection (unverified)

### Caching

The COCRNameMapping class includes an in-memory cache to avoid repeated database queries for the same OCR names. Cache entries are automatically cleared when:
- A mapping is updated
- New session starts
- `ClearCache()` is called explicitly

## Data Flow

```
OCR Detection (Tesseract)
         ↓
CheckIfNameExistsInDB()
         ↓
FindName() [fuzzy match PT4 player table]
         ↓
LookupOCRNameMapping() [check mapping table]
         ↓
Set name_verified flag and name_color
         ↓
Display name with appropriate formatting
```

## API Reference

### COCRNameMapping Class

```cpp
// Set the PostgreSQL connection
void SetConnection(PGconn *pgconn);

// Look up a mapped name
bool LookupActualName(const char *ocr_detected_name, int id_site, SOCRNameMapping *mapping);

// Save or update a mapping
bool SaveMapping(const char *actual_username, const char *ocr_detected_name, int id_site, bool verified);

// Clear in-memory cache
void ClearCache();
```

### CPokerTrackerThread Class

```cpp
// Lookup OCR name in mapping table
bool LookupOCRNameMapping(const char *ocr_name, int id_site, char *actual_name, bool *is_verified);
```

### SPlayerData Struct (updated)

```cpp
struct SPlayerData {
    char  scraped_name[kMaxLengthOfPlayername];     // OCR detected name
    char  pt_name[kMaxLengthOfPlayername];          // Player name from PT4
    bool  found;                                     // Was name found in database
    bool  name_verified;                            // NEW: Is this name verified from hand history
    COLORREF name_color;                            // NEW: Display color (bright green if verified)
};
```

## Troubleshooting

### "ocr_name_mappings table not found" Error
- Ensure the migration SQL was executed in the PT4 database
- Verify database connection credentials in OpenHoldem preferences
- Check that the user has CREATE TABLE permissions

### Names Still Showing as Unverified
- Check that hand history data exists in the PT4 database
- Verify that player names are correctly captured in hand_summary
- Wait for the PokerTracker update thread to check names (usually 5 seconds)

### Performance Issues
- The COCRNameMapping class caches results - first lookup is slower, subsequent lookups are instant
- For heavily populated tables, increase the cache timeout or add more database indexes
- Consider running `VACUUM ANALYZE` on the ocr_name_mappings table

## Future Enhancements

1. **Auto-Learning from Hand Histories**: Automatically create mappings when OCR names match hand history names
2. **Confidence Scoring**: Add a confidence field to indicate how certain the mapping is
3. **Levenshtein-Assisted Mapping**: Use fuzzy matching to suggest mappings for similar names
4. **Bulk Mapping Management**: UI dialog to manage and verify mappings

## Database Query Examples

### View all mappings for a specific site
```sql
SELECT * FROM ocr_name_mappings 
WHERE id_site = 1 
ORDER BY created_at DESC;
```

### Find unverified mappings
```sql
SELECT * FROM ocr_name_mappings 
WHERE verified = FALSE
ORDER BY created_at DESC;
```

### Add a manual mapping
```sql
INSERT INTO ocr_name_mappings (actual_username, ocr_detected_name, id_site, verified)
VALUES ('JohnSmith', 'John_Smith_123', 1, TRUE);
```

### Update a mapping
```sql
UPDATE ocr_name_mappings 
SET verified = TRUE, last_updated = CURRENT_TIMESTAMP
WHERE ocr_detected_name = 'JohnSmith' AND id_site = 1;
```

### Delete incorrect mappings
```sql
DELETE FROM ocr_name_mappings 
WHERE ocr_detected_name = 'BadOCR' AND verified = FALSE;
```

## Notes

- The system maintains backward compatibility - if the mapping table doesn't exist, lookups simply fail gracefully and fall back to OCR results
- All queries use parameterized statements to prevent SQL injection
- The implementation is thread-safe for read operations (caching)
- Write operations (SaveMapping) should be called from the PokerTracker thread
