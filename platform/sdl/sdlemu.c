/*
	sdlemu.c - based on platform/linux/gp2x.c

	(c) Copyright 2006 notaz, All rights reserved.
	Free for non-commercial use.

	For commercial use, separate licencing terms must be obtained.

*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <SDL/SDL.h>

#include <Pico/PicoInt.h>

#include "../common/emu.h"

#include "sdlemu.h"
#include "scaler.h"

void *sdl_screen; // working buffer 320*230*2 + 320*2
SDL_Surface *screen;
SDL_Surface *menu_screen;
static int current_bpp = 8;
int current_pal[256*256];

void sdl_video_flip_320(void);
void sdl_video_flip_400(void);
void sdl_video_flip_480(void);

void (*sdl_video_flip_p)(void) = sdl_video_flip_320;   

void sdl_init(void)
{
	printf("Initializing SDL... ");
	if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO) < 0) {
		printf("ERROR: %s.\n", SDL_GetError());
		return;
	}
	printf("Ok.\n");

	{
		int i = 0; // 0 - 320x240, 1 - 400x240, 2 - 480x272
		int surfacewidth, surfaceheight;
		#define NUMOFVIDEOMODES 3
		struct {
			int x;
			int y;
			void (*p)(void);
		} vm[NUMOFVIDEOMODES] = {
			{320, 240, sdl_video_flip_320},
			{400, 240, sdl_video_flip_400},
			{480, 272, sdl_video_flip_480}
		};

		// check 3 videomodes: 480x272, 400x240, 320x240
		for(i = NUMOFVIDEOMODES-1; i >= 0; i--) {
			if(SDL_VideoModeOK(vm[i].x, vm[i].y, 16, SDL_HWSURFACE) != 0) {
				surfacewidth = vm[i].x;
				surfaceheight = vm[i].y;
				sdl_video_flip_p = vm[i].p;
				break;
			}
		}
		screen = SDL_SetVideoMode(surfacewidth, surfaceheight, 16, SDL_HWSURFACE);
	}
	SDL_ShowCursor(0);

	sdl_screen = malloc(320*240*2 + 320*2);
	memset(sdl_screen, 0, 320*240*2 + 320*2);

	menu_screen = SDL_CreateRGBSurfaceFrom(sdl_screen, 320, 240, 16, 640, 0xF800, 0x7E0, 0x1F, 0);
}

void sdl_deinit(void)
{
	SDL_FreeSurface(screen);
	SDL_Quit();
}

/* video */
void sdl_video_flip_320(void)
{
	int i;
	unsigned int *fbp = (unsigned int *)screen->pixels;

	if (current_bpp == 8) {
		unsigned short *pixels = sdl_screen;

		for (i = 320*240/2; i--;) {
			*fbp++ = current_pal[*pixels++];
		}
	} else {
		unsigned int *pixels = sdl_screen;

		for (i = 320*240/2; i--;) {
			*fbp++ = *pixels++;
		}
	}

}

void sdl_video_flip_400(void)
{
	int i, j;

	if (current_bpp == 8)
	{
		if(currentConfig.scaling == 1) upscale_320x224x8_to_400x240((uint32_t *)screen->pixels, (uint8_t *)sdl_screen); else
		{
			unsigned int *fbp = (unsigned int *)screen->pixels + (240 - 240) / 4 * 400 + (400 - 320) / 4;
			unsigned short *pixels = sdl_screen;

			for(i = 240; i--;) {
				for (j = 320/2; j--;) {
					*fbp++ = current_pal[*pixels++];
				}
				fbp += (400 - 320) / 2;
			}
		}
	}
	else
	{
		if(currentConfig.scaling == 1) upscale_320x224x16_to_400x240((uint32_t *)screen->pixels, (uint32_t *)sdl_screen); else
		{
			unsigned int *fbp = (unsigned int *)screen->pixels + (240 - 240) / 4 * 400 + (400 - 320) / 4;
			unsigned int *pixels = sdl_screen;

			for(i = 240; i--;) {
				for (j = 320/2; j--;) {
					*fbp++ = *pixels++;
				}
				fbp += (400 - 320) / 2;
			}
		}
	}
}

void sdl_video_flip_480(void)
{
	int i, j;

	if (current_bpp == 8)
	{
		if(currentConfig.scaling == 1) upscale_320x224x8_to_480x272((uint32_t *)screen->pixels, (uint8_t *)sdl_screen); else
		{
			unsigned int *fbp = (unsigned int *)screen->pixels + (272 - 240) / 4 * 480 + (480 - 320) / 4;
			unsigned short *pixels = sdl_screen;

			for(i = 240; i--;) {
				for (j = 320/2; j--;) {
					*fbp++ = current_pal[*pixels++];
				}
				fbp += (480 - 320) / 2;
			}
		}
	}
	else
	{
		if(currentConfig.scaling == 1) upscale_320x224x16_to_480x272((uint32_t *)screen->pixels, (uint32_t *)sdl_screen); else
		{
			unsigned int *fbp = (unsigned int *)screen->pixels + (272 - 240) / 4 * 480 + (480 - 320) / 4;
			unsigned int *pixels = sdl_screen;

			for(i = 240; i--;) {
				for (j = 320/2; j--;) {
					*fbp++ = *pixels++;
				}
				fbp += (480 - 320) / 2;
			}
		}
	}
}

void sdl_video_flip(void)
{
	SDL_LockSurface(screen);
	(*sdl_video_flip_p)();
	SDL_UnlockSurface(screen);
	SDL_Flip(screen);
}

