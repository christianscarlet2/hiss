-- Automation-only schema changes for the "automation process" feature.
--
-- Run against the `hiss` database. Everything here targets the `automation` SCHEMA and must NOT
-- be folded into CTablemapDB::EnsureSchema(): that runs for Hiss and Vision too, and the
-- tm_regions primary-key change below would silently re-key THEIR region tables. The automation
-- schema was created by cloning public.tablemaps + the tm_* children (LIKE ... INCLUDING ALL);
-- this file records the changes made on top of that clone so a fresh box can reproduce them.
--
-- Idempotent: safe to re-run.

BEGIN;

-- An automation map's regions belong to ONE screenshot of ONE process, not to the map as a whole.
--   process = which flow ('freeroll', 'tournament', 'observe cash game', ...)
--   step    = which of the 9 screenshots in that flow (1-9)
-- The same region name therefore legitimately repeats across steps ("join_button" on step 2 and
-- step 5 are different rectangles on different screens), which is why both columns join the key.
ALTER TABLE automation.tm_regions
  ADD COLUMN IF NOT EXISTS process text     NOT NULL DEFAULT '',
  ADD COLUMN IF NOT EXISTS step    smallint NOT NULL DEFAULT 1;

ALTER TABLE automation.tm_regions DROP CONSTRAINT IF EXISTS tm_regions_pkey;
ALTER TABLE automation.tm_regions
  ADD CONSTRAINT tm_regions_pkey PRIMARY KEY (tablemap_id, process, step, name);

ALTER TABLE automation.tm_regions DROP CONSTRAINT IF EXISTS tm_regions_step_range;
ALTER TABLE automation.tm_regions
  ADD CONSTRAINT tm_regions_step_range CHECK (step BETWEEN 1 AND 9);

CREATE INDEX IF NOT EXISTS tm_regions_process_step_idx
  ON automation.tm_regions (tablemap_id, process, step);

-- The 9 screenshots per process. `pixels` uses the SAME encoding as tm_images -- one %08x per
-- pixel, rows separated by \n -- so the existing serialise/deserialise code is reused rather than
-- a second image format being invented for this one table.
CREATE TABLE IF NOT EXISTS automation.process_screenshots (
  tablemap_id integer   NOT NULL REFERENCES automation.tablemaps(id) ON DELETE CASCADE,
  process     text      NOT NULL,
  step        smallint  NOT NULL CHECK (step BETWEEN 1 AND 9),
  label       text      NOT NULL DEFAULT '',
  width       integer   NOT NULL DEFAULT 0,
  height      integer   NOT NULL DEFAULT 0,
  pixels      text,
  updated_at  timestamp NOT NULL DEFAULT now(),
  PRIMARY KEY (tablemap_id, process, step)
);

COMMIT;
