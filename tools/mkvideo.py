#!/usr/bin/env python3
"""Convert a video into the .M65V format played by the MEGA65 player (src/player.c).

    mkvideo.py input.mp4 VIDEO.M65V [--fps 12] [--box 320x200] [--start 0:30] [--duration 60]

The picture is 256 colours from one palette made over the whole clip, stored as the 8x8-pixel full-colour characters the video chip
wants (64 bytes per character, characters in screen order), so the player copies each frame straight into place. The size is the
largest picture that fits the box with the right shape (the MEGA65's pixels are a little narrower than they are tall: --par),
rounded to whole characters, and the player centres it. Smaller pictures and lower frame rates need less of the SD card's speed:
the data rate is  width * height * fps  bytes a second, plus 12 KB a second of sound.

Needs ffmpeg and ffprobe.
"""
import argparse, json, os, struct, subprocess, sys, tempfile

SECTOR = 512
RING = 0x8000                      # the player's audio ring; the audio chunk size must divide it


def run_probe(path):
    out = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                          'stream=width,height,sample_aspect_ratio', '-of', 'json', path],
                         capture_output=True, text=True, check=True).stdout
    s = json.loads(out)['streams'][0]
    w, h = int(s['width']), int(s['height'])
    sar = s.get('sample_aspect_ratio', '1:1')
    try:
        a, b = (int(x) for x in sar.split(':'))
        sar = a / b if a > 0 and b > 0 else 1.0
    except ValueError:
        sar = 1.0
    return w, h, sar


