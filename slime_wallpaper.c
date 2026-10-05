/*
 * slime_wallpaper.c  -  slime mold simulation as a Wayland live wallpaper
 *
 * How it works:
 *   - No raylib. The sim writes pixels into a wl_shm buffer (plain CPU memory).
 *   - A wlr-layer-shell surface on the BACKGROUND layer puts that buffer
 *     behind all windows, covering the whole output.
 *   - wp_viewporter tells the compositor how big the surface is. When the
 *     buffer is the same size as the screen (GRIDSIZE 1) it is shown 1:1.
 *   - Frames are driven by wl_surface.frame callbacks.
 *
 * Works on compositors that implement wlr-layer-shell: Hyprland, Sway, river,
 * niri, labwc, KDE Plasma, COSMIC, ...   NOT GNOME (Mutter).
 *
 * ============================================================================
 *  CHANGES FOR A CRISP FULL-RESOLUTION WALLPAPER  (search for "[CHANGE")
 * ============================================================================
 *  [CHANGE 1] GRIDSIZE 2 -> 1. One sim cell is now one screen pixel, so the
 *             buffer is 1920x1080 and is shown 1:1. Before, a 960x540 buffer
 *             was stretched 2x by the compositor, which made it blurry.
 *  [CHANGE 2] Cells are now plain byte arrays instead of an array of structs.
 *             Needed because 1920x1080 = ~2 million cells (4x more than a
 *             GRIDSIZE 2 grid on the same screen).
 *  [CHANGE 3] Faster blur: 3x3 box blur done as two 1-D passes, with no bounds
 *             checks in the hot loop. Same result as before in the interior
 *             and at the edges, ~5x less work per cell.
 *  [CHANGE 4] Color lookup table (lut) instead of three multiplies per pixel.
 *  [CHANGE 5] Agents now deposit trail along their whole step, not just at
 *             the start. With 1-pixel cells and speeds of 3-6 px per frame,
 *             depositing one point per step would draw dotted lines.
 *  [CHANGE 6] (Makefile) -march=native so the compiler can use your CPU's
 *             SIMD instructions in the blur loops.
 * ============================================================================
 */

#define _GNU_SOURCE
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* ------------------------------------------------------------------ */
/* Tunables (same meaning as in your raylib version)                   */
/* ------------------------------------------------------------------ */
#define FPS_LIMIT        60        /* try 30 to halve CPU use */

/* [CHANGE 1] Was 2. 1 = one cell per screen pixel = crisp 1920x1080.
 * If your CPU struggles, set it back to 2 (lighter, but chunkier/softer). */
#define GRIDSIZE         1

#define AGENTS_REF       20000      /* agents for a 1200x800 screen ...            */
#define REF_W            1200      /* ... scaled by screen area so density stays  */
#define REF_H            800       /*     the same on any resolution              */
#define SENSEDISTANCE    16
#define DIFFUSE_WEIGHT   0.33f
#define DECAY_RATE       0.11f
#define TURNSPEED        0.2f
#define PI_F             3.14159265358979f

/* Species colors (R, G, B) */
static const uint8_t colors[2][3] = {
	{  30, 150, 255 },   /* blue */
	{   0, 255, 255 },   /* cyan */
};

/* ------------------------------------------------------------------ */
/* Simulation (ported from your example2, raylib removed)              */
/* ------------------------------------------------------------------ */
typedef struct {
	float x, y;
	float speed;
	float angle;
	unsigned int num;     /* was `char` before: overflowed past 127 agents */
	int colorid;
} Agent;

static int W, H;                 /* surface size in pixels (set by compositor) */
static int GW, GH;               /* grid size in cells */

/* [CHANGE 2] The old `Cell { int a; int colorindex; }` array is split into
 * separate byte planes. 1 byte per cell instead of 8 means far less memory
 * traffic, and lets the compiler vectorize the loops below.                   */
static uint8_t  *trail;          /* trail intensity 0..255, index = y*GW + x   */
static uint8_t  *trail_next;     /* double buffer for the blur                 */
static uint8_t  *species;        /* which species last wrote the cell (0 / 1)  */
static uint16_t *hsum;           /* scratch: horizontal 3-sums for the blur    */

/* [CHANGE 4] lut[species][intensity] -> finished XRGB8888 pixel. Built once. */
static uint32_t lut[2][256];

static Agent *agents;
static int agent_count;
static unsigned int tick;

/* Hash function www.cs.ubc.ca/~rbridson/docs/schechter-sca08-turbulence.pdf */
static float hash_rand(unsigned int state){
	state ^= 2747636419u;
	state *= 2654435769u;
	state ^= state >> 30;
	state *= 2654435769u;
	state ^= state >> 30;
	state *= 2654435769u;
	return state / 4294967295.0f;
}

