all: Dokument.dmg


Dokument.dmg: Dokument.app
	sh dmg.sh

Dokument.app: papers.bundle Info.plist Papers.icns launcher.sh
	rm -rf Dokument.app
	gtk-mac-bundler papers.bundle

clean:
	rm -rf Dokument.app Dokument.dmg


.PHONY: all clean
