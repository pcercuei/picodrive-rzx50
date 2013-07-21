/*
	emu.c - based on platform/gp2x/emu.c

	(c) Copyright 2006 notaz, All rights reserved.
	Free for non-commercial use.

	For commercial use, separate licencing terms must be obtained.

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/time.h>
#include <errno.h>

#include "emu.h"
#include "sdlemu.h"
#include "menu.h"

#include "../common/arm_utils.h"
#include "../common/fonts.h"
#include "../common/emu.h"

#include <Pico/PicoInt.h>
#include <Pico/Patch.h>
#include <Pico/sound/mix.h>
#include <zlib/zlib.h>

#define OSD_FPS_X 260

int engineState;
int select_exits = 0;

char romFileName[PATH_MAX];
char *homePath;

static short sndBuffer[2*44100/50];
static struct timeval noticeMsgTime = { 0, 0 }; // when started showing
static int osd_fps_x;
static int combo_keys = 0, combo_acts = 0; // keys and actions which need button combos

char noticeMsg[64];  // notice msg to draw
unsigned char *PicoDraw2FB = NULL;  // temporary buffer for alt renderer
int reset_timing = 0;

static void emu_msg_cb(const char *msg);
static void emu_msg_tray_open(void);

void emu_noticeMsgUpdated(void)
{
	gettimeofday(&noticeMsgTime, 0);
}

void emu_getMainDir(char *dst, int len)
{
	extern char **g_argv;
	int j;

	strncpy(dst, g_argv[0], len);
	len -= 32; // reserve
	if (len < 0) len = 0;
	dst[len] = 0;
	for (j = strlen(dst); j > 0; j--)
		if (dst[j] == '/' || dst[j] == '\\') { dst[j+1] = 0; break; }
}

static void find_combos(void)
{
	int act, u;

	// find out which keys and actions are combos
	combo_keys = combo_acts = 0;
	for (act = 0; act < 32; act++)
	{
		int keyc = 0, keyc2 = 0;
		if (act == 16 || act == 17) continue; // player2 flag
		if (act > 17)
		{
			for (u = 0; u < 32; u++)
				if (currentConfig.KeyBinds[u] & (1 << act)) keyc++;
		}
		else
		{
			for (u = 0; u < 32; u++)
				if ((currentConfig.KeyBinds[u] & 0x30000) == 0 && // pl. 1
					(currentConfig.KeyBinds[u] & (1 << act))) keyc++;
			for (u = 0; u < 32; u++)
				if ((currentConfig.KeyBinds[u] & 0x30000) == 1 && // pl. 2
					(currentConfig.KeyBinds[u] & (1 << act))) keyc2++;
			if (keyc2 > keyc) keyc = keyc2;
		}
		if (keyc > 1)
		{
			// loop again and mark those keys and actions as combo
			for (u = 0; u < 32; u++)
			{
				if (currentConfig.KeyBinds[u] & (1 << act)) {
					combo_keys |= 1 << u;
					combo_acts |= 1 << act;
				}
			}
		}
	}

	printf("combo keys/acts: %08x %08x\n", combo_keys, combo_acts);
}

static void scaling_update(void)
{
	PicoOpt &= ~0x4100;
	/*switch (currentConfig.scaling) {
		default: break; // off
		case 1:  // hw hor
		case 2:  PicoOpt |=  0x0100; break; // hw hor+vert
		case 3:  PicoOpt |=  0x4000; break; // sw hor
	}*/
}


