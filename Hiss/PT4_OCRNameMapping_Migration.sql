-- PT4 Database Migration: OCR Name Mapping Table
-- This table maps OCR-detected player names to actual verified player names
-- from hand history records in PokerTracker 4

-- Create the ocr_name_mappings table
CREATE TABLE IF NOT EXISTS ocr_name_mappings (
    id SERIAL PRIMARY KEY,
    actual_username VARCHAR(255) NOT NULL,
    ocr_detected_name VARCHAR(255) NOT NULL,
    id_site INTEGER NOT NULL,
    verified BOOLEAN DEFAULT FALSE,
    confidence REAL DEFAULT 0.0,  -- Confidence score (0-1) based on how many times name was seen
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    last_updated TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(ocr_detected_name, id_site)
    -- No FK to a sites table: PT4 schema variants name it differently
    -- (e.g. "site" vs "lookup_sites"). id_site is treated as an opaque int.
);

-- Create indexes for fast lookups
CREATE INDEX IF NOT EXISTS idx_ocr_name_mappings_ocr_name ON ocr_name_mappings(ocr_detected_name, id_site);
CREATE INDEX IF NOT EXISTS idx_ocr_name_mappings_actual_name ON ocr_name_mappings(actual_username, id_site);
CREATE INDEX IF NOT EXISTS idx_ocr_name_mappings_verified ON ocr_name_mappings(verified, id_site);

-- Initial data load: Populate all player names from the player table as verified mappings
-- These are our source of truth - actual player names known to the poker site
INSERT INTO ocr_name_mappings (actual_username, ocr_detected_name, id_site, verified, confidence)
SELECT DISTINCT 
    p.player_name AS actual_username,
    p.player_name AS ocr_detected_name,
    p.id_site,
    TRUE,
    1.0  -- Maximum confidence for known player names
FROM player p
WHERE p.player_name IS NOT NULL 
  AND TRIM(p.player_name) != ''
  AND NOT EXISTS (
    SELECT 1 FROM ocr_name_mappings om 
    WHERE om.actual_username = p.player_name 
    AND om.id_site = p.id_site
  )
ON CONFLICT (ocr_detected_name, id_site) DO UPDATE SET 
  verified = TRUE,
  confidence = 1.0,
  last_updated = CURRENT_TIMESTAMP;
