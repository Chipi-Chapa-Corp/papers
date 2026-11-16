# Contributing

## Licensing

Your work is considered a derivative work of the Papers codebase, and
therefore must be licensed as GPLv2+.

You do not need to assign us copyright attribution.
It is our belief that you should always retain copyright on your own work.

When a contribution it's close to being finished, maintainers might amend the
last touches themselves to avoid bikeshedding or endless style nits. You will
keep your authorship on any of those changes.

## Code Style

Dokument uses code formatters to increase consistency and simplify development.
Please fix CI code formatting issues, and configure your development environment
to use clang-format for C code, and rustfmt for Rust code.

### Commit messages

The expected format for git commit messages is as follows:

```plain
subsection: short explanation of the commit

Longer explanation explaining exactly what's changed, whether any
external or private interfaces changed, what bugs were fixed (with bug
tracker reference if applicable) and so forth. Be concise but not too
brief.

Closes #1234
```
 - Always add a brief description of the commit to the _first_ line of
 the commit and terminate by two newlines (it will work without the
 second newline, but that is not nice for the interfaces).

 - Whenever possible, the first line should include the subsystem of
   the project the commit belongs: `shell`, `libdocument`, `libview`,
   `libmisc`, `backends`, `build`, `doc`, `flatpak`.
   e.g. “flatpak: bump version of poppler”

 - First line (the brief description) must only be one sentence and
 should start with a capital letter unless it starts with a lowercase
 symbol or identifier. Don't use a trailing period either. Aim to not
 exceed 72 characters.

 - The main description (the body) is normal prose and should use normal
 punctuation and capital letters where appropriate. Consider the commit
 message as an email sent to the developers (or yourself, six months
 down the line) detailing **why** you changed something. There's no need
 to specify the **how**: the changes can be inlined.

 - While adding the main description please make sure that individual lines
within the body are no longer than 80 columns, ideally a bit less. This makes
it easier to read without scrolling (both on GitHub as well as a terminal with
the default terminal size).

 - When committing code on behalf of others use the `--author` option, e.g.
 `git commit -a --author "Joe Coder <joe@coder.org>"` and `--signoff`.

 - Reference issues using `Fixes #1234` or `Closes #1234` so GitHub can close
   them when the pull request is merged into the default branch.

## GUI tests

Run GUI tests in a private display and D-Bus session, using fixtures and separate
application data. Agent-owned tests must use the local `isolated-gui` skill and
its helper; never launch them on the user's desktop or inherited session bus.

## Troubleshooting

To enable the debug messages, set the environment variable for the section
you want to debug or set `PPS_DEBUG` to enable debug messages for all sections.

The following sections are available:

```c
PPS_DEBUG_JOBS
PPS_DEBUG_SHOW_BORDERS
```

#### Example
```c
PPS_DEBUG_JOBS=1 dokument document.pdf
```

### Asking Development-related questions

Ask development questions and submit pull requests in
[Dokument's repository](https://github.com/Chipi-Chapa-Corp/papers).

### ‘Show borders’ debugging hint

Dokument can show a border around the following graphical elements:

 * text characters
 * links
 * form elements
 * annotations
 * images
 * media elements
 * selections

this can be very helpful when debugging display issues related to those
elements, to activate it you just need to set two env vars when calling
Dokument from a terminal, e.g. to show annotation borders:

```sh
PPS_DEBUG=borders PPS_DEBUG_SHOW_BORDERS=annots dokument
```

where `PPS_DEBUG_SHOW_BORDERS` can be set to any of the following values:
`chars` `links` `forms` `annots` `images` `media` `selections`.

If you need to add additional tracing macros to debug a problem, it is
probably a good idea to submit a patch to add them. Chances are someone
else will need to debug stuff in the future.

### Debug Poppler messages

Poppler is the library used by Dokument to render PDF documents. When a document
presents error, or there are issues in Poppler to handle it, the output can be
seen by setting `G_MESSAGES_DEBUG` to enable debug messages for Poppler.

#### Example

```
G_MESSAGES_DEBUG=Poppler dokument document.pdf
```

or

```
G_MESSAGES_DEBUG=all dokument document.pdf
```

### Builder Cannot Run Dokument

One of the possible problems that can be encountered when running Dokument in
GNOME Builder is missing SDKs or runtimes that were not installed for some reason.
If you encountered similar problem, such as missing `org.gnome.Sdk` or alike:

#### `org.gnome.Sdk//master`

GNOME SDK can be installed from the GNOME Nightly repository, which can be installed with:

```bash
# Dokument is built against the current unstable version of GNOME's platform and SDK which is available from its own gnome-nightly repo
flatpak remote-add --user --if-not-exists flathub-beta https://flathub.org/beta-repo/flathub-beta.flatpakrepo
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak remote-add --user --if-not-exists gnome-nightly https://nightly.gnome.org/gnome-nightly.flatpakrepo
```

Finally, you can install it with:

```bash
flatpak install gnome-nightly org.gnome.Sdk//master
```


#### `org.freedesktop.Sdk.Extension.rust-stable//24.08`

Rust Extension of Freedesktop SDK, on the other hand, can be installed from the similar repository
with:

```bash
flatpak install org.freedesktop.Sdk.Extension.rust-stable//24.08
```
