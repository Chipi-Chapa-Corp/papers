#!/usr/bin/env python3
"""Generate a FlatPark catalog package from an official Dokument Linux release."""

import argparse
import hashlib
import json
import re
import shutil
import tarfile
import tempfile
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP_ID = "com.chipichapa.Dokument"
SOURCE = "https://github.com/Chipi-Chapa-Corp/papers"
ASSET = "dokument-linux-x86_64.tar.gz"


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": "Dokument-FlatPark-submission"})
    return urllib.request.urlopen(request, timeout=30)


def generate(archive, output, tag, date, preview=False):
    if not re.fullmatch(r"v\d+\.\d+(?:\.\d+)?", tag):
        raise ValueError("Expected a stable version tag vX.Y or vX.Y.Z")
    if output.exists():
        raise ValueError(f"Refusing to overwrite an existing package: {output}")
    url = f"{SOURCE}/releases/download/{tag}/{ASSET}"
    with archive.open("rb") as file:
        checksum = hashlib.file_digest(file, "sha256").hexdigest()
    size = archive.stat().st_size
    # Desktop integration is copied from the official binary distribution,
    # preserving its app id, MIME types and original icon.
    with tarfile.open(archive) as tar:
        integration = {
            f"{APP_ID}.desktop": f"./share/applications/{APP_ID}.desktop",
            f"{APP_ID}.metainfo.xml": f"./share/metainfo/{APP_ID}.metainfo.xml",
            f"{APP_ID}.svg": f"./share/icons/hicolor/scalable/apps/{APP_ID}.svg",
            f"{APP_ID}-symbolic.svg": f"./share/icons/hicolor/symbolic/apps/{APP_ID}-symbolic.svg",
        }
        files = {name: tar.extractfile(member).read() for name, member in integration.items()}
    output.mkdir(parents=True)
    for name, content in files.items():
        (output / name).write_bytes(content)
    metadata = ET.fromstring(files[f"{APP_ID}.metainfo.xml"])
    releases = metadata.find("releases")
    if releases is not None:
        releases.clear()
    else:
        releases = ET.SubElement(metadata, "releases")
    ET.SubElement(releases, "release", {"version": tag[1:], "date": date, "type": "stable"})
    # Upstream Papers screenshots do not depict this fork. Supply a current
    # Dokument screenshot when preparing the catalog PR.
    screenshots = metadata.find("screenshots")
    if screenshots is not None:
        metadata.remove(screenshots)
    description = metadata.find("description")
    ET.SubElement(description, "p").text = (
        "USB scanning requires device access. Network scanner discovery uses the local network and Avahi."
    )
    ET.indent(metadata, space="  ")
    (output / f"{APP_ID}.metainfo.xml").write_bytes(ET.tostring(metadata, encoding="utf-8", xml_declaration=True))
    for name in ["apply_extra.sh", "dokument-wrapper", "resolve-update.sh"]:
        shutil.copyfile(ROOT / "build-aux/flatpark" / name, output / name)
        (output / name).chmod(0o755)
    (output / "flatpark.yml").write_text(f"""id: {APP_ID}
name: Dokument
summary: Read, annotate and scan documents with tabs
website: {SOURCE}
source_url: {SOURCE}
build:
  manifest: {APP_ID}.yml
  branch: stable
  mode: extra-data
  arches: [x86_64]
catalog:
  category: Office
  tags: [PDF, Documents, Scanning, GNOME]
update:
  command: ./resolve-update.sh
policy:
  proprietary: false
  dangerous_permissions:
    - --device=all
""")
    commands = [
        "install -Dm755 apply_extra.sh /app/bin/apply_extra",
        "install -Dm755 dokument-wrapper /app/bin/dokument",
        f"install -Dm644 {APP_ID}.desktop /app/share/applications/{APP_ID}.desktop",
        f"install -Dm644 {APP_ID}.metainfo.xml /app/share/metainfo/{APP_ID}.metainfo.xml",
        f"install -Dm644 {APP_ID}.svg /app/share/icons/hicolor/scalable/apps/{APP_ID}.svg",
        f"install -Dm644 {APP_ID}-symbolic.svg /app/share/icons/hicolor/symbolic/apps/{APP_ID}-symbolic.svg",
        "install -d /app/etc /app/share/glib-2.0",
    ]
    # The official build uses /app. These links retain its compiled paths while
    # all vendor binaries/data are downloaded, unmodified, as extra-data.
    for name in ["lib", "libexec"]:
        commands.append(f"ln -s /app/extra/dokument/{name} /app/{name}")
    for name in ["sane.d"]:
        commands.append(f"ln -s /app/extra/dokument/etc/{name} /app/etc/{name}")
    for name in ["locale", "runtime", "poppler", "djvu", "sane", "help", "papers"]:
        commands.append(f"ln -s /app/extra/dokument/share/{name} /app/share/{name}")
    commands.append("ln -s /app/extra/dokument/share/glib-2.0/schemas /app/share/glib-2.0/schemas")
    manifest = f"""id: {APP_ID}
runtime: org.gnome.Platform
runtime-version: "51"
sdk: org.gnome.Sdk
command: dokument
separate-locales: false
build-options:
  no-debuginfo: true
finish-args:
  - --socket=wayland
  - --socket=fallback-x11
  - --share=ipc
  - --device=dri
  # SANE accesses USB scanners directly; declared in policy.dangerous_permissions.
  - --device=all
  - --share=network
  - --system-talk-name=org.freedesktop.Avahi
  - --env=SANE_CONFIG_DIR=/app/etc/sane.d
modules:
  - name: dokument
    buildsystem: simple
    build-commands:
"""
    manifest += "".join(f"      - {json.dumps(command)}\n" for command in commands)
    manifest += f"""    sources:
      # BEGIN MANAGED EXTRA-DATA
      - type: extra-data
        filename: dokument.tar.gz
        only-arches: [x86_64]
        url: {url}
        sha256: {checksum}
        size: {size}
      # END MANAGED EXTRA-DATA
"""
    for name in ["apply_extra.sh", "dokument-wrapper", "resolve-update.sh", *files]:
        manifest += f"      - type: file\n        path: {name}\n"
    (output / f"{APP_ID}.yml").write_text(manifest)
    if preview:
        (output / "UNPUBLISHED.txt").write_text(
            "Local preview only. Regenerate from the published release before submitting: " + url + "\n"
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--preview-archive", type=Path, help="Offline preview only; not valid for submission")
    args = parser.parse_args()
    try:
        if not re.fullmatch(r"v\d+\.\d+(?:\.\d+)?", args.tag):
            raise ValueError("Expected a stable version tag vX.Y or vX.Y.Z")
        if args.preview_archive:
            generate(args.preview_archive, args.output, args.tag, "1970-01-01", preview=True)
        else:
            with fetch(f"https://api.github.com/repos/Chipi-Chapa-Corp/papers/releases/tags/{args.tag}") as response:
                release = json.load(response)
            if release.get("draft") or release.get("prerelease"):
                raise ValueError("The release must be public and stable")
            asset = next(asset for asset in release["assets"] if asset["name"] == ASSET)
            expected = f"{SOURCE}/releases/download/{args.tag}/{ASSET}"
            if asset["browser_download_url"] != expected:
                raise ValueError("Unexpected upstream download URL")
            with tempfile.TemporaryDirectory() as directory:
                archive = Path(directory) / ASSET
                with fetch(expected) as response, archive.open("wb") as file:
                    shutil.copyfileobj(response, file)
                generate(archive, args.output, args.tag, release["published_at"][:10])
    except (OSError, ValueError, KeyError, StopIteration, tarfile.TarError) as error:
        parser.exit(1, f"Cannot prepare FlatPark submission: {error}\n")


if __name__ == "__main__":
    main()