void emu_Init(void)
{
	char *p;
	// make temp buffer for alt renderer
	PicoDraw2FB = malloc((8+320)*(8+240+8));
	if (!PicoDraw2FB) {
		printf("PicoDraw2FB == 0\n");
	}

	// make dirs for saves, cfgs, etc.
	homePath = malloc(PATH_MAX);
#ifndef WIN32
	p = getenv("HOME");
	sprintf(homePath, !p ? ".picodrive/" : "%s/.picodrive/", p);
	mkdir(homePath, 0777);

	if(errno == EROFS || errno == EACCES || errno == EPERM) {
		getcwd(homePath, 512);
		strcat(homePath, "/.picodrive/");
		mkdir(homePath, 0777);
	}

	PicoConfigFile = malloc(PATH_MAX);
	sprintf(PicoConfigFile, "%spicoconfig.bin", homePath);

	brmPath = malloc(PATH_MAX); sprintf(brmPath, "%sbrm/", homePath); mkdir(brmPath, 0777);
	cfgPath = malloc(PATH_MAX); sprintf(cfgPath, "%scfg/", homePath); mkdir(cfgPath, 0777);
	mdsPath = malloc(PATH_MAX); sprintf(mdsPath, "%smds/", homePath); mkdir(mdsPath, 0777);
	srmPath = malloc(PATH_MAX);	sprintf(srmPath, "%ssrm/", homePath); mkdir(srmPath, 0777);
#else
	sprintf(homePath, ".picodrive/");
	mkdir(homePath);

	PicoConfigFile = malloc(PATH_MAX);
	sprintf(PicoConfigFile, ".picodrive/picoconfig.bin");

	brmPath = malloc(PATH_MAX); sprintf(brmPath, "%sbrm/", homePath); mkdir(brmPath);
	cfgPath = malloc(PATH_MAX); sprintf(cfgPath, "%scfg/", homePath); mkdir(cfgPath);
	mdsPath = malloc(PATH_MAX); sprintf(mdsPath, "%smds/", homePath); mkdir(mdsPath);
	srmPath = malloc(PATH_MAX);	sprintf(srmPath, "%ssrm/", homePath); mkdir(srmPath);
#endif

	PicoInit();
	PicoMessage = emu_msg_cb;
	PicoMCDopenTray = emu_msg_tray_open;
	PicoMCDcloseTray = menu_loop_tray;
}


void emu_Deinit(void)
{
	// save SRAM
	if((currentConfig.EmuOpt & 1) && SRam.changed) {
		emu_SaveLoadGame(0, 1);
		SRam.changed = 0;
	}

	if (!(currentConfig.EmuOpt & 0x20)) {
		FILE *f = fopen(PicoConfigFile, "r+b");
		if (!f) emu_WriteConfig(0);
		else {
			// if we already have config, reload it, except last ROM
			fseek(f, sizeof(currentConfig.lastRomFile), SEEK_SET);
			fread(&currentConfig.EmuOpt, 1, sizeof(currentConfig) - sizeof(currentConfig.lastRomFile), f);
			fseek(f, 0, SEEK_SET);
			fwrite(&currentConfig, 1, sizeof(currentConfig), f);
			fflush(f);
			fclose(f);
#ifndef NO_SYNC
			sync();
#endif
		}
	}

	free(PicoDraw2FB);

	free(homePath);
	free(brmPath);
	free(cfgPath);
	free(mdsPath);
	free(srmPath);
	free(PicoConfigFile);

	PicoExit();
}

void emu_setDefaultConfig(void)
{
	memset(&currentConfig, 0, sizeof(currentConfig));
	currentConfig.lastRomFile[0] = 0;
	currentConfig.EmuOpt  = 0x1f | 0x600; // | confirm_save, cd_leds
	currentConfig.PicoOpt = 0x0f | 0xe00; // | use_940, cd_pcm, cd_cdda
	currentConfig.PsndRate = 22050; // 44100;
	currentConfig.PicoRegion = 0; // auto
	currentConfig.PicoAutoRgnOrder = 0x184; // US, EU, JP
	currentConfig.Frameskip = -1; // auto
	currentConfig.CPUclock = 336;
	currentConfig.volume = 50;
	currentConfig.KeyBinds[ 6] = 1<<0; // SACB RLDU
	currentConfig.KeyBinds[27] = 1<<1;
	currentConfig.KeyBinds[ 5] = 1<<2;
	currentConfig.KeyBinds[18] = 1<<3;
	currentConfig.KeyBinds[ 1] = 1<<4;
	currentConfig.KeyBinds[ 0] = 1<<5;
	currentConfig.KeyBinds[ 2] = 1<<6;
	currentConfig.KeyBinds[16] = 1<<7;
	currentConfig.KeyBinds[19] = 1<<26; // switch rend
	currentConfig.KeyBinds[14] = 1<<27; // save state
	currentConfig.KeyBinds[15] = 1<<28; // load state
	currentConfig.KeyBinds[23] = 1<<29; // vol up
	currentConfig.KeyBinds[22] = 1<<30; // vol down
	currentConfig.gamma = 100;
	currentConfig.PicoCDBuffers = 64;
	currentConfig.scaling = 0;
}

