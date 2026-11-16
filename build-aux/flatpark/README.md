# FlatPark publication

Dokument is intended to be listed on [FlatPark](https://flatpark.org/), using
the official GitHub release archive as extra-data. FlatPark downloads this
archive from GitHub when a user installs the app, checks its checksum, and
repackages the unmodified binaries. It does not consume our source manifest or
an independently hosted Flatpak remote.

## Releases and updates

Push a version tag matching `meson.build`. The Linux release workflow runs CI
and builds `dokument-linux-x86_64.tar.gz`, `dokument-linux-x86_64.flatpak`,
`dokument-build.json`, and `SHA256SUMS`.

The archive preserves the GNOME 51 `/app` payload: executable, document
backends, scanner worker and SANE drivers, schemas, data, icons, and licenses.
Split debug symbols are excluded. Filenames stay the same between versions.
The `.flatpak` file is a standalone installer, not FlatPark's input archive.

After the GitHub release is public, `flatpark/publish-action@v1` sends
`com.chipichapa.Dokument` and the version tag to FlatPark. No secret is required.
Its webhook requires an existing catalog entry whose `source_url` is
`https://github.com/Chipi-Chapa-Corp/papers`. Until that entry is accepted, the
notification fails without invalidating the upstream release.

## Initial listing

There are currently no public releases and no FlatPark entry for Dokument.
The first listing therefore cannot be submitted with a working download pin
yet. Once the first GitHub release is public:

1. Generate the catalog package from its published archive:

   ```sh
   python3 .github/scripts/flatpark-submission.py --tag v51.0 \
     --output _build/flatpark-submission/com.chipichapa.Dokument
   ```

   The command downloads the official public archive, pins its SHA-256 and size,
   copies its desktop integration and icons, and generates the descriptor,
   manifest, offline unpacker, launcher, and update resolver. It refuses to
   overwrite an existing output. Add a current Dokument screenshot to the
   submission; inherited Papers screenshots are removed from its metainfo.
2. Copy the generated directory to `registry/com.chipichapa.Dokument/` in a
   separate checkout/worktree of `flatpark/flatpark`.
3. Run its descriptor audit, offline extra-data extraction check, build/install
   verification, and an isolated GUI test of document viewing and scanning.
4. Submit the package with the exact tested features and any hardware limits.
   The public submission can also provide the developer approval required for
   FlatPark's developer-approved shield.

See FlatPark's [developer guide](https://flatpark.org/docs/developers/your-app/),
[submission process](https://flatpark.org/docs/packagers/submitting/), and
[release notification action](https://flatpark.org/docs/developers/publish-action/).

For a local packaging preview before the first release, pass
`--preview-archive dist/release/dokument-linux-x86_64.tar.gz`. Such a directory
is marked `UNPUBLISHED.txt` and must be regenerated from the public download
before submission. Its local checksum is not a pin for a future CI build.
