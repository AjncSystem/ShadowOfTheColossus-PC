# Local helpers for running the Windows release under Wine on Linux.
# Override paths on the command line, e.g.: make game PORT_DIR=/path/to/SotC-PC

PORT_DIR ?= $(HOME)/Games/SotC-PC/SotC-PC
WINE     ?= wine
# zsh passes "PORT_DIR=~/..." with a literal ~: expand it here.
DIR      := $(patsubst ~%,$(HOME)%,$(PORT_DIR))

.PHONY: game launcher build

# Start the game directly with the settings in $(PORT_DIR)/sotc.ini.
game:
	cd "$(DIR)" && $(WINE) SotC.exe --run

# Open the launcher window (it uses its own settings, not sotc.ini).
launcher:
	cd "$(DIR)" && $(WINE) SotC.exe

# Cross build bb-probe.exe on Linux into out/cross (see build-linux.sh).
build:
	./build-linux.sh