void osd_text(int x, int y, const char *text)
{
	int len = strlen(text)*8;

	if ((PicoOpt&0x10)||!(currentConfig.EmuOpt&0x80)) {
		int *p, i, h;
		x &= ~3; // align x
		len = (len+3) >> 2;
		for (h = 0; h < 8; h++) {
			p = (int *) ((unsigned char *) sdl_screen+x+320*(y+h));
			for (i = len; i; i--, p++) *p = 0xe0e0e0e0;
		}
		emu_textOut8(x, y, text);
	} else {
		int *p, i, h;
		x &= ~1; // align x
		len = (len+1) >> 1;
		for (h = 0; h < 8; h++) {
			p = (int *) ((unsigned short *) sdl_screen+x+320*(y+h));
			for (i = len; i; i--, p++) *p = (*p>>2)&0x39e7;
		}
		emu_textOut16(x, y, text);
	}
}

static void cd_leds(void)
{
//	static
	int old_reg;
//	if (!((Pico_mcd->s68k_regs[0] ^ old_reg) & 3)) return; // no change // mmu hack problems?
	old_reg = Pico_mcd->s68k_regs[0];

	if ((PicoOpt&0x10)||!(currentConfig.EmuOpt&0x80)) {
		// 8-bit modes
		unsigned int col_g = (old_reg & 2) ? 0xc0c0c0c0 : 0xe0e0e0e0;
		unsigned int col_r = (old_reg & 1) ? 0xd0d0d0d0 : 0xe0e0e0e0;
		*(unsigned int *)((char *)sdl_screen + 320*2+ 4) =
		*(unsigned int *)((char *)sdl_screen + 320*3+ 4) =
		*(unsigned int *)((char *)sdl_screen + 320*4+ 4) = col_g;
		*(unsigned int *)((char *)sdl_screen + 320*2+12) =
		*(unsigned int *)((char *)sdl_screen + 320*3+12) =
		*(unsigned int *)((char *)sdl_screen + 320*4+12) = col_r;
	} else {
		// 16-bit modes
		unsigned int *p = (unsigned int *)((short *)sdl_screen + 320*2+4);
		unsigned int col_g = (old_reg & 2) ? 0x06000600 : 0;
		unsigned int col_r = (old_reg & 1) ? 0xc000c000 : 0;
		*p++ = col_g; *p++ = col_g; p+=2; *p++ = col_r; *p++ = col_r; p += 320/2 - 12/2;
		*p++ = col_g; *p++ = col_g; p+=2; *p++ = col_r; *p++ = col_r; p += 320/2 - 12/2;
		*p++ = col_g; *p++ = col_g; p+=2; *p++ = col_r; *p++ = col_r;
	}
}

static int EmuScan16(unsigned int num, void *sdata)
{
	if (!(Pico.video.reg[1]&8)) num += 8;
	DrawLineDest = (unsigned short *) sdl_screen + 320*(num+1);

	return 0;
}

static int EmuScan8(unsigned int num, void *sdata)
{
	if (!(Pico.video.reg[1]&8)) num += 8;
	DrawLineDest = (unsigned char *)  sdl_screen + 320*(num+1);

	return 0;
}

int localPal[0x100];
static void (*vidCpyM2)(void *dest, void *src) = NULL;

