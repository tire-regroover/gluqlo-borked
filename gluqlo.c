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

#include <stdbool.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_syswm.h>
#include <SDL2/SDL2_gfxPrimitives.h>
#include <SDL2/SDL2_rotozoom.h>

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
static const int MAX_METEORS = 30;
static const int MAX_FIREFLIES = 100;
static const int MAX_SATELLITES = 5;
static bool profile_rendering = false;
static bool show_meteors = true;
static bool show_solar_flares = true;
static bool show_moon_phases = true;
static bool show_constellations = true;
static bool show_fireflies = true;
static bool show_ocean_waves = true;
static bool show_sandstorm = false;
static bool show_volcano = false;
static bool show_rainbow = false;
static bool show_satellites = true;

// Function declarations for visual effects
void render_meteor_shower(SDL_Renderer *renderer, Uint32 ticks, int scale);
void render_solar_flares(SDL_Renderer *renderer, Uint32 ticks, int scale);
void render_moon_phases(SDL_Renderer *renderer, Uint32 ticks, int scale);
void render_constellations(SDL_Renderer *renderer, Uint32 ticks, int scale);
void render_fireflies(SDL_Renderer *renderer, Uint32 ticks, int scale);
void render_ocean_waves(SDL_Renderer *renderer, Uint32 ticks, int scale);
void render_sandstorm(SDL_Renderer *renderer, Uint32 ticks, int scale);
void render_volcanic_eruption(SDL_Renderer *renderer, Uint32 ticks, int scale);
void render_rainbow(SDL_Renderer *renderer, Uint32 ticks, int scale);
void render_satellite_trails(SDL_Renderer *renderer, Uint32 ticks, int scale);

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
		{ 0.10, 0.00, 0.00, 0.95, 0.02, 0.08, 0.00, 0.00, 255, 173, 45 },
		{ 0.25, 0.00, 0.00, 0.72, 0.12, 0.15, 0.00, 0.00, 198, 47, 28 },
		{ 0.78, 0.00, 0.00, 0.10, 0.20, 0.18, 0.00, 0.00, 67, 92, 116 },
		{ 0.74, 0.36, 0.00, 0.12, 0.26, 0.34, 0.00, 0.00, 67, 112, 139 },
		{ 0.90, 0.52, 0.00, 0.02, 0.14, 0.38, 0.00, 0.00, 54, 104, 137 },
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

	Uint32 weather_slot = ticks / weather_duration;
	Uint32 current_seed = weather_slot * 2654435761u + 0x9e3779b9u;
	Uint32 previous_seed = (weather_slot - 1) * 2246822519u + 3266489917u;
	int current = current_seed % weather_count;
	int previous = previous_seed % weather_count;
	if(previous == current) {
		previous = (previous + 1 + (current_seed >> 8) % (weather_count - 1)) % weather_count;
	}
	Uint32 weather_phase = ticks % weather_duration;
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

