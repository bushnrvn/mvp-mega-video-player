/* M65V video player for the MEGA65.
 *
 * Reads a .M65V file (made by tools/mkvideo.py) from the SD card through the hypervisor's DOS and plays it: 256-colour full-colour
 * characters for the picture (two buffers, flipped by switching the screen map), and 8-bit PCM through two audio DMA channels
 * (one per side) looping over a 32 KB ring buffer. The audio position is the clock: a frame is shown when the sound has played up to
 * it, and a frame that is already late is dropped instead.
 *
 * File layout (all sizes are multiples of 512, the SD sector, because the DOS hands the file over one sector at a time):
 *   header, 1024 bytes (see parse_header)
 *   LEAD audio chunks
 *   for every frame: its picture (the characters, 64 bytes each, in screen order), then the audio chunk for frame + LEAD
 */
#include <conio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define POKE(a, v) (*(volatile uint8_t *)(a) = (uint8_t)(v))
#define PEEK(a)    (*(volatile uint8_t *)(a))

/* hyppo.s */
extern uint8_t h_a, h_x, h_y, h_c;
void hyppo(uint8_t func);
extern char namebuf[256], dirbuf[256];

#define DOS_OPENDIR   0x12
#define DOS_READDIR   0x14
#define DOS_CLOSEDIR  0x16
#define DOS_OPENFILE  0x18
#define DOS_READFILE  0x1A
#define DOS_CLOSEFILE 0x20
#define DOS_CLOSEALL  0x22
#define DOS_SETNAME   0x2E
#define DOS_FINDFILE  0x34
#define DOS_CHDIRROOT 0x3C

#define BUF0_ADDR   0x40000UL           /* the two picture buffers: 1024 characters of 64 bytes each, in banks 4 and 5 */
#define BUF1_ADDR   0x50000UL
#define BUF0_CHAR   4096u               /* their first character numbers (address / 64) */
#define BUF1_CHAR   5120u
#define SCR0        0x12100UL           /* the two screen maps (2 bytes per character) */
#define SCR1        0x12900UL
#define COLOUR_OFS  0x2000u             /* colour RAM offset, in $FF80000 */
#define RING        0x18000UL           /* the audio ring: 32 KB, $18000-$1FFFF */
#define RING_SIZE   0x8000u
#define SECTOR_BUF  0xD6E00UL           /* where the DOS leaves a sector: $FFD6E00 */

/* ------------------------------------------------------------------ DMA -- */
static uint8_t dmalist[17];

static void dma_copy28(uint8_t src_mb, uint32_t src, uint8_t dst_mb, uint32_t dst, uint16_t count)
{
    dmalist[0] = 0x0B;                     /* 12-byte list format */
    dmalist[1] = 0x80; dmalist[2] = src_mb;
    dmalist[3] = 0x81; dmalist[4] = dst_mb;
    dmalist[5] = 0x00;                     /* end of options */
    dmalist[6] = 0x00;                     /* copy */
    dmalist[7] = (uint8_t)count; dmalist[8] = (uint8_t)(count >> 8);
    dmalist[9] = (uint8_t)src; dmalist[10] = (uint8_t)(src >> 8); dmalist[11] = (uint8_t)((src >> 16) & 0x0F);
    dmalist[12] = (uint8_t)dst; dmalist[13] = (uint8_t)(dst >> 8); dmalist[14] = (uint8_t)((dst >> 16) & 0x0F);
    dmalist[15] = 0x00;
    POKE(0xD702, 0x00);                    /* the list is in bank 0 */
    POKE(0xD701, (uint16_t)dmalist >> 8);
    POKE(0xD705, (uint8_t)(uint16_t)dmalist);   /* writing the LSB here runs an enhanced job */
}

