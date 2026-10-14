APP_NAME := BorderColor
APP := build/$(APP_NAME).app
# Ad-hoc by default; pass SIGN_IDENTITY="Developer ID Application: ..." for distribution.
SIGN_IDENTITY ?= -

.PHONY: all app run debug clean

all: app

app:
	swift build -c release
	rm -rf "$(APP)"
	mkdir -p "$(APP)/Contents/MacOS"
	cp "$$(swift build -c release --show-bin-path)/$(APP_NAME)" "$(APP)/Contents/MacOS/"
	cp Resources/Info.plist "$(APP)/Contents/Info.plist"
	codesign --force --options runtime --sign "$(SIGN_IDENTITY)" "$(APP)"

run: app
	-pkill -x $(APP_NAME)
	open "$(APP)"

# Runs in the foreground with engine logging on stderr.
debug: app
	-pkill -x $(APP_NAME)
	BORDERCOLOR_DEBUG=1 "$(APP)/Contents/MacOS/$(APP_NAME)"

clean:
	rm -rf .build build
