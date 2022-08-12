PORT ?= /dev/tty.usbmodem14101

# PORT ?= /dev/ttyACM0
BUILDDIR ?= build
IDF_PATH ?= $(shell pwd)/esp-idf
ADF_PATH ?= $(shell pwd)/esp-adf

export ADF_PATH IDF_PATH

IDF_EXPORT_QUIET ?= 0
SHELL := /usr/bin/env bash

.PHONY: prepare clean build flash monitor menuconfig

all: prepare build install

prepare:
	git submodule update --init --recursive
	cd esp-idf; bash install.sh

clean:
	rm -rf "$(BUILDDIR)"

build:
	source "$(IDF_PATH)/export.sh" && idf.py build

install: build
	python3 tools/webusb_push.py "Audio player" build/main.bin --run

monitor:
	source "$(IDF_PATH)/export.sh" && idf.py monitor -p $(PORT)

menuconfig:
	source "$(IDF_PATH)/export.sh" && idf.py menuconfig