static void dma_fill(uint8_t val, uint16_t count, uint8_t dst_mb, uint32_t dst)
{
    dmalist[0] = 0x0B; dmalist[1] = 0x81; dmalist[2] = dst_mb; dmalist[3] = 0x00;
    dmalist[4] = 0x03;                     /* fill */
    dmalist[5] = (uint8_t)count; dmalist[6] = (uint8_t)(count >> 8);
    dmalist[7] = val; dmalist[8] = 0; dmalist[9] = 0;
    dmalist[10] = (uint8_t)dst; dmalist[11] = (uint8_t)(dst >> 8); dmalist[12] = (uint8_t)((dst >> 16) & 0x0F);
    dmalist[13] = 0; dmalist[14] = 0; dmalist[15] = 0;
    POKE(0xD702, 0x00);
    POKE(0xD701, (uint16_t)dmalist >> 8);
    POKE(0xD705, (uint8_t)(uint16_t)dmalist);
}

/* ------------------------------------------------------------------ files -- */
static uint8_t file_fd;
static uint8_t read_error;               /* DOS error code of the first failed read, or 0 */

static uint8_t dos_open(const char *name)
{
    POKE(0xD689, PEEK(0xD689) | 0x80);        /* the SD sector buffer (not the floppy buffer) is what user programs see at $FFD6E00 */
    hyppo(DOS_CHDIRROOT);
    strncpy(namebuf, name, 63);
    namebuf[63] = 0;
    h_y = (uint8_t)((uint16_t)namebuf >> 8);
    hyppo(DOS_SETNAME);
    if (!h_c) return 0;
    hyppo(DOS_FINDFILE);
    if (!h_c) return 0;
    h_x = 0; h_y = 0;
    hyppo(DOS_OPENFILE);
    if (!h_c) return 0;
    file_fd = h_a;
    return 1;
}

/* n sectors of the open file into chip RAM (megabyte 0) at dst */
static void read_chip(uint32_t dst, uint16_t n)
{
    while (n--) {
        hyppo(DOS_READFILE);
        if (!h_c) { if (!read_error) read_error = h_a ? h_a : 0xFF; return; }
        dma_copy28(0xFF, SECTOR_BUF, 0, dst, 512);
        dst += 512;
    }
}

static void read_ram(uint8_t *dst, uint16_t n)       /* n sectors into ordinary memory */
{
    while (n--) {
        hyppo(DOS_READFILE);
        if (!h_c) { if (!read_error) read_error = h_a ? h_a : 0xFF; return; }
        dma_copy28(0xFF, SECTOR_BUF, 0, (uint16_t)dst, 512);
        dst += 512;
    }
}

/* ------------------------------------------------------------------ the file's header -- */
static uint8_t hdr[1024];
static uint8_t fps, flags, lead;
static uint16_t vw, vh, abytes, vsect, rate, cols, rows;
static uint32_t nframes;

static uint16_t rd16(uint8_t o) { return hdr[o] | ((uint16_t)hdr[o + 1] << 8); }

static uint8_t parse_header(void)
{
    if (hdr[0] != 0x4D || hdr[1] != 0x36 || hdr[2] != 0x35 || hdr[3] != 0x56 || hdr[4] != 1) return 0;   /* "M65V", version 1 */
    fps = hdr[5];
    vw = rd16(6); vh = rd16(8);
    flags = hdr[10];
    lead = hdr[11];
    abytes = rd16(12);
    nframes = rd16(14) | ((uint32_t)rd16(16) << 16);
    vsect = rd16(18);
    rate = rd16(20);
    cols = vw >> 3; rows = vh >> 3;
    if (cols < 1 || cols > 40 || rows < 1 || rows > 25) return 0;
    if (abytes == 0 || (abytes & 511) || (RING_SIZE % abytes)) return 0;
    if ((uint32_t)vsect * 512UL < (uint32_t)cols * rows * 64UL) return 0;
    if (vsect > 128) return 0;                                  /* a picture buffer is 64 KB: a bigger one would be written over other memory */
    return 1;
}

/* ------------------------------------------------------------------ video -- */
static uint8_t saved[0x40];              /* $D040-$D07F as found */
static uint8_t saved_misc[8];            /* $D011, $D016, $D018, $D020, $D021, $D030, $D031, $D070 */
static const uint16_t misc_addr[8] = { 0xD011, 0xD016, 0xD018, 0xD020, 0xD021, 0xD030, 0xD031, 0xD070 };
static uint16_t scr[1000];
static uint16_t x0, y0;

