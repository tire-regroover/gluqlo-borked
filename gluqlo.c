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
	double fog;
	double wind;
	double lightning;
	double aurora;
	double palette[3];
} SceneWeather;

static SceneWeather get_scene_weather(Uint32 ticks) {
	static const double weather_levels[][11] = {
		{ 0.10, 0.00, 0.00, 0.95, 0.02, 0.08, 0.00, 0.00, 174, 103, 54 },
		{ 0.25, 0.00, 0.00, 0.72, 0.12, 0.15, 0.00, 0.00, 191, 91, 55 },
		{ 0.78, 0.00, 0.00, 0.10, 0.20, 0.18, 0.00, 0.00, 67, 92, 116 },
		{ 0.74, 0.42, 0.00, 0.12, 0.26, 0.45, 0.00, 0.00, 67, 112, 139 },
		{ 0.90, 0.80, 0.00, 0.02, 0.14, 0.70, 0.08, 0.00, 54, 104, 137 },
		{ 1.00, 0.92, 0.00, 0.00, 0.04, 0.95, 0.90, 0.00, 73, 80, 112 },
		{ 0.72, 0.00, 0.72, 0.12, 0.24, 0.40, 0.00, 0.00, 145, 171, 192 },
		{ 0.97, 0.00, 1.00, 0.00, 0.54, 0.88, 0.04, 0.00, 176, 197, 211 },
		{ 0.36, 0.00, 0.12, 0.00, 0.13, 0.25, 0.00, 0.90, 62, 132, 142 },
		{ 0.88, 0.08, 0.00, 0.15, 1.00, 0.05, 0.00, 0.00, 132, 156, 155 }
	};
	const Uint32 season_duration = 300000;
	const Uint32 season_transition = 20000;
	const Uint32 weather_duration = 65000;
	const Uint32 weather_transition = 12000;
	const int weather_count = sizeof(weather_levels) / sizeof(weather_levels[0]);
	SceneWeather scene = { { 0.0, 0.0, 0.0, 0.0 },
		0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, { 0.0, 0.0, 0.0 } };

	Uint32 season_elapsed = ticks % (season_duration * 4);
	int season = season_elapsed / season_duration;
	Uint32 season_phase = season_elapsed % season_duration;
	double season_blend = smooth_step(season_duration - season_transition,
		season_duration, season_phase);
	scene.season[season] = 1.0 - season_blend;
	scene.season[(season + 1) % 4] = season_blend;

	Uint32 weather_elapsed = ticks % (weather_duration * weather_count);
	int current = weather_elapsed / weather_duration;
	int previous = (current + weather_count - 1) % weather_count;
	Uint32 weather_phase = weather_elapsed % weather_duration;
	double weather_blend = smooth_step(0.0, weather_transition, weather_phase);
	scene.cloud = weather_levels[previous][0]
		+ (weather_levels[current][0] - weather_levels[previous][0]) * weather_blend;
	scene.rain = weather_levels[previous][1]
		+ (weather_levels[current][1] - weather_levels[previous][1]) * weather_blend;
	scene.snow = weather_levels[previous][2]
		+ (weather_levels[current][2] - weather_levels[previous][2]) * weather_blend;
	scene.sunshine = weather_levels[previous][3]
		+ (weather_levels[current][3] - weather_levels[previous][3]) * weather_blend;
	scene.fog = weather_levels[previous][4]
		+ (weather_levels[current][4] - weather_levels[previous][4]) * weather_blend;
	scene.wind = weather_levels[previous][5]
		+ (weather_levels[current][5] - weather_levels[previous][5]) * weather_blend;
	scene.lightning = weather_levels[previous][6]
		+ (weather_levels[current][6] - weather_levels[previous][6]) * weather_blend;
	scene.aurora = weather_levels[previous][7]
		+ (weather_levels[current][7] - weather_levels[previous][7]) * weather_blend;
	for(int channel = 0; channel < 3; channel++) {
		scene.palette[channel] = weather_levels[previous][8 + channel]
			+ (weather_levels[current][8 + channel] - weather_levels[previous][8 + channel])
			* weather_blend;
	}
	return scene;
}

static void render_weather_effects(SDL_Surface *surface, Uint32 ticks,
	const SceneWeather *weather, int scale) {
	if(weather->aurora > 0.01) {
		for(int band = 0; band < 3; band++) {
			int previous_x = 0;
			int previous_y = surface->h * (19 + band * 12) / 100;
			for(int segment = 1; segment <= 24; segment++) {
				int x = surface->w * segment / 24;
				double nx = (double)x / surface->w;
				int y = surface->h * (19 + band * 12) / 100
					+ (int)(sin(nx * 6.28318530718 + ticks / (1100.0 + band * 270)
						+ band * 1.9) * scale * (0.03 + band * 0.008));
				Uint8 alpha = (Uint8)(weather->aurora * (28 - band * 5));
				thickLineRGBA(surface, previous_x, previous_y, x, y, 2,
					(Uint8)(70 + band * 24), (Uint8)(164 + band * 18),
					(Uint8)(137 + band * 30), alpha);
				previous_x = x;
				previous_y = y;
			}
		}
	}

	if(weather->fog > 0.01) {
		for(int band = 0; band < 3; band++) {
			int y = surface->h * (57 + band * 12) / 100
				+ (int)(sin(ticks / (2900.0 + band * 530)) * scale * 0.012);
			int height = scale / (30 + band * 8);
			if(height < 1) height = 1;
			Uint8 alpha = (Uint8)(weather->fog * (21 - band * 4));
			boxRGBA(surface, 0, y, surface->w - 1, y + height,
				150, 177, 181, alpha);
		}
	}

	if(weather->lightning > 0.01) {
		Uint32 phase = ticks % 7200;
		double flash = 0.0;
		if(phase < 180) flash = 1.0 - phase / 180.0;
		else if(phase >= 300 && phase < 410) flash = 0.45 * (1.0 - (phase - 300) / 110.0);
		if(flash > 0.0) {
			Uint8 alpha = (Uint8)(weather->lightning * 29 * flash);
			boxRGBA(surface, 0, 0, surface->w - 1, surface->h - 1,
				188, 205, 225, alpha);
			unsigned int seed = ticks / 7200 * 2654435761u + 0x9e3779b9u;
			int x = surface->w * (18 + seed % 64) / 100;
			int y = surface->h * 27 / 100;
			for(int segment = 0; segment < 7; segment++) {
				int next_x = x + (int)((seed >> (segment % 16)) % (scale / 16 + 1))
					- scale / 32;
				int next_y = y + scale / 24;
				thickLineRGBA(surface, x, y, next_x, next_y, 2,
					207, 220, 244, (Uint8)(alpha * 0.9));
				if(segment == 3) {
					thickLineRGBA(surface, x, y, x - scale / 30,
						y + scale / 11, 1, 180, 203, 236, alpha);
				}
				x = next_x;
				y = next_y;
				seed = seed * 1664525u + 1013904223u;
			}
		}
	}
}

