# MEGA65 video player. Needs cc65 with the mega65 target (set CC65_HOME / CL65 if it is not on your path), Python 3 and ffmpeg.
CC65_HOME ?= $(HOME)/Desktop/DugOut/cc65
CL65 ?= $(CC65_HOME)/bin/cl65
export CC65_HOME
OUT := build/m65play.prg

all: $(OUT)

$(OUT): src/player.c src/hyppo.s cfg/m65play.cfg
	@mkdir -p build
	$(CL65) -t mega65 -C cfg/m65play.cfg -O -o $@ src/player.c src/hyppo.s

clean:
	rm -rf build/*.prg build/*.o

.PHONY: all clean

# a build for the Xemu test run: plays the first file by itself and tells Xemu to save a screenshot after TEST_FRAMES pictures
build/m65play-test.prg: src/player.c src/hyppo.s cfg/m65play.cfg
	@mkdir -p build
	$(CL65) -t mega65 -C cfg/m65play.cfg -O -DTEST_FRAMES=$(or $(FRAMES),30) -o $@ src/player.c src/hyppo.s
