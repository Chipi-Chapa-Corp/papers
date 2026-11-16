#!/usr/bin/env python3
"""Prepare and package Dokument's tag-driven Flatpak releases."""

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP_ID = "com.chipichapa.Dokument"
BRANCH = "stable"
ARCH = "x86_64"
RUNTIME_REPO = "https://dl.flathub.org/repo/flathub.flatpakrepo"


def version(root=ROOT, tag=None):
    match = re.search(r"\bversion:\s*'([^']+)'", (root / "meson.build").read_text())
    if not match or not re.fullmatch(r"\d+\.\d+(?:\.\d+)?", match[1]):
        raise ValueError("Stable releases require a numeric X.Y or X.Y.Z Meson version")
    value = match[1]
    if tag is not None and tag != f"v{value}":
        raise ValueError(f"Tag {tag!r} does not match meson.build version v{value}")
    return value


def prepare(output, root=ROOT):
    manifest = json.loads((root / "build-aux/flatpak/com.chipichapa.Dokument.json").read_text())
    # Copy the exact checkout, including initialized submodules. Never build main
    # while publishing a version tag. Keep generated outputs outside that copy.
    manifest["modules"][-1]["sources"] = [{
        "type": "dir", "path": str(root.resolve()),
        "skip": [".git", "_build", ".flatpak-builder", "repo", "dist", "target"],
    }]
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=4) + "\n")


def package(repository, build, assets, run=subprocess.run):
    value = version()
    repository = repository.resolve()
    build = build.resolve()
    assets = assets.resolve()
    if not (repository / "summary").is_file():
        raise ValueError("Build and export the Flatpak repository before packaging it")
    for name in ["bin/dokument", "libexec/dokument-scan-worker"]:
        if not (build / "files" / name).is_file():
            raise ValueError(f"Missing release payload: {name}")
    assets.mkdir(parents=True, exist_ok=True)
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    (assets / "dokument-build.json").write_text(json.dumps({
        "version": value, "commit": commit, "appId": APP_ID,
        "arch": ARCH, "branch": BRANCH, "runtime": "org.gnome.Platform/51",
        "prefix": "/app",
    }, indent=2) + "\n")
    # FlatPark consumes official prebuilt Linux archives as extra-data. Preserve
    # the /app tree, including scanner drivers and schemas; omit split debuginfo.
    epoch = subprocess.check_output(["git", "show", "-s", "--format=%ct", "HEAD"], cwd=ROOT, text=True).strip()
    archive = assets / "dokument-linux-x86_64.tar.gz"
    run([
        "tar", "--sort=name", f"--mtime=@{epoch}", "--numeric-owner", "--owner=0", "--group=0",
        "--exclude=./lib/debug", "-czf", str(archive), "-C", str(build / "files"), ".",
    ], check=True)
    bundle = assets / "dokument-linux-x86_64.flatpak"
    run([
        "flatpak", "build-bundle", f"--arch={ARCH}",
        f"--runtime-repo={RUNTIME_REPO}", str(repository), str(bundle), APP_ID, BRANCH,
    ], check=True)
    filenames = [archive.name, bundle.name, "dokument-build.json"]
    checksums = []
    for name in filenames:
        with (assets / name).open("rb") as file:
            digest = hashlib.file_digest(file, "sha256").hexdigest()
        checksums.append(f"{digest}  {name}\n")
    (assets / "SHA256SUMS").write_text("".join(checksums))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("version")
    check.add_argument("--tag")
    manifest = commands.add_parser("prepare")
    manifest.add_argument("output", type=Path)
    pack = commands.add_parser("package")
    pack.add_argument("--repository", type=Path, default=Path("dist/flatpak-repo"))
    pack.add_argument("--build", type=Path, default=Path("dist/flatpak-build"))
    pack.add_argument("--assets", type=Path, default=Path("dist/release"))
    args = parser.parse_args()
    try:
        if args.command == "version":
            print(version(tag=args.tag))
        elif args.command == "prepare":
            prepare(args.output)
        else:
            package(args.repository, args.build, args.assets)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
