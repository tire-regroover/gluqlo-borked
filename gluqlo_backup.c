/* Backup of original gluqlo.c before adding new visual events */

#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <time.h>
#include <math.h>

#include "SDL.h"
#include "SDL_ttf.h"
#include "SDL_syswm.h"
#include "SDL_gfxPrimitives.h"
#include "SDL_rotozoom.h"

#ifndef FONT
#define FONT "/usr/share/gluqlo/gluqlo.ttf"
#endif

const char* TITLE = "Gluqlo 1.1";
const int DEFAULT_WIDTH = 1024;
const int DEFAULT_HEIGHT = 768;

bool twentyfourh = true;
bool leadingzero = false;
bool fullscreen = false;
bool animate = true;
bool anykeyclose = false;

int past_h = -1, past_m = -1, past_yday = -1;
int burnin_x = 0, burnin_y = 0;
int date_y = 0;
static SDL_Surface *date_surface_cache;
static int date_surface_yday = -1;

int width = DEFAULT_WIDTH;
int height = DEFAULT_HEIGHT;
static const Uint32 AMBIENT_FRAME_INTERVAL = 100;
static const int MAX_RAIN_DROPS = 1200;
static bool profile_rendering = false;

TTF_Font *font_time = NULL;
TTF_Font *font_mode = NULL;
TTF_Font *font_date = NULL;

const SDL_Color FONT_COLOR = { 0xda, 0xda, 0xe2 };
const SDL_Color DATE_COLOR = { 0xc6, 0xc2, 0xcf };
const SDL_Color BACKGROUND_COLOR = { 0x12, 0x14, 0x1d };
const SDL_Color CARD_BORDER_COLOR = { 0x37, 0x3d, 0x4e };

SDL_Surface *screen;

SDL_Rect hourBackground;
SDL_Rect minBackground;

SDL_Rect bgrect;
SDL_Surface *bg;
static SDL_Surface *backdrop_base;

// ... [rest of original file content] ...