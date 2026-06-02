#!/usr/bin/env bash
#
# tesseract-demo.sh
# -----------------------------------------------------------------------------
# Self-contained Tesseract 5.5 command-line playground for Windows (Git Bash).
#
#   ./tesseract-demo.sh setup     # download Tesseract 5.5 + extract into ./tesseract
#   ./tesseract-demo.sh sample    # generate a sample.png test image
#   ./tesseract-demo.sh demo      # run a tour of the most-used OCR options
#   ./tesseract-demo.sh help      # print the command cheat-sheet
#   ./tesseract-demo.sh           # = setup + sample + demo (one-shot)
#
# The Tesseract binaries are downloaded and extracted locally (portable, no
# admin install). Everything lives next to this script.
# -----------------------------------------------------------------------------

set -euo pipefail

# --- Resolve paths (works no matter where you call it from) ------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TESS_HOME="$SCRIPT_DIR/tesseract"          # where the binaries get extracted
TESS_BIN="$TESS_HOME/tesseract.exe"
TESSDATA_DIR="$TESS_HOME/tessdata"
INSTALLER="$SCRIPT_DIR/tesseract-ocr-w64-setup-5.5.0.20241111.exe"
OUT_DIR="$SCRIPT_DIR/out"
SAMPLE="$SCRIPT_DIR/sample.png"

# Tesseract finds language data via this env var; point it at our local copy.
export TESSDATA_PREFIX="$TESSDATA_DIR"

# Download mirrors (tried in order).
URLS=(
  "https://github.com/tesseract-ocr/tesseract/releases/download/5.5.0/tesseract-ocr-w64-setup-5.5.0.20241111.exe"
  "https://digi.bib.uni-mannheim.de/tesseract/tesseract-ocr-w64-setup-5.5.0.20241111.exe"
  "https://sourceforge.net/projects/tesseract-ocr.mirror/files/5.5.0/tesseract-ocr-w64-setup-5.5.0.20241111.exe/download"
)

