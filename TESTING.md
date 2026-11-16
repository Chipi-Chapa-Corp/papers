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
libarchive, DjVuLibre, libtiff, libsecret, and the Rust and Meson toolchains.
Then build from the repository root:

```sh
git submodule update --init --recursive
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