static void blit(const char *fps, const char *notice)
{
	int emu_opt = currentConfig.EmuOpt;

	if (PicoOpt&0x10) {
		// 8bit fast renderer
		if (Pico.m.dirtyPal) {
			Pico.m.dirtyPal = 0;
			vidConvCpyRGB32(localPal, Pico.cram, 0x40);
			// feed new palette to our device
			sdl_video_setpalette(localPal, 0x40);
		}
		vidCpyM2((unsigned char *)sdl_screen+320*8, PicoDraw2FB+328*8);
	} else if (!(emu_opt&0x80)) {
		// 8bit accurate renderer
		if (Pico.m.dirtyPal) {
			Pico.m.dirtyPal = 0;
			if(Pico.video.reg[0xC]&8) { // shadow/hilight mode
				vidConvCpyRGB32(localPal, Pico.cram, 0x40);
				vidConvCpyRGB32sh(localPal+0x40, Pico.cram, 0x40);
				vidConvCpyRGB32hi(localPal+0x80, Pico.cram, 0x40);
				blockcpy(localPal+0xc0, localPal+0x40, 0x40*4);
				localPal[0xc0] = 0x0000c000;
				localPal[0xd0] = 0x00c00000;
				localPal[0xe0] = 0x00000000; // reserved pixels for OSD
				localPal[0xf0] = 0x00ffffff;
				sdl_video_setpalette(localPal, 0x100);
			} else if (rendstatus & 0x20) { // mid-frame palette changes
				vidConvCpyRGB32(localPal, Pico.cram, 0x40);
				vidConvCpyRGB32(localPal+0x40, HighPal, 0x40);
				vidConvCpyRGB32(localPal+0x80, HighPal+0x40, 0x40);
				sdl_video_setpalette(localPal, 0xc0);
			} else {
				vidConvCpyRGB32(localPal, Pico.cram, 0x40);
				sdl_video_setpalette(localPal, 0x40);
			}
		}
	}

	if(notice || (emu_opt & 2)) {
		int h = 232;
		if(currentConfig.scaling == 2 && !(Pico.video.reg[1]&8)) h -= 8;
		if(notice) osd_text(4, h, notice);
		if(emu_opt & 2)  osd_text(osd_fps_x, h, fps);
	}
	if((emu_opt & 0x400) && (PicoMCD & 1)) cd_leds();

	sdl_video_flip();

	if (!(PicoOpt&0x10)) {
		if (!(Pico.video.reg[1]&8)) {
			if (currentConfig.EmuOpt&0x80) {
				DrawLineDest = (unsigned short *) sdl_screen + 320*8;
			} else {
				DrawLineDest = (unsigned char  *) sdl_screen + 320*8;
			}
		} else {
			DrawLineDest = sdl_screen;
		}
	}
}

// clears whole screen or just the notice area (in all buffers)
static void clearArea(int full)
{
	if ((PicoOpt&0x10)||!(currentConfig.EmuOpt&0x80)) {
		// 8-bit renderers
		if (full) sdl_memset_all_buffers(0, 0xe0, 320*240);
		else      sdl_memset_all_buffers(320*232, 0xe0, 320*8);
	} else {
		// 16bit accurate renderer
		if (full) sdl_memset_all_buffers(0, 0, 320*240*2);
		else      sdl_memset_all_buffers(320*232*2, 0, 320*8*2);
	}
}

static void vidResetMode(void)
{
	if (PicoOpt&0x10) {
		sdl_video_changemode(8);
	} else if (currentConfig.EmuOpt&0x80) {
		sdl_video_changemode(16);
		PicoDrawSetColorFormat(1);
		PicoScan = EmuScan16;
		PicoScan(0, 0);
	} else {
		sdl_video_changemode(8);
		PicoDrawSetColorFormat(2);
		PicoScan = EmuScan8;
		PicoScan(0, 0);
	}
	if ((PicoOpt&0x10)||!(currentConfig.EmuOpt&0x80)) {
		// setup pal for 8-bit modes
		localPal[0xc0] = 0x0000c000; // MCD LEDs
		localPal[0xd0] = 0x00c00000;
		localPal[0xe0] = 0x00000000; // reserved pixels for OSD
		localPal[0xf0] = 0x00ffffff;
		sdl_video_setpalette(localPal, 0x100);
		sdl_memset_all_buffers(0, 0xe0, 320*240);
		sdl_video_flip();
	} else sdl_memset_all_buffers(0, 0, 320*240*2);
	Pico.m.dirtyPal = 1;
	// reset scaling
	if (currentConfig.scaling == 2 && !(Pico.video.reg[1]&8))
	     sdl_video_RGB_setscaling(8, (PicoOpt&0x100)&&!(Pico.video.reg[12]&1) ? 256 : 320, 224);
	else sdl_video_RGB_setscaling(0, (PicoOpt&0x100)&&!(Pico.video.reg[12]&1) ? 256 : 320, 240);
}

