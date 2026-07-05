#!/usr/bin/env python3
"""
lint_handoff.py -- mechanical handoff/config linter (s14 T5, docs/TOOLING_WORKORDER.md).

Purpose: enforce the s13 audit rules (docs/LESSONS.md #21/#22) so a broken handoff
is caught the day it is written, not the next session in. Four checks:

  1. Evidence-citation existence: every path-like citation in STATUS.md (a token
     that starts with scratch/, tools/, docs/, yakuza/, libs/, or runtime/ and ends
     in a file extension) must exist on disk -- unless the citation is annotated
     `(GONE` (the established marker for a known-lost evidence file, e.g.
     `scratch/foo.err (GONE -- s13 audit: not on disk)`). A trailing shell-glob
     citation (contains `*`) is checked with glob-existence (>=1 match), not a
     literal stat. A combined suffix citation (`scratch/x.out/.err`, meaning "both
     x.out and x.err") is split and each half is checked.
  2. STATUS.md is <= 62,000 bytes AND contains a line starting `## ⚡ NEXT ACTION`.
  3. FLAGS sync: every `getenv("YZ_...")` name found under runtime/, libs/,
     yakuza/*.{cpp,c,h} must appear somewhere in docs/FLAGS.md. A flag present in
     FLAGS.md but never found in code is reported INFO ("retired?"), not a FAIL.
  4. docs/DONT_RECHASE.md exists and its `|`-delimited table rows each have >= 4
     pipe characters (INFO-level only -- never fails the run).

CLI:
    py -3 tools\\lint_handoff.py [--verbose]

Exit code: 0 if checks 1/2/3 all PASS, 1 if any of checks 1/2/3 FAIL. Check 4 is
INFO-only and never affects the exit code (per spec, it is explicitly INFO-level).

Everything is read-only: this tool never edits STATUS.md/FLAGS.md/DONT_RECHASE.md.
"""

import argparse
import glob
import os
import re
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

STATUS_MD = os.path.join(REPO_ROOT, "STATUS.md")
FLAGS_MD = os.path.join(REPO_ROOT, "docs", "FLAGS.md")
DONT_RECHASE_MD = os.path.join(REPO_ROOT, "docs", "DONT_RECHASE.md")

STATUS_MAX_BYTES = 62000
NEXT_ACTION_PREFIX = "## ⚡ NEXT ACTION"  # '## ⚡ NEXT ACTION'

# Directories a citation token must start with to be considered "path-like".
CITATION_DIRS = ("scratch", "tools", "docs", "yakuza", "libs", "runtime")

# A citation token: one of the CITATION_DIRS, a '/', then path chars, ending in
# a dot + an extension (letters/digits only, so we don't swallow trailing
# punctuation like '.' at a sentence end -- extension regex is greedy on the
# LAST dot-group so "file.out/.err" is captured whole and split later).
CITATION_RE = re.compile(
    r"\b(?:%s)/[A-Za-z0-9_./\\*-]*\.[A-Za-z0-9]+(?:/\.[A-Za-z0-9]+)*"
    % "|".join(CITATION_DIRS)
)

GETENV_RE = re.compile(r'getenv\(\s*"(YZ_[A-Za-z0-9_]*)"\s*\)')
FLAG_TOKEN_RE = re.compile(r"\bYZ_[A-Za-z0-9_]+\b")


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def path_exists_on_disk(rel_path):
    """rel_path is repo-root-relative, POSIX-style ('/'), possibly a glob."""
    native = rel_path.replace("/", os.sep)
    abs_path = os.path.join(REPO_ROOT, native)
    if "*" in rel_path or "?" in rel_path:
        return len(glob.glob(abs_path)) > 0
    return os.path.exists(abs_path)