say()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
run()  { printf '\033[0;90m$ %s\033[0m\n' "$*"; eval "$*"; }
die()  { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

# --- Find a 7-Zip we can use to unpack the NSIS installer --------------------
find_7z() {
  local c
  for c in 7z 7za "/c/Program Files/7-Zip/7z.exe" "/c/Program Files (x86)/7-Zip/7z.exe"; do
    if command -v "$c" >/dev/null 2>&1 || [ -x "$c" ]; then echo "$c"; return 0; fi
  done
  return 1
}

# -----------------------------------------------------------------------------
# setup: download + extract Tesseract 5.5 into ./tesseract
# -----------------------------------------------------------------------------
cmd_setup() {
  if [ -x "$TESS_BIN" ]; then
    say "Tesseract already present at: $TESS_BIN"
    "$TESS_BIN" --version | head -n1
    return 0
  fi

  # 1) Download the installer if we don't have it yet.
  if [ ! -f "$INSTALLER" ]; then
    say "Downloading Tesseract 5.5.0 installer (~50 MB)"
    local ok=0 url
    for url in "${URLS[@]}"; do
      printf '   trying: %s\n' "$url"
      if curl -fL --retry 3 -o "$INSTALLER" "$url"; then ok=1; break; fi
    done
    [ "$ok" = 1 ] || die "Could not download the installer from any mirror."
  else
    say "Installer already downloaded: $(basename "$INSTALLER")"
  fi

  # 2) Extract it. The installer is an NSIS package -> 7-Zip unpacks it cleanly
  #    into a portable folder. No admin rights, no registry changes.
  say "Extracting binaries into ./tesseract"
  if SEVENZIP="$(find_7z)"; then
    rm -rf "$TESS_HOME"
    run "\"$SEVENZIP\" x -y -o\"$TESS_HOME\" \"$INSTALLER\" >/dev/null"
    # NSIS leaves some junk metadata folders; harmless, but tidy up.
    rm -rf "$TESS_HOME/\$PLUGINSDIR" "$TESS_HOME/\$R0" 2>/dev/null || true
  else
    say "7-Zip not found - falling back to a silent install into ./tesseract"
    # NSIS silent switches: /S = silent, /D=<dir> MUST be last and unquoted.
    local winpath; winpath="$(cygpath -w "$TESS_HOME" 2>/dev/null || echo "$TESS_HOME")"
    run "\"$INSTALLER\" /S /D=$winpath"
  fi

  [ -x "$TESS_BIN" ] || die "tesseract.exe not found after extraction ($TESS_BIN). Install 7-Zip and retry."

  say "Installed Tesseract:"
  "$TESS_BIN" --version | head -n3
  say "Available languages:"
  "$TESS_BIN" --list-langs || true
}

# -----------------------------------------------------------------------------
# sample: make a test image to OCR (PowerShell GDI+, always available on Win;
#         falls back to ImageMagick if you have it).
# -----------------------------------------------------------------------------
cmd_sample() {
  if [ -f "$SAMPLE" ]; then say "Sample image already exists: $SAMPLE"; return 0; fi
  say "Generating sample.png"
  local winpath; winpath="$(cygpath -w "$SAMPLE" 2>/dev/null || echo "$SAMPLE")"
  if command -v powershell >/dev/null 2>&1; then
    powershell -NoProfile -Command "
      Add-Type -AssemblyName System.Drawing;
      \$bmp = New-Object System.Drawing.Bitmap 700,160;
      \$g = [System.Drawing.Graphics]::FromImage(\$bmp);
      \$g.Clear([System.Drawing.Color]::White);
      \$g.TextRenderingHint = 'AntiAliasGridFit';
      \$f = New-Object System.Drawing.Font('Arial',40,[System.Drawing.FontStyle]::Regular);
      \$g.DrawString('Tesseract 5.5  OCR  12345', \$f, [System.Drawing.Brushes]::Black, 15, 50);
      \$g.Dispose();
      \$bmp.Save('$winpath', [System.Drawing.Imaging.ImageFormat]::Png);
      \$bmp.Dispose();"
  elif command -v magick >/dev/null 2>&1; then
    run "magick -size 700x160 xc:white -font Arial -pointsize 40 -fill black -annotate +15+90 'Tesseract 5.5  OCR  12345' \"$SAMPLE\""
  else
    die "No image generator available. Drop any .png/.jpg/.tif here as sample.png."
  fi
  say "Wrote $SAMPLE"
}

# -----------------------------------------------------------------------------
# demo: run the most commonly used tesseract invocations against sample.png
# -----------------------------------------------------------------------------
cmd_demo() {
  [ -x "$TESS_BIN" ] || die "Run './tesseract-demo.sh setup' first."
  [ -f "$SAMPLE" ]    || cmd_sample
  mkdir -p "$OUT_DIR"
  local T="$TESS_BIN"

  say "1) Version & build info"
  run "\"$T\" --version | head -n3"

  say "2) List installed languages"
  run "\"$T\" --list-langs"

  say "3) Basic OCR  ->  out/basic.txt   (image  output  -l LANG)"
  run "\"$T\" \"$SAMPLE\" \"$OUT_DIR/basic\" -l eng"
  run "cat \"$OUT_DIR/basic.txt\""

  say "4) OCR straight to stdout (use 'stdout' as the output name)"
  run "\"$T\" \"$SAMPLE\" stdout -l eng"

  say "5) Page Segmentation Modes (--psm). 6 = single uniform block of text"
  run "\"$T\" \"$SAMPLE\" stdout -l eng --psm 6"
  echo "   common --psm values:"
  echo "     3  fully automatic page segmentation (DEFAULT)"
  echo "     4  single column of variable-size text"
  echo "     6  single uniform block of text"
  echo "     7  treat image as a single text line"
  echo "     8  treat image as a single word"
  echo "    10  treat image as a single character"
  echo "    11  sparse text - find as much text as possible, no order"
  echo "    13  raw line - bypass Tesseract-specific hacks"

  say "6) OCR Engine Mode (--oem). 1 = LSTM neural net (best for 5.x)"
  run "\"$T\" \"$SAMPLE\" stdout -l eng --oem 1 --psm 6"
  echo "   --oem values: 0=legacy  1=LSTM  2=legacy+LSTM  3=default(auto)"

  say "7) Tell Tesseract the source DPI (improves accuracy on odd images)"
  run "\"$T\" \"$SAMPLE\" stdout -l eng --dpi 300"

  say "8) Restrict recognised characters (digits only) via -c whitelist"
  run "\"$T\" \"$SAMPLE\" stdout -l eng --psm 6 -c tessedit_char_whitelist=0123456789"

  say "9) Multiple output formats at once: txt + tsv + hocr + alto + pdf"
  run "\"$T\" \"$SAMPLE\" \"$OUT_DIR/multi\" -l eng txt tsv hocr alto pdf"
  run "ls -1 \"$OUT_DIR\"/multi.*"

  say "10) Searchable PDF (image + invisible text layer)"
  run "\"$T\" \"$SAMPLE\" \"$OUT_DIR/searchable\" -l eng pdf"

  say "11) Word/line bounding boxes as TSV (great for scripting/layout)"
  run "\"$T\" \"$SAMPLE\" stdout -l eng tsv | head -n 8"

  say "12) Keep multiple spaces between words"
  run "\"$T\" \"$SAMPLE\" stdout -l eng -c preserve_interword_spaces=1"

  say "Done. Outputs are in: $OUT_DIR"
}

# -----------------------------------------------------------------------------
# help: printable cheat-sheet of the options you'll actually use
# -----------------------------------------------------------------------------
cmd_help() {
cat <<'EOF'

  TESSERACT 5.5 CHEAT-SHEET  (run binaries: ./tesseract/tesseract.exe)
  ---------------------------------------------------------------------------
  SYNTAX:   tesseract IMAGE OUTPUTBASE [options] [configfile...]
            (OUTPUTBASE 'stdout' prints to terminal; else writes OUTPUTBASE.ext)

  INFO
    tesseract --version                 version & linked libs
    tesseract --list-langs              installed languages
    tesseract --help-extra              every CLI option
    tesseract --print-parameters        all -c configurable variables

  CORE
    -l eng                              language (eng+deu for multiple)
    --psm N                             page segmentation mode (see below)
    --oem N                             0 legacy | 1 LSTM | 2 both | 3 auto
    --dpi 300                           declare source DPI
    --tessdata-dir DIR                  use a specific tessdata folder

  PAGE SEG MODES (--psm)
    3  auto (default)   6  single block   7  single line   8  single word
    10 single char      11 sparse text    13 raw line

  OUTPUT FORMATS (append as config words; combine freely)
    txt  tsv  hocr  alto  pdf  box  (e.g.  tesseract in.png out txt pdf hocr)
    pdf  -> searchable PDF (image + hidden text layer)
    tsv  -> per-word boxes + confidence

  HANDY -c VARIABLES
    -c tessedit_char_whitelist=0123456789
    -c tessedit_char_blacklist=!?@#
    -c preserve_interword_spaces=1
    -c tessedit_create_txt=1            (force a given output on/off)

  BATCH (one image per line in list.txt; output base 'result')
    tesseract list.txt result -l eng txt
    # list.txt can also be a list of multi-page TIFFs

  ENV
    TESSDATA_PREFIX = folder that contains tessdata/   (set by this script)
  ---------------------------------------------------------------------------
EOF
}

# --- Dispatch ----------------------------------------------------------------
case "${1:-all}" in
  setup)  cmd_setup ;;
  sample) cmd_sample ;;
  demo)   cmd_demo ;;
  help|-h|--help) cmd_help ;;
  all)    cmd_setup; cmd_sample; cmd_demo; cmd_help ;;
  *) die "Unknown command '$1'. Use: setup | sample | demo | help" ;;
esac