void sdl_menu_flip(void)
{
	SDL_Rect dst;

	dst.x = (screen->w - 320) / 2;
	dst.y = (screen->h - 240) / 2;

	SDL_BlitSurface(menu_screen, 0, screen, &dst);
	SDL_Flip(screen);
}

void sdl_clear_screen(void)
{
	SDL_FillRect(screen, NULL, 0);
}

void sdl_video_changemode(int bpp)
{
	current_bpp = bpp;
	printf("BPP: %i\n", bpp);
}

void sdl_video_setpalette(int *pal, int len)
{
	int i, j;

	for(i = 0; i < 256; i++)
		for(j = 0; j < 256; j++) {
			current_pal[i*256+j] = (pal[j] & 0xFFFF) | (pal[i] << 16);
		}
}

void sdl_video_RGB_setscaling(int v_offs, int W, int H)
{
}

void sdl_memcpy_buffers(int buffers, void *data, int offset, int len)
{
	if ((char *)sdl_screen + offset != data)
		memcpy((char *)sdl_screen + offset, data, len);
}

void sdl_memcpy_all_buffers(void *data, int offset, int len)
{
	memcpy((char *)sdl_screen + offset, data, len);
}


void sdl_memset_all_buffers(int offset, int byte, int len)
{
	memset((char *)sdl_screen + offset, byte, len);
}

void sdl_pd_clone_buffer2(void)
{
	memset(sdl_screen, 0, 320*240*2);
}

/* sound */
#define BUFFER_SIZE (2*2*44100/50)
#define SOUND_BUFFER_COUNT 4
static char sound_buffer[SOUND_BUFFER_COUNT][BUFFER_SIZE];
static unsigned int buf_r, buf_w, samples_count;

static int s_oldrate = 0, s_oldbits = 0, s_oldstereo = 0;
static int s_initialized = 0;

void sdl_sound_callback(void *userdata, Uint8 *stream, int len)
{
	memcpy(stream, sound_buffer[buf_r], len);
	if (++buf_r >= SOUND_BUFFER_COUNT)
		buf_r = 0;
}

void sdl_sound_volume(int l, int r)
{
}

void sdl_stop_sound()
{
	SDL_PauseAudio(1);
	SDL_CloseAudio();
	s_initialized = 0;
}

void sdl_start_sound(int rate, int bits, int stereo)
{
	SDL_AudioSpec as_desired, as_obtained;
	int target_fps = Pico.m.pal ? 50 : 60;

	if(s_initialized) sdl_stop_sound();

	as_desired.freq = rate;
	as_desired.format = AUDIO_S16; // `bits` is always 16
	as_desired.channels = stereo + 1;

	as_desired.samples = (rate / target_fps);
	if (!stereo && as_desired.samples & 1)
		as_desired.samples--;

	samples_count = as_desired.samples * as_desired.channels * 2;
	as_desired.callback = sdl_sound_callback;

	if(SDL_OpenAudio(&as_desired, &as_obtained) == -1) {
		printf("ERROR: can't open audio: %s.\n", SDL_GetError());
		return;
	}

	memset(sound_buffer, 0, sizeof(sound_buffer));
	SDL_PauseAudio(0);
	s_initialized = 1; s_oldrate = rate; s_oldbits = bits; s_oldstereo = stereo;
}

void sdl_sound_write(void *buff, int len)
{
	SDL_LockAudio();

	memcpy(sound_buffer[buf_w], buff, samples_count);

	if (buf_w != buf_r && ++buf_w == SOUND_BUFFER_COUNT)
		buf_w = 0;

	SDL_UnlockAudio();
}

/* joystick emulation */
#define SETKEY(KEY, BUT) \
{ \
	if(event.key.keysym.sym == (KEY)) { \
		if(event.type == SDL_KEYUP) {\
			button_states &= ~(BUT);\
		} else if (event.type == SDL_KEYDOWN) {\
			button_states &= ~(BUT);\
			button_states |= (BUT);\
		} \
	}\
}

static unsigned long button_states = 0;
unsigned long sdl_joystick_read(int allow_usb_joy)
{
	SDL_Event event;

	if(SDL_PollEvent(&event)) {
		if(event.type == SDL_KEYUP || event.type == SDL_KEYDOWN) {
			SETKEY(SDLK_UP, GP2X_UP);
			SETKEY(SDLK_DOWN, GP2X_DOWN);
			SETKEY(SDLK_LEFT, GP2X_LEFT);
			SETKEY(SDLK_RIGHT, GP2X_RIGHT);
			SETKEY(SDLK_LCTRL, GP2X_B); // dingoo A
			SETKEY(SDLK_LALT, GP2X_X); // dingoo B
			SETKEY(SDLK_SPACE, GP2X_Y); // dingoo X
			SETKEY(SDLK_LSHIFT, GP2X_A); // dingoo Y
			SETKEY(SDLK_TAB, GP2X_L);
			SETKEY(SDLK_BACKSPACE, GP2X_R);
			SETKEY(SDLK_ESCAPE, GP2X_SELECT);
			SETKEY(SDLK_RETURN, GP2X_START);
		}
	}

	return button_states;
}


/* misc */
void spend_cycles(int c)
{
	usleep(c/200);
}

/* lprintf stub */
void lprintf(const char *fmt, ...)
{
	va_list vl;

	va_start(vl, fmt);
	vprintf(fmt, vl);
	va_end(vl);
}