static void emu_msg_cb(const char *msg)
{
	if ((PicoOpt&0x10)||!(currentConfig.EmuOpt&0x80)) {
		// 8-bit renderers
		sdl_memset_all_buffers(320*232, 0xe0, 320*8);
		osd_text(4, 232, msg);
		sdl_memcpy_all_buffers((char *)sdl_screen+320*232, 320*232, 320*8);
	} else {
		// 16bit accurate renderer
		sdl_memset_all_buffers(320*232*2, 0, 320*8*2);
		osd_text(4, 232, msg);
		sdl_memcpy_all_buffers((char *)sdl_screen+320*232*2, 320*232*2, 320*8*2);
	}
	gettimeofday(&noticeMsgTime, 0);
	noticeMsgTime.tv_sec -= 2;

	/* assumption: emu_msg_cb gets called only when something slow is about to happen */
	reset_timing = 1;
}

static void emu_state_cb(const char *str)
{
	clearArea(0);
	blit("", str);
}

static void emu_msg_tray_open(void)
{
	strcpy(noticeMsg, "CD tray opened");
	gettimeofday(&noticeMsgTime, 0);
}

static void update_volume(int has_changed, int is_up)
{
}

static void change_fast_forward(int set_on)
{
	static void *set_PsndOut = NULL;
	static int set_Frameskip, set_EmuOpt, is_on = 0;

	if (set_on && !is_on) {
		set_PsndOut = PsndOut;
		set_Frameskip = currentConfig.Frameskip;
		set_EmuOpt = currentConfig.EmuOpt;
		PsndOut = NULL;
		currentConfig.Frameskip = 8;
		currentConfig.EmuOpt &= ~4;
		is_on = 1;
	}
	else if (!set_on && is_on) {
		PsndOut = set_PsndOut;
		currentConfig.Frameskip = set_Frameskip;
		currentConfig.EmuOpt = set_EmuOpt;
		PsndRerate(1);
		update_volume(0, 0);
		reset_timing = 1;
		is_on = 0;
	}
}

static void RunEvents(unsigned int which)
{
	if (which & 0x1800) // save or load (but not both)
	{
		int do_it = 1;
		if ( emu_checkSaveFile(state_slot) &&
				(( (which & 0x1000) && (currentConfig.EmuOpt & 0x800)) ||   // load
				 (!(which & 0x1000) && (currentConfig.EmuOpt & 0x200))) ) { // save
			unsigned long keys;
			blit("", (which & 0x1000) ? "LOAD STATE? (X=yes, B=no)" : "OVERWRITE SAVE? (X=yes, B=no)");
			while( !((keys = sdl_joystick_read(1)) & (GP2X_X|GP2X_Y)) )
				usleep(50*1024);
			if (keys & GP2X_X) do_it = 0;
			clearArea(0);
		}
		if (do_it) {
			osd_text(4, 232, (which & 0x1000) ? "LOADING GAME" : "SAVING GAME");
			PicoStateProgressCB = emu_state_cb;
			sdl_memcpy_all_buffers(sdl_screen, 0, 320*240*2);
			emu_SaveLoadGame((which & 0x1000) >> 12, 0);
			PicoStateProgressCB = NULL;
		}

		reset_timing = 1;
	}
	if (which & 0x0400) // switch renderer
	{
		if      (  PicoOpt&0x10)             { PicoOpt&=~0x10; currentConfig.EmuOpt |= 0x80; }
		else if (!(currentConfig.EmuOpt&0x80)) PicoOpt|= 0x10;
		else   currentConfig.EmuOpt &= ~0x80;

		vidResetMode();

		if (PicoOpt&0x10) {
			strcpy(noticeMsg, " 8bit fast renderer");
		} else if (currentConfig.EmuOpt&0x80) {
			strcpy(noticeMsg, "16bit accurate renderer");
		} else {
			strcpy(noticeMsg, " 8bit accurate renderer");
		}

		gettimeofday(&noticeMsgTime, 0);
	}
	if (which & 0x0300)
	{
		if(which&0x0200) {
			state_slot -= 1;
			if(state_slot < 0) state_slot = 9;
		} else {
			state_slot += 1;
			if(state_slot > 9) state_slot = 0;
		}
		sprintf(noticeMsg, "SAVE SLOT %i [%s]", state_slot, emu_checkSaveFile(state_slot) ? "USED" : "FREE");
		gettimeofday(&noticeMsgTime, 0);
	}
	if (which & 0x0080) {
		engineState = PGS_Menu;
	}
}

