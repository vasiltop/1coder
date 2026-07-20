"""Unit tests for tools/vimdiff.py."""

import io
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import MagicMock, patch

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

_SPEC = importlib.util.spec_from_file_location(
    "vimdiff", os.path.join(ROOT, "tools", "vimdiff.py")
)
vimdiff = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(vimdiff)


class TestNvimSettings(unittest.TestCase):
    def test_settings_clean(self):
        s = vimdiff.NVIM_SETTINGS
        self.assertIn("noswapfile", s)
        for bad in ("sw=", "ts=", "expandtab", "fixendofline"):
            self.assertNotIn(bad, s, "masking setting %r in NVIM_SETTINGS" % bad)


class TestPreflight(unittest.TestCase):
    def test_nvim_missing(self):
        with patch("shutil.which", return_value=None), \
             patch("sys.stderr", new_callable=io.StringIO) as err:
            self.assertFalse(vimdiff.preflight())
        self.assertIn("nvim", err.getvalue())

    def test_editor_absent_or_not_executable(self):
        for isfile, access in ((False, True), (True, False)):
            with patch("shutil.which", return_value="/usr/bin/nvim"), \
                 patch("os.path.isfile", return_value=isfile), \
                 patch("os.access", return_value=access), \
                 patch("sys.stderr", new_callable=io.StringIO) as err:
                self.assertFalse(vimdiff.preflight())
            self.assertIn("editor", err.getvalue())

    def test_both_present(self):
        with patch("shutil.which", return_value="/usr/bin/nvim"), \
             patch("os.path.isfile", return_value=True), \
             patch("os.access", return_value=True):
            self.assertTrue(vimdiff.preflight())

    def test_main_returns_2_when_preflight_fails(self):
        with patch.object(vimdiff, "preflight", return_value=False):
            self.assertEqual(vimdiff.main(["--quick"]), 2)


class TestRunSubprocess(unittest.TestCase):
    def test_timeout_raises_case_error(self):
        with patch("subprocess.run",
                   side_effect=subprocess.TimeoutExpired(["nvim"], 30)):
            with self.assertRaises(vimdiff.CaseError) as ctx:
                vimdiff.run_subprocess(["nvim"], case_id="plain:dw")
        msg = str(ctx.exception)
        self.assertIn("plain:dw", msg)
        self.assertIn("nvim", msg)

    def test_nonzero_exit_raises_with_diagnostics(self):
        mock = MagicMock(returncode=1, stderr=b"bad")
        with patch("subprocess.run", return_value=mock):
            with self.assertRaises(vimdiff.CaseError) as ctx:
                vimdiff.run_subprocess(["nvim"], case_id="plain:dw")
        msg = str(ctx.exception)
        self.assertIn("plain:dw", msg)
        self.assertIn("1", msg)
        self.assertIn("bad", msg)


class TestArtifactsAndCleanup(unittest.TestCase):
    def _mock_ok(self):
        return MagicMock(returncode=0)

    def test_missing_pos_file_raises(self):
        with patch.object(vimdiff, "run_subprocess", return_value=self._mock_ok()):
            with self.assertRaises(vimdiff.CaseError) as ctx:
                vimdiff.run_nvim("hello\n", "w", case_id="plain:w")
        self.assertIn("plain:w", str(ctx.exception))

    def test_cleanup_on_exception(self):
        created = []
        orig = tempfile.NamedTemporaryFile

        class Tracking:
            def __init__(self, *a, **kw):
                kw.setdefault("delete", False)
                self._f = orig(*a, **kw)
                created.append(self._f.name)
            def __enter__(self): return self._f.__enter__()
            def __exit__(self, *a): return self._f.__exit__(*a)

        with patch("tempfile.NamedTemporaryFile", Tracking), \
             patch.object(vimdiff, "run_subprocess", return_value=self._mock_ok()):
            try:
                vimdiff.run_nvim("hello\n", "w")
            except vimdiff.CaseError:
                pass

        for p in created:
            self.assertFalse(os.path.exists(p), "not cleaned: %s" % p)
            self.assertFalse(os.path.exists(p + ".pos"), "not cleaned: %s.pos" % p)


class TestFeedkeys(unittest.TestCase):
    def _argv(self, keys):
        captured = []
        def fake(cmd, **kw):
            captured.extend(cmd)
            return MagicMock(returncode=0)
        with patch.object(vimdiff, "run_subprocess", side_effect=fake):
            try:
                vimdiff.run_nvim("hello\n", keys)
            except vimdiff.CaseError:
                pass
        return captured

    def test_feedkeys_ntx_not_normal_bang(self):
        argv = self._argv("dw")
        fk = next(a for a in argv if "feedkeys" in a)
        self.assertIn('"ntx"', fk)
        self.assertFalse(any("normal!" in a for a in argv))

    def test_named_keys_encoded(self):
        argv = self._argv("i<Esc>dw<C-r>")
        fk = next(a for a in argv if "feedkeys" in a)
        self.assertIn("\\<Esc>", fk)
        self.assertIn("\\<C-r>", fk)


@unittest.skipUnless(shutil.which("nvim"), "nvim not available")
class TestNvimIntegration(unittest.TestCase):
    def test_failed_motion_does_not_abort_later_keys(self):
        text, _ = vimdiff.run_nvim("hello world\n", "jdw")
        self.assertEqual(text, "world\n")

    def test_named_key_esc_exits_insert_mode(self):
        text, _ = vimdiff.run_nvim("hello\n", "iX<Esc>")
        self.assertEqual(text, "Xhello\n")


class TestCorpus(unittest.TestCase):
    def test_corpus_constants(self):
        self.assertEqual(vimdiff.TEXTS.get("empty"), "")
        self.assertEqual(vimdiff.TEXTS.get("lone-newline"), "\n")
        self.assertIn("\t", vimdiff.TEXTS.get("tab-indented", ""))
        self.assertTrue(any(ord(c) > 127 for c in vimdiff.TEXTS.get("utf8", "")))
        self.assertFalse(hasattr(vimdiff, "KNOWN_DIFFERENCES"))

    def test_focused_cases_valid(self):
        n = len(vimdiff.FOCUSED_CASES)
        self.assertGreaterEqual(n, 12)
        self.assertLessEqual(n, 18)
        for tn, _ in vimdiff.FOCUSED_CASES:
            self.assertIn(tn, vimdiff.TEXTS)

    def test_focused_excluded_quick_included_full(self):
        quick = {(tn, k) for tn, _, k in vimdiff.all_cases(quick=True)}
        full  = {(tn, k) for tn, _, k in vimdiff.all_cases(quick=False)}
        self.assertLessEqual(quick, full)
        for tn, k in vimdiff.FOCUSED_CASES:
            self.assertNotIn((tn, k), quick, "focused in quick: %r" % k)
            self.assertIn((tn, k), full,    "focused missing from full: %r" % k)

    def test_full_case_ids_unique(self):
        seen = set()
        for tn, _, k in vimdiff.all_cases(quick=False):
            pair = (tn, k)
            self.assertNotIn(pair, seen, "duplicate %r:%r" % pair)
            seen.add(pair)

    def test_filter_reaches_focused_cases(self):
        # "100dw" and "100G" are focused; "100" absent from all cross-product keys
        filtered = [c for c in vimdiff.all_cases(quick=False) if "100" in c[2]]
        self.assertEqual(len(filtered), 2)


if __name__ == "__main__":
    unittest.main()
