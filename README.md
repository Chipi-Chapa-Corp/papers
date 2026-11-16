# ![Dokument logo][logo] Dokument

Dokument is a fork of [Papers](https://apps.gnome.org/Papers/) with
quality-of-life improvements, tabs, and document scanning. It supports PDF,
DjVu, TIFF, and comic book archives.

## Build and install

Dokument is licensed under the [GPLv2 or later](COPYING).

Stable releases are published to [GitHub Releases](https://github.com/Chipi-Chapa-Corp/papers/releases)
and Dokument's Flatpak update repository:

```sh
flatpak install --user https://chipi-chapa-corp.github.io/papers/flatpak/dokument.flatpakref
```

The repository is created by the first version-tag release. The
`.flatpakref` installs Dokument and configures updates; `.flatpakrepo` and
standalone `.flatpak` installers are also attached to each release.

### Development builds

Build a development Flatpak with GNOME Builder, or run:

```sh
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak remote-add --user --if-not-exists gnome-nightly https://nightly.gnome.org/gnome-nightly.flatpakrepo
flatpak-builder --user --install --install-deps-from=flathub --install-deps-from=gnome-nightly --force-clean _build/flatpak build-aux/flatpak/com.chipichapa.Dokument.Devel.json
flatpak run com.chipichapa.Dokument.Devel
```

The development build uses the GNOME nightly SDK and Rust SDK extension.
The build command installs missing GNOME runtimes; install the Rust extension
from Flathub if it is not already available.

### Publishing a release

Set the version in `meson.build`, add its release notes to
`data/org.gnome.Papers.metainfo.xml.in.in`, commit the changes, and push a matching
version tag, for example `v51.0` for version `51.0`:

```sh
git tag v51.0
git push origin v51.0
```

The **Flatpak release** workflow validates the tag, runs CI, builds the stable
`com.chipichapa.Dokument` package for x86_64, deploys its update repository to
GitHub Pages, and publishes a GitHub release with installers and SHA-256 checksums.
Release builds use GNOME 51 and the default profile, with no striped development
header. The repository's GitHub Pages source must be set to **GitHub Actions**.
Version tags use `vX.Y` or `vX.Y.Z`. A manual workflow run builds and tests the
package and uploads artifacts without publishing a release.

## Reporting and development

Report bugs and propose changes in [this repository](https://github.com/Chipi-Chapa-Corp/papers).
See [testing and reporting](TESTING.md) and [contributing](CONTRIBUTING.md).
Builds, formatting checks, and tests run through [GitHub Actions](https://github.com/Chipi-Chapa-Corp/papers/actions).

### Requirements

* [GNOME platform libraries](https://www.gnome.org/)
* [Poppler for PDF viewing](https://poppler.freedesktop.org/)

### Optional backends

* [DjVuLibre for DjVu viewing](https://djvu.sourceforge.net/)
* [Libarchive for comic book archives](https://libarchive.org/)
* [LibTIFF for multipage TIFF viewing](https://libtiff.gitlab.io/libtiff/)

The underlying libraries retain the Papers names. Their upstream API reference
is available for [libppsview](https://gnome.pages.gitlab.gnome.org/papers/view/)
and [libppsdocument](https://gnome.pages.gitlab.gnome.org/papers/document/).

## Code of conduct

The [GNOME Code of Conduct](https://conduct.gnome.org/) applies when interacting with this project.

[logo]: data/icons/scalable/apps/org.gnome.Papers.svg