static void render_world_jump(SDL_Surface *surface, Uint32 ticks, int scale) {
		const Uint32 cycle = 360000;
		const Uint32 duration = 7800;
		Uint32 slot = ticks / cycle;
		Uint32 phase = ticks % cycle;
		Uint32 seed = slot * 2246822519u + 3266489917u;
		if(seed % 5 != 0 || phase >= duration) return;
		seed ^= seed >> 15;
		seed *= 2654435761u;
		seed ^= seed >> 13;
		double progress = (double)phase / duration;
		double fade = smooth_step(0.0, 350.0, phase)
			* (1.0 - smooth_step(duration - 450.0, duration, phase));
		int center_x = surface->w / 2 + (int)(sin(ticks / 115.0) * scale * 0.012);
		int center_y = surface->h * 43 / 100;
		double tunnel_open = sin(progress * 3.14159265359);
		int portal_x_radius = scale * (0.045 + tunnel_open * 0.22);
		int portal_y_radius = scale * (0.035 + tunnel_open * 0.15);
		Uint8 tunnel_alpha = (Uint8)(118 * fade * (0.35 + tunnel_open * 0.65));

		for(int ring = 0; ring < 9; ring++) {
			double ring_scale = 0.34 + ring * 0.105;
			int radius_x = (int)(portal_x_radius * ring_scale);
			int radius_y = (int)(portal_y_radius * ring_scale);
			Uint8 ring_alpha = (Uint8)(tunnel_alpha * (1.0 - ring * 0.065));
			ellipseRGBA(surface, center_x, center_y, radius_x, radius_y,
				(Uint8)(100 + ring * 14), (Uint8)(151 + ring * 9),
				(Uint8)(218 + ring * 4), ring_alpha);
		}
		for(int streak = 0; streak < 48; streak++) {
			unsigned int streak_seed = seed + streak * 2246822519u;
			double angle = (streak_seed % 6283) / 1000.0 + ticks / 700.0;
			double travel = fmod((streak_seed >> 8) / 4294967295.0 + progress * 1.8, 1.0);
			double distance = portal_x_radius + travel * scale * 0.38;
			int star_x = center_x + (int)(cos(angle) * distance);
			int star_y = center_y + (int)(sin(angle) * distance * 0.68);
			int tail_x = center_x + (int)(cos(angle) * (distance - scale * 0.035));
			int tail_y = center_y + (int)(sin(angle) * (distance - scale * 0.035) * 0.68);
			thickLineRGBA(surface, tail_x, tail_y, star_x, star_y, 1,
				(Uint8)(155 + streak % 5 * 18), (Uint8)(182 + streak % 4 * 16),
				235, (Uint8)(tunnel_alpha * 0.62));
		}

		double destination_mix = smooth_step(3900.0, 5150.0, phase)
			* (1.0 - smooth_step(5850.0, 7200.0, phase));
		static const Uint8 destination_palette[4][3] = {
			{ 36, 90, 112 }, { 128, 66, 75 }, { 74, 103, 68 }, { 98, 79, 132 }
		};
		int destination = seed % 4;
		Uint8 sky_alpha = (Uint8)(84 * destination_mix * fade);
		boxRGBA(surface, 0, 0, surface->w - 1, surface->h - 1,
			destination_palette[destination][0], destination_palette[destination][1],
			destination_palette[destination][2], sky_alpha);
		for(int layer = 0; layer < 3; layer++) {
			int base_y = surface->h * (66 + layer * 9) / 100;
			Uint8 terrain_alpha = (Uint8)(148 * destination_mix * fade * (0.85 - layer * 0.16));
			Uint8 terrain_red = (Uint8)(14 + destination * 13 + layer * 5);
			Uint8 terrain_green = (Uint8)(31 + destination * 9 + layer * 4);
			Uint8 terrain_blue = (Uint8)(43 + destination * 14 + layer * 3);
			for(int segment = 0; segment < 18; segment++) {
				int x1 = surface->w * segment / 18;
				int x2 = surface->w * (segment + 1) / 18;
				int y1 = base_y + (int)(sin(segment * 1.4 + destination) * scale * 0.035);
				int y2 = base_y + (int)(sin((segment + 1) * 1.4 + destination) * scale * 0.035);
				filledTrigonRGBA(surface, x1, y1, x2, y2, x2, surface->h,
					terrain_red, terrain_green, terrain_blue, terrain_alpha);
				filledTrigonRGBA(surface, x1, y1, x2, surface->h, x1, surface->h,
					terrain_red, terrain_green, terrain_blue, terrain_alpha);
			}
		}
		Uint32 return_phase = phase > 5900 ? phase - 5900 : 0;
		if(return_phase < 900) {
			double return_flash = (1.0 - return_phase / 900.0) * fade;
			boxRGBA(surface, 0, 0, surface->w - 1, surface->h - 1,
				225, 233, 241, (Uint8)(150 * return_flash));
		}
	}

	static void render_alien_weather(SDL_Surface *surface, Uint32 ticks, int scale) {
	const Uint32 cycle = 300000;
	Uint32 slot = ticks / cycle;
	Uint32 phase = ticks % cycle;
	Uint32 seed = slot * 2246822519u + 3266489917u;
	if(seed % 4 != 0) return;
	seed ^= seed >> 15;
	seed *= 2246822519u;
	seed ^= seed >> 13;
	Uint32 duration = 52000 + (seed >> 8) % 18000;
	if(phase >= duration) return;
	double fade = smooth_step(0.0, 5000.0, phase)
		* (1.0 - smooth_step(duration - 7000.0, duration, phase));
	int kind = seed % 5;
	static const Uint8 sky_colors[5][3] = {
		{ 66, 142, 106 }, { 99, 81, 157 }, { 62, 113, 156 },
		{ 158, 83, 91 }, { 157, 125, 62 }
	};
	Uint8 red = sky_colors[kind][0];
	Uint8 green = sky_colors[kind][1];
	Uint8 blue = sky_colors[kind][2];
	boxRGBA(surface, 0, 0, surface->w - 1, surface->h - 1,
		red, green, blue, (Uint8)(18 * fade));

	if(kind == 2) {
		int planet_x = surface->w * (25 + (seed >> 12) % 50) / 100;
		int planet_y = surface->h * 28 / 100;
		int radius = scale / 11;
		filledCircleRGBA(surface, planet_x, planet_y, radius,
			32, 38, 51, (Uint8)(150 * fade));
		circleRGBA(surface, planet_x, planet_y, radius + scale / 30,
			134, 194, 210, (Uint8)(100 * fade));
	}

	int count = surface->w * surface->h / 12000;
	if(count < 24) count = 24;
	if(count > 180) count = 180;
	for(int particle = 0; particle < count; particle++) {
		Uint32 particle_seed = (Uint32)particle * 1664525u + seed;
		int start_x = particle_seed % surface->w;
		int start_y = (particle_seed >> 9) % surface->h;
		int speed = 3 + (particle_seed >> 20) % 7;
		int travel = (int)(ticks / (35 + speed * 7));
		int x, y, end_x, end_y;
		if(kind == 0) {
			x = (start_x + travel * speed) % surface->w;
			y = (start_y + travel / 2) % surface->h;
			end_x = x - scale / 90;
			end_y = y + scale / 50;
		} else if(kind == 1) {
			x = (start_x - travel * speed + surface->w * 2) % surface->w;
			y = (start_y - travel) % surface->h;
			if(y < 0) y += surface->h;
			end_x = x + scale / 100;
			end_y = y + scale / 70;
		} else {
			x = (start_x + travel * speed / 2) % surface->w;
			y = (start_y + travel) % surface->h;
			end_x = x - scale / 130;
			end_y = y + scale / 60;
		}
		Uint8 alpha = (Uint8)((18 + (particle_seed >> 24) % 30) * fade);
		thickLineRGBA(surface, x, y, end_x, end_y, 1, red, green, blue, alpha);
		if(particle % 9 == 0) filledCircleRGBA(surface, x, y, 1,
			(Uint8)(red + 25), (Uint8)(green + 25), (Uint8)(blue + 25), alpha);
	}

	if(kind == 3) {
		Uint32 flash_phase = phase % 8400;
		if(flash_phase < 140) {
			Uint8 flash = (Uint8)(45 * fade * (1.0 - flash_phase / 140.0));
			boxRGBA(surface, 0, 0, surface->w - 1, surface->h - 1,
				211, 183, 226, flash);
		}
	}
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
	thickLineRGBA(surface, nose_x, y, back_x, y, 1, 191, 212, 231, alpha);
	thickLineRGBA(surface, nose_x, y, wing_x, wing_top, 1, 186, 206, 220, alpha);
	thickLineRGBA(surface, nose_x, y, wing_x, wing_bottom, 1, 186, 206, 220, alpha);
	thickLineRGBA(surface, wing_x, wing_top, x, y, 1, red, green, blue, alpha);
	thickLineRGBA(surface, x, y, wing_x, wing_bottom, 1, red, green, blue, alpha);
	int cockpit_radius = size / 8;
	if(cockpit_radius < 1) cockpit_radius = 1;
	filledCircleRGBA(surface, x + direction * size / 3, y, cockpit_radius,
		171, 224, 245, (Uint8)(alpha * 0.85));
	thickLineRGBA(surface, back_x - direction * size / 2, y,
		back_x, y, 2, 116, 196, 232, (Uint8)(alpha * 0.35));
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
	int kind = (seed >> 5) % 6;
	if(phase >= duration || kind >= 4) return;

	double fade = smooth_step(0.0, 1300.0, phase)
		* (1.0 - smooth_step(duration - 1200.0, duration, phase));
	if(kind == 0) {
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
	} else if(kind == 1) {
		Uint32 flash_phase = phase % 3400;
		double flash = smooth_step(0.0, 90.0, flash_phase)
			* (1.0 - smooth_step(170.0, 430.0, flash_phase));
		Uint8 alpha = (Uint8)(24 * fade * flash);
		if(alpha > 0) {
			boxRGBA(surface, 0, 0, surface->w - 1, surface->h - 1,
				170, 192, 220, alpha);
		}
	} else if(kind == 2) {
		boxRGBA(surface, 0, 0, surface->w - 1, surface->h - 1,
			17, 25, 44, (Uint8)(28 * fade));
		int radius = scale / 8;
		int moon_x = surface->w * (25 + (seed >> 12) % 50) / 100;
		int moon_y = surface->h * (18 + (seed >> 20) % 24) / 100;
		filledCircleRGBA(surface, moon_x, moon_y, radius + scale / 18,
			119, 158, 194, (Uint8)(24 * fade));
		filledCircleRGBA(surface, moon_x, moon_y, radius,
			171, 183, 190, (Uint8)(190 * fade));
		filledCircleRGBA(surface, moon_x + radius / 3, moon_y, radius - radius / 7,
			22, 27, 38, (Uint8)(220 * fade));
	} else {
		int flare_x = surface->w * (28 + (seed >> 12) % 44) / 100;
		int flare_y = surface->h * (18 + (seed >> 20) % 25) / 100;
		int radius = scale / 13;
		boxRGBA(surface, 0, 0, surface->w - 1, surface->h - 1,
			177, 82, 45, (Uint8)(13 * fade));
		filledCircleRGBA(surface, flare_x, flare_y, radius,
			245, 137, 75, (Uint8)(45 * fade));
		filledCircleRGBA(surface, flare_x, flare_y, radius / 3,
			255, 222, 169, (Uint8)(90 * fade));
		for(int ray = 0; ray < 12; ray++) {
			double angle = ray * 0.52359877559 + ticks / 9000.0;
			int end_x = flare_x + (int)(cos(angle) * scale * 0.42);
			int end_y = flare_y + (int)(sin(angle) * scale * 0.30);
			thickLineRGBA(surface, flare_x, flare_y, end_x, end_y, 1,
				251, 174, 105, (Uint8)(18 * fade));
		}
	}
}

