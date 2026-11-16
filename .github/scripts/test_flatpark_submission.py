import hashlib
import importlib.util
import io
import tarfile
import tempfile
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location("flatpark_submission", Path(__file__).with_name("flatpark-submission.py"))
submission = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(submission)


class FlatParkSubmissionTests(unittest.TestCase):
    def test_generated_package_pins_official_archive_and_preserves_desktop_integration(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "dokument.tar.gz"
            payload = {
                f"share/applications/{submission.APP_ID}.desktop": b"[Desktop Entry]\nExec=dokument %U\n",
                f"share/metainfo/{submission.APP_ID}.metainfo.xml": b"<component><description><p>Dokument</p></description><screenshots/><releases/></component>",
                f"share/icons/hicolor/scalable/apps/{submission.APP_ID}.svg": b"<svg/>",
                f"share/icons/hicolor/symbolic/apps/{submission.APP_ID}-symbolic.svg": b"<svg/>",
            }
            with tarfile.open(archive, "w:gz") as tar:
                for name, content in payload.items():
                    entry = tarfile.TarInfo("./" + name)
                    entry.size = len(content)
                    tar.addfile(entry, io.BytesIO(content))
            output = Path(directory) / submission.APP_ID
            submission.generate(archive, output, "v51.0", "2026-11-16", preview=True)
            manifest = (output / f"{submission.APP_ID}.yml").read_text()
            self.assertIn("type: extra-data", manifest)
            self.assertIn(f"{submission.SOURCE}/releases/download/v51.0/{submission.ASSET}", manifest)
            self.assertIn(hashlib.sha256(archive.read_bytes()).hexdigest(), manifest)
            self.assertIn(f"size: {archive.stat().st_size}", manifest)
            self.assertIn("/app/extra/dokument/lib /app/lib", manifest)
            self.assertIn("/app/extra/dokument/libexec /app/libexec", manifest)
            self.assertEqual((output / f"{submission.APP_ID}.desktop").read_bytes(), next(iter(payload.values())))
            self.assertTrue((output / "UNPUBLISHED.txt").exists())
            for script in ["apply_extra.sh", "dokument-wrapper", "resolve-update.sh"]:
                self.assertTrue((output / script).stat().st_mode & 0o111)
            metadata = (output / f"{submission.APP_ID}.metainfo.xml").read_text()
            self.assertNotIn("<screenshots", metadata)
            self.assertIn('version="51.0"', metadata)
            with self.assertRaises(ValueError):
                submission.generate(archive, output, "v51.0", "2026-11-16")
