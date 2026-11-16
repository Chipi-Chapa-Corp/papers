# Testing and reporting

Test Dokument from a recent checkout of this repository. See the
[README](README.md#build-and-install) for development Flatpak build instructions.

## Reporting issues

Search [existing issues](https://github.com/Chipi-Chapa-Corp/papers/issues), then
[open an issue](https://github.com/Chipi-Chapa-Corp/papers/issues/new) with:

* Steps to reproduce, expected behavior, and actual behavior.
* A sample document, if it can be shared.
* The commit (`git rev-parse HEAD`), distribution, desktop, and installation method.
* The debugging information from **About Dokument → Troubleshooting**, or the output of
  `flatpak info com.chipichapa.Dokument.Devel` for Flatpak builds.

For terminal logs, run:

```sh
flatpak run com.chipichapa.Dokument.Devel
```

## Native builds

Install the development dependencies for GTK 4, libadwaita, Poppler, Exempi,
libarchive, DjVuLibre, libtiff, libsecret, SANE (`sane-backends-devel` on Fedora),
and the Rust and Meson toolchains.
Then build from the repository root:

```sh
git submodule update --init --checkout --recursive
meson setup _build -Dprofile=devel -Ddocumentation=false -Duser_doc=false -Dnautilus=false
meson compile -C _build
meson devenv -C _build dokument
```

Run `meson test -C _build --print-errorlogs` in a private graphical test session;
the Rust tests and some C tests create GTK windows. Agent-owned GUI work must use
the isolated-gui helper described in [Contributing](CONTRIBUTING.md#gui-tests).
GitHub Actions runs the suite with its own Xvfb display and D-Bus session.

## Debugging

To enable internal document diagnostics:

```sh
PPS_DEBUG_JOBS=1 flatpak run com.chipichapa.Dokument.Devel document.pdf
G_MESSAGES_DEBUG=all flatpak run com.chipichapa.Dokument.Devel document.pdf
```

For a Flatpak debugger shell, install the matching GNOME SDK, then run:

```sh
flatpak run --command=sh --devel com.chipichapa.Dokument.Devel
```

Inside the shell, `gdb --args dokument document.pdf` starts a debugging session.
Attach relevant logs or a backtrace to the GitHub issue.

## Scanning without hardware

The `scan-worker` test uses a temporary SANE configuration that enables only
SANE’s virtual `test` backend. Install the scanner drivers package to enable it
(`sane-backends-drivers-scanners` on Fedora). No physical scanners are used.

The scan view tests cover scanner progress, native PDF navigation, page and
whole-document edits, annotation preservation across edits and repeated exports,
sidebar layout, menu actions, dismissible preferences, recropping from the
original page, and physical paper sizes. The `pps-selection-gui` regression
replaces a live text selection during a text request to check snapshot ownership.
Run these GTK tests through the private GUI helper described above.

For native Toolbx builds, the scan worker uses the host system bus socket when
the container has no system bus. This lets SANE’s network backends discover
scanners through Avahi. The graphical application keeps its own session bus.