static void render_background_battle(SDL_Surface *surface, Uint32 ticks, int scale) {
	const Uint32 cycle = 60000;
	const Uint32 duration = 14000;
	Uint32 phase = ticks % cycle;
	if(phase >= duration) return;

	int size = scale / 170;
	if(size < 4) size = 4;
	double fade = smooth_step(0.0, 900.0, phase)
		* (1.0 - smooth_step(duration - 1100.0, duration, phase));
	double progress = (double)phase / duration;
	int left_x = (int)(-size + progress * (surface->w + 2.0 * size));
	int right_x = surface->w - left_x;
	int center_y = surface->h * 17 / 100;
	int arc = (int)(sin(progress * 6.28318530718) * scale * 0.018);
	int maneuver = (int)(sin(progress * 12.56637061436) * scale * 0.014);
	int left_y = center_y + arc + maneuver;
	int right_y = center_y - arc - maneuver;
	Uint8 alpha = (Uint8)(100 * fade);

	draw_battle_ship(surface, left_x, left_y, size, 1, 105, 163, 183, alpha);
	draw_battle_ship(surface, right_x, right_y, size, -1, 177, 130, 102, alpha);

	Uint32 volley_phase = phase % 2600;
	if(volley_phase < 300 && progress > 0.08 && progress < 0.94) {
		int bolt_length = scale / 18;
		if(bolt_length < 12) bolt_length = 12;
		Uint8 bolt_alpha = (Uint8)(alpha * 0.8);
		thickLineRGBA(surface, left_x + size, left_y,
			left_x + size + bolt_length, left_y - size / 3,
			1, 143, 204, 224, bolt_alpha);
		thickLineRGBA(surface, right_x - size, right_y,
			right_x - size - bolt_length, right_y + size / 3,
			1, 220, 164, 126, bolt_alpha);
	}
	if(volley_phase > 1850 && volley_phase < 2250) {
		double hit_progress = (double)(volley_phase - 1850) / 400.0;
		int impact_x = surface->w / 2
			+ (int)(sin(phase / 1700.0) * scale * 0.045);
		int impact_y = center_y + (int)(sin(phase / 950.0) * scale * 0.025);
		Uint8 impact_alpha = (Uint8)(alpha * (1.0 - hit_progress));
		int radius = size / 2 + (int)(size * hit_progress);
		filledCircleRGBA(surface, impact_x, impact_y, radius,
			219, 158, 111, (Uint8)(impact_alpha * 0.65));
		filledCircleRGBA(surface, impact_x, impact_y, size / 4,
			238, 216, 177, impact_alpha);
	}
}

static void render_fleet_battle(SDL_Surface *surface, Uint32 ticks, int scale) {
	const Uint32 cycle = 210000;
	const Uint32 duration = 36000;
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
	int size = scale / 72;
	if(size < 10) size = 10;
	int fighter_size = size / 3;
	if(fighter_size < 2) fighter_size = 2;
	int left_x = (int)(-size * 4 + progress * (surface->w + size * 8));
	int right_x = surface->w - left_x;
	int center_y = surface->h * 19 / 100;
	int arc = (int)(sin(progress * 6.28318530718) * scale * 0.022);
	Uint8 left_alpha = (Uint8)(88 * fade * (winner == 0 ? 1.0 : 1.0 - defeat));
	Uint8 right_alpha = (Uint8)(88 * fade * (winner == 1 ? 1.0 : 1.0 - defeat));
	Uint8 left_red = 100, left_green = 147, left_blue = 167;
	Uint8 right_red = 170, right_green = 126, right_blue = 101;

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
		int weave = (int)(sin(ticks / (320.0 + fighter * 17) + fighter * 2.4)
			* fighter_size * 2);
		int side_alpha = (Uint8)(100 * fade);
		draw_battle_ship(surface, left_x + spread, center_y + arc + y_offset + weave,
			fighter_size, 1, left_red, left_green, left_blue,
			winner == 0 ? side_alpha : (Uint8)(side_alpha * (1.0 - defeat)));
		draw_battle_ship(surface, right_x - spread, center_y - arc - y_offset - weave,
			fighter_size, -1, right_red, right_green, right_blue,
			winner == 1 ? side_alpha : (Uint8)(side_alpha * (1.0 - defeat)));
	}

	if((phase % 2900) < 260 && progress > 0.06 && progress < 0.94) {
		int volley_y = center_y + (int)(sin(ticks / 800.0) * scale * 0.035);
		int bolt_length = scale / 11;
		if(bolt_length < 18) bolt_length = 18;
		thickLineRGBA(surface, left_x + size * 3, volley_y,
			left_x + size * 3 + bolt_length, volley_y - size,
			1, 135, 192, 212, (Uint8)(72 * fade));
		thickLineRGBA(surface, right_x - size * 3, volley_y + scale / 80,
			right_x - size * 3 - bolt_length, volley_y + size,
			1, 210, 154, 119, (Uint8)(72 * fade));
	}

	if(progress > 0.64 && progress < 0.97 && (phase % 1800) < 900) {
		int loser_x = winner == 0 ? right_x : left_x;
		int loser_y = center_y + (winner == 0 ? -arc : arc);
		unsigned int blast_seed = seed ^ 0xa511e9b3u ^ (phase / 600 * 0x9e3779b9u);
		int blast_x = loser_x + (int)(blast_seed % (unsigned int)(size * 8 + 1))
			- size * 4;
		int blast_y = loser_y + (int)((blast_seed >> 8) % (unsigned int)(scale / 12 + 1))
			- scale / 24;
		Uint8 blast_alpha = (Uint8)(115 * fade * (1.0 - defeat * 0.55));
		filledCircleRGBA(surface, blast_x, blast_y, size / 3,
			221, 155, 110, blast_alpha);
		filledCircleRGBA(surface, blast_x, blast_y, size / 6,
			244, 220, 178, blast_alpha);
	}
}