static void wait_frame(void)
{
    while (!(PEEK(0xD011) & 0x80)) { }
    while (PEEK(0xD011) & 0x80) { }
}

static void unlock_vic4(void) { POKE(0xD02F, 0x47); POKE(0xD02F, 0x53); }

static void video_regs(void)
{
    uint16_t bot = y0 + rows * 16;
    POKE(0xD05D, PEEK(0xD05D) & 0x7F);               /* HOTREG off: on real hardware the chip otherwise recomputes the layout from the VIC-II registers
                                                        whenever they are touched, wiping what is set below */
    POKE(0xD030, PEEK(0xD030) | 0x04);
    POKE(0xD070, 0x10);                              /* the picture uses palette bank 1 ($D070 bits 5-4) */
    POKE(0xD020, 0); POKE(0xD021, 0);
    POKE(0xD031, (PEEK(0xD031) | 0x80) & ~0x08);     /* H640, 200-line mode: a picture row is two rasters */
    POKE(0xD054, 0x45);                              /* 40 MHz, CHR16 (13-bit character numbers), full-colour characters above $FF */
    POKE(0xD05B, 0x01);                              /* two rasters per character line ... */
    POKE(0xD05A, 60);                                /* ... and 16 pixels per character: every picture pixel is 2 x 2 screen pixels */
    POKE(0xD05E, cols);
    POKE(0xD058, cols * 2); POKE(0xD059, 0);
    POKE(0xD07B, rows - 1);
    POKE(0xD063, PEEK(0xD063) & 0xF0);
    POKE(0xD064, (uint8_t)COLOUR_OFS);
    POKE(0xD065, (uint8_t)(COLOUR_OFS >> 8));
    POKE(0xD04C, (uint8_t)x0);
    POKE(0xD04D, (PEEK(0xD04D) & 0xF0) | (x0 >> 8));
    POKE(0xD04E, (uint8_t)y0);
    POKE(0xD04F, (PEEK(0xD04F) & 0xF0) | (y0 >> 8));
    POKE(0xD048, (uint8_t)y0);
    POKE(0xD049, (PEEK(0xD049) & 0xF0) | (y0 >> 8));
    POKE(0xD04A, (uint8_t)bot);
    POKE(0xD04B, (PEEK(0xD04B) & 0xF0) | (bot >> 8));
    POKE(0xD05C, 0x10);
    POKE(0xD05D, PEEK(0xD05D) & 0x40);
}

static void show_buffer(uint8_t b)
{
    uint32_t s = b ? SCR1 : SCR0;
    wait_frame();                                    /* switch while the picture is not being drawn, or the frame would tear */
    POKE(0xD060, (uint8_t)s);
    POKE(0xD061, (uint8_t)(s >> 8));
    POKE(0xD062, (uint8_t)(s >> 16));
}

