#!/bin/sh

test -f Dokument.dmg && rm Dokument.dmg
create-dmg \
  --volname "Dokument Installer" \
  --volicon "Papers.icns" \
  --window-pos 200 120 \
  --window-size 800 400 \
  --icon-size 100 \
  --icon "Dokument.app" 200 190 \
  --hide-extension "Dokument.app" \
  --app-drop-link 600 185 \
  "Dokument.dmg" \
  "Dokument.app/"
