/*
* Gluqlo: Fliqlo for Linux
* https://github.com/alexanderk23/gluqlo
*
* Copyright (c) 2010-2012 Kuźniarski Jacek
* Copyright (c) 2014 Alexander Kovalenko
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
* WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
* MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
* ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
* WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
* ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
* OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

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
int queued_minute = -1;
int burnin_x = 0, burnin_y = 0;
int date_y = 0;
static SDL_Surface *date_surface_cache;
static int date_surface_yday = -1;

int width = DEFAULT_WIDTH;
int height = DEFAULT_HEIGHT;

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

// draw rounded box
// see http://lists.libsdl.org/pipermail/sdl-libsdl.org/2006-December/058868.html
void fill_rounded_box_b(SDL_Surface* dst, SDL_Rect *coords, int r, SDL_Color color, Uint8 alpha) {
	Uint32 pixcolor = SDL_MapRGBA(dst->format, color.r, color.g, color.b, alpha);

	int i, j;
	int rpsqrt2 = (int) (r / sqrt(2));
	int yd = dst->pitch / dst->format->BytesPerPixel;
	int w = coords->w / 2 - 1;
	int h = coords->h / 2 - 1;
	int xo = coords->x + w;
	int yo = coords->y + h;

	w -= r;
	h -= r;

	if(w <= 0 || h <= 0) return;

	SDL_LockSurface(dst);
	Uint32 *pixels = (Uint32*)(dst->pixels);

	int sy = (yo - h) * yd;
	int ey = (yo + h) * yd;
	int sx = xo - w;
	int ex = xo + w;

	for(i = sy; i <= ey; i += yd)
		for(j = sx - r; j <= ex + r; j++)
			pixels[i + j] = pixcolor;

	int d = -r;
	int x2m1 = -1;
	int y = r;

	for(int x = 0; x <= rpsqrt2; x++) {
		x2m1 += 2;
		d += x2m1;
		if(d >= 0) {
			y--;
			d -= y * 2;
		}

		for(i = sx - x; i <= ex + x; i++) {
			pixels[sy - y * yd + i] = pixcolor;
		}

		for(i = sx - y; i <= ex + y; i++) {
			pixels[sy - x * yd + i] = pixcolor;
		}

		for(i = sx - y; i <= ex + y; i++) {
			pixels[ey + x * yd + i] = pixcolor;
		}

		for(i = sx - x; i <= ex + x; i++) {
			pixels[ey + y * yd + i] = pixcolor;
		}
	}

	SDL_UnlockSurface(dst);
}

void render_ampm(SDL_Surface *surface, SDL_Rect *rect, int pm) {
	char mode[3];
	SDL_Rect coords;
	snprintf(mode, 3, "%cM", pm ? 'P' : 'A');
	SDL_Surface *ampm = TTF_RenderText_Blended(font_mode, mode, FONT_COLOR);
	int offset = rect->h * 0.127;
	coords.x = rect->x + rect->h * 0.07;
	coords.y = rect->y + (pm ? rect->h - offset - ampm->h : offset);
	SDL_BlitSurface(ampm, 0, surface, &coords);
	SDL_FreeSurface(ampm);
}

static double hash_noise(int x, int y, Uint32 seed) {
	Uint32 value = (Uint32)x * 374761393u + (Uint32)y * 668265263u + seed * 2246822519u;
	value = (value ^ (value >> 13)) * 1274126177u;
	value ^= value >> 16;
	return value / 4294967295.0;
}

static double smooth_noise(double x, double y, Uint32 seed) {
	int x0 = (int)floor(x);
	int y0 = (int)floor(y);
	double fx = x - x0;
	double fy = y - y0;
	fx = fx * fx * (3.0 - 2.0 * fx);
	fy = fy * fy * (3.0 - 2.0 * fy);
	double top = hash_noise(x0, y0, seed) * (1.0 - fx)
		+ hash_noise(x0 + 1, y0, seed) * fx;
	double bottom = hash_noise(x0, y0 + 1, seed) * (1.0 - fx)
		+ hash_noise(x0 + 1, y0 + 1, seed) * fx;
	return top * (1.0 - fy) + bottom * fy;
}

static Uint8 blend_channel(Uint8 from, Uint8 to, double amount) {
	if(amount < 0.0) amount = 0.0;
	if(amount > 1.0) amount = 1.0;
	return (Uint8)(from + (to - from) * amount);
}

static double smooth_step(double edge0, double edge1, double value) {
	double t = (value - edge0) / (edge1 - edge0);
	if(t < 0.0) t = 0.0;
	if(t > 1.0) t = 1.0;
	return t * t * (3.0 - 2.0 * t);
}

typedef struct {
	double season[4];
	double cloud;
	double rain;
	double snow;
	double sunshine;
} SceneWeather;

static SceneWeather get_scene_weather(Uint32 ticks) {
	static const double weather_levels[][3] = {
		{ 0.10, 0.0, 0.95 }, { 0.58, 0.0, 0.12 }, { 0.86, 0.72, 0.0 },
		{ 1.0, 1.0, 0.0 }, { 0.64, 0.0, 0.28 }, { 0.12, 0.0, 0.78 }
	};
	const Uint32 season_duration = 300000;
	const Uint32 season_transition = 20000;
	const Uint32 weather_duration = 90000;
	const Uint32 weather_transition = 14000;
	SceneWeather scene = { { 0.0, 0.0, 0.0, 0.0 }, 0.0, 0.0, 0.0, 0.0 };

	Uint32 season_elapsed = ticks % (season_duration * 4);
	int season = season_elapsed / season_duration;
	Uint32 season_phase = season_elapsed % season_duration;
	double season_blend = smooth_step(season_duration - season_transition,
		season_duration, season_phase);
	scene.season[season] = 1.0 - season_blend;
	scene.season[(season + 1) % 4] = season_blend;

	Uint32 weather_elapsed = ticks % (weather_duration * 6);
	int current = weather_elapsed / weather_duration;
	int previous = (current + 5) % 6;
	Uint32 weather_phase = weather_elapsed % weather_duration;
	double weather_blend = smooth_step(0.0, weather_transition, weather_phase);
	scene.cloud = weather_levels[previous][0]
		+ (weather_levels[current][0] - weather_levels[previous][0]) * weather_blend;
	double precipitation = weather_levels[previous][1]
		+ (weather_levels[current][1] - weather_levels[previous][1]) * weather_blend;
	scene.sunshine = weather_levels[previous][2]
		+ (weather_levels[current][2] - weather_levels[previous][2]) * weather_blend;
	double winter = scene.season[3];
	scene.snow = precipitation * winter;
	scene.rain = precipitation * (1.0 - winter);
	return scene;
}

static void draw_bird(SDL_Surface *surface, int x, int y, int size, int wing_lift) {
	const Uint8 shade = 23, alpha = 185;
	int body_length = size;
	int wing_span = size;
	if(wing_lift < 1) wing_lift = 1;

	thickLineRGBA(surface, x - body_length / 2, y, x + body_length / 2, y,
		2, shade, 30, 43, alpha);
	filledCircleRGBA(surface, x + body_length / 2, y - 1, 1, shade, 30, 43, alpha);
	thickLineRGBA(surface, x + body_length / 2 + 1, y - 1,
		x + body_length / 2 + 3, y, 1, shade, 30, 43, alpha);
	thickLineRGBA(surface, x - body_length / 2, y,
		x - body_length / 2 - size / 2, y - size / 3, 1, shade, 30, 43, alpha);
	thickLineRGBA(surface, x - body_length / 2, y,
		x - body_length / 2 - size / 2, y + size / 3, 1, shade, 30, 43, alpha);

	int shoulder_x = x - size / 5;
	int far_tip_x = x - wing_span;
	int near_tip_x = x - wing_span * 3 / 4;
	int far_tip_y = y - wing_lift;
	int near_tip_y = y + wing_lift * 3 / 4;
	thickLineRGBA(surface, shoulder_x, y, x - size / 2, y - wing_lift / 2,
		2, shade, 30, 43, alpha);
	thickLineRGBA(surface, x - size / 2, y - wing_lift / 2,
		far_tip_x, far_tip_y, 2, shade, 30, 43, alpha);
	thickLineRGBA(surface, shoulder_x, y, x - size / 2, y + wing_lift / 3,
		2, shade, 30, 43, alpha);
	thickLineRGBA(surface, x - size / 2, y + wing_lift / 3,
		near_tip_x, near_tip_y, 2, shade, 30, 43, alpha);
}

static void draw_battle_ship(SDL_Surface *surface, int x, int y, int size,
	int direction, Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha) {
	int nose_x = x + direction * size;
	int back_x = x - direction * size;
	int wing_x = x - direction * size / 3;
	int wing_top = y - size / 2;
	int wing_bottom = y + size / 2;

	filledTrigonRGBA(surface, nose_x, y, wing_x, wing_top, back_x, y,
		red, green, blue, alpha);
	filledTrigonRGBA(surface, nose_x, y, back_x, y, wing_x, wing_bottom,
		red, green, blue, alpha);
	thickLineRGBA(surface, nose_x, y, back_x, y, 2, 191, 212, 231, alpha);
	thickLineRGBA(surface, wing_x, wing_top, x, y, 1, red, green, blue, alpha);
	thickLineRGBA(surface, x, y, wing_x, wing_bottom, 1, red, green, blue, alpha);
	filledCircleRGBA(surface, back_x, y, size / 7, 111, 201, 232,
		(Uint8)(alpha * 3 / 4));
}

static void draw_frigate(SDL_Surface *surface, int x, int y, int size,
	int direction, Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha) {
	int nose_x = x + direction * size * 3;
	int tail_x = x - direction * size * 3;
	int shoulder_x = x - direction * size;
	int top_y = y - size / 2;
	int bottom_y = y + size / 2;
	int half_x[] = { nose_x, shoulder_x, tail_x, x, nose_x };
	int upper_y[] = { y, top_y, y - size / 3, y, y };
	int lower_y[] = { y, y, y + size / 3, bottom_y, y };

	filledTrigonRGBA(surface, half_x[0], upper_y[0], half_x[1], upper_y[1],
		half_x[2], upper_y[2], red, green, blue, alpha);
	filledTrigonRGBA(surface, half_x[0], lower_y[0], half_x[2], lower_y[2],
		half_x[3], lower_y[3], red, green, blue, alpha);
	thickLineRGBA(surface, nose_x, y, tail_x, y, 2, 191, 212, 231, alpha);

	for(int turret = -1; turret <= 1; turret += 2) {
		int turret_x = x + turret * size / 2;
		int turret_y = y + turret * size / 5;
		filledCircleRGBA(surface, turret_x, turret_y, size / 7,
			(Uint8)(red + 18), (Uint8)(green + 18), (Uint8)(blue + 18), alpha);
		thickLineRGBA(surface, turret_x, turret_y,
			turret_x + direction * size / 2, turret_y - turret * size / 5,
			1, red, green, blue, alpha);
	}
	filledCircleRGBA(surface, tail_x, y, size / 6, 111, 201, 232,
		(Uint8)(alpha * 3 / 4));
}

static void render_fullscreen_event(SDL_Surface *surface, Uint32 ticks, int scale) {
	const Uint32 cycle = 90000;
	const Uint32 duration = 8500;
	Uint32 slot = ticks / cycle;
	Uint32 phase = ticks % cycle;
	Uint32 seed = slot * 2654435761u + 0x9e3779b9u;
	if(phase >= duration || (seed >> 5) % 4 >= 2) return;

	double fade = smooth_step(0.0, 1300.0, phase)
		* (1.0 - smooth_step(duration - 1200.0, duration, phase));
	if((seed >> 5) % 4 == 0) {
		boxRGBA(surface, 0, 0, surface->w - 1, surface->h - 1,
			62, 94, 132, (Uint8)(10 * fade));
		int segments = 20;
		for(int band = 0; band < 3; band++) {
			int previous_x = 0;
			int previous_y = surface->h * (18 + band * 12) / 100;
			for(int i = 1; i <= segments; i++) {
				int x = surface->w * i / segments;
				double nx = (double)x / surface->w;
				int y = surface->h * (18 + band * 12) / 100
					+ (int)(sin(nx * 6.28318530718 + ticks / 900.0 + band * 1.7)
						* scale * (0.025 + band * 0.006));
				Uint8 alpha = (Uint8)((30 - band * 5) * fade);
				thickLineRGBA(surface, previous_x, previous_y, x, y,
					2, (Uint8)(82 + band * 28), (Uint8)(166 + band * 15),
					(Uint8)(167 + band * 24), alpha);
				previous_x = x;
				previous_y = y;
			}
		}
	} else {
		Uint32 flash_phase = phase % 3400;
		double flash = smooth_step(0.0, 90.0, flash_phase)
			* (1.0 - smooth_step(170.0, 430.0, flash_phase));
		Uint8 alpha = (Uint8)(24 * fade * flash);
		if(alpha > 0) {
			boxRGBA(surface, 0, 0, surface->w - 1, surface->h - 1,
				170, 192, 220, alpha);
		}
	}
}

static void render_background_battle(SDL_Surface *surface, Uint32 ticks, int scale) {
	const Uint32 cycle = 60000;
	const Uint32 duration = 6000;
	Uint32 phase = ticks % cycle;
	if(phase >= duration) return;

	int size = scale / 125;
	if(size < 4) size = 4;
	double fade = smooth_step(0.0, 900.0, phase)
		* (1.0 - smooth_step(duration - 900.0, duration, phase));
	double progress = (double)phase / duration;
	int left_x = (int)(-size + progress * (surface->w + 2.0 * size));
	int right_x = surface->w - left_x;
	int center_y = surface->h * 17 / 100;
	int arc = (int)(sin(progress * 6.28318530718) * scale * 0.018);
	int left_y = center_y + arc;
	int right_y = center_y - arc;
	Uint8 alpha = (Uint8)(112 * fade);

	draw_battle_ship(surface, left_x, left_y, size, 1, 103, 189, 219, alpha);
	draw_battle_ship(surface, right_x, right_y, size, -1, 221, 151, 105, alpha);

	if((phase > 1700 && phase < 3000) || (phase > 4400 && phase < 5700)) {
		int bolt_alpha = (int)(alpha * 0.75);
		thickLineRGBA(surface, left_x + size, left_y, right_x - size, right_y,
			1, 136, 204, 239, bolt_alpha);
		thickLineRGBA(surface, right_x - size, right_y + size / 3,
			left_x + size, left_y + size / 3, 1, 244, 164, 116, bolt_alpha);
	}
	if(phase > 3200 && phase < 4100) {
		int impact_x = surface->w / 2;
		int impact_y = (left_y + right_y) / 2;
		int impact_alpha = (int)(alpha * 0.55);
		filledCircleRGBA(surface, impact_x, impact_y, size / 2,
			228, 185, 143, impact_alpha);
		filledCircleRGBA(surface, impact_x, impact_y, size / 5,
			244, 224, 188, impact_alpha);
	}
}

static void render_fleet_battle(SDL_Surface *surface, Uint32 ticks, int scale) {
	const Uint32 cycle = 240000;
	const Uint32 duration = 18000;
	static Uint32 session_seed;
	static bool seed_initialized = false;
	Uint32 phase = ticks % cycle;
	if(phase >= duration) return;

	if(!seed_initialized) {
		session_seed = (Uint32)time(NULL);
		seed_initialized = true;
	}
	Uint32 encounter = ticks / cycle;
	Uint32 seed = encounter * 2654435761u + session_seed + 0x9e3779b9u;
	int winner = (seed ^ (seed >> 13) ^ (seed >> 23)) & 1;
	double progress = (double)phase / duration;
	double fade = smooth_step(0.0, 1400.0, phase)
		* (1.0 - smooth_step(duration - 1200.0, duration, phase));
	double defeat = smooth_step(0.62, 0.91, progress);
	int size = scale / 42;
	if(size < 10) size = 10;
	int fighter_size = size / 4;
	if(fighter_size < 3) fighter_size = 3;
	int left_x = (int)(-size * 4 + progress * (surface->w + size * 8));
	int right_x = surface->w - left_x;
	int center_y = surface->h * 22 / 100;
	int arc = (int)(sin(progress * 6.28318530718) * scale * 0.025);
	Uint8 left_alpha = (Uint8)(105 * fade * (winner == 0 ? 1.0 : 1.0 - defeat));
	Uint8 right_alpha = (Uint8)(105 * fade * (winner == 1 ? 1.0 : 1.0 - defeat));
	Uint8 left_red = 87, left_green = 154, left_blue = 190;
	Uint8 right_red = 195, right_green = 125, right_blue = 87;

	for(int ship = 0; ship < 2; ship++) {
		int offset = (ship == 0 ? -1 : 1) * size * 2;
		int y_offset = (ship == 0 ? -1 : 1) * scale / 32;
		draw_frigate(surface, left_x + offset, center_y + arc + y_offset,
			size, 1, left_red, left_green, left_blue, left_alpha);
		draw_frigate(surface, right_x - offset, center_y - arc - y_offset,
			size, -1, right_red, right_green, right_blue, right_alpha);
	}

	for(int fighter = 0; fighter < 7; fighter++) {
		int column = fighter % 4;
		int row = fighter / 4;
		int spread = size * (3 + column);
		int y_offset = (row == 0 ? -1 : 1) * (scale / 40 + column * scale / 180);
		int weave = (int)(sin(ticks / 170.0 + fighter * 2.4) * fighter_size);
		int side_alpha = (Uint8)(130 * fade);
		draw_battle_ship(surface, left_x + spread, center_y + arc + y_offset + weave,
			fighter_size, 1, left_red, left_green, left_blue,
			winner == 0 ? side_alpha : (Uint8)(side_alpha * (1.0 - defeat)));
		draw_battle_ship(surface, right_x - spread, center_y - arc - y_offset - weave,
			fighter_size, -1, right_red, right_green, right_blue,
			winner == 1 ? side_alpha : (Uint8)(side_alpha * (1.0 - defeat)));
	}

	if((phase % 2200) < 280) {
		int volley_y = center_y + (int)(sin(ticks / 800.0) * scale * 0.04);
		thickLineRGBA(surface, left_x + size * 4, volley_y,
			right_x - size * 4, center_y - arc, 1, 120, 194, 231,
			(Uint8)(85 * fade));
		thickLineRGBA(surface, right_x - size * 4, volley_y + scale / 80,
			left_x + size * 4, center_y + arc, 1, 226, 151, 105,
			(Uint8)(85 * fade));
	}

	if(progress > 0.64 && progress < 0.95) {
		int loser_x = winner == 0 ? right_x : left_x;
		int loser_y = center_y + (winner == 0 ? -arc : arc);
		unsigned int blast_seed = seed ^ 0xa511e9b3u;
		int blast_x = loser_x + (int)(blast_seed % (unsigned int)(size * 8 + 1))
			- size * 4;
		int blast_y = loser_y + (int)((blast_seed >> 8) % (unsigned int)(scale / 12 + 1))
			- scale / 24;
		Uint8 blast_alpha = (Uint8)(130 * fade * (1.0 - defeat * 0.35));
		filledCircleRGBA(surface, blast_x, blast_y, size / 2,
			221, 155, 110, blast_alpha);
		filledCircleRGBA(surface, blast_x, blast_y, size / 4,
			244, 220, 178, blast_alpha);
	}
}

static void render_rare_sky_event(SDL_Surface *surface, Uint32 ticks, int scale) {
	const Uint32 event_cycle = 48000;
	const Uint32 event_duration = 3000;
	Uint32 slot = ticks / event_cycle;
	Uint32 phase = ticks % event_cycle;
	Uint32 seed = slot * 2246822519u + 3266489917u;
	if(phase >= event_duration || seed % 3 != 0) return;

	seed ^= seed >> 15;
	seed *= 2246822519u;
	seed ^= seed >> 13;
	double progress = (double)phase / event_duration;
	double fade = smooth_step(0.0, 350.0, phase)
		* (1.0 - smooth_step(event_duration - 400.0, event_duration, phase));
	int direction = (seed & 1) ? 1 : -1;
	int y = surface->h * (8 + (seed >> 8) % 39) / 100;
	int margin = scale / 12;
	int x = direction > 0
		? -margin + (int)(progress * (surface->w + 2.0 * margin))
		: surface->w + margin - (int)(progress * (surface->w + 2.0 * margin));
	int size = scale / (26 + (seed >> 16) % 24);
	if(size < 4) size = 4;
	Uint8 alpha = (Uint8)(150 * fade);
	int kind = (seed >> 24) % 4;

	switch(kind) {
		case 0: {
			int panel = size * 2;
			thickLineRGBA(surface, x - direction * panel, y,
				x + direction * panel, y, 2, 174, 193, 205, alpha);
			filledCircleRGBA(surface, x, y, size / 3, 224, 225, 214, alpha);
			boxRGBA(surface, x - direction * panel - size / 2, y - size / 2,
				x - direction * panel + size / 2, y + size / 2,
				104, 151, 185, alpha);
			boxRGBA(surface, x + direction * panel - size / 2, y - size / 2,
				x + direction * panel + size / 2, y + size / 2,
				104, 151, 185, alpha);
			break;
		}
		case 1:
			draw_battle_ship(surface, x, y, size, direction,
				181, 190, 198, alpha);
			thickLineRGBA(surface, x - direction * size, y,
				x - direction * size * 3, y + size / 3,
				1, 161, 190, 218, (Uint8)(alpha * 0.55));
			break;
		case 2: {
			int tail_x = x - direction * size * 4;
			int tail_y = y - size;
			thickLineRGBA(surface, x, y, tail_x, tail_y,
				2, 194, 205, 218, alpha);
			filledCircleRGBA(surface, x, y, size / 2,
				227, 224, 211, alpha);
			break;
		}
		default: {
			int trail_x = x - direction * size * 4;
			int trail_y = y + size;
			thickLineRGBA(surface, trail_x, trail_y, x, y,
				2, 202, 177, 130, alpha);
			filledCircleRGBA(surface, x, y, size / 2,
				235, 221, 185, alpha);
			for(int fragment = 1; fragment <= 2; fragment++) {
				int fragment_x = x - direction * size * (fragment + 1);
				int fragment_y = y + size * fragment;
				pixelRGBA(surface, fragment_x, fragment_y,
					214, 193, 159, (Uint8)(alpha * 0.75));
			}
			break;
		}
	}
}

static void render_foreground_battle(SDL_Surface *surface, Uint32 ticks,
	SDL_Rect *first_card, SDL_Rect *second_card) {
	const Uint32 cycle = 180000;
	const Uint32 duration = 2600;
	Uint32 phase = (ticks + 25000) % cycle;
	if(phase >= duration) return;

	double progress = (double)phase / duration;
	double fade = smooth_step(0.0, 350.0, phase)
		* (1.0 - smooth_step(duration - 350.0, duration, phase));
	int size = first_card->w / 11;
	if(size < 14) size = 14;
	if(size > 46) size = 46;

	int start_x = first_card->x + first_card->w / 2;
	int start_y = first_card->y + first_card->h / 2;
	int end_x = second_card->x + second_card->w / 2;
	int end_y = second_card->y + second_card->h / 2;
	double wave = sin(progress * 6.28318530718) * size * 0.65;
	int x1 = start_x + (int)((end_x - start_x) * progress + wave);
	int y1 = start_y + (int)((end_y - start_y) * progress);
	int x2 = start_x + (int)((end_x - start_x) * (1.0 - progress) - wave);
	int y2 = end_y + (int)((start_y - end_y) * progress);
	Uint8 alpha = (Uint8)(190 * fade);

	SDL_Rect *cards[] = { first_card, second_card };
	for(size_t i = 0; i < sizeof(cards) / sizeof(cards[0]); i++) {
		SDL_SetClipRect(surface, cards[i]);
		draw_battle_ship(surface, x1, y1, size, 1, 102, 190, 225, alpha);
		draw_battle_ship(surface, x2, y2, size * 3 / 4, -1, 231, 160, 112, alpha);
		if(phase > 700 && phase < duration - 500) {
			thickLineRGBA(surface, x1 + size, y1, x2 - size, y2,
				2, 151, 211, 241, (Uint8)(alpha * 0.7));
		}
	}
	SDL_SetClipRect(surface, NULL);
}

void render_backdrop(SDL_Surface *surface, Uint32 ticks) {
	int scale = surface->h < surface->w ? surface->h : surface->w;
	if(scale < 1) scale = 1;
	if(surface->format->BytesPerPixel != 4) {
		fprintf(stderr, "Unsupported screen pixel format: expected 32-bit pixels\n");
		return;
	}
	if(backdrop_base && (backdrop_base->w != surface->w || backdrop_base->h != surface->h)) {
		SDL_FreeSurface(backdrop_base);
		backdrop_base = NULL;
	}

	bool cached = backdrop_base && SDL_BlitSurface(backdrop_base, NULL, surface, NULL) == 0;
	if(backdrop_base && !cached) {
		fprintf(stderr, "Unable to restore backdrop cache: %s\n", SDL_GetError());
	}
	if(!cached) {
		if(SDL_MUSTLOCK(surface) && SDL_LockSurface(surface) < 0) {
			fprintf(stderr, "Unable to lock screen surface: %s\n", SDL_GetError());
			return;
		}

		double drift = ticks * 0.000018;
		for(int y = 0; y < surface->h; y += 4) {
			double ny = (double)(y + 2) / surface->h;
			double sky_blend = ny / 0.68;
			for(int x = 0; x < surface->w; x += 4) {
				double nx = (double)(x + 2) / surface->w;
				double fine = smooth_noise(nx * 25.0 + drift * 0.35, ny * 21.0, 41);
				Uint8 red = blend_channel(5, 35, sky_blend);
				Uint8 green = blend_channel(8, 37, sky_blend);
				Uint8 blue = blend_channel(21, 67, sky_blend);

				if(ny < 0.62) {
					double broad = smooth_noise(nx * 3.2 + drift * 0.25, ny * 3.8, 101);
					double middle = smooth_noise(nx * 8.5 - drift * 0.42, ny * 10.0 + drift * 0.12, 211);
					double detail = smooth_noise(nx * 23.0 + drift * 0.75, ny * 28.0 - drift * 0.2, 307);
					double turbulence = broad * 0.48 + middle * 0.34 + detail * 0.18;
					double band = 0.27 + (smooth_noise(nx * 2.3 + drift * 0.08, 1.4, 419) - 0.5) * 0.24;
					double band_width = 0.24 + broad * 0.06;
					double band_edge = 1.0 - fabs(ny - band) / band_width;
					double nebula = band_edge > 0.0
						? (turbulence - 0.36) * 3.4 * band_edge : 0.0;
					if(nebula > 0.0) {
						double hue = smooth_noise(nx * 4.0 - drift * 0.08, ny * 5.5, 523);
						Uint8 nebula_red = blend_channel(62, 151, hue);
						Uint8 nebula_green = blend_channel(49, 83, hue);
						Uint8 nebula_blue = blend_channel(145, 226, 1.0 - hue * 0.45);
						double light = nebula * (0.24 + 0.30 * turbulence);
						red = blend_channel(red, nebula_red, light);
						green = blend_channel(green, nebula_green, light);
						blue = blend_channel(blue, nebula_blue, light);
					}

					double cloud_mass = smooth_noise(nx * 4.2 + drift * 0.42, ny * 6.0 + drift * 0.08, 631) * 0.62
						+ smooth_noise(nx * 13.0 + drift * 0.7, ny * 19.0 - drift * 0.16, 719) * 0.38;
					double cloud_band = 0.48 + 0.035 * (smooth_noise(nx * 3.0 + drift * 0.14, 2.0, 811) - 0.5);
					double cloud_envelope = 1.0 - fabs(ny - cloud_band) / 0.14;
					double cloud_density = cloud_envelope > 0.0
						? (cloud_mass - 0.50) * 3.2 * cloud_envelope : 0.0;
					if(cloud_density > 0.0) {
						double amount = cloud_density * 0.42;
						red = blend_channel(red, (Uint8)(123 + 32 * fine), amount);
						green = blend_channel(green, (Uint8)(139 + 29 * fine), amount);
						blue = blend_channel(blue, (Uint8)(174 + 34 * fine), amount);
					}
				}

				double far_edge = 0.68 + (smooth_noise(nx * 3.6, 1.0, 907) - 0.5) * 0.13;
				double middle_edge = 0.79 + (smooth_noise(nx * 5.2 + 8.0, 2.0, 1013) - 0.5) * 0.10;
				double near_edge = 0.88 + (smooth_noise(nx * 8.0 - 3.0, 3.0, 1123) - 0.5) * 0.09;
				double ground_noise = smooth_noise(nx * 28.0, ny * 32.0, 1237);
				double far_cover = smooth_step(far_edge - 0.014, far_edge + 0.014, ny);
				red = blend_channel(red, (Uint8)(35 + 17 * ground_noise), far_cover);
				green = blend_channel(green, (Uint8)(44 + 21 * ground_noise), far_cover);
				blue = blend_channel(blue, (Uint8)(60 + 24 * ground_noise), far_cover);
				double middle_cover = smooth_step(middle_edge - 0.012, middle_edge + 0.012, ny);
				red = blend_channel(red, (Uint8)(20 + 10 * ground_noise), middle_cover);
				green = blend_channel(green, (Uint8)(31 + 15 * ground_noise), middle_cover);
				blue = blend_channel(blue, (Uint8)(39 + 17 * ground_noise), middle_cover);
				double near_cover = smooth_step(near_edge - 0.010, near_edge + 0.010, ny);
				red = blend_channel(red, (Uint8)(9 + 8 * ground_noise), near_cover);
				green = blend_channel(green, (Uint8)(17 + 10 * ground_noise), near_cover);
				blue = blend_channel(blue, (Uint8)(25 + 12 * ground_noise), near_cover);

				Uint32 pixel = SDL_MapRGB(surface->format, red, green, blue);
				int block_width = surface->w - x < 4 ? surface->w - x : 4;
				int block_height = surface->h - y < 4 ? surface->h - y : 4;
				for(int row_index = 0; row_index < block_height; row_index++) {
					Uint32 *row = (Uint32 *)((Uint8 *)surface->pixels + (y + row_index) * surface->pitch);
					for(int column = 0; column < block_width; column++) {
						row[x + column] = pixel;
					}
				}
			}
		}
		if(SDL_MUSTLOCK(surface)) SDL_UnlockSurface(surface);

		backdrop_base = SDL_ConvertSurface(surface, surface->format, SDL_SWSURFACE);
		if(!backdrop_base) {
			fprintf(stderr, "Unable to cache backdrop: %s\n", SDL_GetError());
		}
	}

	SceneWeather weather = get_scene_weather(ticks);
	static const Uint8 season_tint[4][3] = {
		{ 40, 68, 43 }, { 72, 77, 43 }, { 92, 48, 23 }, { 40, 56, 83 }
	};
	int tint_red = 0, tint_green = 0, tint_blue = 0;
	for(int i = 0; i < 4; i++) {
		tint_red += season_tint[i][0] * weather.season[i];
		tint_green += season_tint[i][1] * weather.season[i];
		tint_blue += season_tint[i][2] * weather.season[i];
	}
	double cloud_mix = weather.cloud * 0.55;
	tint_red = tint_red * (1.0 - cloud_mix) + 48 * cloud_mix;
	tint_green = tint_green * (1.0 - cloud_mix) + 59 * cloud_mix;
	tint_blue = tint_blue * (1.0 - cloud_mix) + 78 * cloud_mix;
	Uint8 tint_alpha = (Uint8)(15 + weather.cloud * 13 + weather.sunshine * 5);
	boxRGBA(surface, 0, 0, surface->w - 1, surface->h - 1,
		tint_red, tint_green, tint_blue, tint_alpha);
	render_fullscreen_event(surface, ticks, scale);
	if(weather.sunshine > 0.01) {
		int sun_x = surface->w * 3 / 4;
		int sun_y = surface->h * 27 / 100;
		int glow_radius = scale / 11;
		Uint8 glow_alpha = (Uint8)(weather.sunshine * 10);
		filledCircleRGBA(surface, sun_x, sun_y, glow_radius, 211, 171, 112, glow_alpha);
		filledCircleRGBA(surface, sun_x, sun_y, glow_radius / 2, 239, 205, 146,
			(Uint8)(weather.sunshine * 20));
		filledCircleRGBA(surface, sun_x, sun_y, glow_radius / 5, 255, 231, 183,
			(Uint8)(weather.sunshine * 105));
	}

	Uint32 star_seed = 0x4f1bbcdcu;
	int star_count = (int)(surface->w * surface->h / 2800.0 * (1.0 - weather.sunshine * 0.8));
	for(int i = 0; i < star_count; i++) {
		star_seed = star_seed * 1664525u + 1013904223u;
		int sx = star_seed % surface->w;
		star_seed = star_seed * 1664525u + 1013904223u;
		int sy = star_seed % (surface->h * 54 / 100);
		double twinkle = 0.5 + 0.5 * sin(ticks / (700.0 + i % 13 * 91) + i * 1.73);
		Uint8 alpha = (Uint8)(35 + twinkle * 95);
		if(i % 31 == 0) filledCircleRGBA(surface, sx, sy, 3, 126, 158, 226, 13);
		pixelRGBA(surface, sx, sy, 205, 214, 235, alpha);
		if(i % 31 == 0) filledCircleRGBA(surface, sx, sy, 1, 237, 232, 222, alpha);
	}

	static const int bird_group_x[] = { 8, 43, 76 };
	static const int bird_group_y[] = { 29, 43, 34 };
	for(size_t group = 0; group < sizeof(bird_group_x) / sizeof(bird_group_x[0]); group++) {
		int bird_span = scale / 85;
		if(bird_span < 4) bird_span = 4;
		int travel = (int)((Uint64)ticks * surface->w / 90000);
		int group_x = (surface->w * bird_group_x[group] / 100 + travel)
			% (surface->w + bird_span * 5) - bird_span * 2;
		int group_y = surface->h * bird_group_y[group] / 100
			+ (int)(sin(ticks / 1700.0 + group * 2.1) * scale * 0.008);
		static const int formation_x[] = { 0, -3, -6, -9, -12, -15, -18 };
		static const int formation_y[] = { 0, 3, -3, 5, -6, 1, 7 };
		for(int bird = 0; bird < 7; bird++) {
			int bird_x = group_x + formation_x[bird] * bird_span;
			int bird_y = group_y + formation_y[bird] * bird_span;
			int flap = (int)((0.5 + 0.5 * sin(ticks / 260.0 + group * 1.8 + bird)) * (bird_span / 2));
			draw_bird(surface, bird_x, bird_y, bird_span, flap);
		}
	}

	render_background_battle(surface, ticks, scale);
	render_fleet_battle(surface, ticks, scale);
	render_rare_sky_event(surface, ticks, scale);

	for(int i = 0; i < 10; i++) {
		unsigned int mote = (unsigned int)i * 2246822519u + 3266489917u;
		int base_x = (mote % surface->w + ticks / (45 + i % 4 * 13)) % surface->w;
		double nx = (double)base_x / surface->w;
		double ridge = 0.68 + (smooth_noise(nx * 3.6, 1.0, 907) - 0.5) * 0.13;
		int y = surface->h * (ridge + 0.035) + (int)(sin(ticks / (520.0 + i * 47) + i) * scale * 0.018);
		filledCircleRGBA(surface, base_x, y, 1, 200, 206, 164,
			(Uint8)(35 + 30 * (0.5 + 0.5 * sin(ticks / (230.0 + i * 31) + i))));
	}

	int autumn = weather.season[2] * 16;
	for(int i = 0; i < autumn; i++) {
		unsigned int seed = (unsigned int)i * 2246822519u + 3266489917u;
		int x = (seed + ticks / (32 + i % 5 * 9)) % surface->w;
		int y = (seed / 11 + ticks * (2 + i % 3) / 55) % surface->h;
		int sway = (int)(sin(ticks / 370.0 + i * 1.7) * scale * 0.012);
		Uint8 alpha = (Uint8)(55 + 90 * weather.season[2]);
		thickLineRGBA(surface, x, y, x + sway + 2, y + 3,
			2, (Uint8)(154 + i % 3 * 20), (Uint8)(76 + i % 4 * 13), 43, alpha);
	}

	Uint32 meteor_period = ticks % 19000;
	if(meteor_period < 800) {
		double progress = meteor_period / 800.0;
		double fade = 1.0 - progress;
		unsigned int event_seed = ticks / 19000 * 2654435761u + 0x9e3779b9u;
		int meteor_x = (int)(surface->w * (0.16 + (event_seed % 58) / 100.0)
			+ progress * surface->w * 0.20);
		int meteor_y = (int)(surface->h * (0.07 + (event_seed >> 8) % 25 / 100.0)
			+ progress * surface->h * 0.12);
		Uint8 alpha = (Uint8)(130 * fade);
		thickLineRGBA(surface, meteor_x, meteor_y, meteor_x - scale / 34,
			meteor_y - scale / 52, 2, 211, 221, 246, alpha);
		filledCircleRGBA(surface, meteor_x, meteor_y, 2, 241, 238, 228, alpha);
	}

	unsigned int rain_seed = 0x7f4a7c15u;
	int rain_count = (int)(surface->w * surface->h / 1700.0 * weather.rain);
	for(int i = 0; i < rain_count; i++) {
		rain_seed = rain_seed * 1664525u + 1013904223u;
		int start_x = rain_seed % surface->w;
		rain_seed = rain_seed * 1664525u + 1013904223u;
		int start_y = rain_seed % surface->h;
		int length = 5 + (rain_seed >> 24) % 10;
		int speed = 5 + i % 8;
		double travel = ticks * speed / 150.0;
		int y = (start_y + (int)travel) % (surface->h + length);
		int x = (start_x + (int)(travel * 0.16)) % surface->w;
		int x_end = x - length / 3;
		int y_start = y - length;
		Uint8 alpha = (Uint8)(38 + (rain_seed >> 16) % 48);
		thickLineRGBA(surface, x, y, x_end, y_start, 1,
			154, 174, 207, alpha);
	}

	int snow_count = (int)(surface->w * surface->h / 3400.0 * weather.snow);
	for(int i = 0; i < snow_count; i++) {
		unsigned int seed = (unsigned int)i * 1664525u + 1013904223u;
		int x = (seed + ticks / (12 + i % 7 * 4)) % surface->w;
		int y = (seed / 13 + ticks / (18 + i % 5 * 7)) % surface->h;
		filledCircleRGBA(surface, x, y, i % 9 == 0 ? 2 : 1,
			202, 216, 231, (Uint8)(85 + weather.snow * 80));
	}
}



void blit_digits(SDL_Surface *surface, SDL_Rect *rect, int spc, char digits[], SDL_Color color) {
	int min_x, max_x, min_y, max_y, advance;
	int adjust_x = (digits[0] == '1') ? 2.5 * spc : 0; // special case
	int center_x = rect->x + rect->w / 2 - adjust_x;

	SDL_Surface *glyph;
	SDL_Rect coords;

	if(digits[1]) {
		// first digit
		TTF_GlyphMetrics(font_time, digits[0], &min_x, &max_x, &min_y, &max_y, &advance);
		glyph = TTF_RenderGlyph_Blended(font_time, digits[0], color);
		coords.x = center_x - max_x + min_x - spc - (adjust_x ? spc : 0);
		coords.y = rect->y + (rect->h - glyph->h) / 2;
		SDL_BlitSurface(glyph, 0, surface, &coords);
		SDL_FreeSurface(glyph);
		// second digit
		TTF_GlyphMetrics(font_time, digits[1], &min_x, &max_x, &min_y, &max_y, &advance);
		glyph = TTF_RenderGlyph_Blended(font_time, digits[1], color);
		coords.y = rect->y + (rect->h - glyph->h) / 2;
		coords.x = center_x + spc / 2;
		SDL_BlitSurface(glyph, 0, surface, &coords);
		SDL_FreeSurface(glyph);
	} else {
		// single digit
		glyph = TTF_RenderGlyph_Blended(font_time, digits[0], color);
		coords.x = center_x - glyph->w / 2;
		coords.y = rect->y + (rect->h - glyph->h) / 2;
		SDL_BlitSurface(glyph, 0, surface, &coords);
		SDL_FreeSurface(glyph);
	}
}


void render_digits(SDL_Surface *surface, SDL_Rect *background, char digits[], char prevdigits[], int maxsteps, int step) {
	SDL_Rect rect, dstrect;
	SDL_Color color;
	double scale;
	double intensity;

	// int spc = surface->h * .0125;
	bool is_h = surface->h < surface->w;
	int spc = is_h ? surface->h * .0125 : surface->w * .0125;

	if(step == maxsteps - 1) {
		rect = *background;
		SDL_SetClipRect(surface, &rect);
		SDL_BlitSurface(bg, NULL, surface, &rect);
		blit_digits(surface, background, spc, digits, FONT_COLOR);
		SDL_SetClipRect(surface, NULL);
		if(!animate) return;

		rect.h = (is_h ? surface->h : surface->w) * 0.005;
		rect.w = background->w;
		rect.x = background->x;
		rect.y = background->y + (background->h - rect.h) / 2;
		SDL_FillRect(surface, &rect, SDL_MapRGB(surface->format, 7, 9, 15));
		rect.y += rect.h;
		rect.h = 1;
		SDL_FillRect(surface, &rect, SDL_MapRGB(surface->format, 0x35, 0x3a, 0x49));
		return;
	}

	// blit upper halves of current digits
	rect.x = background->x;
	rect.y = background->y;
	rect.w = background->w;
	rect.h = background->h/2;
	SDL_SetClipRect(surface, &rect);
	SDL_BlitSurface(bg, 0, surface, &rect);
	blit_digits(surface, background, spc, digits, FONT_COLOR);
	SDL_SetClipRect(surface, NULL);

	int halfsteps = maxsteps / 2;
	int upperhalf = (step+1) <= halfsteps;
	if(upperhalf) {
		scale = 1.0 - (1.0 * step) / (halfsteps - 1);
		intensity = 1.0 - (1.0 * step) / (halfsteps - 1);
	} else {
		scale = ((1.0 * step) - halfsteps + 1) / halfsteps;
		intensity = ((1.0 * step) - halfsteps + 1) / halfsteps;
	}
	color.r = FONT_COLOR.r * intensity;
	color.g = FONT_COLOR.g * intensity;
	color.b = FONT_COLOR.b * intensity;

	// create surface to scale from filled background surface
	SDL_Surface *bgcopy = SDL_ConvertSurface(bg, bg->format, bg->flags);
	rect.x = 0;
	rect.y = 0;
	rect.w = bgcopy->w;
	rect.h = bgcopy->h;
	blit_digits(bgcopy, &rect, spc, upperhalf ? prevdigits : digits, color);

	// scale and blend it to dest
	SDL_Surface *scaled = zoomSurface(bgcopy, 1.0, scale, 1);
	rect.x = 0;
	rect.y = upperhalf ? 0 : scaled->h / 2;
	rect.w = scaled->w;
	rect.h = scaled->h / 2;
	dstrect.x = background->x;
	dstrect.y = background->y + ( upperhalf ? ((background->h - scaled->h) / 2) : background->h / 2);
	dstrect.w = rect.w;
	dstrect.h = rect.h;
	SDL_SetClipRect(surface, &dstrect);
	SDL_BlitSurface(scaled, &rect, surface, &dstrect);
	SDL_SetClipRect(surface, NULL);
	SDL_FreeSurface(scaled);
	SDL_FreeSurface(bgcopy);

	if(!animate) return;
	// draw divider
	rect.h = (is_h ? surface->h : surface->w) * 0.005;
	rect.w = background->w;
	rect.x = background->x;
	rect.y = background->y + (background->h - rect.h) / 2;
	SDL_FillRect(surface, &rect, SDL_MapRGB(surface->format, 7, 9, 15));
	rect.y += rect.h;
	rect.h = 1;
	SDL_FillRect(surface, &rect, SDL_MapRGB(surface->format, 0x35, 0x3a, 0x49));
}

void render_clock(int maxsteps, int step, bool ambient_frame) {
	char buffer[3], buffer2[3];
	struct tm *_time;
	time_t rawtime;
	static const int offset_x[] = { -1, 0, 1, 1, 1, 0, -1, -1 };
	static const int offset_y[] = { 0, -1, -1, 0, 1, 1, 0, -1 };
	int phase, new_burnin_x, new_burnin_y;
	bool moved = false;

	time(&rawtime);
	_time = localtime(&rawtime);
	phase = (rawtime / 300) % 8;
	new_burnin_x = offset_x[phase] * (screen->w / 80);
	new_burnin_y = offset_y[phase] * (screen->h / 80);
	if(new_burnin_x != burnin_x || new_burnin_y != burnin_y) {
		hourBackground.x += new_burnin_x - burnin_x;
		hourBackground.y += new_burnin_y - burnin_y;
		minBackground.x += new_burnin_x - burnin_x;
		minBackground.y += new_burnin_y - burnin_y;
		burnin_x = new_burnin_x;
		burnin_y = new_burnin_y;
		moved = true;
	}
	if(moved || ambient_frame) {
		render_backdrop(screen, SDL_GetTicks());
		past_h = past_m = -1;
	}
	bool redraw = moved || ambient_frame;
	int render_steps = moved ? 20 : maxsteps;
	int render_step = moved ? 19 : step;

	// draw hours
	if(_time->tm_hour != past_h || redraw) {
		int h = twentyfourh ? _time->tm_hour : (_time->tm_hour + 11) % 12 + 1;
		if(leadingzero) {
			snprintf(buffer, 3, "%02d", h);
			snprintf(buffer2, 3, "%02d", redraw ? h : past_h);
		} else {
			snprintf(buffer, 3, "%d", h);
			snprintf(buffer2, 3, "%d", redraw ? h : past_h);
		}
		render_digits(screen, &hourBackground, buffer, buffer2, render_steps, render_step);
		// draw am/pm
		if(!twentyfourh) render_ampm(screen, &hourBackground, _time->tm_hour >= 12);
	}

	// draw minutes
	if(_time->tm_min != past_m || redraw) {
		snprintf(buffer, 3, "%02d", _time->tm_min);
		snprintf(buffer2, 3, "%02d", redraw ? _time->tm_min : past_m);
		render_digits(screen, &minBackground, buffer, buffer2, render_steps, render_step);
	}

	if(_time->tm_yday != date_surface_yday) {
		char date[128];
		strftime(date, sizeof(date), "%A, %B", _time);
		snprintf(date + strlen(date), sizeof(date) - strlen(date), " %d, %d", _time->tm_mday, _time->tm_year + 1900);
		SDL_Surface *new_date_surface = TTF_RenderText_Blended(font_date, date, DATE_COLOR);
		if(!new_date_surface) {
			fprintf(stderr, "Unable to render date: %s\n", TTF_GetError());
		} else {
			if(date_surface_cache) SDL_FreeSurface(date_surface_cache);
			date_surface_cache = new_date_surface;
			date_surface_yday = _time->tm_yday;
		}
	}
	if((redraw || _time->tm_yday != past_yday)
		&& date_surface_cache && date_surface_yday == _time->tm_yday) {
		SDL_Rect date_coords;
		date_coords.x = (screen->w - date_surface_cache->w) / 2 + burnin_x;
		date_coords.y = date_y + burnin_y;
		SDL_BlitSurface(date_surface_cache, NULL, screen, &date_coords);
		past_yday = _time->tm_yday;
	}

	SceneWeather weather = get_scene_weather(SDL_GetTicks());
	SDL_Rect *clock_cards[] = { &hourBackground, &minBackground };
	for(size_t card_index = 0; card_index < sizeof(clock_cards) / sizeof(clock_cards[0]); card_index++) {
		SDL_Rect *card = clock_cards[card_index];
		SDL_SetClipRect(screen, card);
		int drops = (int)(card->w / 42.0 * weather.rain);
		for(int i = 0; i < drops; i++) {
			Uint32 seed = (Uint32)(i + card_index * 97) * 2246822519u + 3266489917u;
			int drop_x = card->x + seed % card->w;
			int length = 4 + (seed >> 24) % 7;
			int speed = 7 + (seed >> 12) % 7;
			int drop_y = card->y
				+ (int)((seed + SDL_GetTicks() * speed / 90) % card->h);
			Uint8 alpha = (Uint8)(24 + (seed >> 18) % 30);
			thickLineRGBA(screen, drop_x, drop_y, drop_x - 1, drop_y - length,
				1, 151, 174, 211, alpha);
		}
		int flakes = (int)(card->w / 50.0 * weather.snow);
		for(int i = 0; i < flakes; i++) {
			Uint32 seed = (Uint32)(i + card_index * 61) * 1664525u + 1013904223u;
			int flake_x = card->x + seed % card->w;
			int flake_y = card->y
				+ (int)((seed + SDL_GetTicks() * (2 + (seed >> 16) % 4) / 80) % card->h);
			pixelRGBA(screen, flake_x, flake_y, 208, 221, 236, 110);
		}
	}
	SDL_SetClipRect(screen, NULL);
	render_foreground_battle(screen, SDL_GetTicks(), &hourBackground, &minBackground);

	// flip backbuffer
	SDL_Flip(screen);

	if(moved || step == maxsteps-1) {
		past_h = _time->tm_hour;
		past_m = _time->tm_min;
	}
}

void render_animation() {
	if(!animate) {
		render_clock(20, 19, false);
		return;
	}

	const int DURATION = 260;
	const int FRAME_COUNT = 20;
	int start_tick = SDL_GetTicks();
	for(int frame = 0; frame < FRAME_COUNT; frame++) {
		render_clock(FRAME_COUNT, frame, false);
		int elapsed = SDL_GetTicks() - start_tick;
		int next_frame = DURATION * (frame + 1) / FRAME_COUNT;
		if(elapsed < next_frame) SDL_Delay(next_frame - elapsed);
	}
}

Uint32 update_time(Uint32 interval, void *param) {
	SDL_Event e;
	time_t rawtime;
	struct tm *time_i;

	time(&rawtime);
	time_i = localtime(&rawtime);

	if(time_i->tm_min != past_m && time_i->tm_min != queued_minute) {
		queued_minute = time_i->tm_min;
		e.type = SDL_USEREVENT;
		e.user.code = 0;
		e.user.data1 = NULL;
		e.user.data2 = NULL;
		SDL_PushEvent(&e);
	} else if(time_i->tm_min == past_m) {
		e.type = SDL_USEREVENT;
		e.user.code = 1;
		e.user.data1 = NULL;
		e.user.data2 = NULL;
		SDL_PushEvent(&e);
	}

	return 100;
}

int main(int argc, char** argv ) {
	char *wid_env;
	static char sdlwid[100];
	double display_scale_factor = 1;

	Uint32 wid = 0;
	Display *display;
	XWindowAttributes windowAttributes;

	for(int i = 1; i < argc; i++) {
		if(strcmp("--help",argv[i]) == 0 || strcmp("-help", argv[i]) == 0) {
			printf("Usage: %s [OPTION...]\nOptions:\n", argv[0]);
			printf("  -help\t\tDisplay this\n");
			printf("  -root, -f\tFullscreen\n");
			printf("  -noflip\tDisable the flip animation (change time in one frame)\n");
			printf("  -anykeyclose\tClose app when mouse move or any key pressed\n");
			printf("  -ampm\t\tUse 12-hour clock format (AM/PM)\n");
			printf("  -leadingzero\tAlways display hour with two digits\n");
			printf("  -w\t\tCustom width\n");
			printf("  -h\t\tCustom height\n");
			printf("  -r\t\tCustom resolution in WxH format\n");
			printf("  -s\t\tCustom display scale factor\n");
			return 0;
		} else if(strcmp("-root", argv[i]) == 0 || strcmp("-f", argv[i]) == 0 || strcmp("--fullscreen", argv[i]) == 0) {
			fullscreen = true;
		} else if(strcmp("-noflip", argv[i]) == 0) {
			animate = false;
		} else if(strcmp("-anykeyclose", argv[i]) == 0) {
			anykeyclose = true;
		} else if(strcmp("-ampm", argv[i]) == 0) {
			twentyfourh = false;
		} else if(strcmp("-leadingzero", argv[i]) == 0) {
			leadingzero = true;
		} else if(strcmp("-r", argv[i]) == 0 || strcmp("--resolution", argv[i]) == 0) {
			char *resolution = argv[i+1];
			char *val = strtok(resolution, "x");
			width = atoi(val);
			val = strtok(NULL, "x");
			height = atoi(val);
			i++;
		} else if(strcmp("-w", argv[i]) == 0) {
			width = atoi(argv[i+1]);
			i++;
		} else if(strcmp("-h", argv[i]) == 0) {
			height = atoi(argv[i+1]);
			i++;
		} else if(strcmp("-s", argv[i]) == 0) {
			display_scale_factor = atof(argv[i+1]);
			i++;
		} else if(strcmp("-window-id", argv[i]) == 0) {
			wid = strtol(argv[i+1], (char **) NULL, 0);
			i++;
		} else {
			printf("Invalid option -- %s\n", argv[i]);
			printf("Try --help for more information.\n");
			return 0;
		}
	}

	/* If no window argument, check environment */
	if(wid == 0) {
		if ((wid_env = getenv("XSCREENSAVER_WINDOW")) != NULL ) {
			wid = strtol(wid_env, (char **) NULL, 0); /* Base 0 autodetects hex/dec */
		}
	}

	/* Get win attrs if we've been given a window, otherwise we'll use our own */
	if(wid != 0) {
		if ((display = XOpenDisplay(NULL)) != NULL) { /* Use the default display */
			XGetWindowAttributes(display, (Window) wid, &windowAttributes);
			XCloseDisplay(display);
			snprintf(sdlwid, 100, "SDL_WINDOWID=0x%X", wid);
			putenv(sdlwid); /* Tell SDL to use this window */
			width = windowAttributes.width;
			height = windowAttributes.height;
		}
	}

	if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER) < 0) {
		fprintf(stderr, "Unable to init SDL: %s\n", SDL_GetError());
		return 1;
	}
	atexit(SDL_Quit);

	if(fullscreen && (!wid)) {
		screen = SDL_SetVideoMode(0, 0, 32, SDL_HWSURFACE|SDL_DOUBLEBUF|SDL_FULLSCREEN);
	} else {
		screen = SDL_SetVideoMode(width, height, 32, SDL_HWSURFACE|SDL_DOUBLEBUF);
	}

	if (!screen) {
		fprintf(stderr, "Unable to set video mode: %s\n", SDL_GetError());
		return 1;
	}

	if(fullscreen || wid) {
		SDL_ShowCursor(SDL_DISABLE);
	}

	SDL_WM_SetCaption(TITLE, TITLE);

	width = screen->w * display_scale_factor;
	height = screen->h * display_scale_factor;

	bool is_horizontal = width > height;

	TTF_Init();
	atexit(TTF_Quit);
	font_time = TTF_OpenFont(FONT, (is_horizontal ? height : width) / 1.68 );
	font_mode = TTF_OpenFont(FONT, (is_horizontal ? height : width) / 16.5);
	if (!font_time || !font_mode) {
		fprintf(stderr, "TTF_OpenFont: %s\n", TTF_GetError());
		return 1;
	}
	static const char *date_font_paths[] = {
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
		"/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
		"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
		"/usr/share/fonts/truetype/freefont/FreeSans.ttf"
	};
	int date_font_size = (is_horizontal ? height : width) / 24;
	for(size_t i = 0; i < sizeof(date_font_paths) / sizeof(date_font_paths[0]) && !font_date; i++) {
		font_date = TTF_OpenFont(date_font_paths[i], date_font_size);
	}
	if (!font_date) {
		fprintf(stderr, "TTF_OpenFont: %s\n", TTF_GetError());
		return 1;
	}

	// Draw the layered forest before placing the clock over it.
	render_backdrop(screen, SDL_GetTicks());

	// calculate box coordinates
	int rectsize;
	int spacing;
	int radius;
	int date_gap = TTF_FontHeight(font_date) / 2;

	if (is_horizontal) {
		rectsize = height * 0.6;
		spacing = width * .031;
		radius = rectsize * .0952;
	}
	else {
		spacing = height * .031;
		int max_rectsize = (height - TTF_FontHeight(font_date) - date_gap * 2 - spacing) / 2;
		rectsize = width * 0.6 < max_rectsize ? width * 0.6 : max_rectsize;
		radius = rectsize * .0952;
	}

	int jitter_width  = 1;
	int jitter_height = 1;
	if (display_scale_factor != 1) {
		jitter_width  = (screen->w - width) * 0.5;
		jitter_height = (screen->h - height) * 0.5;
	}

	hourBackground.w = rectsize;
	hourBackground.h = rectsize;
	minBackground.w = rectsize;
	minBackground.h = rectsize;

	if (is_horizontal) {
		hourBackground.x = 0.5 * (width - (2 * rectsize) - spacing)
										+ jitter_width;
		hourBackground.y = (height - rectsize - date_gap - TTF_FontHeight(font_date)) / 2 + jitter_height;

		minBackground.x = hourBackground.x + rectsize + spacing;
		minBackground.y = hourBackground.y;
		date_y = hourBackground.y + rectsize + date_gap;
	}
	else {
		hourBackground.y = (height - (2 * rectsize) - spacing - date_gap - TTF_FontHeight(font_date)) / 2
										+ jitter_height;
		hourBackground.x = (width - rectsize) / 2 + jitter_width;

		minBackground.y = hourBackground.y + rectsize + spacing;
		minBackground.x = hourBackground.x;
		date_y = minBackground.y + rectsize + date_gap;
	}

	// create background surface
	bgrect.x = 0;
	bgrect.y = 0;
	bgrect.w = rectsize;
	bgrect.h = rectsize;
	Uint32 alpha_mask = ~(screen->format->Rmask | screen->format->Gmask | screen->format->Bmask);
	bg = SDL_CreateRGBSurface(SDL_SWSURFACE|SDL_SRCALPHA, rectsize, rectsize, 32,
		screen->format->Rmask, screen->format->Gmask, screen->format->Bmask, alpha_mask);
	fill_rounded_box_b(bg, &bgrect, radius, CARD_BORDER_COLOR, 232);
	int border_width = rectsize / 120;
	if(border_width < 1) border_width = 1;
	SDL_Rect cardrect;
	cardrect.x = border_width;
	cardrect.y = border_width;
	cardrect.w = rectsize - 2 * border_width;
	cardrect.h = rectsize - 2 * border_width;
	int inner_radius = radius > border_width ? radius - border_width : 1;
	fill_rounded_box_b(bg, &cardrect, inner_radius, BACKGROUND_COLOR, 218);

	// draw current time
	render_clock(20, 19, false);

	// main loop
	bool done = false;
	SDL_Event event;
	SDL_TimerID timer = SDL_AddTimer(100, update_time, NULL);
	if(!timer) {
		fprintf(stderr, "Unable to start clock timer: %s\n", SDL_GetError());
		SDL_FreeSurface(bg);
		if(backdrop_base) SDL_FreeSurface(backdrop_base);
		if(date_surface_cache) SDL_FreeSurface(date_surface_cache);
		TTF_CloseFont(font_time);
		TTF_CloseFont(font_mode);
		TTF_CloseFont(font_date);
		return 1;
	}

	int mouse_x = -1;
	int mouse_y = -1;

	while(!done && SDL_WaitEvent(&event)) {
		switch(event.type) {
			case SDL_USEREVENT:
				if(event.user.code == 1) {
					render_clock(20, 19, true);
				} else {
					render_animation();
				}
				break;
			case SDL_KEYDOWN:
				if(anykeyclose){
					done = true;
					break;
				}
				switch(event.key.keysym.sym) {
					case SDLK_ESCAPE:
					case SDLK_q:
						done = true;
						break;
					default:
						break;
				}
				break;

			case SDL_MOUSEMOTION:
				if ( (mouse_x == -1) || (mouse_y == -1) ) //lifehack
					{
						mouse_x = event.motion.x;
						mouse_y = event.motion.y;

					}

				if(((mouse_x != event.motion.x) || (mouse_y != event.motion.y)) && anykeyclose)
					done = true;
				break;

			case SDL_QUIT:
				done = true;
				break;
		}
	}

	SDL_RemoveTimer(timer);

	SDL_FreeSurface(bg);
	if(backdrop_base) SDL_FreeSurface(backdrop_base);
	if(date_surface_cache) SDL_FreeSurface(date_surface_cache);

	TTF_CloseFont(font_time);
	TTF_CloseFont(font_mode);
	TTF_CloseFont(font_date);

	return 0;
}
