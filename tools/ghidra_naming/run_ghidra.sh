#!/usr/bin/env bash
# Headless Ghidra analysis + name export pipeline for an Xbox XBE.
#
# Steps:
#   1. (re)build the flat raw image from the XBE via extract_for_ghidra.py
#   2. import the flat image into a Ghidra project as raw x86:LE:32 @ 0x10000
#   3. run auto-analysis (FunctionID/FidDb + RTTI + decompiler analyzers on)
#   4. run ExportXbeNames.py post-script to dump functions/symbols JSON
#      (and optionally decompiled C)
#
# Usage:
#   tools/ghidra_naming/run_ghidra.sh                 # analyze + export funcs/symbols
#   tools/ghidra_naming/run_ghidra.sh decompile 4000  # also decompile up to 4000
#   tools/ghidra_naming/run_ghidra.sh decompile all   # decompile everything (slow)
#
# Re-runs: if the Ghidra project already contains the imported program, pass
#   IMPORT=0 to skip re-import and only re-run analysis/export, or
#   ANALYZE=0 to skip analysis and only re-export (e.g. to continue decompiling).
#
# Env overrides:
#   GHIDRA_ROOT  (default: /c/tools/ghidra) - searched for the newest
#                ghidra_*_PUBLIC when GHIDRA_HOME is unset
#   GHIDRA_HOME  an exact install, overriding the search
#   XBE          (REQUIRED: path to the Xbox default.xbe to analyze)
#   IMPORT=0     skip import step (program already in project)
#   ANALYZE=0    skip analysis (-noanalysis); just (re)run export post-script

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

# Ghidra's install directory carries its version, so pinning one in a default
# dates the script the first time anyone updates. Take the newest
# ghidra_*_PUBLIC under GHIDRA_ROOT, and accept GHIDRA_ROOT itself being an
# install (or a symlink to one) for anyone who keeps it unversioned.
# Reported by @DarthSidious666 (#26), who was 10 revisions ahead of the pin.
GHIDRA_ROOT="${GHIDRA_ROOT:-/c/tools/ghidra}"
if [ -z "${GHIDRA_HOME:-}" ]; then
    if [ -f "$GHIDRA_ROOT/support/analyzeHeadless.bat" ]; then
        GHIDRA_HOME="$GHIDRA_ROOT"
    else
        GHIDRA_HOME="$(ls -d "$GHIDRA_ROOT"/ghidra_*_PUBLIC 2>/dev/null \
                       | sort -V | tail -1)"
    fi
fi
if [ -z "${GHIDRA_HOME:-}" ]; then
    echo "ERROR: no Ghidra install found under $GHIDRA_ROOT." >&2
    echo "       Set GHIDRA_HOME=/path/to/ghidra_<version>_PUBLIC," >&2
    echo "       or GHIDRA_ROOT to the directory holding them." >&2
    exit 1
fi
HEADLESS="$GHIDRA_HOME/support/analyzeHeadless.bat"
XBE="${XBE:?ERROR: set XBE=/path/to/Xbox/default.xbe (the XBE to analyze)}"

WORK="$HERE/work"
PROJ_DIR="$WORK/ghidra_project"
PROJ_NAME="$(basename "$(dirname "$XBE")" 2>/dev/null)"
case "$PROJ_NAME" in ""|"."|"/") PROJ_NAME="xbe";; esac
EXPORT_DIR="$HERE/export"
FLAT="$WORK/xbe_flat.bin"
SCRIPT_DIR="$HERE/ghidra_scripts"
PROG_NAME="xbe_flat.bin"   # name inside the Ghidra project

DO_DECOMPILE="${1:-nodecompile}"
DECOMP_LIMIT="${2:-4000}"
DECOMP_TIMEOUT="${3:-30}"

mkdir -p "$WORK" "$EXPORT_DIR" "$PROJ_DIR"

