# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import os
from pathlib import Path
import tempfile
import tomllib
import unittest

spec = importlib.util.spec_from_file_location("context", Path(__file__).parents[3] / "scripts/toolbox_codex_context.py")
context = importlib.util.module_from_spec(spec)
spec.loader.exec_module(context)


class ContextTests(unittest.TestCase):
    def test_round_trip_preserves_other_text_and_backup(self):
        original = b'\xef\xbb\xbf# comment\r\nmodel = "some-model"\r\ntext = """\r\nmodel_context_window = 17\r\n[not_a_table]\r\n"""\r\n[profiles.other]\r\nmodel_context_window = 42\r\n'
        with tempfile.TemporaryDirectory() as directory:
            old = os.environ.get("CODEX_HOME")
            os.environ["CODEX_HOME"] = directory
            try:
                path = Path(directory) / "config.toml"
                path.write_bytes(original)
                current = context.execute("read")
                self.assertEqual(current["state"], "default")
                result = context.execute("on", current["revision"])
                self.assertEqual(result["state"], "1m")
                self.assertEqual(Path(result["backup"]).read_bytes(), original)
                self.assertEqual(tomllib.loads(path.read_text(encoding="utf-8-sig"))["model_auto_compact_token_limit"], 900000)
                with self.assertRaisesRegex(ValueError, "changed"):
                    context.execute("off", current["revision"])
                result = context.execute("off", result["revision"])
                self.assertEqual(result["state"], "default")
                self.assertEqual(path.read_bytes(), original)
                self.assertFalse((Path(directory) / ".snow-toolbox-context.lock").exists())
            finally:
                if old is None:
                    os.environ.pop("CODEX_HOME", None)
                else:
                    os.environ["CODEX_HOME"] = old

    def test_active_profile_default_and_quoted_keys(self):
        original = b'profile = "work"\n"model_context_window" = 500_000 # old\n\'model_auto_compact_token_limit\' = 450000\n[profiles."work"]\nmodel_context_window = 640000\nmodel = "x"\n[profiles.other]\nmodel_context_window = 17\n'
        for enabled in (True, False):
            result = context.patch(original, enabled)
            data = tomllib.loads(result.decode())
            self.assertEqual(data["profiles"]["other"]["model_context_window"], 17)
            self.assertNotIn("model_context_window", data["profiles"]["work"])
            self.assertEqual(context.snapshot(result)["state"], "1m" if enabled else "default")
            self.assertEqual(data["profiles"]["work"]["model"], "x")

    def test_invalid_and_unsupported_layout_not_rewritten(self):
        with self.assertRaises(tomllib.TOMLDecodeError):
            context.patch(b'model = [', True)
        with self.assertRaisesRegex(ValueError, "layout"):
            context.patch(b'profile="work"\nprofiles = {work = {model_context_window=123}}\n', False)

    def test_empty_default_does_not_create_config(self):
        self.assertEqual(context.patch(b"", False), b"")
        self.assertEqual(context.snapshot(context.patch(b"", True))["state"], "1m")


if __name__ == "__main__":
    unittest.main()