def split_combined_citation(token):
    """
    Split a combined-suffix citation like 'scratch/_s13bracket.out/.err' into
    ['scratch/_s13bracket.out', 'scratch/_s13bracket.err']. Tokens without the
    combined form are returned as a single-element list unchanged.
    """
    m = re.match(r"^(.*)\.([A-Za-z0-9]+)((?:/\.[A-Za-z0-9]+)+)$", token)
    if not m:
        return [token]
    stem, first_ext, rest = m.group(1), m.group(2), m.group(3)
    out = [stem + "." + first_ext]
    for extra in rest.split("/"):
        if extra:
            out.append(stem + extra)  # extra already starts with '.'
    return out


def check1_citations(verbose):
    """Every path-like citation in STATUS.md exists, unless annotated '(GONE'."""
    text = read_text(STATUS_MD)
    missing = []
    checked = []
    gone_skipped = []
    for m in CITATION_RE.finditer(text):
        token = m.group(0)
        end = m.end()
        # Look at what immediately follows the token (allow the token's own
        # trailing punctuation grabbed by the regex, e.g. a stray ')' -- so
        # peek a small window and also try trimming one trailing punct char).
        tail = text[end:end + 8]
        trimmed = token
        # The regex can occasionally swallow a trailing non-path char if it
        # was itself alnum-adjacent; guard by stripping common trailing
        # punctuation that isn't part of a real path.
        while trimmed and trimmed[-1] in ").,:;":
            trimmed = trimmed[:-1]
        is_gone = tail.startswith(" (GONE") or tail.startswith("(GONE")
        for sub in split_combined_citation(trimmed):
            checked.append(sub)
            if is_gone:
                gone_skipped.append(sub)
                continue
            if not path_exists_on_disk(sub):
                missing.append(sub)
    ok = len(missing) == 0
    detail = "%d citations checked, %d skipped (GONE), %d missing" % (
        len(checked), len(gone_skipped), len(missing))
    if verbose:
        if gone_skipped:
            detail += "\n    GONE-skipped: " + ", ".join(sorted(set(gone_skipped)))
        if missing:
            detail += "\n    MISSING: " + ", ".join(sorted(set(missing)))
    return ok, detail, missing


def check2_status_shape(verbose):
    """STATUS.md <= 62000 bytes AND contains a '## NEXT ACTION' line."""
    size = os.path.getsize(STATUS_MD)
    text = read_text(STATUS_MD)
    has_next_action = any(
        line.startswith(NEXT_ACTION_PREFIX) for line in text.splitlines()
    )
    size_ok = size <= STATUS_MAX_BYTES
    ok = size_ok and has_next_action
    detail = "size=%d bytes (cap %d, %s); NEXT ACTION line %s" % (
        size, STATUS_MAX_BYTES, "OK" if size_ok else "OVER",
        "present" if has_next_action else "MISSING",
    )
    return ok, detail


def collect_code_flags(verbose):
    flags = {}  # name -> list of (file, lineno)
    for topdir in ("runtime", "libs"):
        base = os.path.join(REPO_ROOT, topdir)
        if not os.path.isdir(base):
            continue
        for root, _dirs, files in os.walk(base):
            for fn in files:
                if fn.endswith((".c", ".h", ".cpp")):
                    fpath = os.path.join(root, fn)
                    _scan_file_for_getenv(fpath, flags)
    yakuza_dir = os.path.join(REPO_ROOT, "yakuza")
    if os.path.isdir(yakuza_dir):
        for fn in os.listdir(yakuza_dir):
            if fn.endswith((".cpp", ".c", ".h")):
                _scan_file_for_getenv(os.path.join(yakuza_dir, fn), flags)
    return flags


def _scan_file_for_getenv(fpath, flags):
    try:
        text = read_text(fpath)
    except OSError:
        return
    rel = os.path.relpath(fpath, REPO_ROOT).replace(os.sep, "/")
    for i, line in enumerate(text.splitlines(), start=1):
        for m in GETENV_RE.finditer(line):
            flags.setdefault(m.group(1), []).append("%s:%d" % (rel, i))


def collect_doc_flags():
    text = read_text(FLAGS_MD)
    return set(FLAG_TOKEN_RE.findall(text))


