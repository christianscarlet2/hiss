-- ****************************************************************************
--
-- Hiss tablemap database schema
--
-- Stores OpenScrape/Hiss tablemaps (formerly *.tm text files) in PostgreSQL.
-- A parent "tablemaps" row owns the eight section tables (sizes/z$, symbols/s$,
-- regions/r$, fonts/t$, hash points/p$, hash values/h$, images/i$,
-- templates/tpl$) via tablemap_id ... ON DELETE CASCADE.
--
-- This script is idempotent: run it against the server to (re)create the
-- database and tables. The applications also run the CREATE TABLE IF NOT EXISTS
-- statements at startup via CTablemapDB::EnsureSchema().
--
-- Notes:
--   * SQL reserved words left/right are stored as rgn_left/rgn_right.
--   * 32-bit unsigned values (color, hash) are stored as BIGINT.
--   * Constants mirror the C++ code: 10 font groups, 4 hash groups.
--
-- Connect/create:   psql -U postgres -h localhost -f hiss_schema.sql
-- ****************************************************************************

-- The CREATE DATABASE statement must be run outside a transaction and against
-- an existing database. Run it manually first if needed:
--     CREATE DATABASE hiss;
-- Then connect to it and run the rest:
--     \c hiss

-- Application settings: one row per setting key, value is arbitrary JSON.
-- e.g. key='ocr_models'           value='{"a0":"my_model","a1":"my_model"}'
--      key='decimal_split_fields' value='{"fields":["balance","pot","bet"]}'
CREATE TABLE IF NOT EXISTS settings (
  key        TEXT PRIMARY KEY,
  value      JSONB NOT NULL DEFAULT '{}'::jsonb,
  updated_at TIMESTAMP DEFAULT now()
);

CREATE TABLE IF NOT EXISTS tablemaps (
  id             SERIAL PRIMARY KEY,
  name           TEXT UNIQUE NOT NULL,   -- identity; default = .tm filename stem
  sitename       TEXT,
  titletext      TEXT,
  source_file    TEXT,                   -- original .tm path at import time
  bits_per_pixel INT  DEFAULT 32,
  updated_at     TIMESTAMP DEFAULT now()
);

CREATE TABLE IF NOT EXISTS tm_sizes (        -- z$
  tablemap_id INT REFERENCES tablemaps(id) ON DELETE CASCADE,
  name TEXT, width INT, height INT,
  PRIMARY KEY (tablemap_id, name));

CREATE TABLE IF NOT EXISTS tm_symbols (      -- s$
  tablemap_id INT REFERENCES tablemaps(id) ON DELETE CASCADE,
  name TEXT, text TEXT,
  PRIMARY KEY (tablemap_id, name));

CREATE TABLE IF NOT EXISTS tm_regions (      -- r$
  tablemap_id INT REFERENCES tablemaps(id) ON DELETE CASCADE,
  name TEXT, rgn_left INT, rgn_top INT, rgn_right INT, rgn_bottom INT,
  color BIGINT, radius INT, transform TEXT,
  use_default BOOL, threshold INT, use_cropping BOOL, crop_size INT,
  match_mode INT, sharpen INT,
  -- Optional second colour cube for the "Color" (C) transform: a region matches if
  -- the sampled colour is within tolerance (radius) of color OR (when enabled) color2.
  color2 BIGINT DEFAULT 0, color2_enabled BOOL DEFAULT false,
  PRIMARY KEY (tablemap_id, name));

CREATE TABLE IF NOT EXISTS tm_fonts (        -- t$  (x_values = space-separated hex tokens, e.g. "1e 1 1 1f")
  tablemap_id INT REFERENCES tablemaps(id) ON DELETE CASCADE,
  font_group INT, ch TEXT, hexmash TEXT, x_values TEXT,
  PRIMARY KEY (tablemap_id, font_group, hexmash));

CREATE TABLE IF NOT EXISTS tm_hash_points (  -- p$
  tablemap_id INT REFERENCES tablemaps(id) ON DELETE CASCADE,
  hash_group INT, x INT, y INT,
  PRIMARY KEY (tablemap_id, hash_group, x, y));

CREATE TABLE IF NOT EXISTS tm_hash_values (  -- h$
  tablemap_id INT REFERENCES tablemaps(id) ON DELETE CASCADE,
  hash_group INT, name TEXT, hash BIGINT,
  PRIMARY KEY (tablemap_id, hash_group, hash));

-- i$ images are keyed in memory by a hash of name+pixels, so one tablemap can
-- hold several images with the SAME name but different pixels (e.g. multiple
-- samples of a card). A surrogate id is therefore the primary key, not the name.
CREATE TABLE IF NOT EXISTS tm_images (       -- i$  (pixels = %08x per pixel, rows newline-joined)
  id SERIAL PRIMARY KEY,
  tablemap_id INT REFERENCES tablemaps(id) ON DELETE CASCADE,
  name TEXT, width INT, height INT, pixels TEXT);
CREATE INDEX IF NOT EXISTS idx_tm_images_tm ON tm_images(tablemap_id);

CREATE TABLE IF NOT EXISTS tm_templates (    -- tpl$
  tablemap_id INT REFERENCES tablemaps(id) ON DELETE CASCADE,
  name TEXT, rgn_left INT, rgn_top INT, rgn_right INT, rgn_bottom INT,
  width INT, height INT, use_default BOOL, match_mode INT, created BOOL,
  pixels TEXT,
  PRIMARY KEY (tablemap_id, name));