/* replacement for raylib's GetRandomValue(lo, hi) */
static int rand_range(int lo, int hi){
	static unsigned int counter = 1;
	float r = hash_rand(counter++ * 2246822519u + (unsigned int)time(NULL));
	int v = lo + (int)(r * (float)(hi - lo + 1));
	return v > hi ? hi : v;
}

static void sim_free(void){
	free(trail); free(trail_next); free(species); free(hsum); free(agents);
	trail = trail_next = species = NULL;
	hsum = NULL;
	agents = NULL;
}

static void sim_init(int w, int h){
	W = w; H = h;
	GW = W / GRIDSIZE;
	GH = H / GRIDSIZE;

	trail      = calloc((size_t)GW * GH, 1);
	trail_next = calloc((size_t)GW * GH, 1);
	species    = calloc((size_t)GW * GH, 1);
	hsum       = calloc((size_t)GW * GH, sizeof(uint16_t));

	/* [CHANGE 4] precompute every possible pixel color once */
	for (int s = 0; s < 2; s++){
		for (int a = 0; a < 256; a++){
			lut[s][a] = ((uint32_t)(colors[s][0] * a / 255) << 16) |
			            ((uint32_t)(colors[s][1] * a / 255) << 8)  |
			             (uint32_t)(colors[s][2] * a / 255);
		}
	}

	agent_count = (int)((float)AGENTS_REF * ((float)W * H) / ((float)REF_W * REF_H));
	if (agent_count < 500) agent_count = 500;
	agents = calloc(agent_count, sizeof(Agent));

	for (int x = 0; x < agent_count; x++){
		agents[x].num   = x;
		agents[x].angle = (2.0f * PI_F * x) / agent_count;
		agents[x].x = rand_range(W / 2 - 30, W / 2 + 30);
		agents[x].y = rand_range(H / 2 - 30, H / 2 + 30);
		agents[x].speed = 3.0f + hash_rand((unsigned int)x * 747796405u + 2891336453u) * 3.0f;
		agents[x].colorid = rand_range(0, 1);
	}
}

static float sense(float angle, const Agent *agent){
	int x = agent->x + cosf(agent->angle + angle) * SENSEDISTANCE;
	int y = agent->y + sinf(agent->angle + angle) * SENSEDISTANCE;
	x /= GRIDSIZE;
	y /= GRIDSIZE;
	if (x >= 0 && x < GW && y >= 0 && y < GH)
		return trail[y * GW + x];
	return 0;
}

/* [CHANGE 3] One tiny inline helper for the blend that used to live in the main loop. */
static inline uint8_t blend(uint8_t a, int blurred){
	float b = a * (1.0f - DIFFUSE_WEIGHT) + blurred * DIFFUSE_WEIGHT;
	b -= DECAY_RATE;
	return b > 0.0f ? (uint8_t)b : 0;
}

/*
 * [CHANGE 3] Decay + 3x3 box blur + blend, trail -> trail_next.
 *
 * The old code called blur() per cell: 9 reads, each with 4 bounds checks.
 * Now the 3x3 average is split into a horizontal 3-sum and a vertical 3-sum,
 * so each cell needs ~6 reads and the interior loop has no branches at all.
 * Edge cells divide by 6 (or 4 in corners) exactly like the old
 * "divide by number of in-bounds neighbours" code did.
 */
static void diffuse_and_decay(void){
	const int gw = GW, gh = GH;

	/* pass 1: decay by 1 (same as the old `if (a > 0) a -= 1`) */
	{
		uint8_t *restrict t = trail;
		for (int i = 0; i < gw * gh; i++) t[i] -= (t[i] > 0);
	}

	/* pass 2: horizontal 3-sum */
	for (int y = 0; y < gh; y++){
		const uint8_t  *restrict row = trail + (size_t)y * gw;
		uint16_t       *restrict h   = hsum  + (size_t)y * gw;
		h[0] = row[0] + row[1];
		for (int x = 1; x < gw - 1; x++) h[x] = row[x - 1] + row[x] + row[x + 1];
		h[gw - 1] = row[gw - 1] + row[gw - 2];
	}

	/* pass 3: vertical 3-sum, divide, blend with the original value */
	for (int y = 0; y < gh; y++){
		const uint16_t *hm = y > 0      ? hsum + (size_t)(y - 1) * gw : NULL;
		const uint16_t *h0 =              hsum + (size_t)y * gw;
		const uint16_t *hp = y < gh - 1 ? hsum + (size_t)(y + 1) * gw : NULL;
		const uint8_t  *restrict in  = trail      + (size_t)y * gw;
		uint8_t        *restrict out = trail_next + (size_t)y * gw;

		if (hm && hp){
			/* interior rows: 9 neighbours, constant divisor -> fast vector loop */
			for (int x = 1; x < gw - 1; x++)
				out[x] = blend(in[x], (hm[x] + h0[x] + hp[x]) / 9);
			out[0]      = blend(in[0],      (hm[0]      + h0[0]      + hp[0])      / 6);
			out[gw - 1] = blend(in[gw - 1], (hm[gw - 1] + h0[gw - 1] + hp[gw - 1]) / 6);
		} else {
			/* top / bottom row: only 2 rows exist */
			const uint16_t *other = hm ? hm : hp;
			for (int x = 0; x < gw; x++){
				int cols = (x == 0 || x == gw - 1) ? 2 : 3;
				out[x] = blend(in[x], (h0[x] + other[x]) / (2 * cols));
			}
		}
	}
}

