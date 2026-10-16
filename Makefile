APP_NAME := BorderColor
APP := build/$(APP_NAME).app
VERSION := $(shell /usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" Resources/Info.plist)
ZIP := build/$(APP_NAME)-$(VERSION).zip
# Universal binary: Apple silicon and Intel.
ARCHS := --arch arm64 --arch x86_64
# Ad-hoc by default; pass SIGN_IDENTITY="Developer ID Application: ..." for distribution.
SIGN_IDENTITY ?= -

.PHONY: all app run debug release icon clean

all: app

app:
	swift build -c release $(ARCHS)
	rm -rf "$(APP)"
	mkdir -p "$(APP)/Contents/MacOS" "$(APP)/Contents/Resources"
	cp "$$(swift build -c release $(ARCHS) --show-bin-path)/$(APP_NAME)" "$(APP)/Contents/MacOS/"
	cp Resources/Info.plist "$(APP)/Contents/Info.plist"
	cp Resources/AppIcon.icns "$(APP)/Contents/Resources/"
	codesign --force --options runtime --sign "$(SIGN_IDENTITY)" "$(APP)"

run: app
	-pkill -x $(APP_NAME)
	open "$(APP)"

# Runs in the foreground with engine logging on stderr.
debug: app
	-pkill -x $(APP_NAME)
	BORDERCOLOR_DEBUG=1 "$(APP)/Contents/MacOS/$(APP_NAME)"

# Zipped app to attach to a GitHub release.
release: app
	rm -f "$(ZIP)"
	ditto -c -k --keepParent "$(APP)" "$(ZIP)"
	shasum -a 256 "$(ZIP)"

# Redraws Resources/AppIcon.icns from scripts/make-icon.swift.
icon:
	rm -rf build/AppIcon.iconset
	swift scripts/make-icon.swift build/AppIcon.iconset
	iconutil -c icns build/AppIcon.iconset -o Resources/AppIcon.icns

clean:
	rm -rf .build build
