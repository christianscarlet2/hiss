-- PT4 Database Migration: OCR Name Mapping Table
-- This table maps OCR-detected player names to actual verified player names
-- from hand history records

-- Create the ocr_name_mappings table
CREATE TABLE IF NOT EXISTS ocr_name_mappings (
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

-- Create index for fast lookups by ocr_detected_name
CREATE INDEX IF NOT EXISTS idx_ocr_name_mappings_ocr_name ON ocr_name_mappings(ocr_detected_name, id_site);

-- Create index for lookups by actual_username
CREATE INDEX IF NOT EXISTS idx_ocr_name_mappings_actual_name ON ocr_name_mappings(actual_username, id_site);

-- Initial data load: Create unverified mappings from distinct player names in recent hands
-- This query extracts player names from hand_summary where they appear
INSERT INTO ocr_name_mappings (actual_username, ocr_detected_name, id_site, verified)
SELECT DISTINCT 
    p.player_name AS actual_username,
    p.player_name AS ocr_detected_name,
    p.id_site,
    TRUE
FROM player p
WHERE p.player_name IS NOT NULL 
  AND p.player_name != ''
  AND NOT EXISTS (
    SELECT 1 FROM ocr_name_mappings om 
    WHERE om.actual_username = p.player_name 
    AND om.id_site = p.id_site
  )
ON CONFLICT DO NOTHING;
