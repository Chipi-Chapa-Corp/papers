# ![Dokument logo][logo] Dokument

Dokument is a fork of [Papers](https://apps.gnome.org/Papers/) with
quality-of-life improvements, tabs, and document scanning. It supports PDF,
DjVu, TIFF, and comic book archives.

## Build and install

Dokument is licensed under the [GPLv2 or later](COPYING).

Download the Linux `.flatpak` installer from
[GitHub Releases](https://github.com/Chipi-Chapa-Corp/papers/releases).
Releases also include the prebuilt Linux archive used for FlatPark packaging.

[FlatPark](https://flatpark.org/) is the intended catalog and update channel.
The initial listing needs a public release and catalog review; it is not available
there yet. See [FlatPark publication](build-aux/flatpark/README.md).

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

The **Linux release** workflow validates the tag, runs CI, builds the stable
`com.chipichapa.Dokument` package for x86_64, and publishes the Linux archive,
standalone Flatpak installer, build metadata, and SHA-256 checksums on GitHub.
After publishing, `flatpark/publish-action` notifies FlatPark to pick up the new
archive. This requires the initial catalog listing to have been accepted.
Release builds use GNOME 51 and the default profile, with no striped development
header.
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

[logo]: data/icons/scalable/apps/com.chipichapa.Dokument.svg
