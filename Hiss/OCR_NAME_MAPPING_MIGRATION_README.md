# OCR Name Mapping Migration - Quick Start Guide

This directory contains all the necessary files to set up the OCR name mapping system in your PokerTracker 4 database.

## What This Does

The OCR Name Mapping system:
- Creates a new `ocr_name_mappings` table in your PT4 database
- Populates it with verified player names from your poker site history
- Allows OpenHoldem to validate OCR-detected player names against known players
- Displays verified names in **bright green** in real-time during scraping

## Files in This Directory

| File | Purpose |
|------|---------|
| `PT4_OCRNameMapping_Migration.sql` | SQL migration script - creates table and initial data |
| `RunOCRNameMappingMigration.ps1` | PowerShell script - easy way to run migration |
| `RunOCRNameMappingMigration.bat` | Batch script - alternative Windows method |
| `COCRNameMapping.h` | C++ header for name mapping class |
| `COCRNameMapping.cpp` | C++ implementation of name mapping |
| `CPokerTrackerThread.h/.cpp` | Updated PokerTracker integration files |

## Step 1: Install PostgreSQL Client (if not already installed)

The migration scripts use `psql` (PostgreSQL command-line client).

### Check if PostgreSQL is already installed:
```cmd
psql --version
```

If that returns a version number, you're good to go!

### If not installed:

**Windows:**
1. Download PostgreSQL from: https://www.postgresql.org/download/windows/
2. During installation, choose "pgAdmin 4" and "Stack Builder" (optional)
3. **Important:** Make note of the PostgreSQL password you set during installation
4. Add PostgreSQL to your PATH (usually automatic, but verify with `psql --version`)

**macOS:**
```bash
brew install postgresql
```

**Linux (Ubuntu/Debian):**
```bash
sudo apt-get install postgresql-client
```

## Step 2: Run the Migration

Choose the method that works best for you:

### Option A: PowerShell (Recommended for Windows)

```powershell
# Navigate to the Hiss directory
cd c:\www\openholdembot_old\Hiss

# Run with default settings (localhost, port 5432, user postgres)
.\RunOCRNameMappingMigration.ps1

# Or specify custom connection details
.\RunOCRNameMappingMigration.ps1 -Host "myserver.com" -Port "5432" -Username "my_user" -Database "pt4"
```

### Option B: Batch File (Windows Command Prompt)

```cmd
# Navigate to the Hiss directory
cd c:\www\openholdembot_old\Hiss

# Run the migration
RunOCRNameMappingMigration.bat
```

**Note:** Edit the `.bat` file to change `PT4_HOST`, `PT4_PORT`, `PT4_USER`, or `PT4_DATABASE` if needed.

### Option C: Direct psql Command

```bash
# From the Hiss directory
psql -h localhost -p 5432 -U postgres -d pokertracker -f PT4_OCRNameMapping_Migration.sql
```

**Common Connection Parameters:**
- `-h` : Hostname (default: localhost)
- `-p` : Port (default: 5432)
- `-U` : Username (usually postgres or pokertracker)
- `-d` : Database name (usually pokertracker or pt4)
- `-f` : SQL file to execute

## Step 3: Verify the Migration

After the migration completes, verify the table was created:

```bash
# Connect to your PT4 database
psql -h localhost -p 5432 -U postgres -d pokertracker

# In psql prompt, run:
SELECT COUNT(*) FROM ocr_name_mappings;
SELECT * FROM ocr_name_mappings LIMIT 5;
```

You should see:
- A count of player names (matching your `player` table)
- Sample rows with actual player names

## Step 4: Rebuild OpenHoldem

1. Open `OpenHoldem.sln` in Visual Studio
2. Add the new files to your project:
   - `Hiss/COCRNameMapping.h`
   - `Hiss/COCRNameMapping.cpp`
3. Verify `CPokerTrackerThread.h` and `CPokerTrackerThread.cpp` are updated
4. Build → Rebuild Solution
5. Ensure no compilation errors

## Step 5: Start Using It

1. Start OpenHoldem
2. Ensure PokerTracker 4 is running and database connection is configured
3. Begin scraping table names
4. Watch the table window - **verified player names will display in BRIGHT GREEN**

## Troubleshooting

### "psql: command not found"
- PostgreSQL client tools are not in your PATH
- Solution: Install PostgreSQL or add its bin directory to PATH
- Path is usually: `C:\Program Files\PostgreSQL\15\bin` (Windows)

### "FATAL: database 'pokertracker' does not exist"
- Wrong database name in connection string
- Solution: Check your PT4 database name (might be 'pt4', 'pokertracker', or custom name)
- List databases: `psql -l`

### "FATAL: Ident authentication failed"
- Authentication issue - usually wrong password or wrong username
- Solution: Verify credentials in your PokerTracker 4 connection settings
- Test manually: `psql -h localhost -U postgres`

### "ERROR: permission denied to create schema public"
- User doesn't have CREATE TABLE permissions
- Solution: Grant permissions or use a user with admin rights

### Migration runs but names don't show as verified
- The new C++ files weren't compiled into the executable
- Solution: Rebuild OpenHoldem solution in Visual Studio
- Verify COCRNameMapping files are in the project

### No names appear in bright green
- Possible causes:
  1. OpenHoldem wasn't rebuilt after code changes
  2. PokerTracker 4 database connection is not established
  3. No hand history data exists for the players being scraped
- Solution:
  1. Check OpenHoldem debug log: `View → Debug`
  2. Verify PT4 connection in Preferences
  3. Ensure PokerTracker has imported hand histories from your poker site

## Database Query Reference

### View all mapped names
```sql
SELECT * FROM ocr_name_mappings WHERE id_site = 1 ORDER BY created_at DESC;
```

### Find unverified mappings (should be none initially)
```sql
SELECT * FROM ocr_name_mappings WHERE verified = FALSE;
```

### Add a custom mapping manually
```sql
INSERT INTO ocr_name_mappings (actual_username, ocr_detected_name, id_site, verified, confidence)
VALUES ('JohnSmith', 'John_Smith_OCR', 1, TRUE, 0.95);
```

### Update a mapping's confidence
```sql
UPDATE ocr_name_mappings 
SET confidence = 0.85, last_updated = CURRENT_TIMESTAMP
WHERE ocr_detected_name = 'SomeName' AND id_site = 1;
```

### Check mapping statistics
```sql
SELECT 
    id_site,
    COUNT(*) as total_mappings,
    SUM(CASE WHEN verified THEN 1 ELSE 0 END) as verified_count,
    AVG(confidence) as avg_confidence
FROM ocr_name_mappings
GROUP BY id_site;
```

## Performance Tips

1. **First Load:** The first query might take a few seconds (database lookup)
2. **Caching:** Subsequent lookups for the same name are instant (in-memory cache)
3. **For Large Databases:** If you have thousands of players, index creation might take time
   - This is normal and only happens once
   - Indexes are created automatically by the migration script

## Support & Questions

For issues or questions:
1. Check the debug log in OpenHoldem (`View → Debug → PokerTracker Debug`)
2. Review the OCR_NAME_MAPPING_SETUP_GUIDE.md for detailed API documentation
3. Ensure all files (COCRNameMapping.h/.cpp, updated CPokerTrackerThread files) are present and up-to-date

## Next Steps

After successful migration:
- Watch for verified names to display in bright green during scraping
- Monitor performance and cache hit rates in debug log
- Consider setting up automated backups of your PT4 database
- Review mapping statistics periodically to ensure accuracy
