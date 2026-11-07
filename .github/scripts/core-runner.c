/*
 * Headless libretro frontend for testing a libretro core on Android over adb shell.
 * On SIGSEGV/SIGBUS/SIGILL it prints the faulting PC, LR, address and registers as offsets
 * inside the core .so (map them with the unstripped .so from the CI artifacts).
 *
 * usage: jit-runner <core.so> <frames> <content|-> [key=value ...]
 *   key=value: core options (puae_*) passed to the core; also sysdir=, savedir=, quiet=1.
 *   input=FRAME:BUTTON[:LEN],...  press a RetroPad button of port 0 at FRAME for LEN frames
 *     (default 6); BUTTON is b y select start up down left right a x l r l2 r2 l3 r3 or a number.
 *   shots=FRAME,...  also write the frame to shot_FRAME.ppm at those frames.
 * Prints one status line every 50 frames, then a summary:
 *   resets, exceptions (illegal/A-line/F-line traps logged by the core), frame and chip RAM hashes.
 * Writes the last frame to last.ppm.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include "libretro.h"

#define MAX_OPTS 512
static const char *opt_key[MAX_OPTS], *opt_val[MAX_OPTS];
static int n_opts;
static char sys_dir[1024], save_dir[1024];
static unsigned resets, traps;
static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static uint8_t *frame;
static unsigned frame_w, frame_h;
static size_t frame_pitch;
static int quiet_log;
static int cur_frame;
#define MAX_PRESS 128
static struct { int frame, len, id; } press[MAX_PRESS];
static int n_press;
#define MAX_SHOTS 64
static int shot[MAX_SHOTS], n_shots;
static const char *btn_names[] = { "b", "y", "select", "start", "up", "down", "left", "right",
	"a", "x", "l", "r", "l2", "r2", "l3", "r3" };

static void parse_input(char *s)
{
	for (char *tok = strtok(s, ","); tok && n_press < MAX_PRESS; tok = strtok(NULL, ",")) {
		char name[32] = "";
		int fr = 0, len = 6;
		if (sscanf(tok, "%d:%31[^:]:%d", &fr, name, &len) < 2)
			continue;
		int id = -1;
		for (int i = 0; i < 16; i++)
			if (!strcmp(name, btn_names[i]))
				id = i;
		if (id < 0)
			id = atoi(name);
		press[n_press].frame = fr; press[n_press].len = len; press[n_press++].id = id;
	}
}

static uint32_t fnv(const uint8_t *p, size_t n, uint32_t h)
{
	while (n--)
		h = (h ^ *p++) * 16777619u;
	return h;
}

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
	char msg[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (strstr(msg, "CPU reset PC"))
		resets++;
	if (strstr(msg, "-Trap ") || strstr(msg, "Illegal instruction"))
		traps++;
	if (!quiet_log || level >= RETRO_LOG_WARN || strstr(msg, "JIT") || strstr(msg, "Trap")
	    || strstr(msg, "Illegal") || strstr(msg, "reset"))
		printf("[core] %s", msg);
}

static bool environment(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
		*(const char **)data = sys_dir;
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = save_dir;
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable *v = data;
		for (int i = 0; i < n_opts; i++)
			if (!strcmp(v->key, opt_key[i])) {
				v->value = opt_val[i];
				return true;
			}
		v->value = NULL;
		return false;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool *)data = false;
		return true;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = log_cb;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		pixfmt = *(const enum retro_pixel_format *)data;
		return true;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*(unsigned *)data = 0;  /* legacy SET_VARIABLES is enough */
		return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES:
		/* like a frontend: options not given on the command line take the core's default,
		 * the first value of "Description; default|other|..." */
		for (const struct retro_variable *v = data; v && v->key; v++) {
			int known = 0;
			for (int i = 0; i < n_opts; i++)
				known |= !strcmp(v->key, opt_key[i]);
			const char *semi = v->value ? strstr(v->value, "; ") : NULL;
			if (known || !semi || n_opts >= MAX_OPTS)
				continue;
			char *def = strdup(semi + 2), *bar = strchr(def, '|');
			if (bar)
				*bar = 0;
			opt_key[n_opts] = strdup(v->key);
			opt_val[n_opts++] = def;
		}
		return true;
	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
		return true;
	case RETRO_ENVIRONMENT_GET_LANGUAGE:
		*(unsigned *)data = RETRO_LANGUAGE_ENGLISH;
		return true;
	default:
		return false;
	}
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
	if (!data)
		return;
	size_t need = pitch * h;
	frame = realloc(frame, need);
	memcpy(frame, data, need);
	frame_w = w; frame_h = h; frame_pitch = pitch;
}
static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb(const int16_t *d, size_t n) { (void)d; return n; }
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned p, unsigned d, unsigned i, unsigned id)
{
	if (p != 0 || d != RETRO_DEVICE_JOYPAD)
		return 0;
	int16_t mask = 0;
	for (int k = 0; k < n_press; k++)
		if (cur_frame >= press[k].frame && cur_frame < press[k].frame + press[k].len)
			mask |= 1 << press[k].id;
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
		return mask;
	return id < 16 ? (mask >> id) & 1 : 0;
}