/* One simulation step. Also writes the frame into `dst` (XRGB8888, GW*GH pixels). */
static void sim_step(uint32_t *dst){
	tick++;

	for (int i = 0; i < agent_count; i++){
		Agent *ag = &agents[i];

		float vx = cosf(ag->angle) * ag->speed;
		float vy = sinf(ag->angle) * ag->speed;

		/* [CHANGE 5] Deposit along the whole step, not just at the start point.
		 * n sub-points per step (about one per cell travelled) -> solid lines. */
		int n = (int)(ag->speed / GRIDSIZE) + 1;
		for (int k = 0; k < n; k++){
			float t = (float)k / n;
			int gx = (int)((ag->x + vx * t) / GRIDSIZE);
			int gy = (int)((ag->y + vy * t) / GRIDSIZE);
			if (gx >= 0 && gx < GW && gy >= 0 && gy < GH){
				trail[gy * GW + gx]   = 255;
				species[gy * GW + gx] = ag->colorid;
			}
		}

		ag->x += vx;
		ag->y += vy;

		float rnd = hash_rand(ag->num * 747796405u + tick * 2891336453u);

		float center = sense(0, ag);
		float right  = sense(TURNSPEED, ag);
		float left   = sense(-TURNSPEED, ag);

		float turnStrength = 0.3f + rnd * 0.7f;
		float wander = (rnd - 0.5f) * 0.15f;

		if (right > left){
			if (center >= right) ag->angle += wander;
			else                 ag->angle += turnStrength;
		} else {
			if (center >= left)  ag->angle += wander;
			else                 ag->angle -= turnStrength;
		}

		if (ag->x < 0)   { ag->x = 0;     ag->angle = PI_F - ag->angle; }
		if (ag->x >= W)  { ag->x = W - 1; ag->angle = PI_F - ag->angle; }
		if (ag->y < 0)   { ag->y = 0;     ag->angle = -ag->angle; }
		if (ag->y >= H)  { ag->y = H - 1; ag->angle = -ag->angle; }
	}

	diffuse_and_decay();

	/* swap instead of memcpy */
	uint8_t *tmp = trail; trail = trail_next; trail_next = tmp;

	/* [CHANGE 4] one table lookup per pixel instead of 3 multiplies + shifts */
	const size_t total = (size_t)GW * GH;
	for (size_t i = 0; i < total; i++)
		dst[i] = lut[species[i]][trail[i]];
}

/* ------------------------------------------------------------------ */
/* Wayland plumbing                                                    */
/* ------------------------------------------------------------------ */
static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wp_viewporter *viewporter;

static struct wl_surface *surface;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct wp_viewport *viewport;
static struct wl_callback *frame_cb;

static bool configured;
static bool running = true;

typedef struct {
	struct wl_buffer *wl;
	uint32_t *data;
	bool busy;            /* compositor still reading it */
} Buf;

static Buf bufs[2];
static void *pool_data;
static size_t pool_size;

static void buffer_release(void *data, struct wl_buffer *b){
	(void)b;
	((Buf *)data)->busy = false;
}
static const struct wl_buffer_listener buffer_listener = { .release = buffer_release };

static void create_buffers(void){
	size_t stride = (size_t)GW * 4;
	size_t one = stride * GH;
	pool_size = one * 2;

	int fd = memfd_create("slime-wallpaper", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, pool_size) < 0){
		perror("memfd/ftruncate");
		exit(1);
	}
	pool_data = mmap(NULL, pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pool_data == MAP_FAILED){
		perror("mmap");
		exit(1);
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, pool_size);
	for (int i = 0; i < 2; i++){
		bufs[i].wl = wl_shm_pool_create_buffer(pool, i * one, GW, GH, stride, WL_SHM_FORMAT_XRGB8888);
		bufs[i].data = (uint32_t *)((char *)pool_data + i * one);
		bufs[i].busy = false;
		wl_buffer_add_listener(bufs[i].wl, &buffer_listener, &bufs[i]);
	}
	wl_shm_pool_destroy(pool);   /* buffers keep the memory alive */
	close(fd);
}

