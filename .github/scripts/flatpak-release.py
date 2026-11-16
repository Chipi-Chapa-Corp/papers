#!/usr/bin/env python3
"""Prepare and package Dokument's tag-driven Flatpak releases."""

import argparse
import hashlib
import html
import json
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PACKAGING = ROOT / "build-aux/flatpak"
APP_ID = "com.chipichapa.Dokument"
BRANCH = "stable"
ARCH = "x86_64"
REPO_URL = "https://chipi-chapa-corp.github.io/papers/flatpak"
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


def package(repository, pages, assets, run=subprocess.run):
    value = version()
    repository = repository.resolve()
    pages = pages.resolve()
    assets = assets.resolve()
    if repository != pages / "flatpak":
        raise ValueError("The exported repository must be the Pages directory's flatpak child")
    if not (repository / "summary").is_file():
        raise ValueError("Build and export the Flatpak repository before packaging it")
    assets.mkdir(parents=True, exist_ok=True)
    run([
        "flatpak", "build-update-repo", "--title=Dokument",
        "--comment=Stable releases of Dokument",
        "--description=Read, annotate, and scan documents with Dokument",
        "--homepage=https://github.com/Chipi-Chapa-Corp/papers",
        f"--icon={REPO_URL}/dokument.svg", f"--default-branch={BRANCH}",
        "--generate-static-deltas", str(repository),
    ], check=True)
    for name in ["dokument.flatpakref", "dokument.flatpakrepo"]:
        shutil.copyfile(PACKAGING / name, repository / name)
        shutil.copyfile(PACKAGING / name, assets / name)
    shutil.copyfile(ROOT / "data/icons/scalable/apps/org.gnome.Papers.svg", repository / "dokument.svg")
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    (repository / "dokument-build.json").write_text(json.dumps({
        "version": value, "commit": commit, "appId": APP_ID,
        "arch": ARCH, "branch": BRANCH, "repoUrl": REPO_URL,
    }, indent=2) + "\n")
    (pages / ".nojekyll").touch()
    (pages / "index.html").write_text(
        '<!doctype html><html lang="en"><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width, initial-scale=1">'
        f'<title>Dokument {html.escape(value)}</title><h1>Dokument {html.escape(value)}</h1>'
        '<p>Read, annotate, and scan documents.</p>'
        '<p><a href="flatpak/dokument.flatpakref">Install Dokument</a> · '
        '<a href="flatpak/dokument.flatpakrepo">Add the update repository</a> · '
        '<a href="https://github.com/Chipi-Chapa-Corp/papers/releases">Downloads</a></p>'
        f'<pre>flatpak install --user {REPO_URL}/dokument.flatpakref</pre>'
        '<p>Installed copies receive new releases through Flatpak updates.</p></html>\n'
    )
    bundle = assets / "dokument-linux-x86_64.flatpak"
    run([
        "flatpak", "build-bundle", f"--arch={ARCH}", f"--repo-url={REPO_URL}",
        f"--runtime-repo={RUNTIME_REPO}", str(repository), str(bundle), APP_ID, BRANCH,
    ], check=True)
    filenames = [bundle.name, "dokument.flatpakref", "dokument.flatpakrepo"]
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
    pack.add_argument("--repository", type=Path, default=Path("dist/flatpak-pages/flatpak"))
    pack.add_argument("--pages", type=Path, default=Path("dist/flatpak-pages"))
    pack.add_argument("--assets", type=Path, default=Path("dist/release"))
    args = parser.parse_args()
    try:
        if args.command == "version":
            print(version(tag=args.tag))
        elif args.command == "prepare":
            prepare(args.output)
        else:
            package(args.repository, args.pages, args.assets)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