def check3_flags_sync(verbose):
    code_flags = collect_code_flags(verbose)
    doc_flags = collect_doc_flags()
    missing_from_docs = sorted(set(code_flags) - doc_flags)
    retired_maybe = sorted(doc_flags - set(code_flags))
    ok = len(missing_from_docs) == 0
    detail = "%d flags in code, %d in FLAGS.md; %d missing from FLAGS.md; %d in FLAGS.md not in code (INFO)" % (
        len(code_flags), len(doc_flags), len(missing_from_docs), len(retired_maybe))
    if verbose or missing_from_docs:
        if missing_from_docs:
            lines = []
            for flag in missing_from_docs:
                sites = code_flags.get(flag, [])
                sample = sites[0] if sites else "?"
                lines.append("      %-28s first seen %s (%d call site(s))" % (flag, sample, len(sites)))
            detail += "\n    MISSING FROM FLAGS.md:\n" + "\n".join(lines)
        if verbose and retired_maybe:
            detail += "\n    INFO retired? (in FLAGS.md, not in code): " + ", ".join(retired_maybe)
    return ok, detail, missing_from_docs, retired_maybe


def check4_dont_rechase(verbose):
    if not os.path.exists(DONT_RECHASE_MD):
        return False, "docs/DONT_RECHASE.md does not exist"
    text = read_text(DONT_RECHASE_MD)
    rows = [l for l in text.splitlines() if l.strip().startswith("|")]
    bad_rows = []
    for l in rows:
        pipe_count = l.count("|")
        if pipe_count < 4:
            bad_rows.append((pipe_count, l.strip()[:80]))
    ok_shape = len(bad_rows) == 0
    detail = "%d table rows found, %d with < 4 pipes (INFO-only, never fails)" % (
        len(rows), len(bad_rows))
    if verbose and bad_rows:
        detail += "\n    " + "\n    ".join(
            "pipes=%d: %s" % (c, r) for c, r in bad_rows)
    # Per spec this check is INFO-level: always reported PASS/INFO, never FAIL.
    return True, detail if os.path.exists(DONT_RECHASE_MD) else detail


def main():
    ap = argparse.ArgumentParser(description="Handoff/config linter (s14 T5).")
    ap.add_argument("--verbose", action="store_true", help="print extra detail per check")
    args = ap.parse_args()

    rows = []
    exit_code = 0

    ok1, detail1, _missing1 = check1_citations(args.verbose)
    rows.append(("1. STATUS.md citations exist", "PASS" if ok1 else "FAIL", detail1))
    if not ok1:
        exit_code = 1

    ok2, detail2 = check2_status_shape(args.verbose)
    rows.append(("2. STATUS.md size + NEXT ACTION", "PASS" if ok2 else "FAIL", detail2))
    if not ok2:
        exit_code = 1

    ok3, detail3, missing3, _retired3 = check3_flags_sync(args.verbose)
    rows.append(("3. FLAGS.md sync (code -> docs)", "PASS" if ok3 else "FAIL", detail3))
    if not ok3:
        exit_code = 1

    ok4, detail4 = check4_dont_rechase(args.verbose)
    rows.append(("4. DONT_RECHASE.md table shape", "INFO", detail4))
    # check 4 is INFO-level: never affects exit_code, per spec.

    print("=" * 78)
    print("lint_handoff.py -- %s" % ("ALL GATING CHECKS PASS" if exit_code == 0 else "FAILURES FOUND"))
    print("=" * 78)
    for name, status, detail in rows:
        print("%-34s %-5s %s" % (name, status, detail.splitlines()[0]))
        for extra in detail.splitlines()[1:]:
            print(" " * 41 + extra)
    print("=" * 78)

    if missing3:
        print()
        print("Flags in code but NOT in docs/FLAGS.md (%d) -- for parent backfill:" % len(missing3))
        for flag in missing3:
            print("  " + flag)

    return exit_code


if __name__ == "__main__":
    sys.exit(main())
