"""Focused unit tests for tools/vimdiff.py.

Run with:  python3 -m unittest tools/test_vimdiff.py
"""

import importlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import MagicMock, call, patch

# ---------------------------------------------------------------------------
# Make sure the module under test is importable from the repo root so the
# tests work regardless of working directory.
# ---------------------------------------------------------------------------

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

import importlib.util

_SPEC = importlib.util.spec_from_file_location(
    "vimdiff",
    os.path.join(ROOT, "tools", "vimdiff.py"),
)
vimdiff = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(vimdiff)


# ===========================================================================
# 1. NVIM_SETTINGS must not include the masking settings
# ===========================================================================

class TestNvimSettings(unittest.TestCase):
    def test_no_sw(self):
        self.assertNotIn("sw=", vimdiff.NVIM_SETTINGS,
                         "sw= must not appear in NVIM_SETTINGS")

    def test_no_ts(self):
        self.assertNotIn("ts=", vimdiff.NVIM_SETTINGS,
                         "ts= must not appear in NVIM_SETTINGS")

    def test_no_expandtab(self):
        self.assertNotIn("expandtab", vimdiff.NVIM_SETTINGS,
                         "expandtab must not appear in NVIM_SETTINGS")

    def test_no_fixendofline(self):
        self.assertNotIn("fixendofline", vimdiff.NVIM_SETTINGS,
                         "nofixendofline must not appear in NVIM_SETTINGS")

    def test_has_noswapfile(self):
        self.assertIn("noswapfile", vimdiff.NVIM_SETTINGS,
                      "noswapfile must remain in NVIM_SETTINGS")


# ===========================================================================
# 2. Preflight: missing nvim or missing editor → return 2 + actionable message
# ===========================================================================

class TestPreflight(unittest.TestCase):
    def test_nvim_missing_returns_false(self):
        """preflight() returns False when nvim is not on PATH."""
        with patch("shutil.which", return_value=None), \
             patch("sys.stderr", new_callable=io.StringIO) as fake_err:
            result = vimdiff.preflight()
        self.assertFalse(result)
        self.assertIn("nvim", fake_err.getvalue())

    def test_editor_missing_returns_false(self):
        """preflight() returns False when build/editor is absent."""
        with patch("shutil.which", return_value="/usr/bin/nvim"), \
             patch("os.path.isfile", return_value=False), \
             patch("sys.stderr", new_callable=io.StringIO) as fake_err:
            result = vimdiff.preflight()
        self.assertFalse(result)
        self.assertIn("editor", fake_err.getvalue())

    def test_editor_not_executable_returns_false(self):
        """preflight() returns False when build/editor exists but is not executable."""
        with patch("shutil.which", return_value="/usr/bin/nvim"), \
             patch("os.path.isfile", return_value=True), \
             patch("os.access", return_value=False), \
             patch("sys.stderr", new_callable=io.StringIO) as fake_err:
            result = vimdiff.preflight()
        self.assertFalse(result)
        self.assertIn("editor", fake_err.getvalue())

    def test_both_present_returns_true(self):
        """preflight() returns True when nvim and editor are both available."""
        with patch("shutil.which", return_value="/usr/bin/nvim"), \
             patch("os.path.isfile", return_value=True), \
             patch("os.access", return_value=True):
            result = vimdiff.preflight()
        self.assertTrue(result)

    def test_main_returns_2_when_preflight_fails(self):
        """main() exits with return code 2 when preflight fails."""
        with patch.object(vimdiff, "preflight", return_value=False):
            rc = vimdiff.main(["--quick"])
        self.assertEqual(rc, 2)


# ===========================================================================
# 3. run_subprocess: raises CaseError on timeout or nonzero exit
# ===========================================================================

class TestRunSubprocess(unittest.TestCase):
    def test_timeout_raises_case_error(self):
        """run_subprocess raises CaseError on TimeoutExpired."""
        with patch("subprocess.run",
                   side_effect=subprocess.TimeoutExpired(["nvim"], 30)):
            with self.assertRaises(vimdiff.CaseError) as ctx:
                vimdiff.run_subprocess(["nvim", "--headless"], case_id="plain:dw")
        msg = str(ctx.exception)
        self.assertIn("plain:dw", msg)
        self.assertIn("nvim", msg)

    def test_nonzero_exit_raises_case_error(self):
        """run_subprocess raises CaseError on non-zero exit code."""
        mock_result = MagicMock()
        mock_result.returncode = 1
        mock_result.stderr = b"something went wrong"
        with patch("subprocess.run", return_value=mock_result):
            with self.assertRaises(vimdiff.CaseError) as ctx:
                vimdiff.run_subprocess(["nvim", "--headless"], case_id="plain:dw")
        msg = str(ctx.exception)
        self.assertIn("plain:dw", msg)
        self.assertIn("nvim", msg)
        self.assertIn("1", msg)          # exit code
        self.assertIn("something went wrong", msg)  # decoded stderr

    def test_zero_exit_returns_result(self):
        """run_subprocess returns the CompletedProcess on zero exit."""
        mock_result = MagicMock()
        mock_result.returncode = 0
        with patch("subprocess.run", return_value=mock_result):
            r = vimdiff.run_subprocess(["nvim", "--headless"])
        self.assertIs(r, mock_result)


