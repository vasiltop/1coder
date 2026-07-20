#!/usr/bin/env python3
"""Differential test: run the same keystrokes through neovim and through this
editor, and compare what each did.

Unit tests only cover the input I thought to write down, which is exactly where
this editor's bugs have hidden -- a blank line before the text being operated
on, a file with no trailing newline, a cursor sitting at the end of a line.
Neovim is the specification, so it can generate the expected answer instead of
me guessing it.

Both the resulting buffer *and* the final cursor position are compared: most
motions move the cursor without touching the text, so text alone would miss half
the surface.

Usage:
    tools/vimdiff.py            # the full corpus
    tools/vimdiff.py --quick    # a smaller subset
    tools/vimdiff.py -k dw      # only cases whose keys contain "dw"
    tools/vimdiff.py -v         # show agreements too
"""

import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EDITOR = os.path.join(ROOT, "build", "editor")

# Minimal settings: process isolation only. No behaviour settings that would
# mask differences between neovim and this editor.
#
#   nocompatible   vim defaults rather than vi's
#   noswapfile     no stray state between runs
NVIM_SETTINGS = "set nocompatible noswapfile"

# Key names this editor and neovim both understand. Everything else is passed
# through literally, so `<<` stays two less-than characters rather than being
# mistaken for a key name.
KEY_NAMES = {
    "Esc", "CR", "Tab", "BS", "Space", "Del", "Up", "Down", "Left", "Right",
    "Home", "End", "PageUp", "PageDown",
}


class CaseError(Exception):
    """Raised when a single test case fails to run (timeout, crash, or missing artifact)."""


def run_subprocess(cmd, *, timeout=30, case_id="", env=None):
    """Run *cmd*, raise CaseError on timeout or nonzero exit.

    Diagnostics include the executable name, case identity, exit code, and
    decoded stderr so failures are immediately actionable.
    """
    try:
        result = subprocess.run(
            cmd, capture_output=True, timeout=timeout, env=env
        )
    except subprocess.TimeoutExpired:
        raise CaseError(
            "timeout running %s for case %r" % (cmd[0], case_id)
        )
    if result.returncode != 0:
        raise CaseError(
            "exit %d from %s for case %r: %s"
            % (result.returncode, cmd[0], case_id,
               result.stderr.decode(errors="replace"))
        )
    return result


def preflight():
    """Verify prerequisites. Prints one actionable message and returns False if absent."""
    if shutil.which("nvim") is None:
        print("nvim not found: install neovim and ensure it is on PATH",
              file=sys.stderr)
        return False
    if not (os.path.isfile(EDITOR) and os.access(EDITOR, os.X_OK)):
        print("editor not built or not executable: %s" % EDITOR,
              file=sys.stderr)
        return False
    return True


def to_nvim_keys(keys):
    """Translate this editor's binding notation into a feedkeys argument string.

    Returns something ready to sit inside double quotes in a Vim expression:
    named keys become `\\<Esc>` and literal characters are escaped. Escaping
    has to happen per character -- escaping the whole string afterwards would
    turn the backslash of `\\<Esc>` into a literal one, and every insert-mode
    case would silently end up typing the text `<Esc>` instead of pressing
    escape.
    """
    out = []
    i = 0
    while i < len(keys):
        if keys[i] == "<":
            close = keys.find(">", i)
            if close > i:
                body = keys[i + 1:close]
                prefixed = body[:2] in ("C-", "S-", "A-", "M-", "D-")
                name = body[2:] if prefixed else body
                if name in KEY_NAMES or prefixed:
                    out.append("\\<" + body + ">")
                    i = close + 1
                    continue
        ch = keys[i]
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        else:
            out.append(ch)
        i += 1
    return "".join(out)


def run_nvim(text, keys, case_id=""):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write(text)
        path = f.name
    pos_path = path + ".pos"

    escaped = to_nvim_keys(keys)
    try:
        run_subprocess(
            ["nvim", "--headless", "-u", "NONE", "-i", "NONE", path,
             "-c", NVIM_SETTINGS,
             "-c", 'call feedkeys("%s", "ntx")' % escaped,
             "-c", 'call writefile([line(".").":".col(".")], "%s")' % pos_path,
             "-c", "wq"],
            case_id=case_id,
        )
        if not os.path.exists(path):
            raise CaseError("missing edited file for case %r" % case_id)
        if not os.path.exists(pos_path):
            raise CaseError("missing cursor dump for case %r" % case_id)
        with open(path) as fh:
            out = fh.read()
        with open(pos_path) as fh:
            pos = fh.read().strip()
    finally:
        for p in (path, pos_path):
            if os.path.exists(p):
                os.unlink(p)
    return out, pos


def run_ours(text, keys, case_id=""):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write(text)
        path = f.name
    pos_path = path + ".pos"

    env = dict(os.environ, SDL_VIDEODRIVER="dummy")
    try:
        run_subprocess(
            [EDITOR, path, "--keys", keys + ":w<CR>", "--dump", pos_path],
            env=env,
            case_id=case_id,
        )
        if not os.path.exists(path):
            raise CaseError("missing edited file for case %r" % case_id)
        if not os.path.exists(pos_path):
            raise CaseError("missing cursor dump for case %r" % case_id)
        with open(path) as fh:
            out = fh.read()
        with open(pos_path) as fh:
            # "line:col MODE" -- only the position is compared.
            pos = fh.read().strip().split(" ")[0]
    finally:
        for p in (path, pos_path):
            if os.path.exists(p):
                os.unlink(p)
    return out, pos