static void video_setup(void)
{
    uint8_t i, n, k;
    uint16_t c, nchars = cols * rows;
    unlock_vic4();                                   /* (before the registers are read: they are the VIC-II's until then) */
    for (i = 0; i < 0x40; ++i) saved[i] = PEEK(0xD040 + i);
    for (i = 0; i < 8; ++i) saved_misc[i] = PEEK(misc_addr[i]);

    x0 = 80 + (640 - cols * 16) / 2;
    y0 = 104 + (400 - rows * 16) / 2;

    POKE(0xD05D, PEEK(0xD05D) & 0x7F);
    POKE(0xD011, PEEK(0xD011) & 0xEF);               /* blank until the first picture is in place */

    /* the palette goes in bank 1 (mapped in at $D100 by $D070 bits 7-6), which the picture then uses; the nybbles of each value are swapped */
    POKE(0xD070, 0x40 | 0x10);
    for (c = 0; c < 256; ++c) {
        uint8_t r = hdr[64 + c], g = hdr[320 + c], b = hdr[576 + c];
        POKE(0xD100 + c, (r << 4) | (r >> 4));
        POKE(0xD200 + c, (g << 4) | (g >> 4));
        POKE(0xD300 + c, (b << 4) | (b >> 4));
    }
    POKE(0xD070, 0x10);

    for (c = 0; c < nchars; ++c) scr[c] = BUF0_CHAR + c;
    dma_copy28(0, (uint16_t)scr, 0, SCR0, nchars * 2);
    for (c = 0; c < nchars; ++c) scr[c] = BUF1_CHAR + c;
    dma_copy28(0, (uint16_t)scr, 0, SCR1, nchars * 2);
    dma_fill(0, nchars * 2, 0xFF, 0x80000UL + COLOUR_OFS);          /* colour RAM: plain characters */
    dma_fill(0, 0x4000, 0, BUF0_ADDR);                              /* black to start with */
    dma_fill(0, 0x4000, 0, BUF1_ADDR);
    show_buffer(0);

    for (i = 0, n = 0; i < 40 && n < 2; ++i) {       /* set the registers until they have stayed put twice running (the chip can reset them late) */
        video_regs();
        show_buffer(0);
        for (k = 0; k < 6; ++k) wait_frame();
        if (PEEK(0xD05E) == cols && PEEK(0xD04C) == (uint8_t)x0 && PEEK(0xD048) == (uint8_t)y0) ++n; else n = 0;
    }
    POKE(0xD015, 0x00);                              /* no sprites */
}

static void video_show(void) { POKE(0xD011, PEEK(0xD011) | 0x10); }

static void video_restore(void)
{
    uint8_t i;
    POKE(0xD011, PEEK(0xD011) & 0xEF);
    unlock_vic4();
    POKE(0xD05D, PEEK(0xD05D) & 0x7F);
    POKE(0xD054, 0x00);
    for (i = 0; i < 0x40; ++i) POKE(0xD040 + i, saved[i]);
    for (i = 0; i < 8; ++i) POKE(misc_addr[i], saved_misc[i]);
    /* (the video chip stays in its VIC-IV mode: the KERNAL's screen editor reads $D062 and the others, which a VIC-III mode hides) */
}

/* ------------------------------------------------------------------ keys -- */
static uint8_t key_down(uint8_t col, uint8_t row)
{
    uint8_t n;
    POKE(0xDC00, (uint8_t)~(1u << col));
    for (n = 0; n < 24; ++n) __asm__("nop");
    return (PEEK(0xDC01) & (1u << row)) == 0;
}

static void keys_init(void) { POKE(0xDC02, 0xFF); POKE(0xDC03, 0x00); }
static void keys_done(void) { POKE(0xDC00, 0xFF); POKE(0xDC02, 0xFF); POKE(0xDC03, 0x00); }

/* ------------------------------------------------------------------ audio -- */
static uint32_t a_total;                 /* bytes of sound played since the start */
static uint16_t a_last;                  /* the last offset seen in the ring */
static uint16_t ring_wp;                 /* where the next chunk goes */
static uint8_t  audio_ctl;

static void audio_start(void)
{
    uint32_t fr = ((uint32_t)rate * 41423UL + 50000UL) / 100000UL;   /* the sample clock counts at 40.5 MHz in 24 bits */
    uint8_t c;
    POKE(0xD711, 0x80);                              /* audio DMA on */
    audio_ctl = 0xC2 | ((flags & 1) ? 0x20 : 0);     /* on, looping, 8-bit */
    for (c = 0; c < 4; c += 2) {                     /* channels 0 and 2: one on each side of the mix */
        uint16_t b = 0xD720 + c * 0x10;
        POKE(b, 0);
        POKE(b + 1, (uint8_t)RING); POKE(b + 2, (uint8_t)(RING >> 8)); POKE(b + 3, (uint8_t)(RING >> 16));
        POKE(b + 4, (uint8_t)fr); POKE(b + 5, (uint8_t)(fr >> 8)); POKE(b + 6, (uint8_t)(fr >> 16));
        POKE(b + 7, 0xFF); POKE(b + 8, 0xFF);        /* top address: end of the ring */
        POKE(b + 9, 0xFF);                           /* full volume */
        POKE(b + 0xA, (uint8_t)RING); POKE(b + 0xB, (uint8_t)(RING >> 8)); POKE(b + 0xC, (uint8_t)(RING >> 16));
    }
    POKE(0xD720, audio_ctl);
    POKE(0xD740, audio_ctl);
    a_total = 0;
    a_last = 0;
}