static void render_long_scene(SDL_Surface *surface, Uint32 ticks, int scale) {
		const Uint32 cycle = 210000;
		Uint32 slot = ticks / cycle;
		Uint32 phase = ticks % cycle;
		Uint32 seed = slot * 2246822519u + 3266489917u;
		if(seed % 5 != 0) return;
		seed ^= seed >> 15;
		seed *= 2246822519u;
		seed ^= seed >> 13;
		Uint32 duration = 72000 + (seed >> 8) % 28000;
		if(phase >= duration) return;
		double progress = (double)phase / duration;
		double fade = smooth_step(0.0, 6000.0, phase)
			* (1.0 - smooth_step(duration - 7000.0, duration, phase));
		int kind = seed % 5;

		if(kind == 0) {
			for(int tree = 0; tree < 7; tree++) {
				int tree_x = surface->w * (8 + tree * 13) / 100;
				double nx = (double)tree_x / surface->w;
				double ridge = 0.88 + (smooth_noise(nx * 8.0 - 3.0, 3.0, 1123) - 0.5) * 0.09;
				int base_y = (int)(surface->h * ridge);
				double growth = smooth_step(0.03 + tree * 0.025, 0.72 + tree * 0.025, progress);
				int tree_height = (int)(scale * (0.035 + (tree % 3) * 0.012) * growth);
				if(tree_height < 2) continue;
				Uint8 alpha = (Uint8)(175 * fade);
				thickLineRGBA(surface, tree_x, base_y, tree_x, base_y - tree_height,
					2 + tree % 2, 61, 72, 61, alpha);
				for(int branch = 1; branch <= 3; branch++) {
					int branch_y = base_y - tree_height * branch / 4;
					int branch_length = tree_height * (4 - branch) / 5;
					thickLineRGBA(surface, tree_x, branch_y,
						tree_x - branch_length, branch_y - branch_length / 2,
						2, 67, 82, 66, alpha);
					thickLineRGBA(surface, tree_x, branch_y,
						tree_x + branch_length, branch_y - branch_length / 2,
						2, 67, 82, 66, alpha);
					if(growth > 0.72) {
						filledCircleRGBA(surface, tree_x - branch_length,
							branch_y - branch_length / 2, branch_length / 3 + 1,
							74, 112, 78, alpha);
						filledCircleRGBA(surface, tree_x + branch_length,
							branch_y - branch_length / 2, branch_length / 3 + 1,
							86, 124, 83, alpha);
					}
				}
			}
		} else if(kind == 1) {
			int flood_y = surface->h * (82 - (int)(progress * 9)) / 100;
			boxRGBA(surface, 0, flood_y, surface->w - 1, surface->h - 1,
				28, 79, 116, (Uint8)(58 * fade));
			for(int band = 0; band < 5; band++) {
				int previous_x = 0;
				int previous_y = flood_y + band * scale / 38;
				for(int segment = 1; segment <= 20; segment++) {
					int next_x = surface->w * segment / 20;
					int next_y = flood_y + band * scale / 38
						+ (int)(sin(segment * 0.8 + ticks / (500.0 + band * 80))
							* scale * 0.012);
					thickLineRGBA(surface, previous_x, previous_y, next_x, next_y,
						1 + band % 2, 115, 183, 208, (Uint8)(70 * fade));
					previous_x = next_x;
					previous_y = next_y;
				}
			}
		} else if(kind == 2) {
			boxRGBA(surface, 0, 0, surface->w - 1, surface->h - 1,
				135, 57, 27, (Uint8)(17 * fade));
			for(int fire = 0; fire < 12; fire++) {
				int fire_x = surface->w * (8 + fire * 7) / 100;
				double nx = (double)fire_x / surface->w;
				double ridge = 0.79 + (smooth_noise(nx * 5.2 + 8.0, 2.0, 1013) - 0.5) * 0.10;
				int fire_y = (int)(surface->h * ridge);
				int flicker = (int)(fabs(sin(ticks / (170.0 + fire * 11) + fire)) * scale * 0.025);
				thickLineRGBA(surface, fire_x, fire_y, fire_x + 2, fire_y - flicker,
					2, 238, 112, 55, (Uint8)(120 * fade));
				filledCircleRGBA(surface, fire_x + 4, fire_y - flicker, 2,
					255, 192, 106, (Uint8)(145 * fade));
			}
		} else if(kind == 3) {
			boxRGBA(surface, 0, 0, surface->w - 1, surface->h - 1,
				36, 57, 78, (Uint8)(24 * fade));
			for(int band = 0; band < 6; band++) {
				int previous_x = 0;
				int previous_y = surface->h * (17 + band * 10) / 100;
				for(int segment = 1; segment <= 18; segment++) {
					int next_x = surface->w * segment / 18;
					int next_y = surface->h * (17 + band * 10) / 100
						+ (int)(sin(segment * 0.6 + ticks / 1400.0 + band) * scale * 0.05);
					thickLineRGBA(surface, previous_x, previous_y, next_x, next_y, 2,
						78, 162, 163, (Uint8)(28 * fade));
					previous_x = next_x;
					previous_y = next_y;
				}
			}
		} else {
			boxRGBA(surface, 0, 0, surface->w - 1, surface->h - 1,
				17, 20, 32, (Uint8)(34 * fade));
			int figure_x = surface->w * (25 + seed % 50) / 100;
			int figure_y = surface->h * 83 / 100;
			int figure_height = scale / 10;
			filledTrigonRGBA(surface, figure_x, figure_y - figure_height,
				figure_x - figure_height / 3, figure_y,
				figure_x + figure_height / 3, figure_y, 12, 16, 25,
				(Uint8)(150 * fade));
			filledCircleRGBA(surface, figure_x, figure_y - figure_height,
				figure_height / 6, 18, 22, 30, (Uint8)(160 * fade));
		}
	}

	static void render_rare_sky_event(SDL_Surface *surface, Uint32 ticks, int scale) {
	const Uint32 event_cycle = 48000;
	const Uint32 event_duration = 6800;
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
	int kind = (seed >> 24) % 43;
	if(kind >= 23) {
		unsigned int rare_gate = slot * 2654435761u + 0x9e3779b9u;
		if(rare_gate % 7 != 0) return;
	}
	if(kind >= 23) {
		int center_x = x;
		int center_y = y;
		if(kind == 23) {
			int ground_x = center_x < 0 ? 0 : center_x;
			if(ground_x >= surface->w) ground_x = surface->w - 1;
			double nx = (double)ground_x / surface->w;
			double ridge = 0.88 + (smooth_noise(nx * 8.0 - 3.0, 3.0, 1123) - 0.5) * 0.09;
			int ground_y = (int)(surface->h * ridge);
			Uint8 pulse = (Uint8)(alpha * (0.45 + 0.55 * (0.5 + 0.5 * sin(progress * 18.0))));
			boxRGBA(surface, center_x - size, ground_y - size / 2,
				center_x + size, ground_y + size / 2, 35, 38, 42, alpha);
			filledCircleRGBA(surface, center_x - size / 2, ground_y - size / 2,
				2, 83, 123, 77, alpha);
			filledCircleRGBA(surface, center_x - size, ground_y - size,
				size / 2, 83, 56, 55, pulse);
			filledCircleRGBA(surface, center_x + size, ground_y - size,
				size / 2, 54, 75, 120, pulse);
		} else if(kind == 24) {
			int fold_size = size * 3;
			int unfold = (int)(fold_size * (0.35 + 0.65 * sin(progress * 3.14159265359)));
			filledTrigonRGBA(surface, center_x, center_y - fold_size,
				center_x - unfold, center_y + fold_size / 2,
				center_x, center_y, 167, 193, 205, alpha);
			filledTrigonRGBA(surface, center_x, center_y - fold_size,
				center_x, center_y, center_x + unfold, center_y + fold_size / 2,
				211, 177, 137, alpha);
			filledTrigonRGBA(surface, center_x, center_y - fold_size,
				center_x - fold_size / 3, center_y + fold_size,
				center_x + fold_size / 3, center_y + fold_size, 120, 151, 169, alpha);
			thickLineRGBA(surface, center_x, center_y - fold_size,
				center_x, center_y + fold_size, 1, 244, 227, 198, alpha);
			for(int crease = 1; crease <= 4; crease++) {
				int crease_x = center_x + (crease % 2 ? unfold : -unfold) * crease / 4;
				thickLineRGBA(surface, center_x, center_y - fold_size + crease * fold_size / 2,
					crease_x, center_y + fold_size / 2, 1, 235, 220, 193, alpha);
			}
		} else if(kind == 25) {
			int wheel_radius = scale / 180;
			if(wheel_radius < 3) wheel_radius = 3;
			int wheelbase = scale / 18;
			if(wheelbase < 34) wheelbase = 34;
			int rear_x = direction > 0
				? -wheelbase + (int)(progress * (surface->w + 2.0 * wheelbase))
				: surface->w + wheelbase - (int)(progress * (surface->w + 2.0 * wheelbase));
			int front_x = rear_x + direction * wheelbase;
			int ridge_x = rear_x + direction * wheelbase / 2;
			if(ridge_x < 0) ridge_x = 0;
			if(ridge_x >= surface->w) ridge_x = surface->w - 1;
			double nx = (double)ridge_x / surface->w;
			double ridge = 0.88 + (smooth_noise(nx * 8.0 - 3.0, 3.0, 1123) - 0.5) * 0.09;
			int wheel_y = (int)(surface->h * ridge) - wheel_radius;
			int seat_x = rear_x + direction * wheelbase * 38 / 100;
			int crank_x = rear_x + direction * wheelbase * 64 / 100;
			int seat_y = wheel_y - wheel_radius * 3;
			circleRGBA(surface, rear_x, wheel_y, wheel_radius, 26, 30, 36, alpha);
			circleRGBA(surface, front_x, wheel_y, wheel_radius, 26, 30, 36, alpha);
			for(int spoke = 0; spoke < 6; spoke++) {
				double spoke_angle = progress * 6.28318530718 + spoke * 1.0471975512;
				int spoke_dx = (int)(cos(spoke_angle) * wheel_radius);
				int spoke_dy = (int)(sin(spoke_angle) * wheel_radius);
				thickLineRGBA(surface, rear_x, wheel_y, rear_x + spoke_dx,
					wheel_y + spoke_dy, 1, 119, 130, 137, alpha);
				thickLineRGBA(surface, front_x, wheel_y, front_x + spoke_dx,
					wheel_y + spoke_dy, 1, 119, 130, 137, alpha);
			}
			thickLineRGBA(surface, rear_x, wheel_y, seat_x, seat_y, 2, 205, 120, 74, alpha);
			thickLineRGBA(surface, seat_x, seat_y, crank_x, wheel_y, 2, 205, 120, 74, alpha);
			thickLineRGBA(surface, crank_x, wheel_y, rear_x, wheel_y, 2, 205, 120, 74, alpha);
			thickLineRGBA(surface, seat_x, seat_y, front_x, wheel_y, 2, 205, 120, 74, alpha);
			thickLineRGBA(surface, seat_x, seat_y - wheel_radius * 2,
				crank_x, wheel_y - wheel_radius * 4, 3, 54, 65, 77, alpha);
			double pedal_angle = progress * 12.56637061436;
			int pedal_x = crank_x + (int)(cos(pedal_angle) * wheel_radius * 2);
			int pedal_y = wheel_y + (int)(sin(pedal_angle) * wheel_radius * 2);
			int handlebar_x = front_x - direction * wheelbase / 8;
			int handlebar_y = wheel_y - wheel_radius * 3;
			thickLineRGBA(surface, seat_x, seat_y - wheel_radius * 2,
				handlebar_x, handlebar_y, 2, 54, 65, 77, alpha);
			thickLineRGBA(surface, seat_x, seat_y - wheel_radius * 2,
				pedal_x, pedal_y, 2, 54, 65, 77, alpha);
			thickLineRGBA(surface, pedal_x, pedal_y, crank_x, wheel_y,
				1, 205, 120, 74, alpha);
			filledCircleRGBA(surface, crank_x, wheel_y - wheel_radius * 6,
				wheel_radius / 2 + 1, 224, 194, 151, alpha);
		} else if(kind == 26) {
			for(int creature = 0; creature < 5; creature++) {
				int creature_x = center_x + (creature - 2) * size * 2;
				int creature_y = center_y + (int)(sin(progress * 6.28 + creature) * size * 2);
				thickLineRGBA(surface, creature_x - size / 2, creature_y,
					creature_x, creature_y - size / 2, 1, 25, 30, 36, alpha);
				thickLineRGBA(surface, creature_x, creature_y - size / 2,
					creature_x + size / 2, creature_y, 1, 25, 30, 36, alpha);
			}
			int fish_y = surface->h * 61 / 100
				- (int)(sin(progress * 3.14159265359) * scale * 0.14);
			filledTrigonRGBA(surface, center_x + size * 2, fish_y,
				center_x - size, fish_y - size, center_x - size, fish_y + size,
				105, 187, 203, alpha);
			filledTrigonRGBA(surface, center_x - size, fish_y - size,
				center_x - size * 2, fish_y, center_x - size, fish_y + size,
				84, 157, 176, alpha);
		} else if(kind == 27) {
			for(int cube = 0; cube < 4; cube++) {
				int cube_x = center_x + (cube - 2) * size * 2;
				int cube_y = center_y + (int)(sin(progress * 6.28 + cube) * size * 2);
				int half = size / 2 + 2;
				double rotation = progress * 6.28318530718 + cube * 0.9;
				int offset_x = (int)(cos(rotation) * half);
				int offset_y = (int)(sin(rotation) * half);
				int top_x = cube_x + offset_x;
				int top_y = cube_y - offset_y;
				int right_x = cube_x + offset_y;
				int right_y = cube_y + offset_x;
				int bottom_x = cube_x - offset_x;
				int bottom_y = cube_y + offset_y;
				int left_x = cube_x - offset_y;
				int left_y = cube_y - offset_x;
				filledTrigonRGBA(surface, top_x, top_y,
					right_x, right_y, bottom_x, bottom_y,
					155, 214, 237, (Uint8)(alpha * 0.38));
				filledTrigonRGBA(surface, top_x, top_y,
					left_x, left_y, bottom_x, bottom_y,
					200, 230, 241, (Uint8)(alpha * 0.28));
				thickLineRGBA(surface, top_x, top_y, bottom_x, bottom_y,
					1, 224, 245, 251, alpha);
				filledCircleRGBA(surface, top_x, top_y, 1, 255, 255, 255, alpha);
			}
		} else if(kind == 28) {
			for(int card = 0; card < 3; card++) {
				int card_x = center_x + (card - 1)
					* (int)(size * (2.0 + 0.3 * sin(progress * 6.28318530718 + card)));
				int card_y = center_y - abs(card - 1) * size / 2
					+ (int)(cos(progress * 6.28318530718 + card) * size / 3);
				int card_half_width = (int)(size * (0.18
					+ 0.32 * fabs(cos(progress * 6.28318530718 + card))));
				if(card_half_width < 2) card_half_width = 2;
				boxRGBA(surface, card_x - card_half_width, card_y - size,
					card_x + card_half_width, card_y + size, 225, 220, 203, alpha);
				thickLineRGBA(surface, card_x, card_y - size / 2,
					card_x, card_y + size / 2, 1, 163, 57, 64, alpha);
			}
			int coin_radius = size / 2 + 2;
			int coin_x = center_x + (int)(sin(progress * 12.56) * size * 4);
			int coin_y = center_y - size * 2 - (int)(fabs(sin(progress * 12.56)) * size * 3);
			filledCircleRGBA(surface, coin_x, coin_y, coin_radius, 205, 161, 83, alpha);
			circleRGBA(surface, coin_x, coin_y, coin_radius - 1, 255, 226, 159, alpha);
		} else if(kind == 29) {
			boxRGBA(surface, 0, 0, surface->w - 1, surface->h - 1,
				82, 17, 29, (Uint8)(15 * fade));
			unsigned int bolt_seed = seed;
			int bolt_x = center_x;
			int bolt_y = surface->h * 12 / 100;
			for(int bolt = 0; bolt < 8; bolt++) {
				bolt_seed = bolt_seed * 1664525u + 1013904223u;
				int next_x = bolt_x + (int)(bolt_seed % (scale / 12 + 1)) - scale / 24;
				int next_y = bolt_y + scale / 15;
				thickLineRGBA(surface, bolt_x, bolt_y, next_x, next_y,
					2, 232, 67, 83, alpha);
				bolt_x = next_x;
				bolt_y = next_y;
			}
		} else if(kind == 30) {
			for(int cup = 0; cup < 3; cup++) {
				int cup_x = center_x + (cup - 1) * size * 3;
				int cup_y = center_y + abs(cup - 1) * size / 3;
				boxRGBA(surface, cup_x - size, cup_y - size,
					cup_x + size, cup_y + size, 211, 204, 190, alpha);
				filledCircleRGBA(surface, cup_x, cup_y - size, size / 2,
					(Uint8)(105 + cup * 33), (Uint8)(59 + cup * 21),
					(Uint8)(36 + cup * 19), alpha);
				circleRGBA(surface, cup_x + size, cup_y, size / 2,
					211, 204, 190, alpha);
				for(int steam = 0; steam < 3; steam++) {
					int steam_x = cup_x - size / 2 + steam * size / 2;
					int steam_y = cup_y - size * 2
						- (int)(fabs(sin(progress * 8.0 + steam)) * size);
					thickLineRGBA(surface, steam_x, steam_y + size,
						steam_x + size / 4, steam_y, 1, 206, 206, 198,
						(Uint8)(alpha * 0.65));
				}
			}
		} else if(kind == 31) {
			for(int sprig = 0; sprig < 5; sprig++) {
				int stem_x = center_x + (sprig - 2) * size;
				int stem_y = center_y + size * 2;
				int tip_y = stem_y - size * (3 + sprig % 3);
				int sway = (int)(sin(ticks / 330.0 + sprig * 1.4) * size / 5);
				thickLineRGBA(surface, stem_x, stem_y, stem_x + sway, tip_y,
					2, 92, 130, 73, alpha);
				for(int leaf = 1; leaf <= 4; leaf++) {
					int leaf_y = stem_y - leaf * size * 3 / 4;
					int leaf_direction = leaf % 2 ? -1 : 1;
					int leaf_x = stem_x + sway * leaf / 4;
					int leaf_tip_x = leaf_x + leaf_direction * size;
					filledTrigonRGBA(surface, leaf_x, leaf_y,
						leaf_tip_x, leaf_y - size / 3,
						leaf_tip_x, leaf_y + size / 3,
						(Uint8)(74 + sprig * 6), (Uint8)(126 + leaf * 9), 71, alpha);
				}
			}
		} else if(kind == 32) {
			for(int light = 0; light < 18; light++) {
				int light_x = (seed + light * surface->w / 18) % surface->w;
				int light_y = surface->h * (14 + (light * 17 + seed) % 62) / 100;
				Uint8 pulse = (Uint8)(alpha * (0.35 + 0.65
					* (0.5 + 0.5 * sin(progress * 18.0 + light))));
				filledCircleRGBA(surface, light_x, light_y, 2 + light % 3,
					(Uint8)(90 + light * 7), (Uint8)(129 + light * 5),
					(Uint8)(176 + light * 3), pulse);
			}
		} else if(kind == 33) {
			int burner_y = surface->h * 78 / 100;
			filledTrigonRGBA(surface, center_x, burner_y - size,
				center_x - size, burner_y + size / 2,
				center_x + size, burner_y + size / 2, 104, 87, 67, alpha);
			for(int curl = 0; curl < 4; curl++) {
				int smoke_x = center_x + (int)(sin(progress * 7.0 + curl) * size * 2);
				int smoke_y = burner_y - size * (2 + curl)
					- (int)(progress * size * (curl + 1));
				filledCircleRGBA(surface, smoke_x, smoke_y, size / 3 + 1,
					(Uint8)(151 + curl * 12), (Uint8)(159 + curl * 10),
					(Uint8)(166 + curl * 8), (Uint8)(alpha * (0.45 - curl * 0.07)));
			}
		} else if(kind == 34) {
			for(int wisp = 0; wisp < 7; wisp++) {
				int smoke_x = center_x + (int)(sin(progress * 5.0 + wisp) * size * (2 + wisp));
				int smoke_y = center_y - wisp * size / 2;
				filledCircleRGBA(surface, smoke_x, smoke_y, size / 2 + 1,
					(Uint8)(143 + wisp * 9), (Uint8)(150 + wisp * 8),
					(Uint8)(162 + wisp * 7), (Uint8)(alpha * 0.42));
			}
		} else if(kind == 35) {
			int temple_y = surface->h * 71 / 100;
			for(int step = 0; step < 5; step++) {
				int half_width = size * (5 - step);
				int top_y = temple_y - step * size / 2;
				boxRGBA(surface, center_x - half_width, top_y - size / 2,
					center_x + half_width, top_y + size / 2,
					(Uint8)(116 + step * 12), (Uint8)(102 + step * 8), 83, alpha);
			}
			for(int ray = 0; ray < 12; ray++) {
				double angle = ray * 0.52359877559 + progress;
				int end_x = center_x + (int)(cos(angle) * size * 5);
				int end_y = surface->h * 32 / 100 + (int)(sin(angle) * size * 3);
				thickLineRGBA(surface, center_x, surface->h * 32 / 100,
					end_x, end_y, 1, 218, 183, 120, (Uint8)(alpha * 0.45));
			}
		} else if(kind == 36) {
			boxRGBA(surface, 0, 0, surface->w - 1, surface->h - 1,
				24, 12, 39, (Uint8)(20 * fade));
			int eye_x = center_x;
			int eye_y = surface->h * 36 / 100;
			int eye_radius = size * 2;
			filledCircleRGBA(surface, eye_x, eye_y, eye_radius, 5, 4, 14, alpha);
			circleRGBA(surface, eye_x, eye_y, eye_radius + size,
				117, 89, 161, (Uint8)(alpha * 0.8));
			for(int tendril = 0; tendril < 8; tendril++) {
				double angle = tendril * 0.78539816339 + progress * direction;
				int previous_x = eye_x + (int)(cos(angle) * eye_radius);
				int previous_y = eye_y + (int)(sin(angle) * eye_radius);
				for(int segment = 1; segment <= 5; segment++) {
					int next_x = eye_x + (int)(cos(angle) * (eye_radius + segment * size));
					int next_y = eye_y + (int)(sin(angle) * (eye_radius + segment * size)
						+ sin(progress * 10.0 + segment + tendril) * size / 2);
					thickLineRGBA(surface, previous_x, previous_y, next_x, next_y,
						2, 83, 68, 127, (Uint8)(alpha * 0.72));
					previous_x = next_x;
					previous_y = next_y;
				}
			}
		} else if(kind == 37) {
			int mirror_x = surface->w * (30 + (seed >> 8) % 40) / 100;
			int mirror_y = surface->h * 43 / 100;
			int mirror_width = size * 6;
			int mirror_height = size * 10;
			boxRGBA(surface, mirror_x - mirror_width / 2, mirror_y - mirror_height / 2,
				mirror_x + mirror_width / 2, mirror_y + mirror_height / 2,
				37, 55, 76, (Uint8)(alpha * 0.45));
			thickLineRGBA(surface, mirror_x - mirror_width / 2, mirror_y - mirror_height / 2,
				mirror_x + mirror_width / 2, mirror_y - mirror_height / 2,
				2, 205, 221, 231, alpha);
			thickLineRGBA(surface, mirror_x - mirror_width / 2, mirror_y + mirror_height / 2,
				mirror_x + mirror_width / 2, mirror_y + mirror_height / 2,
				2, 205, 221, 231, alpha);
			for(int reflection = 0; reflection < 8; reflection++) {
				int line_y = mirror_y - mirror_height / 2 + reflection * mirror_height / 7;
				int ripple = (int)(sin(progress * 12.0 + reflection) * size / 2);
				thickLineRGBA(surface, mirror_x - mirror_width / 2 + size / 3 + ripple,
					line_y, mirror_x + mirror_width / 2 - size / 3 - ripple,
					line_y + size / 4, 1, 150, 196, 218,
					(Uint8)(alpha * (0.45 + reflection * 0.04)));
			}
			int reflected_sun_y = mirror_y + mirror_height / 3
				+ (int)(sin(progress * 8.0) * size / 2);
			filledCircleRGBA(surface, mirror_x, reflected_sun_y, size / 2,
				226, 218, 188, (Uint8)(alpha * 0.72));
		} else if(kind == 38) {
			int water_y = surface->h * 77 / 100;
			for(int wave = 0; wave < 9; wave++) {
				int previous_x = 0;
				int previous_y = water_y + wave * size / 2;
				for(int segment = 1; segment <= 16; segment++) {
					int next_x = surface->w * segment / 16;
					int next_y = water_y + wave * size / 2
						+ (int)(sin(segment * 0.75 + progress * 10.0 + wave) * size / 3);
					thickLineRGBA(surface, previous_x, previous_y, next_x, next_y,
						1, 101, 156, 190, (Uint8)(alpha * 0.55));
					previous_x = next_x;
					previous_y = next_y;
				}
			}
			for(int glint = 0; glint < 14; glint++) {
				int glint_x = (seed + glint * surface->w / 14) % surface->w;
				int glint_y = water_y + (int)(sin(progress * 8.0 + glint) * size * 2);
				thickLineRGBA(surface, glint_x - size / 3, glint_y,
					glint_x + size / 3, glint_y, 1,
					192, 222, 231, (Uint8)(alpha * 0.65));
			}
		} else if(kind == 39) {
			int orbit_x = center_x;
			int orbit_y = center_y;
			int orbit_radius = size * 5;
			ellipseRGBA(surface, orbit_x, orbit_y, orbit_radius,
				orbit_radius / 2, 137, 172, 201, (Uint8)(alpha * 0.6));
			for(int satellite = 0; satellite < 5; satellite++) {
				double angle = progress * 12.56637061436 + satellite * 1.25663706144;
				int satellite_x = orbit_x + (int)(cos(angle) * orbit_radius);
				int satellite_y = orbit_y + (int)(sin(angle) * orbit_radius / 2);
				filledCircleRGBA(surface, satellite_x, satellite_y,
					2 + satellite % 3, 180, 204, 221, alpha);
			}
			filledCircleRGBA(surface, orbit_x, orbit_y, size,
				(Uint8)(180 + (int)(35 * sin(progress * 12.56))), 141, 215, alpha);
		} else if(kind == 40) {
			int plane_y = center_y + (int)(sin(progress * 6.28318530718) * size * 2);
			int nose_x = center_x + direction * size * 3;
			int tail_x = center_x - direction * size * 3;
			thickLineRGBA(surface, tail_x, plane_y, nose_x, plane_y,
				3, 140, 153, 164, alpha);
			thickLineRGBA(surface, center_x - direction * size,
				plane_y - size / 5, center_x + direction * size,
				plane_y + size / 5, 2, 170, 182, 191, alpha);
			filledTrigonRGBA(surface, tail_x + direction * size,
				plane_y, tail_x - direction * size, plane_y - size,
				tail_x - direction * size, plane_y + size,
				126, 141, 154, alpha);
			if((phase / 240) % 2 == 0) {
				filledCircleRGBA(surface, center_x - size * 2, plane_y,
					2, 255, 54, 45, alpha);
				filledCircleRGBA(surface, center_x + size * 2, plane_y,
					2, 255, 54, 45, alpha);
			}
		} else if(kind == 41) {
			int planet_x = surface->w / 2;
			int planet_y = surface->h * 66 / 100;
			int satellite_x = planet_x + (int)(cos(progress * 6.28318530718) * scale * 0.22);
			int satellite_y = planet_y + (int)(sin(progress * 6.28318530718) * scale * 0.11);
			filledCircleRGBA(surface, planet_x, planet_y, scale / 12,
				32, 46, 65, (Uint8)(alpha * 0.6));
			int panel_width = size * 2;
			boxRGBA(surface, satellite_x - panel_width * 2, satellite_y - size / 3,
				satellite_x - size, satellite_y + size / 3,
				79, 133, 163, alpha);
			boxRGBA(surface, satellite_x + size, satellite_y - size / 3,
				satellite_x + panel_width * 2, satellite_y + size / 3,
				79, 133, 163, alpha);
			boxRGBA(surface, satellite_x - size, satellite_y - size / 2,
				satellite_x + size, satellite_y + size / 2,
				151, 164, 170, alpha);
			if((phase / 300) % 2 == 0) {
				filledCircleRGBA(surface, satellite_x, satellite_y,
					2, 255, 48, 41, alpha);
			}
		} else {
			int blimp_y = center_y + (int)(sin(progress * 6.28318530718) * size);
			int blimp_length = size * 5;
			for(int section = -2; section <= 2; section++) {
				int section_radius = size * (3 - abs(section)) / 2;
				if(section_radius < 2) section_radius = 2;
				filledCircleRGBA(surface, center_x + section * size,
					blimp_y, section_radius, 165, 169, 158, alpha);
			}
			thickLineRGBA(surface, center_x - blimp_length / 3,
				blimp_y + size / 2, center_x + blimp_length / 3,
				blimp_y + size / 2, 2, 100, 103, 99, alpha);
			filledTrigonRGBA(surface, center_x + direction * blimp_length / 2,
				blimp_y, center_x + direction * blimp_length / 2 - direction * size,
				blimp_y - size, center_x + direction * blimp_length / 2 - direction * size,
				blimp_y + size, 127, 137, 133, alpha);
			if((phase / 360) % 2 == 0) {
				filledCircleRGBA(surface, center_x, blimp_y,
					2, 255, 48, 41, alpha);
			}
		}
		return;
	}

	switch(kind % 4) {
		case 0: {
			int pulse_radius = size + (int)(progress * size * 2);
			filledCircleRGBA(surface, x, y, size / 2 + 1,
				245, 233, 207, alpha);
			circleRGBA(surface, x, y, pulse_radius,
				162, 193, 213, (Uint8)(alpha * 0.72));
			for(int ray = 0; ray < 8; ray++) {
				double angle = ray * 0.78539816339 + progress;
				int ray_x = x + (int)(cos(angle) * pulse_radius * 1.7);
				int ray_y = y + (int)(sin(angle) * pulse_radius * 1.7);
				thickLineRGBA(surface, x, y, ray_x, ray_y, 1,
					180, 203, 220, (Uint8)(alpha * 0.35));
			}
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

static void render_meteor(SDL_Surface *surface, Uint32 ticks, int scale) {
	const Uint32 flight_duration = 1600;
	const Uint32 impact_duration = 350;
	const Uint32 event_duration = flight_duration + impact_duration;
	Uint32 period = ticks % 19000;
	if(period >= event_duration) return;

	unsigned int seed = ticks / 19000 * 2654435761u + 0x9e3779b9u;
	int start_x = (int)(surface->w * (0.16 + (seed % 58) / 100.0));
	int start_y = (int)(surface->h * (0.07 + (seed >> 8) % 25 / 100.0));
	int impact_x = start_x + surface->w / 5;
	if(impact_x >= surface->w) impact_x = surface->w - 1;
	double impact_nx = (double)impact_x / surface->w;
	double ridge = 0.88 + (smooth_noise(impact_nx * 8.0 - 3.0, 3.0, 1123) - 0.5) * 0.09;
	int impact_y = (int)(surface->h * ridge);
	Uint32 flight_tick = period < flight_duration ? period : flight_duration;
	double progress = (double)flight_tick / flight_duration;
	int meteor_x = start_x + (int)((impact_x - start_x) * progress);
	int meteor_y = start_y + (int)((impact_y - start_y) * progress);
	Uint8 alpha = (Uint8)(150 * smooth_step(0.0, 100.0, period)
		* (1.0 - smooth_step(flight_duration + 200.0, event_duration, period)));

	if(period < flight_duration) {
		double dx = impact_x - start_x;
		double dy = impact_y - start_y;
		double distance = sqrt(dx * dx + dy * dy);
		int tail_x = meteor_x - (int)(dx / distance * scale / 7.0);
		int tail_y = meteor_y - (int)(dy / distance * scale / 7.0);
		int glow_width = scale / 150;
		if(glow_width < 3) glow_width = 3;
		int core_width = scale / 380;
		if(core_width < 1) core_width = 1;
		thickLineRGBA(surface, tail_x, tail_y, meteor_x, meteor_y,
			glow_width, 255, 151, 83, (Uint8)(alpha * 0.35));
		thickLineRGBA(surface, tail_x, tail_y, meteor_x, meteor_y,
			core_width, 255, 224, 178, alpha);
		filledCircleRGBA(surface, meteor_x, meteor_y, core_width + 1,
			255, 245, 218, alpha);
	} else {
		double impact_progress = (double)(period - flight_duration) / impact_duration;
		Uint8 impact_alpha = (Uint8)(210 * (1.0 - smooth_step(0.0, 1.0, impact_progress)));
		int radius = scale / 38;
		if(radius < 8) radius = 8;
		filledCircleRGBA(surface, impact_x, impact_y, radius,
			255, 162, 85, (Uint8)(impact_alpha * 0.45));
		filledCircleRGBA(surface, impact_x, impact_y, radius / 2,
			255, 233, 190, impact_alpha);
		thickLineRGBA(surface, impact_x - radius, impact_y,
			impact_x + radius, impact_y, 2, 255, 191, 119, impact_alpha);
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

static void render_foreground_event(SDL_Surface *surface, Uint32 ticks, int scale) {
	const Uint32 cycle = 180000;
	const Uint32 duration = 6500;
	Uint32 slot = ticks / cycle;
	Uint32 phase = ticks % cycle;
	Uint32 seed = slot * 2654435761u + 0x9e3779b9u;
	if(seed % 2 != 0 || phase >= duration) return;
	seed ^= seed >> 15;
	seed *= 2246822519u;
	seed ^= seed >> 13;
	int kind = seed % 4;
	double progress = (double)phase / duration;
	double fade = smooth_step(0.0, 650.0, phase)
		* (1.0 - smooth_step(duration - 900.0, duration, phase));
	int count = 24 + kind * 7;
	for(int particle = 0; particle < count; particle++) {
		Uint32 particle_seed = (Uint32)particle * 1664525u + seed;
		int start_x = particle_seed % surface->w;
		int start_y = (particle_seed >> 9) % surface->h;
		int speed = 3 + (particle_seed >> 20) % 8;
		int drift = (int)(ticks / (30 + speed * 7));
		int particle_x = (start_x + drift + surface->w) % surface->w;
		int particle_y = start_y;
		Uint8 red = 191, green = 204, blue = 215;
		Uint8 alpha = (Uint8)((18 + (particle_seed >> 24) % 32) * fade);
		if(kind == 0) {
			particle_y = (start_y + drift * 2) % surface->h;
			particle_x += (int)(sin(ticks / 500.0 + particle) * scale * 0.02);
			red = 207;
			green = (Uint8)(91 + particle % 5 * 18);
			blue = 54;
			thickLineRGBA(surface, particle_x, particle_y,
				particle_x - 2, particle_y + 4, 1, red, green, blue, alpha);
		} else if(kind == 1) {
			particle_y = (start_y - drift + surface->h) % surface->h;
			red = 245;
			green = (Uint8)(112 + particle % 4 * 18);
			blue = 67;
			filledCircleRGBA(surface, particle_x, particle_y, 1 + particle % 2,
				red, green, blue, alpha);
		} else if(kind == 2) {
			particle_y = (start_y + drift * 3) % surface->h;
			red = 167;
			green = 213;
			blue = 231;
			filledCircleRGBA(surface, particle_x, particle_y, 1 + particle % 2,
				red, green, blue, alpha);
		} else {
			particle_y = surface->h * (28 + particle_seed % 58) / 100
				- (int)(progress * scale * (0.025 + particle % 3 * 0.01));
			int radius = scale / 230;
			if(radius < 2) radius = 2;
			filledCircleRGBA(surface, particle_x, particle_y, radius,
				153, 167, 180, (Uint8)(alpha * 0.3));
		}
	}
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
	double tint_red = 0.0, tint_green = 0.0, tint_blue = 0.0;
	for(int i = 0; i < 4; i++) {
		tint_red += season_tint[i][0] * weather.season[i];
		tint_green += season_tint[i][1] * weather.season[i];
		tint_blue += season_tint[i][2] * weather.season[i];
	}
	double cloud_mix = weather.cloud * 0.55;
	tint_red = tint_red * (1.0 - cloud_mix) + 48 * cloud_mix;
	tint_green = tint_green * (1.0 - cloud_mix) + 59 * cloud_mix;
	tint_blue = tint_blue * (1.0 - cloud_mix) + 78 * cloud_mix;
	double palette_mix = 0.70 + weather.fog * 0.10 + weather.aurora * 0.10;
	tint_red = tint_red * (1.0 - palette_mix) + weather.palette[0] * palette_mix;
	tint_green = tint_green * (1.0 - palette_mix) + weather.palette[1] * palette_mix;
	tint_blue = tint_blue * (1.0 - palette_mix) + weather.palette[2] * palette_mix;
	double warmth = weather.sunshine * 0.24;
	tint_red = tint_red * (1.0 - warmth) + 235 * warmth;
	tint_green = tint_green * (1.0 - warmth) + 151 * warmth;
	tint_blue = tint_blue * (1.0 - warmth) + 78 * warmth;
	Uint8 tint_alpha = (Uint8)(19 + weather.cloud * 12 + weather.fog * 18
		+ weather.lightning * 12 + weather.aurora * 12 + weather.sunshine * 9);
	boxRGBA(surface, 0, 0, surface->w - 1, surface->h - 1,
		(Uint8)tint_red, (Uint8)tint_green, (Uint8)tint_blue, tint_alpha);
	render_fullscreen_event(surface, ticks, scale);
	render_weather_effects(surface, ticks, &weather, scale);
	render_alien_weather(surface, ticks, scale);
	render_world_jump(surface, ticks, scale);
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

	static const int bird_group_x[] = { 8, 36, 65, 91 };
	static const int bird_group_y[] = { 29, 43, 34, 48 };
	for(size_t group = 0; group < sizeof(bird_group_x) / sizeof(bird_group_x[0]); group++) {
		int bird_span = scale / 85;
		if(bird_span < 4) bird_span = 4;
		int travel = (int)((Uint64)ticks * surface->w / 90000);
		int group_x = (surface->w * bird_group_x[group] / 100 + travel)
			% (surface->w + bird_span * 5) - bird_span * 2;
		int group_y = surface->h * bird_group_y[group] / 100
			+ (int)(sin(ticks / 1700.0 + group * 2.1) * scale * 0.008);
		static const int formation_x[] = { 0, -3, -6, -9, -12, -15, -18, -21, -24, -27 };
		static const int formation_y[] = { 0, 3, -3, 5, -6, 1, 7, -4, 4, -1 };
		for(int bird = 0; bird < 10; bird++) {
			int bird_x = group_x + formation_x[bird] * bird_span;
			int bird_y = group_y + formation_y[bird] * bird_span;
			int flap = (int)((0.5 + 0.5 * sin(ticks / 260.0 + group * 1.8 + bird)) * (bird_span / 2));
			draw_bird(surface, bird_x, bird_y, bird_span, flap);
		}
	}

	render_background_battle(surface, ticks, scale);
	render_fleet_battle(surface, ticks, scale);
	render_rare_sky_event(surface, ticks, scale);
	render_long_scene(surface, ticks, scale);

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
	int scene_scale = screen->h < screen->w ? screen->h : screen->w;
	render_meteor(screen, SDL_GetTicks(), scene_scale);
	render_foreground_event(screen, SDL_GetTicks(), scene_scale);

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