def fit(box_w, box_h, dar, par):
    ratio = dar / par              # picture width / height, in picture pixels
    if box_w / box_h <= ratio:
        w, h = box_w, box_w / ratio
    else:
        w, h = box_h * ratio, box_h
    w = max(8, min(box_w, int(round(w / 8)) * 8))
    h = max(8, min(box_h, int(round(h / 8)) * 8))
    return w, h


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('input')
    ap.add_argument('output')
    ap.add_argument('--fps', type=int, default=12, help='frames per second (default 12)')
    ap.add_argument('--box', default='320x200', help='largest picture, in pixels (up to 320x200; default 320x200)')
    ap.add_argument('--par', type=float, default=0.889, help='shape of one MEGA65 pixel, width / height (default 0.889)')
    ap.add_argument('--start', help='start time, as ffmpeg writes it (30, 1:30, ...)')
    ap.add_argument('--duration', help='length to convert')
    ap.add_argument('--lead', type=int, default=8, help='frames of sound read ahead of the picture (default 8)')
    ap.add_argument('--dither', default='bayer', choices=['none', 'bayer', 'sierra2_4a', 'floyd_steinberg'],
                    help='how colours are mixed (default bayer: steady from frame to frame)')
    ap.add_argument('--unsigned-audio', action='store_true', help='store the sound as unsigned bytes')
    ap.add_argument('--keep-aspect-source', action='store_true', help=argparse.SUPPRESS)
    args = ap.parse_args()

    box_w, box_h = (int(x) for x in args.box.lower().split('x'))
    if not (8 <= box_w <= 320 and 8 <= box_h <= 200):
        sys.exit('the box must be between 8x8 and 320x200')
    if not 1 <= args.fps <= 60:
        sys.exit('fps must be 1-60')
    if not 1 <= args.lead <= 24:
        sys.exit('lead must be 1-24')

    sw, sh, sar = run_probe(args.input)
    w, h = fit(box_w, box_h, sw * sar / sh, args.par)
    cols, rows = w // 8, h // 8

    # sound: whole chunks of 512, 1024, 2048 or 4096 bytes, whichever gives a sample rate nearest 14 kHz
    abytes = min((512, 1024, 2048, 4096), key=lambda a: abs(args.fps * a - 14000))
    assert RING % abytes == 0
    rate = args.fps * abytes
    if rate > 40000:
        sys.exit('that frame rate would need too high a sample rate')

    vbytes = cols * rows * 64
    vsect = (vbytes + SECTOR - 1) // SECTOR
    print(f'source {sw}x{sh}  ->  {w}x{h} ({cols}x{rows} characters), {args.fps} fps, sound {rate} Hz mono')
    print(f'data rate: {(vsect * SECTOR + abytes) * args.fps / 1024:.0f} KB/s')

    seek = []
    if args.start:
        seek += ['-ss', args.start]
    if args.duration:
        seek += ['-t', args.duration]

    with tempfile.TemporaryDirectory() as tmp:
        pal_png = os.path.join(tmp, 'palette.png')
        vf = f'fps={args.fps},scale={w}:{h}:flags=lanczos'
        # pass 1: one palette for the whole clip
        print('pass 1: palette ...')
        subprocess.run(['ffmpeg', '-v', 'error', '-y'] + seek + ['-i', args.input, '-an',
                        '-vf', vf + ',palettegen=max_colors=256:stats_mode=full', '-frames:v', '1', pal_png], check=True)
        pal_raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', pal_png, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
                                 capture_output=True, check=True).stdout
        pal = [tuple(pal_raw[i * 3:i * 3 + 3]) for i in range(256)]
        # black goes first: it is the border colour
        k = min(range(256), key=lambda i: sum(pal[i]))
        pal[0], pal[k] = pal[k], pal[0]
        remap = bytearray(range(256))
        remap[0], remap[k] = k, 0
        remap = bytes(remap)               # translate old index -> new index (swapping 0 and k is its own inverse)

        # sound, all of it (it is only about 12 KB a second)
        print('sound ...')
        aud = subprocess.run(['ffmpeg', '-v', 'error'] + seek + ['-i', args.input, '-vn', '-ac', '1', '-ar', str(rate),
                              '-af', 'loudnorm=I=-16:TP=-1.5,aresample=' + str(rate), '-f', 'u8' if args.unsigned_audio else 's8', '-'],
                             capture_output=True)
        if aud.returncode != 0:                      # no sound track, or loudnorm unhappy: try plainly, else silence
            aud = subprocess.run(['ffmpeg', '-v', 'error'] + seek + ['-i', args.input, '-vn', '-ac', '1', '-ar', str(rate),
                                  '-f', 'u8' if args.unsigned_audio else 's8', '-'], capture_output=True)
        audio = aud.stdout if aud.returncode == 0 else b''
        silence = 128 if args.unsigned_audio else 0

        def chunk(i):
            c = audio[i * abytes:(i + 1) * abytes]
            return c + bytes([silence]) * (abytes - len(c))

        # pass 2: the pictures, as raw 8-bit indexes into that palette
        print('pass 2: pictures ...')
        dither = f'dither={args.dither}' + (':bayer_scale=3' if args.dither == 'bayer' else '')
        ff = subprocess.Popen(['ffmpeg', '-v', 'error'] + seek + ['-i', args.input, '-i', pal_png, '-an', '-lavfi',
                               f'[0:v]{vf}[x];[x][1:v]paletteuse={dither}', '-pix_fmt', 'pal8', '-f', 'rawvideo', '-'],
                              stdout=subprocess.PIPE)
        out = open(args.output, 'wb')
        out.write(b'\0' * 1024)                      # the header goes in at the end
        for i in range(args.lead):
            out.write(chunk(i))
        n = 0
        pad = bytes(vsect * SECTOR - vbytes)
        fsize = w * h
        while True:
            fr = ff.stdout.read(fsize + 1024)        # (ffmpeg's raw 8-bit palette frames carry their 1024-byte palette after the pixels)
            if len(fr) < fsize + 1024:
                break
            fr = fr[:fsize].translate(remap)
            parts = []
            for cr in range(rows):
                lines = [fr[(cr * 8 + y) * w:(cr * 8 + y + 1) * w] for y in range(8)]
                for cc in range(cols):
                    a = cc * 8
                    parts.append(b''.join(l[a:a + 8] for l in lines))
            out.write(b''.join(parts))
            out.write(pad)
            out.write(chunk(n + args.lead))
            n += 1
            if n % 100 == 0:
                print(f'  {n} frames', end='\r', flush=True)
        ff.stdout.close()
        if ff.wait() != 0 or n == 0:
            sys.exit('ffmpeg failed while reading the pictures')

        hdr = struct.pack('<4sBBHHBBHIHH', b'M65V', 1, args.fps, w, h, 1 if args.unsigned_audio else 0, args.lead,
                          abytes, n, vsect, rate)
        hdr += bytes(64 - len(hdr))
        hdr += bytes(p[0] for p in pal) + bytes(p[1] for p in pal) + bytes(p[2] for p in pal)
        hdr += bytes(1024 - len(hdr))
        out.seek(0)
        out.write(hdr)
        size = out.seek(0, 2)
        out.close()
    print(f'{n} frames ({n / args.fps:.1f} s), {size / 1048576:.1f} MB -> {args.output}')


if __name__ == '__main__':
    main()