static void audio_stop(void)
{
    POKE(0xD720, 0x08); POKE(0xD740, 0x08);
    POKE(0xD720, 0x00); POKE(0xD740, 0x00);
}

static void audio_flip_sign(void)
{
    audio_ctl ^= 0x20;
    POKE(0xD720, audio_ctl);
    POKE(0xD740, audio_ctl);
}

static uint32_t audio_pos(void)
{
    uint8_t lo, hi, hi2;
    uint16_t cur;
    do { hi = PEEK(0xD72B); lo = PEEK(0xD72A); hi2 = PEEK(0xD72B); } while (hi != hi2);
    cur = (((uint16_t)hi << 8) | lo) & (RING_SIZE - 1);
    a_total += (uint16_t)(cur - a_last) & (RING_SIZE - 1);
    a_last = cur;
    return a_total;
}

static void read_audio_chunk(void)
{
    uint16_t n = abytes >> 9;
    read_chip(RING + ring_wp, n);
    ring_wp = (ring_wp + abytes) & (RING_SIZE - 1);
}

/* ------------------------------------------------------------------ playing -- */
static uint32_t shown, dropped;
static uint8_t quit;

static void check_keys(void)
{
    if (key_down(7, 7) || key_down(7, 6)) quit = 1;             /* RUN/STOP or Q */
    else if (key_down(3, 6)) {                                  /* U: flip the sample sign, in case the audio sounds like noise */
        audio_flip_sign();
        while (key_down(3, 6)) { }
    }
}

static void play(void)
{
    uint32_t f, target;
    uint8_t back = 0, i;
    uint16_t poll;

    read_error = 0;
    ring_wp = 0;
    for (i = 0; i < lead; ++i) read_audio_chunk();
    for (f = 0; f < nframes && !quit && !read_error; ++f) {
        read_chip(back ? BUF1_ADDR : BUF0_ADDR, vsect);
        read_audio_chunk();
        if (f == 0) {
            show_buffer(back);
            video_show();
            audio_start();
            ++shown;
        } else {
            target = f * abytes;
            if (audio_pos() >= target + abytes) {               /* a whole frame late: skip this one */
                ++dropped;
                continue;
            }
            poll = 0;
            while (audio_pos() < target) {
                if (!(++poll & 0x3F)) { check_keys(); if (quit) break; }
            }
            show_buffer(back);
            ++shown;
        }
        back ^= 1;
        check_keys();
#ifdef TEST_FRAMES
        if (shown == TEST_FRAMES) POKE(0xD6CF, 0x42);   /* tells Xemu (-testing) to exit and save a screenshot */
#endif
    }
    if (!quit && !read_error) {                                 /* let the last chunk of sound finish */
        target = nframes * abytes;
        poll = 0;
        while (audio_pos() < target && !quit) if (!(++poll & 0x3F)) check_keys();
    }
    audio_stop();
}

/* ------------------------------------------------------------------ the file list -- */
#define MAX_FILES 12
static char names[MAX_FILES][33];
static uint8_t nfiles;

static const uint8_t ext[5] = { 0x2E, 0x4D, 0x36, 0x35, 0x56 };      /* ".M65V" */