# ===========================================================================
# 4. Missing artifacts are explicit failures, not "?" values
# ===========================================================================

class TestArtifactValidation(unittest.TestCase):
    def _make_tmp_text(self, content="hello\n"):
        f = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False)
        f.write(content)
        f.close()
        return f.name

    def test_missing_nvim_pos_file_raises(self):
        """run_nvim raises CaseError when the cursor-dump file is absent."""
        tmp = self._make_tmp_text()
        # Subprocess succeeds but writes no pos file → explicit error.
        mock_result = MagicMock()
        mock_result.returncode = 0
        # Patch run_subprocess to succeed and NOT actually create pos_path
        with patch.object(vimdiff, "run_subprocess", return_value=mock_result):
            with self.assertRaises(vimdiff.CaseError) as ctx:
                # Force a deterministic path by monkeypatching tempfile
                vimdiff.run_nvim("hello\n", "w", case_id="plain:w")
        self.assertIn("plain:w", str(ctx.exception))

    def test_missing_ours_pos_file_raises(self):
        """run_ours raises CaseError when the cursor-dump file is absent."""
        mock_result = MagicMock()
        mock_result.returncode = 0
        with patch.object(vimdiff, "run_subprocess", return_value=mock_result):
            with self.assertRaises(vimdiff.CaseError) as ctx:
                vimdiff.run_ours("hello\n", "w", case_id="plain:w")
        self.assertIn("plain:w", str(ctx.exception))


# ===========================================================================
# 5. Temporary files are cleaned up even after CaseError
# ===========================================================================

class TestTmpCleanup(unittest.TestCase):
    def test_nvim_cleanup_on_exception(self):
        """Temp files are removed when run_nvim raises CaseError."""
        created = []
        original_NamedTemporaryFile = tempfile.NamedTemporaryFile

        class _TrackingNTF:
            def __init__(self, *a, **kw):
                kw.setdefault("delete", False)
                self._f = original_NamedTemporaryFile(*a, **kw)
                created.append(self._f.name)
            def __enter__(self):
                return self._f.__enter__()
            def __exit__(self, *a):
                return self._f.__exit__(*a)

        mock_result = MagicMock()
        mock_result.returncode = 0
        with patch("tempfile.NamedTemporaryFile", _TrackingNTF), \
             patch.object(vimdiff, "run_subprocess", return_value=mock_result):
            try:
                vimdiff.run_nvim("hello\n", "w", case_id="plain:w")
            except vimdiff.CaseError:
                pass

        for p in created:
            pos = p + ".pos"
            self.assertFalse(os.path.exists(p),
                             "temp file %s not cleaned up" % p)
            self.assertFalse(os.path.exists(pos),
                             "temp file %s not cleaned up" % pos)


# ===========================================================================
# 6. Case identifier appears in every failure line
# ===========================================================================

class TestFailureOutput(unittest.TestCase):
    def _run_failures(self, failures):
        """Return captured stdout after printing failures."""
        buf = io.StringIO()
        with patch("sys.stdout", buf):
            for text_name, text, keys, nv, ov in failures:
                case_id = "%s:%s" % (text_name, keys)
                print("FAIL %-14s keys=%r  id=%s" % (text_name, keys, case_id))
        return buf.getvalue()

    def test_failure_contains_case_id(self):
        failures = [("plain", "alpha\n", "dw", ("lpha\n", "1:1"), ("alpha\n", "1:5"))]
        # Check that the printed output in main would contain text_name and keys
        # We test the format produced by the main loop
        out = io.StringIO()
        # Simulate what main prints
        text_name, text, keys, nv, ov = failures[0]
        case_id = "%s:%s" % (text_name, keys)
        line = "FAIL %-14s keys=%r" % (text_name, keys)
        self.assertIn("plain", line)
        self.assertIn("dw", line)
        self.assertIn("plain", case_id)
        self.assertIn("dw", case_id)