static uint32_t frame_hash(void)
{
	uint32_t h = 2166136261u;
	unsigned bpp = pixfmt == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
	for (unsigned y = 0; frame && y < frame_h; y++)
		h = fnv(frame + y * frame_pitch, frame_w * bpp, h);
	return h;
}

static void write_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f || !frame)
		return;
	fprintf(f, "P6\n%u %u\n255\n", frame_w, frame_h);
	for (unsigned y = 0; y < frame_h; y++)
		for (unsigned x = 0; x < frame_w; x++) {
			uint8_t rgb[3];
			if (pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
				uint32_t p = ((uint32_t *)(frame + y * frame_pitch))[x];
				rgb[0] = p >> 16; rgb[1] = p >> 8; rgb[2] = p;
			} else if (pixfmt == RETRO_PIXEL_FORMAT_RGB565) {
				uint16_t p = ((uint16_t *)(frame + y * frame_pitch))[x];
				rgb[0] = (p >> 11) << 3; rgb[1] = ((p >> 5) & 63) << 2; rgb[2] = (p & 31) << 3;
			} else {
				uint16_t p = ((uint16_t *)(frame + y * frame_pitch))[x];
				rgb[0] = ((p >> 10) & 31) << 3; rgb[1] = ((p >> 5) & 31) << 3; rgb[2] = (p & 31) << 3;
			}
			fwrite(rgb, 1, 3, f);
		}
	fclose(f);
}

static uintptr_t core_base;