echo "=============================================================="
echo " Xbox XBE Ghidra naming pipeline"
echo "=============================================================="
echo " GHIDRA_HOME : $GHIDRA_HOME"
echo " XBE         : $XBE"
echo " Work dir    : $WORK"
echo " Export dir  : $EXPORT_DIR"
echo " Decompile   : $DO_DECOMPILE (limit=$DECOMP_LIMIT timeout=${DECOMP_TIMEOUT}s)"
echo "=============================================================="

if [ ! -f "$HEADLESS" ]; then
  echo "ERROR: analyzeHeadless.bat not found at $HEADLESS" >&2
  exit 1
fi
if [ ! -f "$XBE" ]; then
  echo "ERROR: XBE not found at $XBE" >&2
  exit 1
fi

# Step 1: build flat image (idempotent)
echo "[1/3] Building flat image from XBE ..."
py -3 "$HERE/extract_for_ghidra.py" "$XBE" --out-dir "$WORK"

# Convert paths to Windows form for the .bat (analyzeHeadless is a Windows batch).
to_win() { cygpath -w "$1" 2>/dev/null || echo "$1"; }
W_PROJ_DIR="$(to_win "$PROJ_DIR")"
W_EXPORT_DIR="$(to_win "$EXPORT_DIR")"
W_FLAT="$(to_win "$FLAT")"
W_SCRIPT_DIR="$(to_win "$SCRIPT_DIR")"

IMPORT="${IMPORT:-1}"
ANALYZE="${ANALYZE:-1}"

# Analyzer enablement is done by the -preScript (SetAnalysisOptions.java).
# Do NOT pass "-analysisTimeoutPerFile 0": in headless that means a ZERO-second
# timeout (analysis aborts immediately), not "unlimited". Omitting it = no
# per-file timeout, which is what we want for a full 7.6 MB analysis.
ANALYSIS_PROPS=()

# Step 2/3: import + analyze + export, OR re-open + export.
if [ "$IMPORT" = "1" ]; then
  echo "[2/3] Importing flat image + analyzing + exporting ..."
  ANALYZE_FLAG=()
  if [ "$ANALYZE" = "0" ]; then ANALYZE_FLAG=(-noanalysis); fi
  "$HEADLESS" "$W_PROJ_DIR" "$PROJ_NAME" \
    -import "$W_FLAT" \
    -loader BinaryLoader \
    -loader-baseAddr 0x10000 \
    -processor "x86:LE:32:default" \
    -cspec windows \
    -overwrite \
    "${ANALYZE_FLAG[@]}" \
    "${ANALYSIS_PROPS[@]}" \
    -scriptPath "$W_SCRIPT_DIR" \
    -preScript SetAnalysisOptions.java \
    -postScript ExportXbeNames.py "$W_EXPORT_DIR" "$DO_DECOMPILE" "$DECOMP_LIMIT" "$DECOMP_TIMEOUT"
else
  echo "[2/3] Re-opening existing program + exporting (no re-import) ..."
  # -process opens the already-imported program. Add -noanalysis to skip
  # re-running auto-analysis (use this to (re)export or continue decompiling
  # without paying for analysis again). ANALYZE=1 re-runs analysis on the
  # existing program.
  NOANALYZE_FLAG=(-noanalysis)
  if [ "$ANALYZE" = "1" ]; then NOANALYZE_FLAG=(); fi
  "$HEADLESS" "$W_PROJ_DIR" "$PROJ_NAME" \
    -process "$PROG_NAME" \
    "${NOANALYZE_FLAG[@]}" \
    -scriptPath "$W_SCRIPT_DIR" \
    -postScript ExportXbeNames.py "$W_EXPORT_DIR" "$DO_DECOMPILE" "$DECOMP_LIMIT" "$DECOMP_TIMEOUT"
fi

echo "=============================================================="
echo " Done. Exports in: $EXPORT_DIR"
ls -la "$EXPORT_DIR" || true
echo "=============================================================="