static void render_weather_effects(SDL_Renderer *renderer, Uint32 ticks,
	const SceneWeather *weather, int scale) {
    // Existing weather effects...

    // Call new visual effects after existing weather effects
    render_meteor_shower(renderer, ticks, scale);
    render_solar_flares(renderer, ticks, scale);
    render_moon_phases(renderer, ticks, scale);
    render_constellations(renderer, ticks, scale);
    render_fireflies(renderer, ticks, scale);
    render_ocean_waves(renderer, ticks, scale);
    render_sandstorm(renderer, ticks, scale);
    render_volcanic_eruption(renderer, ticks, scale);
    render_rainbow(renderer, ticks, scale);
    render_satellite_trails(renderer, ticks, scale);
	if(weather->aurora > 0.01) {
		int width, height;
		SDL_GetRendererOutputSize(renderer, &width, &height);
		for(int band = 0; band < 3; band++) {
			int previous_x = 0;
			int previous_y = height * (19 + band * 12) / 100;
			for(int segment = 1; segment <= 24; segment++) {
				int x = width * segment / 24;
				double nx = (double)x / width;
				int y = height * (19 + band * 12) / 100
					+ (int)(sin(nx * 6.28318530718 + ticks / (1100.0 + band * 270)
						+ band * 1.9) * scale * (0.03 + band * 0.008));
				Uint8 alpha = (Uint8)(weather->aurora * (28 - band * 5));
				thickLineRGBA(renderer, previous_x, previous_y, x, y, 2,
					(Uint8)(70 + band * 24), (Uint8)(164 + band * 18),
					(Uint8)(137 + band * 30), alpha);
				previous_x = x;
				previous_y = y;
			}
		}
	}

	if(weather->fog > 0.01) {
		int width, height;
		SDL_GetRendererOutputSize(renderer, &width, &height);
		for(int band = 0; band < 3; band++) {
			int y = height * (57 + band * 12) / 100
				+ (int)(sin(ticks / (2900.0 + band * 530)) * scale * 0.012);
			int h = scale / (30 + band * 8);
			if(h < 1) h = 1;
			Uint8 alpha = (Uint8)(weather->fog * (21 - band * 4));
			boxRGBA(renderer, 0, y, width - 1, y + h,
				150, 177, 181, alpha);
		}
	}
int width, height;
		SDL_GetRendererOutputSize(renderer, &width, &height);
		Uint32 phase = ticks % 6400;
		double flash = 0.0;
		if(phase < 220) flash = 1.0 - phase / 220.0;
		else if(phase >= 390 && phase < 530) flash = 0.55 * (1.0 - (phase - 390) / 140.0);
		if(flash > 0.0) {
			Uint8 alpha = (Uint8)(weather->lightning * 72 * flash);
			boxRGBA(renderer, 0, 0, width - 1, height - 1,
				188, 205, 225, alpha);
			unsigned int seed = ticks / 7200 * 2654435761u + 0x9e3779b9u;
			int x = width * (18 + seed % 64) / 100;
			int y = height * 27 / 100;
			for(int segment = 0; segment < 7; segment++) {
				int next_x = x + (int)((seed >> (segment % 16)) % (scale / 16 + 1))
					- scale / 32;
				int next_y = y + scale / 24;
				thickLineRGBA(renderer, x, y, next_x, next_y, 2,
					207, 220, 244, (Uint8)(alpha * 0.9));
				if(segment == 3) {
					thickLineRGBA(renderer)(alpha * 0.9));
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

static void render_constellations(SDL_Renderer *renderer, Uint32 ticks, int scale) {
    if(!show_constellations) return;

    static const struct {
        const char *name;
        int stars[20]; // x1,y1,x2,y2,... pairs
    } constellations[] = {
        {"Ursa Major", {50,20, 55,25, 55,25, 60,30, 60,30, 65,25, 65,25, 70,30}},
        {"Orion", {30,50, 35,55, 35,55, 40,50, 40,50, 45,55, 45,55, 50,50}},
        {"Cassiopeia", {70,30, 75,35, 75,35, 80,30, 80,30, 85,35}}
    };

    int width, height;
    SDL_GetRendererOutputSize(renderer, &width, &height);

    for(int c = 0; c < 3; c++) {
        for(int s = 0; s < 10 && constellations[c].stars[s*2] != 0; s++) {
            int x1 = width * constellations[c].stars[s*2] / 100;
            int y1 = height * constellations[c].stars[s*2+1] / 100;
            int x2 = width * constellations[c].stars[s*2+2] / 100;
            int y2 = height * constellations[c].stars[s*2+3] / 100;

            aalineRGBA(renderer, x1, y1, x2, y2, 200, 200, 255, 120);
            filledCircleRGBA(renderer, x1, y1, 2, 255, 255, 255, 180);
            filledCircleRGBA(renderer, x2, y2, 2, 255, 255, 255, 180);
        }
    }
}

static void render_fireflies(SDL_Renderer *renderer, Uint32 ticks, int scale) {
    if(!show_fireflies) return;

    const int count = 30;
    static int positions[count][2];
    static int speeds[count];
    static bool initialized = false;

    int width, height;
    SDL_GetRendererOutputSize(renderer, &width, &height);

    if(!initialized) {
        for(int i = 0; i < count; i++) {
            positions[i][0] = rand() % width;
            positions[i][1] = height/2 + rand() % (height/2);
            speeds[i] = 3 + rand() % 5;
        }
        initialized = true;
    }

    for(int i = 0; i < count; i++) {
        // Update position
        positions[i][0] = (positions[i][0] + speeds[i]) % width;

        // Flicker effect
        double brightness = 0.5 + 0.5 * sin(ticks/200.0 + i*100);

        // Draw firefly
        filledCircleRGBA(renderer, positions[i][0], positions[i][1],
                        2, 255, 255, 100, (Uint8)(brightness * 200));
    }
}

static void render_ocean_waves(SDL_Renderer *renderer, Uint32 ticks, int scale) {
    if(!show_ocean_waves) return;

    int width, height;
    SDL_GetRendererOutputSize(renderer, &width, &height);

    int horizon = height * 70 / 100;

    for(int y = horizon; y < height; y++) {
        double wave_height = 5.0 * sin(ticks/500.0 + y/20.0);
        int wave_y = y + (int)wave_height;

        Uint8 blue = 50 + (y - horizon) * 2;
        Uint8 alpha = 100 + (height - y) * 100 / (height - horizon);

        for(int x = 0; x < width; x++) {
            if(rand() % 100 < 5) { // Sparkles
                pixelRGBA(renderer, x, wave_y, 255, 255, 255, 150);
            }
        }
        hlineRGBA(renderer, 0, width-1, wave_y, 30, 90, blue, alpha);
    }
}

static void render_sandstorm(SDL_Renderer *renderer, Uint32 ticks, int scale) {
    if(!show_sandstorm) return;

    const int particles = 200;
    static int positions[particles][2];
    static bool initialized = false;

    int width, height;
    SDL_GetRendererOutputSize(renderer, &width, &height);

    if(!initialized) {
        for(int i = 0; i < particles; i++) {
            positions[i][0] = rand() % width;
            positions[i][1] = rand() % height;
        }
        initialized = true;
    }

    for(int i = 0; i < particles; i++) {
        positions[i][0] = (positions[i][0] + 5) % width;
        positions[i][1] = (positions[i][1] + 2) % height;

        pixelRGBA(renderer, positions[i][0], positions[i][1],
                 210, 180, 100, 120);
    }
}

static void render_volcanic_eruption(SDL_Renderer *renderer, Uint32 ticks, int scale) {
    if(!show_volcano) return;

    const Uint32 duration = 30000;
    Uint32 phase = ticks % duration;

    if(phase < 5000 || phase > 25000) return; // Dormant periods

    double intensity = (phase < 10000) ?
        (phase - 5000) / 5000.0 :
        (1.0 - (phase - 10000) / 15000.0);

    int width, height;
    SDL_GetRendererOutputSize(renderer, &width, &height);

    // Volcano base
    int base_x = width * 20 / 100;
    int base_y = height * 80 / 100;

    // Lava flow
    for(int i = 0; i < 5; i++) {
        int flow_y = base_y - i * 20;
        int flow_width = (int)(scale * 0.1 * intensity);

        boxRGBA(renderer, base_x - flow_width, flow_y,
               base_x + flow_width, flow_y + 10,
               255, 100 + i*20, 50, (Uint8)(intensity * 200));
    }

    // Eruption particles
    for(int i = 0; i < 50 * intensity; i++) {
        int part_x = base_x + (rand() % 50) - 25;
        int part_y = base_y - (rand() % (int)(200 * intensity));

        filledCircleRGBA(renderer, part_x, part_y, 2 + rand()%3,
                        255, 150, 100, (Uint8)(intensity * 180));
    }
}

static void render_rainbow(SDL_Renderer *renderer, Uint32 ticks, int scale) {
    if(!show_rainbow) return;

    const Uint32 duration = 60000;
    Uint32 phase = ticks % duration;

    if(phase > 30000) return; // Only show after rain

    double progress = phase / 30000.0;

    int width, height;
    SDL_GetRendererOutputSize(renderer, &width, &height);

    int center_x = width / 2;
    int center_y = height + scale * 0.3;

    const Uint8 colors[7][3] = {
        {255, 0, 0}, {255, 127, 0}, {255, 255, 0},
        {0, 255, 0}, {0, 0, 255}, {75, 0, 130}, {148, 0, 211}
    };

    for(int i = 0; i < 7; i++) {
        int radius = scale * (0.5 + i * 0.05);

        // Draw rainbow using multiple arcs
        for(int angle = 180; angle < 360; angle += 5) {
            aacircleRGBA(renderer, center_x, center_y, radius,
                       colors[i][0], colors[i][1], colors[i][2],
                       (Uint8)((1.0 - progress) * 200));
        }
    }
}

static void render_satellite_trails(SDL_Renderer *renderer, Uint32 ticks, int scale) {
    if(!show_satellites) return;

    const int count = 3;
    snt width, height;
    SDL_GetRendererOutputSize(renderer, &width, &height);

    if(!initialized) {
        for(int i = 0; i < count; i++) {
            positions[i][0] = rand() % width;
            positions[i][1] = rand() % (height/3);
            speeds[i] = 1 + rand() % 3;
        }
        initialized = true;
    }

    for(int i = 0; i < count; i++) {
        positions[i][0] = (positions[i][0] + speeds[i]) % width;

        // Draw trail
        for(int j = 0; j < 5; j++) {
            int trail_x = (positions[i][0] - j * speeds[i] + width) % width;
            int trail_y = positions[i][1] + j * 2;

            if(trail_y < height) {
                pixelRGBA(rendererons[i][0] - j * speeds[i] + surface->w) % surface->w;
            int trail_y = positions[i][1] + j * 2;

            if(trail_y < surface->h) {
                pixelRGBA(surface, trail_x, trail_y,
                         150, 150, 255, 100 - j*20);
            }
        }
    }
}