static void updateKeys(void)
{
	unsigned long keys, allActions[2] = { 0, 0 }, events;
	static unsigned long prevEvents = 0;
	int joy, i;

	keys = sdl_joystick_read(0);
	if (keys & GP2X_SELECT) {
		engineState = select_exits ? PGS_Quit : PGS_Menu;
		// wait until select is released, so menu would not resume game
		while (sdl_joystick_read(1) & GP2X_SELECT) usleep(50*1000);
	}

	keys &= CONFIGURABLE_KEYS;

	for (i = 0; i < 32; i++) {
		if (keys & (1 << i)) {
			int pl, acts = currentConfig.KeyBinds[i];
			if (!acts) continue;
			pl = (acts >> 16) & 1;
			if (combo_keys & (1 << i)) {
				int u = i+1, acts_c = acts & combo_acts;
				// let's try to find the other one
				if (acts_c)
					for (; u < 32; u++)
						if ( (currentConfig.KeyBinds[u] & acts_c) && (keys & (1 << u)) ) {
							allActions[pl] |= acts_c;
							keys &= ~((1 << i) | (1 << u));
							break;
						}
				// add non-combo actions if combo ones were not found
				if (!acts_c || u == 32)
					allActions[pl] |= acts & ~combo_acts;
			} else {
				allActions[pl] |= acts;
			}
		}
	}

	PicoPad[0] = (unsigned short) allActions[0];
	PicoPad[1] = (unsigned short) allActions[1];

	events = (allActions[0] | allActions[1]) >> 16;

	if ((events ^ prevEvents) & 0x40)
		change_fast_forward(events & 0x40);

	events &= ~prevEvents;
	if (events) RunEvents(events);
	if (movie_data) emu_updateMovie();

	prevEvents = (allActions[0] | allActions[1]) >> 16;
}


static void updateSound(int len)
{
	if (PicoOpt&8) len<<=1;

	/* avoid writing audio when lagging behind to prevent audio lag */
	if (PicoSkipFrame != 2)
		sdl_sound_write(PsndOut, len<<1);
}

static void SkipFrame(int do_audio)
{
	PicoSkipFrame=do_audio ? 1 : 2;
	PicoFrame();
	PicoSkipFrame=0;
}


void emu_forcedFrame(void)
{
	int po_old = PicoOpt;
	int eo_old = currentConfig.EmuOpt;

	PicoOpt &= ~0x0010;
	PicoOpt |=  0x4080; // soft_scale | acc_sprites
	currentConfig.EmuOpt |= 0x80;

	//vidResetMode();
	PicoDrawSetColorFormat(1);
	PicoScan = EmuScan16;
	PicoScan(0, 0);
	Pico.m.dirtyPal = 1;
	PicoFrameDrawOnly();

/*
	if (!(Pico.video.reg[12]&1)) {
		vidCpyM2 = vidCpyM2_32col;
		clearArea(1);
	} else	vidCpyM2 = vidCpyM2_40col;

	vidCpyM2((unsigned char *)gp2x_screen+320*8, PicoDraw2FB+328*8);
	vidConvCpyRGB32(localPal, Pico.cram, 0x40);
	gp2x_video_setpalette(localPal, 0x40);
*/
	PicoOpt = po_old;
	currentConfig.EmuOpt = eo_old;
}

