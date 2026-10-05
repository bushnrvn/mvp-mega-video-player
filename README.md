# MEGA65 video player

A native player for the MEGA65 (45GS02, built with cc65) that plays `.M65V` files from the SD card: 256 colours, up to 320x200,
8-bit mono sound, sound-clocked so picture and audio stay together (late frames are dropped, never the sound).

## Use
1. `make` builds `build/m65play.prg`.
2. Convert a video (needs ffmpeg): `python3 tools/mkvideo.py movie.mp4 MOVIE.M65V --fps 12`
   - `--box 320x200` largest picture (default), `--start`, `--duration`, `--dither none|bayer|sierra2_4a|floyd_steinberg`.
   - Data rate is shown; it is about `width * height * fps` bytes a second. If playback drops frames on the real machine, lower `--fps` or `--box`.
3. Copy `m65play.prg` and the `.M65V` files to the top folder of the SD card. Run the PRG in C65 mode (it lists the `.M65V` files).
4. Keys: number/letter picks a file, RUN/STOP or Q stops, U flips the audio sample sign (if the sound is noise).

## Test in Xemu
`./run-xemu.sh` uses the `sd/` folder as the SD card. `SHOT=out.png ./run-xemu.sh` with `make build/m65play-test.prg FRAMES=n` plays and saves a screenshot.

## Untested on real hardware
Everything has been run only in Xemu. Things to check on an R6: SD read speed (frames dropped is shown at the end), audio sign (key U),
picture position and shape, and that the machine returns to the text screen afterwards.

## File format
1024-byte header (`M65V`, version 1, fps, size, flags, lead, audio bytes per frame, frame count, picture sectors, sample rate, palette at 64),
then `lead` audio chunks, then for each frame: its picture as 64-byte full-colour characters in screen order, padded to 512, followed by the audio chunk for frame+lead.

## Licence
MIT, see `LICENSE`. Test videos (such as Big Buck Bunny) and ROM images are not included.