static void list_files(void)
{
    uint8_t fd, len, i, ok;
    nfiles = 0;
    hyppo(DOS_CHDIRROOT);
    h_x = 0; h_y = 0;
    hyppo(DOS_OPENDIR);
    if (!h_c) return;
    fd = h_a;
    while (nfiles < MAX_FILES) {
        h_x = fd; h_y = (uint8_t)((uint16_t)dirbuf >> 8);
        hyppo(DOS_READDIR);
        if (!h_c) break;
        len = (uint8_t)dirbuf[64];
        if ((dirbuf[86] & 0x10) || len < 6 || len > 32) continue;
        ok = 1;
        for (i = 0; i < 5; ++i) {                    /* (plain ASCII numbers: the compiler would turn letters in the source into PETSCII) */
            uint8_t c = (uint8_t)dirbuf[len - 5 + i];
            if (c >= 0x61 && c <= 0x7A) c -= 32;
            if (c != ext[i]) ok = 0;
        }
        if (!ok) continue;
        memcpy(names[nfiles], dirbuf, len);
        names[nfiles][len] = 0;
        ++nfiles;
    }
    h_x = fd;
    hyppo(DOS_CLOSEDIR);
}

static uint8_t choose(void)
{
    uint8_t i, c;
    clrscr();
    cputs("MEGA65 VIDEO PLAYER\r\n\r\n");
    list_files();
    if (!nfiles) {
        cputs("NO .M65V FILES ON THE SD CARD.\r\n\r\nCOPY ONE TO THE CARD'S TOP FOLDER\r\n(MADE WITH TOOLS/MKVIDEO.PY).\r\n\r\nPRESS A KEY.");
        cgetc();
        return 255;
    }
#ifdef TEST_FRAMES
    return 0;                                        /* test builds (run under Xemu with -testing) play the first file by themselves */
#endif
    for (i = 0; i < nfiles; ++i) {
        cputc("123456789ABC"[i]); cputs(". "); cputs(names[i]); cputs("\r\n");
    }
    cputs("\r\nPICK A FILE (Q = QUIT): ");
    for (;;) {
        c = cgetc();
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c == 'Q' && nfiles < 11) return 255;
        for (i = 0; i < nfiles; ++i) if ("123456789ABC"[i] == c) return i;
        if (c == 'Q') return 255;
    }
}

int main(void)
{
    uint8_t pick;
    for (;;) {
        pick = choose();
        if (pick == 255) break;
        clrscr();
        cputs("LOADING "); cputs(names[pick]); cputs("\r\n");
        if (!dos_open(names[pick])) { cputs("\r\nCAN'T OPEN THE FILE. PRESS A KEY."); cgetc(); continue; }
        read_error = 0;
        read_ram(hdr, 2);
        if (read_error || !parse_header()) {
            cputs("\r\nNOT A VALID M65V FILE. PRESS A KEY.\r\n");
            {
                uint8_t k;
                for (k = 0; k < 24; ++k) { cputc("0123456789ABCDEF"[hdr[k] >> 4]); cputc("0123456789ABCDEF"[hdr[k] & 15]); }
            }
            cgetc();
            h_x = file_fd; hyppo(DOS_CLOSEFILE);
            continue;
        }
        shown = dropped = 0; quit = 0;
        __asm__("sei");
        keys_init();
        video_setup();
        play();
        video_restore();
        keys_done();
        __asm__("cli");
        h_x = file_fd; hyppo(DOS_CLOSEFILE);
        clrscr();
        if (read_error) {
            cputs("READ ERROR $"); cputc("0123456789ABCDEF"[read_error >> 4]); cputc("0123456789ABCDEF"[read_error & 15]);
            cputs(" PART WAY THROUGH.\r\n");
        } else cputs("DONE.\r\n");
        {
            char num[12];
            cputs("\r\nFRAMES SHOWN:   "); cputs(ultoa(shown, num, 10));
            cputs("\r\nFRAMES DROPPED: "); cputs(ultoa(dropped, num, 10));
            cputs("\r\n");
        }
        cputs("\r\nPRESS A KEY.");
#ifdef TEST_FRAMES
        unlock_vic4();                               /* ($D6CF is a VIC-IV register) */
        POKE(0xD6CF, 0x42);
#endif
        cgetc();
    }
    clrscr();
    return 0;
}
