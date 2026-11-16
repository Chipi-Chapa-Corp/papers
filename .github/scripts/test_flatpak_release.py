import hashlib
import importlib.util
import json
import subprocess
import tempfile
import tarfile
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

    def payload(self, directory):
        root = Path(directory)
        repository = root / "repo"
        repository.mkdir()
        (repository / "summary").touch()
        build = root / "build"
        for name in ["bin/dokument", "libexec/dokument-scan-worker", "lib/debug/unneeded.debug"]:
            file = build / "files" / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_bytes(name.encode())
            file.chmod(0o755)
        return repository, build, root / "assets"

    def test_packaging_produces_flatpark_payload_and_checksums(self):
        with tempfile.TemporaryDirectory() as directory:
            repository, build, assets = self.payload(directory)
            calls = []

            def run(args, check):
                self.assertTrue(check)
                calls.append(args)
                if args[0] == "tar":
                    subprocess.run(args, check=True)
                else:
                    Path(args[-3]).write_bytes(b"flatpak test bundle")

            release.package(repository, build, assets, run=run)
            self.assertFalse(any(arg.startswith("--repo-url=") for arg in calls[-1]))
            self.assertIn(f"--runtime-repo={release.RUNTIME_REPO}", calls[-1])
            self.assertEqual(calls[-1][-2:], [release.APP_ID, release.BRANCH])
            with tarfile.open(assets / "dokument-linux-x86_64.tar.gz") as archive:
                for name in ["bin/dokument", "libexec/dokument-scan-worker"]:
                    self.assertEqual(archive.extractfile(f"./{name}").read(), name.encode())
                    self.assertTrue(archive.getmember(f"./{name}").mode & 0o111)
                self.assertNotIn("./lib/debug/unneeded.debug", archive.getnames())
            for line in (assets / "SHA256SUMS").read_text().splitlines():
                checksum, name = line.split("  ")
                self.assertEqual(checksum, hashlib.sha256((assets / name).read_bytes()).hexdigest())
            metadata = json.loads((assets / "dokument-build.json").read_text())
            self.assertEqual(metadata["version"], release.version())
            self.assertEqual(metadata["branch"], release.BRANCH)

    def test_failed_bundle_is_not_published_as_a_complete_download(self):
        with tempfile.TemporaryDirectory() as directory:
            repository, build, assets = self.payload(directory)

            def run(args, check):
                if args[0] == "tar":
                    subprocess.run(args, check=True)
                else:
                    raise subprocess.CalledProcessError(1, args)

            with self.assertRaises(subprocess.CalledProcessError):
                release.package(repository, build, assets, run=run)
            self.assertFalse((assets / "SHA256SUMS").exists())


if __name__ == "__main__":
    unittest.main()
