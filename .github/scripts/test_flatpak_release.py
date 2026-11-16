import configparser
import hashlib
import importlib.util
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location("flatpak_release", Path(__file__).with_name("flatpak-release.py"))
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


class FlatpakReleaseTests(unittest.TestCase):
    def test_only_matching_stable_version_tags_can_publish(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for value in ["51.0", "1.2.3"]:
                (root / "meson.build").write_text(f"project('dokument', version: '{value}')")
                self.assertEqual(release.version(root, f"v{value}"), value)
                for invalid in ["v0.0.1", value, f"v{value}-linux-x64"]:
                    with self.assertRaises(ValueError):
                        release.version(root, invalid)
            (root / "meson.build").write_text("project('dokument', version: '1.2.beta')")
            with self.assertRaises(ValueError):
                release.version(root)

    def test_release_manifest_builds_the_checkout_with_scanning_and_normal_header(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "release.json"
            release.prepare(output)
            manifest = json.loads(output.read_text())
        self.assertEqual(manifest["app-id"], release.APP_ID)
        self.assertEqual(manifest["branch"], release.BRANCH)
        self.assertEqual(manifest["runtime-version"], "51")
        self.assertNotIn("--env=G_DEBUG=fatal-criticals", manifest["finish-args"])
        modules = {module["name"]: module for module in manifest["modules"]}
        self.assertTrue({"sane-backends", "sane-airscan"} <= modules.keys())
        options = modules["dokument"]["config-opts"]
        for option in ["-Dprofile=default", "-Dbuildtype=release", "-Dscanning=enabled"]:
            self.assertIn(option, options)
        source = modules["dokument"]["sources"][0]
        self.assertEqual(source["type"], "dir")
        self.assertEqual(source["path"], str(release.ROOT))
        self.assertIn("dist", source["skip"])
        for name, module in modules.items():
            if name == "dokument":
                continue
            for source in module["sources"]:
                self.assertTrue(source.get("sha256") or source.get("tag") or source.get("commit"), name)

    def test_packaging_preserves_update_origin_and_checksums(self):
        with tempfile.TemporaryDirectory() as directory:
            pages = Path(directory) / "pages"
            repository = pages / "flatpak"
            repository.mkdir(parents=True)
            (repository / "summary").touch()
            assets = Path(directory) / "assets"
            calls = []

            def run(args, check):
                self.assertTrue(check)
                calls.append(args)
                if args[1] == "build-bundle":
                    Path(args[-3]).write_bytes(b"flatpak test bundle")

            release.package(repository, pages, assets, run=run)
            self.assertIn(f"--repo-url={release.REPO_URL}", calls[-1])
            self.assertIn(f"--runtime-repo={release.RUNTIME_REPO}", calls[-1])
            self.assertEqual(calls[-1][-2:], [release.APP_ID, release.BRANCH])
            self.assertTrue((pages / ".nojekyll").is_file())
            for name, section in [("dokument.flatpakref", "Flatpak Ref"), ("dokument.flatpakrepo", "Flatpak Repo")]:
                descriptor = configparser.ConfigParser()
                descriptor.read(repository / name)
                self.assertEqual(descriptor[section]["Url"], release.REPO_URL)
                self.assertEqual((assets / name).read_bytes(), (repository / name).read_bytes())
            for line in (assets / "SHA256SUMS").read_text().splitlines():
                checksum, name = line.split("  ")
                self.assertEqual(checksum, hashlib.sha256((assets / name).read_bytes()).hexdigest())
            metadata = json.loads((repository / "dokument-build.json").read_text())
            self.assertEqual(metadata["version"], release.version())
            self.assertEqual(metadata["branch"], release.BRANCH)

    def test_failed_bundle_is_not_published_as_a_complete_download(self):
        with tempfile.TemporaryDirectory() as directory:
            pages = Path(directory) / "pages"
            repository = pages / "flatpak"
            repository.mkdir(parents=True)
            (repository / "summary").touch()
            assets = Path(directory) / "assets"

            def run(args, check):
                if args[1] == "build-bundle":
                    raise subprocess.CalledProcessError(1, args)

            with self.assertRaises(subprocess.CalledProcessError):
                release.package(repository, pages, assets, run=run)
            self.assertFalse((assets / "SHA256SUMS").exists())


if __name__ == "__main__":
    unittest.main()