static void simpleWait(int thissec, int lim_time)
{
	struct timeval tval;

	spend_cycles(1024);
	gettimeofday(&tval, 0);
	if(thissec != tval.tv_sec) tval.tv_usec+=1000000;

	while(tval.tv_usec < lim_time)
	{
		spend_cycles(1024);
		gettimeofday(&tval, 0);
		if(thissec != tval.tv_sec) tval.tv_usec+=1000000;
	}
}

void emu_Loop(void)
{
	static int PsndRate_old = 0, PicoOpt_old = 0, pal_old = 0;
	char fpsbuff[24]; // fps count c string
	struct timeval tval; // timing
	int thissec = 0, frames_done = 0, frames_shown = 0, oldmodes = 0;
	int target_fps, target_frametime, lim_time, vsync_offset, i;
	char *notice = 0;

	printf("entered emu_Loop()\n");

	fpsbuff[0] = 0;

	// make sure we are in correct mode
	vidResetMode();
	scaling_update();
	Pico.m.dirtyPal = 1;
	oldmodes = ((Pico.video.reg[12]&1)<<2) ^ 0xc;
	find_combos();

	// pal/ntsc might have changed, reset related stuff
	target_fps = Pico.m.pal ? 50 : 60;
	target_frametime = 1000000/target_fps;
	reset_timing = 1;

	// prepare sound stuff
	if (currentConfig.EmuOpt & 4)
	{
		int snd_excess_add;
		if (PsndRate != PsndRate_old || (PicoOpt&0x20b) != (PicoOpt_old&0x20b) || Pico.m.pal != pal_old ||
				(PicoOpt&0x200)) {
			PsndRerate(Pico.m.frame_count ? 1 : 0);
		}
		snd_excess_add = ((PsndRate - PsndLen*target_fps)<<16) / target_fps;
		printf("starting audio: %i len: %i (ex: %04x) stereo: %i, pal: %i\n",
			PsndRate, PsndLen, snd_excess_add, (PicoOpt&8)>>3, Pico.m.pal);
		sdl_start_sound(PsndRate, 16, (PicoOpt&8)>>3);
		sdl_sound_volume(currentConfig.volume, currentConfig.volume);
		PicoWriteSound = updateSound;
		update_volume(0, 0);
		memset(sndBuffer, 0, sizeof(sndBuffer));
		PsndOut = sndBuffer;
		PsndRate_old = PsndRate;
		PicoOpt_old  = PicoOpt;
		pal_old = Pico.m.pal;
	} else {
		PsndOut = NULL;
	}

	// prepare CD buffer
	if (PicoMCD & 1) PicoCDBufferInit();

#ifndef WIN32
	do {
		gettimeofday(&tval, 0);
	} while (tval.tv_usec != 0);
#endif
	// emulation loop
	while (engineState == PGS_Running) {
		int modes;

		gettimeofday(&tval, 0);
		if (reset_timing) {
			reset_timing = 0;
			thissec = tval.tv_sec;
			frames_shown = frames_done = tval.tv_usec/target_frametime;
		}

		// show notice message?
		if (noticeMsgTime.tv_sec)
		{
			static int noticeMsgSum;
			if((tval.tv_sec*1000000+tval.tv_usec) - (noticeMsgTime.tv_sec*1000000+noticeMsgTime.tv_usec) > 2000000) { // > 2.0 sec
				noticeMsgTime.tv_sec = noticeMsgTime.tv_usec = 0;
				clearArea(0);
				notice = 0;
			} else {
				int sum = noticeMsg[0]+noticeMsg[1]+noticeMsg[2];
				if (sum != noticeMsgSum) { clearArea(0); noticeMsgSum = sum; }
				notice = noticeMsg;
			}
		}

		// check for mode changes
		modes = ((Pico.video.reg[12]&1)<<2)|(Pico.video.reg[1]&8);
		if (modes != oldmodes)
		{
			int scalex = 320;
			osd_fps_x = OSD_FPS_X;
			if (modes & 4) {
				vidCpyM2 = vidCpyM2_40col;
			} else {
				if (PicoOpt & 0x100) {
					vidCpyM2 = vidCpyM2_32col_nobord;
					scalex = 256;
					osd_fps_x = OSD_FPS_X - 64;
				} else {
					vidCpyM2 = vidCpyM2_32col;
				}
			}
			if (currentConfig.scaling == 2 && !(modes&8)) // want vertical scaling and game is not in 240 line mode
			     sdl_video_RGB_setscaling(8, scalex, 224);
			else sdl_video_RGB_setscaling(0, scalex, 240);
			oldmodes = modes;
			clearArea(1);
		}

		// second changed?
		if (thissec != tval.tv_sec) {
			if (currentConfig.EmuOpt & 2)
				sprintf(fpsbuff, "%02i/%02i", frames_shown, frames_done);
			if (fpsbuff[5] == 0) { fpsbuff[5] = fpsbuff[6] = ' '; fpsbuff[7] = 0; }

			thissec = tval.tv_sec;

			if (PsndOut == 0 && currentConfig.Frameskip >= 0) {
				frames_done = frames_shown = 0;
			} else {
				// it is quite common for this implementation to leave 1 frame unfinished
				// when second changes, but we don't want buffer to starve.
				if(PsndOut && frames_done < target_fps && frames_done > target_fps-5) {
					updateKeys();
					SkipFrame(1); frames_done++;
				}

				frames_done  -= target_fps; if (frames_done  < 0) frames_done  = 0;
				frames_shown -= target_fps; if (frames_shown < 0) frames_shown = 0;
				if (frames_shown > frames_done) frames_shown = frames_done;
			}
		}

		lim_time = (frames_done+1) * target_frametime + vsync_offset;
		if(currentConfig.Frameskip >= 0) { // frameskip enabled
			for(i = 0; i < currentConfig.Frameskip; i++) {
				updateKeys();
				SkipFrame(1); frames_done++;
				if (PsndOut && !reset_timing) { // do framelimitting if sound is enabled
					gettimeofday(&tval, 0);
					if(thissec != tval.tv_sec) tval.tv_usec+=1000000;
					if(tval.tv_usec < lim_time) { // we are too fast
						simpleWait(thissec, lim_time);
					}
				}
				lim_time += target_frametime;
			}
		} else if(tval.tv_usec > lim_time) { // auto frameskip
			// no time left for this frame - skip
			if (tval.tv_usec - lim_time >= 300000) {
				/* something caused a slowdown for us (disk access? cache flush?)
				 * try to recover by resetting timing... */
				reset_timing = 1;
				continue;
			}
			updateKeys();
			SkipFrame(tval.tv_usec < lim_time+target_frametime*2); frames_done++;
			continue;
		}

		updateKeys();
		PicoFrame();

		// check time
		gettimeofday(&tval, 0);
		if (thissec != tval.tv_sec) tval.tv_usec+=1000000;

		if (currentConfig.Frameskip < 0 && tval.tv_usec - lim_time >= 300000) // slowdown detection
			reset_timing = 1;
		else if (PsndOut != NULL || currentConfig.Frameskip < 0)
		{
			// sleep or vsync if we are still too fast
			// usleep sleeps for ~20ms minimum, so it is not a solution here
			if (!reset_timing && tval.tv_usec < lim_time)
			{
				// we are too fast
				simpleWait(thissec, lim_time);
			}
		}

		blit(fpsbuff, notice);

		frames_done++; frames_shown++;
	}

	sdl_stop_sound();
	change_fast_forward(0);

	if (PicoMCD & 1) PicoCDBufferFree();

	// save SRAM
	if((currentConfig.EmuOpt & 1) && SRam.changed) {
		emu_state_cb("Writing SRAM/BRAM..");
		emu_SaveLoadGame(0, 1);
		SRam.changed = 0;
	}

	// if in 8bit mode, generate 16bit image for menu background
	if ((PicoOpt&0x10) || !(currentConfig.EmuOpt&0x80))
		emu_forcedFrame();
}

void emu_ResetGame(void)
{
	PicoReset(0);
	reset_timing = 1;
}