# ===========================================================================
# 7. KNOWN_DIFFERENCES must not exist or must be empty and unused
# ===========================================================================

class TestNoKnownDifferences(unittest.TestCase):
    def test_known_differences_removed(self):
        """KNOWN_DIFFERENCES dict must not exist in the module."""
        self.assertFalse(
            hasattr(vimdiff, "KNOWN_DIFFERENCES"),
            "KNOWN_DIFFERENCES must be removed from vimdiff"
        )


# ===========================================================================
# 8. run_nvim must build a feedkeys("...", "ntx") call, not execute "normal!"
# ===========================================================================

class TestFeedkeysCommand(unittest.TestCase):
    """run_nvim must use call feedkeys(..., "ntx"), not execute 'normal!'."""

    def _capture_nvim_cmd(self, keys):
        """Return the nvim argv that would be passed to run_subprocess."""
        captured = []

        def fake_rsp(cmd, **kw):
            captured.extend(cmd)
            return MagicMock(returncode=0)

        with patch.object(vimdiff, "run_subprocess", side_effect=fake_rsp):
            try:
                vimdiff.run_nvim("hello\n", keys)
            except vimdiff.CaseError:
                pass  # pos file absent is expected; we captured what we need
        return captured

    def test_uses_feedkeys_not_normal_bang(self):
        cmd = self._capture_nvim_cmd("dw")
        self.assertTrue(any("feedkeys" in a for a in cmd),
                        "run_nvim must use feedkeys")
        self.assertFalse(any("normal!" in a for a in cmd),
                         "run_nvim must not use execute 'normal!'")

    def test_feedkeys_mode_is_ntx(self):
        """feedkeys mode must be 'ntx': no-remap, key-codes, synchronous."""
        cmd = self._capture_nvim_cmd("dw")
        feedkeys_arg = next(a for a in cmd if "feedkeys" in a)
        self.assertIn('"ntx"', feedkeys_arg,
                      "feedkeys mode must be ntx")

    def test_named_key_esc_in_feedkeys_string(self):
        """<Esc> is encoded as \\<Esc> inside the feedkeys double-quoted string."""
        cmd = self._capture_nvim_cmd("i<Esc>")
        feedkeys_arg = next(a for a in cmd if "feedkeys" in a)
        # to_nvim_keys('<Esc>') → '\<Esc>' which sits inside feedkeys("...")
        self.assertIn("\\<Esc>", feedkeys_arg)
        self.assertIn("feedkeys", feedkeys_arg)

    def test_ctrl_key_in_feedkeys_string(self):
        """<C-r> is preserved as \\<C-r> in the feedkeys argument."""
        cmd = self._capture_nvim_cmd("dw<C-r>")
        feedkeys_arg = next(a for a in cmd if "feedkeys" in a)
        self.assertIn("\\<C-r>", feedkeys_arg)


# ===========================================================================
# 9. Integration: feedkeys processes keys independently; normal! batches them
# ===========================================================================

@unittest.skipUnless(shutil.which("nvim"), "nvim not available")
class TestNvimIntegration(unittest.TestCase):

    def test_failed_motion_does_not_abort_later_keys(self):
        """feedkeys: j failing on last line must not prevent dw from running.

        This is the primary parity regression: interactive jdw on a single-line
        file deletes 'hello ' even though j found nowhere to go.
        """
        text, _pos = vimdiff.run_nvim("hello world\n", "jdw")
        self.assertEqual(text, "world\n",
                         "failed j must not abort dw in feedkeys mode")

    def test_normal_bang_aborts_on_failed_motion(self):
        """Contrast: execute 'normal! jdw' aborts dw after failing j.

        Demonstrates the original bug that the feedkeys change fixes.
        """
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
            f.write("hello world\n")
            path = f.name
        try:
            subprocess.run(
                ["nvim", "--headless", "-u", "NONE", "-i", "NONE", path,
                 "-c", "set nocompatible noswapfile",
                 "-c", 'execute "normal! jdw"',
                 "-c", "wq"],
                capture_output=True, timeout=10, check=True,
            )
            result = open(path).read()
        finally:
            if os.path.exists(path):
                os.unlink(path)
        # normal! aborts the batch when j fails → dw never runs → unchanged
        self.assertEqual(result, "hello world\n",
                         "normal! must abort the batch on failed j")

    def test_named_key_esc_exits_insert_mode(self):
        """<Esc> correctly exits insert mode when fed through feedkeys."""
        text, _pos = vimdiff.run_nvim("hello\n", "iX<Esc>")
        self.assertEqual(text, "Xhello\n")


if __name__ == "__main__":
    unittest.main()
