SHELL := /usr/bin/env bash

PACKAGE_DIR ?= .
BUILD_DIR ?= build
APP_NAME ?= PingBar
APP_BUNDLE ?= $(BUILD_DIR)/$(APP_NAME).app

.PHONY: debug build run install clean

debug:
	swift build --package-path "$(PACKAGE_DIR)" --scratch-path "$(BUILD_DIR)"

build:
	swift build --package-path "$(PACKAGE_DIR)" --configuration release --scratch-path "$(BUILD_DIR)"
	BIN_PATH="$$(swift build --package-path "$(PACKAGE_DIR)" --configuration release --show-bin-path --scratch-path "$(BUILD_DIR)")/$(APP_NAME)"; \
	APP_ROOT="$(APP_BUNDLE)/Contents"; \
	rm -rf "$(APP_BUNDLE)"; \
	mkdir -p "$$APP_ROOT/MacOS"; \
	cp "$$BIN_PATH" "$$APP_ROOT/MacOS/$(APP_NAME)"; \
	chmod +x "$$APP_ROOT/MacOS/$(APP_NAME)"; \
	/usr/bin/printf '%s\n' \
	'<?xml version="1.0" encoding="UTF-8"?>' \
	'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	'<plist version="1.0">' \
	'<dict>' \
	'  <key>CFBundleDevelopmentRegion</key><string>en</string>' \
	'  <key>CFBundleExecutable</key><string>$(APP_NAME)</string>' \
	'  <key>CFBundleIdentifier</key><string>com.koen.pingbar</string>' \
	'  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>' \
	'  <key>CFBundleName</key><string>$(APP_NAME)</string>' \
	'  <key>CFBundlePackageType</key><string>APPL</string>' \
	'  <key>CFBundleShortVersionString</key><string>1.0</string>' \
	'  <key>CFBundleVersion</key><string>1</string>' \
	'  <key>LSUIElement</key><true/>' \
	'</dict>' \
	'</plist>' \
	> "$$APP_ROOT/Info.plist"; \
	echo "Built app bundle: $(APP_BUNDLE)"

run:
	swift build --package-path "$(PACKAGE_DIR)" --product PingBar --scratch-path "$(BUILD_DIR)"
	BIN_PATH="$$(swift build --package-path "$(PACKAGE_DIR)" --show-bin-path --scratch-path "$(BUILD_DIR)")/PingBar"; \
	exec "$$BIN_PATH"

install: build
	APP_DEST="/Applications/$(APP_NAME).app"; \
	rm -rf "$$APP_DEST"; \
	/usr/bin/ditto "$(APP_BUNDLE)" "$$APP_DEST"; \
	echo "Installed $$APP_DEST"

clean:
	rm -rf "$(BUILD_DIR)" .build
