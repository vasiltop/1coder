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
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EDITOR = os.path.join(ROOT, "build", "editor")

# Settings that make neovim behave the way this editor does. Anything listed
# here is a deliberate difference; anything not listed is a bug if it diverges.
#
#   nocompatible     vim defaults rather than vi's
#   sw=2 ts=2        matches kShiftWidth
#   expandtab        this editor indents with spaces (see KNOWN_DIFFERENCES)
#   nofixendofline   do not silently add a trailing newline on write
#   noswapfile       no stray state between runs
NVIM_SETTINGS = "set nocompatible sw=2 ts=2 expandtab nofixendofline noswapfile"

# Key names this editor and neovim both understand. Everything else is passed
# through literally, so `<<` stays two less-than characters rather than being
# mistaken for a key name.
KEY_NAMES = {
    "Esc", "CR", "Tab", "BS", "Space", "Del", "Up", "Down", "Left", "Right",
    "Home", "End", "PageUp", "PageDown",
}


def to_nvim_keys(keys):
    """Translate this editor's binding notation into a `:execute "normal!"` string.

    Returns something ready to sit inside double quotes: named keys become
    `\\<Esc>` and literal characters are escaped. Escaping has to happen here,
    per character -- escaping the whole string afterwards would turn the
    backslash of `\\<Esc>` into a literal one, and every insert-mode case would
    silently end up typing the text `<Esc>` instead of pressing escape.
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


def run_nvim(text, keys):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write(text)
        path = f.name
    pos_path = path + ".pos"

    escaped = to_nvim_keys(keys)
    try:
        subprocess.run(
            ["nvim", "--headless", "-u", "NONE", "-i", "NONE", path,
             "-c", NVIM_SETTINGS,
             "-c", 'execute "normal! %s"' % escaped,
             "-c", 'call writefile([line(".").":".col(".")], "%s")' % pos_path,
             "-c", "wq"],
            capture_output=True, timeout=30)
        out = open(path).read()
        pos = open(pos_path).read().strip() if os.path.exists(pos_path) else "?"
    finally:
        for p in (path, pos_path):
            if os.path.exists(p):
                os.unlink(p)
    return out, pos


def run_ours(text, keys):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write(text)
        path = f.name
    pos_path = path + ".pos"

    env = dict(os.environ, SDL_VIDEODRIVER="dummy")
    try:
        subprocess.run(
            [EDITOR, path, "--keys", keys + ":w<CR>", "--dump", pos_path],
            capture_output=True, env=env, timeout=30)
        out = open(path).read()
        pos = "?"
        if os.path.exists(pos_path):
            # "line:col MODE" -- only the position is compared.
            pos = open(pos_path).read().strip().split(" ")[0]
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

QUICK_KEYS = ["w", "b", "e", "$", "dw", "dd", "yyp", "ywP", "x", "p", "P", "wdw", "jp"]


def all_cases(quick=False):
    keys = QUICK_KEYS if quick else (MOTIONS + OPERATORS + EDITS + PASTES + COMPOUND)
    for text_name, text in TEXTS.items():
        for k in keys:
            yield text_name, text, k


# Divergences that are deliberate or already known, so the suite can be green
# and a new failure means something. Each needs a reason.
KNOWN_DIFFERENCES = {
    # This editor indents with spaces; the user's nvim has no 'expandtab' and so
    # indents with tabs. The oracle is run with expandtab to compare like for
    # like -- rendering real tabs needs tab stops in the core, which do not
    # exist yet.
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("-k", "--filter", default="")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("-j", "--jobs", type=int, default=8)
    args = ap.parse_args()

    if not os.path.exists(EDITOR):
        print("editor not built: %s" % EDITOR, file=sys.stderr)
        return 2

    cases = [c for c in all_cases(args.quick) if args.filter in c[2]]

    def check(case):
        text_name, text, keys = case
        try:
            nv = run_nvim(text, keys)
            ov = run_ours(text, keys)
        except Exception as exc:  # a timeout or crash is itself a failure
            return (text_name, text, keys, ("EXCEPTION", str(exc)), ("", ""))
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
        if (text_name, keys) in KNOWN_DIFFERENCES:
            continue
        failures.append((text_name, text, keys, nv, ov))

    for text_name, text, keys, nv, ov in failures:
        print("FAIL %-14s keys=%r" % (text_name, keys))
        print("      input %r" % text)
        print("       nvim text=%r cursor=%s" % (nv[0], nv[1]))
        print("       ours text=%r cursor=%s" % (ov[0], ov[1]))

    print("\n%d cases, %d agree, %d differ"
          % (len(results), len(results) - len(failures), len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