static void crash_handler(int sig, siginfo_t *si, void *uc_)
{
	char buf[1024];
	int n = 0;
#if defined(__arm__)
	ucontext_t *uc = uc_;
	struct sigcontext *m = &uc->uc_mcontext;
	n = snprintf(buf, sizeof(buf),
		"CRASH signal %d code %d addr %p
  pc %08lx (core+%lx) lr %08lx (core+%lx) sp %08lx cpsr %08lx
"
		"  r0 %08lx r1 %08lx r2 %08lx r3 %08lx r4 %08lx r5 %08lx r6 %08lx r7 %08lx
"
		"  r8 %08lx r9 %08lx r10 %08lx fp %08lx ip %08lx
",
		sig, si->si_code, si->si_addr, m->arm_pc, m->arm_pc - core_base, m->arm_lr, m->arm_lr - core_base,
		m->arm_sp, m->arm_cpsr, m->arm_r0, m->arm_r1, m->arm_r2, m->arm_r3, m->arm_r4, m->arm_r5,
		m->arm_r6, m->arm_r7, m->arm_r8, m->arm_r9, m->arm_r10, m->arm_fp, m->arm_ip);
	/* return addresses on the stack that point into the core: a rough backtrace */
	unsigned long *sp = (unsigned long *)m->arm_sp;
	for (int i = 0, k = 0; i < 512 && k < 24 && n < (int)sizeof(buf) - 40; i++)
		if (sp[i] > core_base && sp[i] - core_base < 0x800000) {
			n += snprintf(buf + n, sizeof(buf) - n, "  stack[%d] core+%lx
", i, sp[i] - core_base);
			k++;
		}
#else
	(void)uc_;
	n = snprintf(buf, sizeof(buf), "CRASH signal %d addr %p
", sig, si->si_addr);
#endif
	write(1, buf, n);
	_exit(128 + sig);
}

#define SYM(name) do { *(void **)&p_##name = dlsym(core, #name); \
	if (!p_##name) { fprintf(stderr, "missing %s\n", #name); return 2; } } while (0)

int main(int argc, char **argv)
{
	snprintf(sys_dir, sizeof(sys_dir), "system");
	snprintf(save_dir, sizeof(save_dir), "saves");
	if (argc < 4) {
		fprintf(stderr, "usage: %s core.so frames content|- [key=value...]\n", argv[0]);
		return 2;
	}
	int frames = atoi(argv[2]);
	const char *content = strcmp(argv[3], "-") ? argv[3] : NULL;
	for (int i = 4; i < argc && n_opts < MAX_OPTS; i++) {
		char *eq = strchr(argv[i], '=');
		if (!eq)
			continue;
		*eq = 0;
		if (!strcmp(argv[i], "quiet")) { quiet_log = atoi(eq + 1); continue; }
		if (!strcmp(argv[i], "sysdir")) { snprintf(sys_dir, sizeof(sys_dir), "%s", eq + 1); continue; }
		if (!strcmp(argv[i], "savedir")) { snprintf(save_dir, sizeof(save_dir), "%s", eq + 1); continue; }
		if (!strcmp(argv[i], "input")) { parse_input(eq + 1); continue; }
		if (!strcmp(argv[i], "shots")) {
			for (char *tok = strtok(eq + 1, ","); tok && n_shots < MAX_SHOTS; tok = strtok(NULL, ","))
				shot[n_shots++] = atoi(tok);
			continue;
		}
		opt_key[n_opts] = argv[i];
		opt_val[n_opts++] = eq + 1;
	}
	mkdir(sys_dir, 0755);
	mkdir(save_dir, 0755);

	void *core = dlopen(argv[1], RTLD_NOW);
	if (!core) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 2;
	}
	void (*p_retro_set_environment)(retro_environment_t);
	void (*p_retro_set_video_refresh)(retro_video_refresh_t);
	void (*p_retro_set_audio_sample)(retro_audio_sample_t);
	void (*p_retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*p_retro_set_input_poll)(retro_input_poll_t);
	void (*p_retro_set_input_state)(retro_input_state_t);
	void (*p_retro_init)(void);
	bool (*p_retro_load_game)(const struct retro_game_info *);
	void (*p_retro_run)(void);
	void *(*p_retro_get_memory_data)(unsigned);
	size_t (*p_retro_get_memory_size)(unsigned);
	SYM(retro_set_environment); SYM(retro_set_video_refresh); SYM(retro_set_audio_sample);
	SYM(retro_set_audio_sample_batch); SYM(retro_set_input_poll); SYM(retro_set_input_state);
	SYM(retro_init); SYM(retro_load_game); SYM(retro_run);
	SYM(retro_get_memory_data); SYM(retro_get_memory_size);
	{
		Dl_info di;
		if (dladdr((void *)p_retro_run, &di))
			core_base = (uintptr_t)di.dli_fbase;
		printf("core loaded at %p
", (void *)core_base);
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_sigaction = crash_handler;
		sa.sa_flags = SA_SIGINFO;
		sigaction(SIGSEGV, &sa, NULL);
		sigaction(SIGBUS, &sa, NULL);
		sigaction(SIGILL, &sa, NULL);
		sigaction(SIGFPE, &sa, NULL);
	}

	p_retro_set_environment(environment);
	p_retro_set_video_refresh(video_cb);
	p_retro_set_audio_sample(audio_cb);
	p_retro_set_audio_sample_batch(audio_batch_cb);
	p_retro_set_input_poll(input_poll_cb);
	p_retro_set_input_state(input_state_cb);
	p_retro_init();

	struct retro_game_info info = { content, NULL, 0, NULL };
	if (!p_retro_load_game(content ? &info : NULL)) {
		fprintf(stderr, "retro_load_game failed\n");
		return 3;
	}
	for (int f = 1; f <= frames; f++) {
		cur_frame = f;
		p_retro_run();
		for (int k = 0; k < n_shots; k++)
			if (shot[k] == f) {
				char path[64];
				snprintf(path, sizeof(path), "shot_%d.ppm", f);
				write_ppm(path);
			}
		if (f % 50 == 0) {
			printf("frame %d resets %u traps %u screen %08x\n", f, resets, traps, frame_hash());
			fflush(stdout);
		}
	}
	uint8_t *ram = p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
	size_t ram_size = p_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
	printf("RESULT resets=%u traps=%u screen=%08x chipram=%08x\n", resets, traps, frame_hash(),
	       ram ? fnv(ram, ram_size, 2166136261u) : 0);
	write_ppm("last.ppm");
	return 0;
}