# ---------------------------------------------------------------------------
# The corpus
#
# The text shapes matter more than the key sequences. Every bug this harness
# was built after came from a shape the unit tests never contained: a blank
# line, a missing trailing newline, a cursor at a line end.
# ---------------------------------------------------------------------------

TEXTS = {
    "plain":          "alpha beta gamma\n",
    "blank-line":     "alpha\n\n## Build\n",
    "blank-runs":     "a\n\n\nb\n",
    "multi":          "one\ntwo\nthree\n",
    "two-words":      "one two\nthree four\n",
    "indented":       "    indented\n  less\nnone\n",
    "no-eol":         "last line no newline",
    "punctuation":    "foo(bar).baz\n",
    "single-char":    "x\n",
    "trailing-space": "word   \nnext\n",
    "leading-blank":  "\nafter blank\n",
    "empty":          "",
    "lone-newline":   "\n",
    "tab-indented":   "\tword\n\tsecond\n",
    "utf8":           "café\nnaïve\n",
}

MOTIONS = [
    "w", "3w", "W", "b", "B", "e", "E", "0", "^", "$", "_", "2_",
    "gg", "G", "2G", "{", "}", "h", "l", "j", "k", "5l", "2j",
    "fa", "ta", "Fa", "Ta", "%",
]

OPERATORS = [
    "dw", "dW", "de", "db", "d$", "d0", "d^", "d_", "dd", "2dd", "dj", "dk", "dG", "dgg",
    "yw", "yy", "ye", "y$",
    "cwZZ<Esc>", "cc新<Esc>", "c$Q<Esc>",
    ">>", "<<", ">j", "2>>",
]

EDITS = [
    "x", "3x", "X", "D", "C!<Esc>", "J", "2J",
    "ihi<Esc>", "Ihi<Esc>", "ahi<Esc>", "A!<Esc>", "ohi<Esc>", "Ohi<Esc>",
    "rz", "~",
]

PASTES = [
    "yyp", "yyP", "ywp", "ywP", "yep", "ddp", "ddP", "dwp", "dwP",
    "yy2p", "xp", "xP",
]

COMPOUND = [
    "dwu", "dwuu", "dw<C-r>", "dw.", "x.", "ddu", "ihi<Esc>u",
    "wdw", "wwdw", "$x", "$dw", "$p", "jdw", "jp", "jP",
    "vlld", "Vd", "vjd", "vly$p", "viwd",
]

QUICK_KEYS = ["w", "b", "e", "$", "dw", "dd", "yyp", "ywP", "x", "p", "P", "wdw", "jp", ">>", "diw"]

# Shape-specific workflows run only in full mode; not part of the cross-product.
FOCUSED_CASES = [
    # large/clamped and multiplied counts
    ("plain",       "100dw"),
    ("multi",       "100G"),
    # reverse visual selection
    ("plain",       "$v0d"),
    # counted charwise paste
    ("plain",       "y$3p"),
    # dot-repeat and undo/redo depth
    ("plain",       "x.."),
    ("plain",       "dw.u<C-r>"),
    # macro record/replay and count
    ("plain",       "qadwq@a"),
    ("plain",       "qaxq3@a"),
    # named register and yank register
    ("multi",       '"ayy"ap'),
    ("plain",       'yww"0P'),
    # text objects on delimited text
    ("punctuation", "di("),
    ("punctuation", "da("),
    # word search (no /...<CR> prompt needed)
    ("plain",       "*"),
    ("plain",       "*n"),
    ("plain",       "#"),
]


def all_cases(quick=False):
    keys = QUICK_KEYS if quick else (MOTIONS + OPERATORS + EDITS + PASTES + COMPOUND)
    seen = set()
    for text_name, text in TEXTS.items():
        for k in keys:
            pair = (text_name, k)
            if pair not in seen:
                seen.add(pair)
                yield text_name, text, k
    if not quick:
        for text_name, k in FOCUSED_CASES:
            pair = (text_name, k)
            if pair not in seen:
                seen.add(pair)
                yield text_name, TEXTS[text_name], k


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("-k", "--filter", default="")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("-j", "--jobs", type=int, default=8)
    args = ap.parse_args(argv)

    if not preflight():
        return 2

    cases = [c for c in all_cases(args.quick) if args.filter in c[2]]

    def check(case):
        text_name, text, keys = case
        case_id = "%s:%s" % (text_name, keys)
        try:
            nv = run_nvim(text, keys, case_id=case_id)
            ov = run_ours(text, keys, case_id=case_id)
        except CaseError as exc:
            return (text_name, text, keys, ("PROCESS-ERROR", str(exc)), ("", ""))
        return (text_name, text, keys, nv, ov)

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for r in pool.map(check, cases):
            results.append(r)

    failures = []
    for text_name, text, keys, nv, ov in results:
        if nv == ov:
            if args.verbose:
                print("ok   %-14s %s" % (text_name, keys))
            continue
        failures.append((text_name, text, keys, nv, ov))

    for text_name, text, keys, nv, ov in failures:
        case_id = "%s:%s" % (text_name, keys)
        print("FAIL %-14s keys=%-20r  id=%s" % (text_name, keys, case_id))
        print("      input %r" % text)
        print("       nvim text=%r cursor=%s" % (nv[0], nv[1]))
        print("       ours text=%r cursor=%s" % (ov[0], ov[1]))

    print("\n%d cases, %d agree, %d differ"
          % (len(results), len(results) - len(failures), len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
