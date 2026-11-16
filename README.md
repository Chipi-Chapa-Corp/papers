# ![Dokument logo][logo] Dokument

Dokument is a fork of [Papers](https://apps.gnome.org/Papers/) with
quality-of-life improvements, tabs, and document scanning. It supports PDF,
DjVu, TIFF, and comic book archives.

## Build and install

Dokument is licensed under the [GPLv2 or later](COPYING).

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
