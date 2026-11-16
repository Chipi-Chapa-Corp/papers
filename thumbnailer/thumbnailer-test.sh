#!/bin/sh
set -eu
trap 'rm -f thumbnail.png' EXIT
./dokument-thumbnailer "$1" thumbnail.png
test -s thumbnail.png