static void destroy_buffers(void){
	for (int i = 0; i < 2; i++){
		if (bufs[i].wl) wl_buffer_destroy(bufs[i].wl);
		bufs[i].wl = NULL;
	}
	if (pool_data) munmap(pool_data, pool_size);
	pool_data = NULL;
}

static double now_ms(void){
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* Step the sim into a free buffer and attach it. Skips if both buffers are busy. */
static void draw_frame(void){
	Buf *b = NULL;
	for (int i = 0; i < 2; i++){
		if (!bufs[i].busy){ b = &bufs[i]; break; }
	}
	if (!b) return;

	sim_step(b->data);
	b->busy = true;
	wl_surface_attach(surface, b->wl, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, GW, GH);
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener frame_listener = { .done = frame_done };

static void request_frame(void){
	frame_cb = wl_surface_frame(surface);
	wl_callback_add_listener(frame_cb, &frame_listener, NULL);
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time){
	(void)data; (void)time;
	wl_callback_destroy(cb);
	frame_cb = NULL;

	/* Cap to FPS_LIMIT even if the display runs at 144 Hz etc. */
	static double next_due = 0;
	double now = now_ms();
	if (now + 1.0 >= next_due){
		next_due += 1000.0 / FPS_LIMIT;
		if (next_due < now) next_due = now + 1000.0 / FPS_LIMIT;  /* fell behind, don't catch up */
		draw_frame();
	}

	request_frame();
	wl_surface_commit(surface);
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                            uint32_t serial, uint32_t w, uint32_t h){
	(void)data;
	zwlr_layer_surface_v1_ack_configure(ls, serial);

	if (w == 0 || h == 0){ w = 1920; h = 1080; }   /* compositor gave no size; fall back */

	if ((int)w != W || (int)h != H){                /* first configure or resolution change */
		destroy_buffers();
		sim_free();
		sim_init((int)w, (int)h);
		create_buffers();
	}
	wp_viewport_set_destination(viewport, W, H);    /* stretch GW x GH buffer to full output */

	if (!configured){
		configured = true;
		draw_frame();
		request_frame();
	}
	wl_surface_commit(surface);
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls){
	(void)data; (void)ls;
	running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version){
	(void)data;
	if (!strcmp(iface, wl_compositor_interface.name)){
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, version < 4 ? version : 4);
	} else if (!strcmp(iface, wl_shm_interface.name)){
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)){
		layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (!strcmp(iface, wp_viewporter_interface.name)){
		viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
	}
}
static void registry_remove(void *data, struct wl_registry *reg, uint32_t name){
	(void)data; (void)reg; (void)name;
}
static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

int main(void){
	display = wl_display_connect(NULL);
	if (!display){
		fprintf(stderr, "Can't connect to a Wayland compositor (is WAYLAND_DISPLAY set?)\n");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (!compositor || !shm){
		fprintf(stderr, "Compositor is missing wl_compositor or wl_shm\n");
		return 1;
	}
	if (!layer_shell){
		fprintf(stderr, "Compositor doesn't support wlr-layer-shell (GNOME doesn't).\n");
		return 1;
	}
	if (!viewporter){
		fprintf(stderr, "Compositor doesn't support wp_viewporter.\n");
		return 1;
	}

	surface = wl_compositor_create_surface(compositor);
	viewport = wp_viewporter_get_viewport(viewporter, surface);

	/* output = NULL: the compositor picks one (usually your primary monitor) */
	layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "slime-wallpaper");
	zwlr_layer_surface_v1_set_size(layer_surface, 0, 0);   /* 0,0 = fill the anchored area */
	zwlr_layer_surface_v1_set_anchor(layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);   /* ignore bars/panels */
	/* Keyboard interactivity defaults to "none" in the protocol, so we don't
	 * need to call zwlr_layer_surface_v1_set_keyboard_interactivity() at all. */
	zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);

	wl_surface_commit(surface);   /* initial commit (no buffer) -> triggers configure */

	while (running && wl_display_dispatch(display) != -1){
		/* everything happens in the callbacks above */
	}

	if (frame_cb) wl_callback_destroy(frame_cb);
	destroy_buffers();
	sim_free();
	return 0;
}
