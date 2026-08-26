/* xgpuprofile
 * Copyright (c) 2026 Dimitris Gerasimou
 * Licensed under the GNU General Public License v3.
 *
 * Makes the discrete GPU the primary X screen when a configurable rule
 * says it is worth it, and stays out of the way otherwise.
 *
 * On a hybrid laptop the external video outputs are wired to the discrete
 * GPU. While the integrated GPU owns the X screen, those displays are
 * driven by copying every frame across PCIe for the discrete GPU to scan
 * out, which does not keep up at high resolution and refresh. Driving
 * them directly removes the copy, at the cost of the discrete GPU never
 * reaching its deepest idle power state - hence the rule.
 *
 * The decision is written as an xorg.conf.d fragment before Xorg starts.
 * Xorg's GPU assignment is fixed for the life of the server, so nothing
 * is ever switched at runtime: no modules are unloaded, no session is
 * torn down, and there is no daemon. That is the whole design.
 *
 * To understand everything, start reading main().
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef VERSION
#define VERSION "unknown"
#endif

#ifndef CONFIG_PATH
#define CONFIG_PATH "/etc/xgpuprofile.conf"
#endif

#ifndef SNIPPET_PATH
#define SNIPPET_PATH "/etc/X11/xorg.conf.d/10-xgpuprofile.conf"
#endif

#ifndef SYS_PCI_DIR
#define SYS_PCI_DIR "/sys/bus/pci/devices"
#endif

#ifndef SYS_DRM_DIR
#define SYS_DRM_DIR "/sys/class/drm"
#endif

#ifndef SYS_POWER_DIR
#define SYS_POWER_DIR "/sys/class/power_supply"
#endif

#ifndef SYSTEMD_SYSTEM_DIR
#define SYSTEMD_SYSTEM_DIR "/etc/systemd/system"
#endif

#define STRMAX  128
#define SLOTMAX 16

/* When the discrete GPU should take over the display. */
typedef enum {
	RULE_BOTH = 0, /* on mains and an external display attached */
	RULE_EITHER,
	RULE_AC,
	RULE_EXTERNAL,
	RULE_ALWAYS,
} Rule;

/* Which layout to use, and how it is chosen. */
typedef enum {
	MODE_HYBRID = 0, /* never hand over; integrated GPU keeps the screen */
	MODE_AUTO,       /* let the rule decide */
	MODE_DGPU,       /* always hand over to the discrete GPU */
} Mode;

typedef struct {
	Mode mode;
	Rule rule;
	char display_manager[STRMAX];
	char busid[STRMAX];
	char driver[STRMAX];
} Config;

typedef struct {
	char slot[SLOTMAX];   /* PCI slot, e.g. 0000:01:00.0 */
	char busid[STRMAX];   /* Xorg BusID, e.g. PCI:1:0:0 */
	char driver[STRMAX];  /* bound kernel driver */
	int found;

	/* The integrated GPU, which stays in the layout as an inactive
	 * device. The built-in panel is wired to it, so leaving it out
	 * entirely turns the internal display off.
	 */
	char igpu_busid[STRMAX];
	int igpu_found;
} Gpu;

static int verbose;
static const char *prog = "xgpuprofile";

/* Connector name fragments that denote a built-in panel. Everything else
 * (HDMI, DP, DVI, VGA) is an external plug.
 *
 * Excluding these is not cosmetic: on a hybrid laptop the built-in panel
 * is often reachable through the discrete GPU as well, so counting it
 * would make "an external display is attached" permanently true.
 */
static const char *const internal_connectors[] = {
	"eDP", "LVDS", "DSI", "Writeback", NULL,
};

static void warn_(const char *fmt, ...);
static void vinfo(const char *fmt, ...);
static char *trim(char *s);
static int read_line(const char *path, char *buf, size_t bufsz);
static int joinpath(char *buf, size_t bufsz, const char *dir, const char *name);
static int copy_str(char *dst, size_t dstsz, const char *src);

static const char *mode_name(Mode m);
static const char *mode_blurb(Mode m);
static int parse_mode(const char *s, Mode *out);
static const char *rule_name(Rule r);
static const char *rule_blurb(Rule r);
static int parse_rule(const char *s, Rule *out);
static void config_defaults(Config *cfg);
static void config_load(Config *cfg, const char *path);
static int config_set_mode(const char *path, Mode mode);

static int detect_gpu(Gpu *gpu);
static int detect_ac(void);
static int is_pci_slot(const char *s);
static int connector_slot(const char *conn_dir, char *slot, size_t slotsz);
static int count_external(const char *slot);
static int count_external_xrandr(void);

static int snippet_write(const Gpu *gpu, const char *driver);
static int snippet_remove(void);
static int snippet_present(void);

static int dm_resolve(const char *override, char *unit, size_t unitsz);
static int dm_restart(const char *unit);

static int rule_satisfied(Rule r, int ac, int ext);
static int evaluate(const Config *cfg, Gpu *gpu, int *ac, int *ext);
static int act_refresh(const Config *cfg, int dry_run);
static int act_status(const Config *cfg, const char *path);
static int act_restart_x(const Config *cfg);
static int act_mode(Config *cfg, const char *path, const char *want);
static int act_once(const Config *cfg, const char *want);
static void hint_apply(void);
static int need_root(const char *what);
static void usage(void);

/* --- small helpers ------------------------------------------------- */

static void
warn_(const char *fmt, ...)
{
	va_list ap;
	int saved = errno;

	fprintf(stderr, "%s: ", prog);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	if (fmt[0] && fmt[strlen(fmt) - 1] == ':')
		fprintf(stderr, " %s", strerror(saved));
	fputc('\n', stderr);
}

static void
vinfo(const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static char *
trim(char *s)
{
	char *end;

	while (*s && isspace((unsigned char)*s))
		s++;
	if (!*s)
		return s;

	end = s + strlen(s) - 1;
	while (end > s && isspace((unsigned char)*end))
		*end-- = '\0';

	return s;
}

static int
read_line(const char *path, char *buf, size_t bufsz)
{
	FILE *f;

	if (!(f = fopen(path, "r")))
		return -1;

	if (!fgets(buf, (int)bufsz, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);

	buf[strcspn(buf, "\n")] = '\0';
	return 0;
}

/* A truncated path names a different file, so callers must not use one. */
static int
joinpath(char *buf, size_t bufsz, const char *dir, const char *name)
{
	int n = snprintf(buf, bufsz, "%s/%s", dir, name);

	return (n < 0 || (size_t)n >= bufsz) ? -1 : 0;
}

static int
copy_str(char *dst, size_t dstsz, const char *src)
{
	size_t len = strlen(src);

	if (len >= dstsz)
		return -1;

	memcpy(dst, src, len + 1);
	return 0;
}

/* --- configuration -------------------------------------------------- */

static const char *
mode_name(Mode m)
{
	switch (m) {
	case MODE_AUTO: return "auto";
	case MODE_DGPU: return "dgpu";
	default:        return "hybrid";
	}
}

static const char *
mode_blurb(Mode m)
{
	switch (m) {
	case MODE_AUTO: return "let the rule below decide";
	case MODE_DGPU: return "always use the discrete GPU";
	default:        return "always keep the integrated GPU";
	}
}

static int
parse_mode(const char *s, Mode *out)
{
	if (!strcmp(s, "hybrid")) { *out = MODE_HYBRID; return 0; }
	if (!strcmp(s, "auto"))   { *out = MODE_AUTO;   return 0; }
	if (!strcmp(s, "dgpu"))   { *out = MODE_DGPU;   return 0; }
	return -1;
}

static const char *
rule_blurb(Rule r)
{
	switch (r) {
	case RULE_EITHER:   return "on mains OR an external display attached";
	case RULE_AC:       return "on mains power";
	case RULE_EXTERNAL: return "an external display attached";
	case RULE_ALWAYS:   return "always";
	default:            return "on mains AND an external display attached";
	}
}

static const char *
rule_name(Rule r)
{
	switch (r) {
	case RULE_EITHER:   return "either";
	case RULE_AC:       return "ac";
	case RULE_EXTERNAL: return "external";
	case RULE_ALWAYS:   return "always";
	default:            return "both";
	}
}

static int
parse_rule(const char *s, Rule *out)
{
	if (!strcmp(s, "both"))     { *out = RULE_BOTH;     return 0; }
	if (!strcmp(s, "either"))   { *out = RULE_EITHER;   return 0; }
	if (!strcmp(s, "ac"))       { *out = RULE_AC;       return 0; }
	if (!strcmp(s, "external")) { *out = RULE_EXTERNAL; return 0; }
	if (!strcmp(s, "always"))   { *out = RULE_ALWAYS;   return 0; }
	return -1;
}

static void
config_defaults(Config *cfg)
{
	cfg->mode = MODE_HYBRID;
	cfg->rule = RULE_BOTH;
	copy_str(cfg->display_manager, sizeof(cfg->display_manager), "auto");
	copy_str(cfg->busid, sizeof(cfg->busid), "auto");
	copy_str(cfg->driver, sizeof(cfg->driver), "auto");
}

/* A missing file is not an error: the defaults stand, which is the
 * "installed but not configured" state.
 */
static void
config_load(Config *cfg, const char *path)
{
	FILE *f;
	char line[256];
	unsigned int lineno = 0;

	if (!(f = fopen(path, "r"))) {
		if (errno != ENOENT)
			warn_("cannot read %s:", path);
		return;
	}

	while (fgets(line, sizeof(line), f)) {
		char *key, *value, *eq, *hash;

		lineno++;

		if ((hash = strchr(line, '#')))
			*hash = '\0';
		if (!(eq = strchr(line, '=')))
			continue;

		*eq = '\0';
		key = trim(line);
		value = trim(eq + 1);
		if (!*key || !*value)
			continue;

		if (!strcmp(key, "mode")) {
			if (parse_mode(value, &cfg->mode) < 0)
				warn_("%s:%u: unknown mode \"%s\" "
				      "(want hybrid, auto or dgpu)", path, lineno, value);
		} else if (!strcmp(key, "rule")) {
			if (parse_rule(value, &cfg->rule) < 0)
				warn_("%s:%u: unknown rule \"%s\"", path, lineno, value);
		} else if (!strcmp(key, "display_manager")) {
			copy_str(cfg->display_manager, sizeof(cfg->display_manager), value);
		} else if (!strcmp(key, "busid")) {
			copy_str(cfg->busid, sizeof(cfg->busid), value);
		} else if (!strcmp(key, "driver")) {
			copy_str(cfg->driver, sizeof(cfg->driver), value);
		} else {
			warn_("%s:%u: unknown setting \"%s\"", path, lineno, key);
		}
	}

	fclose(f);
}

/* Rewrites the file with `mode` set, preserving every other line. */
static int
config_set_mode(const char *path, Mode mode)
{
	FILE *in, *out;
	char tmp[PATH_MAX], line[256];
	int replaced = 0;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
		warn_("config path too long");
		return -1;
	}

	if (!(out = fopen(tmp, "w"))) {
		warn_("cannot write %s:", tmp);
		return -1;
	}

	if ((in = fopen(path, "r"))) {
		while (fgets(line, sizeof(line), in)) {
			char probe[256], *eq, *hash;

			copy_str(probe, sizeof(probe), line);
			if ((hash = strchr(probe, '#')))
				*hash = '\0';

			if ((eq = strchr(probe, '='))) {
				*eq = '\0';
				if (!strcmp(trim(probe), "mode")) {
					fprintf(out, "mode = %s\n", mode_name(mode));
					replaced = 1;
					continue;
				}
			}
			fputs(line, out);
		}
		fclose(in);
	}

	if (!replaced)
		fprintf(out, "mode = %s\n", mode_name(mode));

	if (fclose(out) != 0 || rename(tmp, path) != 0) {
		warn_("cannot update %s:", path);
		unlink(tmp);
		return -1;
	}

	return 0;
}

/* --- detection ------------------------------------------------------ */

/*
 * The discrete GPU is the PCI display device the firmware did not mark
 * boot_vga. Detecting it means the generated fragment is correct without
 * hardcoding a BusID or a driver name.
 */
static int
detect_gpu(Gpu *gpu)
{
	DIR *d;
	const struct dirent *de;

	memset(gpu, 0, sizeof(*gpu));

	if (!(d = opendir(SYS_PCI_DIR))) {
		warn_("cannot open %s:", SYS_PCI_DIR);
		return -1;
	}

	while ((de = readdir(d))) {
		char path[PATH_MAX], buf[64], target[PATH_MAX];
		unsigned int bus, dev, func;
		ssize_t n;
		int boot_vga = 0;

		if (de->d_name[0] == '.')
			continue;

		if (joinpath(path, sizeof(path), SYS_PCI_DIR, de->d_name) < 0)
			continue;
		if (strlen(path) + sizeof("/boot_vga") >= sizeof(path))
			continue;

		/* PCI class 0x03xxxx is a display controller. */
		snprintf(buf, sizeof(buf), "%s", "");
		{
			char classp[PATH_MAX];

			if (joinpath(classp, sizeof(classp), path, "class") < 0)
				continue;
			if (read_line(classp, buf, sizeof(buf)) < 0)
				continue;
			if (strncmp(buf, "0x03", 4) != 0)
				continue;
		}

		{
			char bootp[PATH_MAX];

			if (joinpath(bootp, sizeof(bootp), path, "boot_vga") == 0
			    && read_line(bootp, buf, sizeof(buf)) == 0
			    && !strcmp(buf, "1"))
				boot_vga = 1;
		}

		/* The GPU the firmware brought up is the integrated one. It is
		 * not what we make primary, but the layout still needs its
		 * BusID.
		 */
		if (boot_vga) {
			if (sscanf(de->d_name, "%*x:%x:%x.%x", &bus, &dev, &func) == 3) {
				snprintf(gpu->igpu_busid, sizeof(gpu->igpu_busid),
				         "PCI:%u:%u:%u", bus, dev, func);
				gpu->igpu_found = 1;
			}
			continue;
		}

		if (copy_str(gpu->slot, sizeof(gpu->slot), de->d_name) < 0)
			continue;

		/* Xorg wants the BusID in decimal; the slot name is hex. */
		if (sscanf(de->d_name, "%*x:%x:%x.%x", &bus, &dev, &func) == 3)
			snprintf(gpu->busid, sizeof(gpu->busid), "PCI:%u:%u:%u",
			         bus, dev, func);

		{
			char drvp[PATH_MAX];

			if (joinpath(drvp, sizeof(drvp), path, "driver") == 0
			    && (n = readlink(drvp, target, sizeof(target) - 1)) > 0) {
				const char *base;

				target[n] = '\0';
				base = strrchr(target, '/');
				copy_str(gpu->driver, sizeof(gpu->driver),
				         base ? base + 1 : target);
			}
		}

		gpu->found = 1;

		/* Do not stop here: the integrated device may still be ahead
		 * of us in the directory order.
		 */
	}
	closedir(d);

	if (!gpu->found) {
		vinfo("no discrete GPU found");
		return -1;
	}

	vinfo("discrete GPU: %s (%s) driver=%s", gpu->slot, gpu->busid,
	      gpu->driver[0] ? gpu->driver : "none");
	if (gpu->igpu_found)
		vinfo("integrated GPU: %s", gpu->igpu_busid);
	else
		vinfo("no integrated GPU found; internal panel may stay dark");
	return 0;
}

/* 1 on mains, 0 on battery, -1 where there is no mains supply at all. */
static int
detect_ac(void)
{
	DIR *d;
	const struct dirent *de;
	int have_mains = 0, online = 0;

	if (!(d = opendir(SYS_POWER_DIR)))
		return -1;

	while ((de = readdir(d))) {
		char dir[PATH_MAX], path[PATH_MAX], buf[64];

		if (de->d_name[0] == '.')
			continue;
		if (joinpath(dir, sizeof(dir), SYS_POWER_DIR, de->d_name) < 0)
			continue;
		if (joinpath(path, sizeof(path), dir, "type") < 0)
			continue;
		if (read_line(path, buf, sizeof(buf)) < 0 || strcmp(buf, "Mains"))
			continue;

		have_mains = 1;

		if (joinpath(path, sizeof(path), dir, "online") == 0
		    && read_line(path, buf, sizeof(buf)) == 0 && !strcmp(buf, "1"))
			online = 1;
	}
	closedir(d);

	return have_mains ? online : -1;
}

static int
is_pci_slot(const char *s)
{
	unsigned int dom, bus, dev, func;
	char tail;

	return sscanf(s, "%x:%x:%x.%x%c", &dom, &bus, &dev, &func, &tail) == 4;
}

/*
 * The PCI slot that owns a DRM connector.
 *
 * A connector's own `device` link points at the DRM card, not at the PCI
 * device, so it cannot be read directly. Resolving the connector's real
 * path gives the whole device chain instead, and its last PCI-shaped
 * component is the GPU the connector hangs off.
 */
static int
connector_slot(const char *conn_dir, char *slot, size_t slotsz)
{
	char resolved[PATH_MAX], found[SLOTMAX] = "";
	char *saveptr = NULL, *tok;

	if (!realpath(conn_dir, resolved))
		return -1;

	for (tok = strtok_r(resolved, "/", &saveptr); tok;
	     tok = strtok_r(NULL, "/", &saveptr)) {
		if (is_pci_slot(tok))
			copy_str(found, sizeof(found), tok);
	}

	if (!found[0])
		return -1;

	return copy_str(slot, slotsz, found);
}

/* Counts connected external displays; if `slot` is set, only those on
 * that PCI device.
 */
static int
count_external(const char *slot)
{
	DIR *d;
	const struct dirent *de;
	int count = 0;

	if (!(d = opendir(SYS_DRM_DIR))) {
		warn_("cannot open %s:", SYS_DRM_DIR);
		return 0;
	}

	while ((de = readdir(d))) {
		char conn[PATH_MAX], path[PATH_MAX], buf[64];
		int internal = 0;

		if (strncmp(de->d_name, "card", 4) || !strchr(de->d_name, '-'))
			continue;

		for (const char *const *p = internal_connectors; *p; p++)
			if (strstr(de->d_name, *p))
				internal = 1;
		if (internal)
			continue;

		if (joinpath(conn, sizeof(conn), SYS_DRM_DIR, de->d_name) < 0)
			continue;

		if (slot) {
			char owner[SLOTMAX];

			if (connector_slot(conn, owner, sizeof(owner)) < 0)
				continue;
			if (strcmp(owner, slot))
				continue;
		}

		if (joinpath(path, sizeof(path), conn, "status") < 0)
			continue;
		if (read_line(path, buf, sizeof(buf)) < 0)
			continue;

		if (!strcmp(buf, "connected")) {
			vinfo("external display: %s", de->d_name);
			count++;
		}
	}
	closedir(d);

	return count;
}

/*
 * Counts connected external outputs by asking a live X server, when one is
 * reachable, rather than the kernel.
 *
 * NVIDIA's DRM/KMS connector status is not reliably kept in sync with
 * reality once its own driver stack is actively managing the display -
 * this is a known rough edge of its KMS support, not specific to any one
 * machine. Before Xorg starts nothing has taken that over yet, so the
 * plain kernel probe in count_external() is accurate; the moment a
 * session exists, xrandr's own view is the trustworthy one, since it is
 * simply reporting what is actually driving the desktop.
 *
 * Returns the count on success, -1 if no X session could be reached (no
 * DISPLAY, or xrandr is missing or failed) so the caller can fall back.
 */
static int
count_external_xrandr(void)
{
	FILE *p;
	char line[512];
	int count = 0;

	if (!getenv("DISPLAY"))
		return -1;

	if (!(p = popen("xrandr -q 2>/dev/null", "r")))
		return -1;

	while (fgets(line, sizeof(line), p)) {
		char name[64] = "";
		int internal = 0;

		/* Output lines start at column 0; indented lines are modes. */
		if (isspace((unsigned char)line[0]))
			continue;
		if (sscanf(line, "%63s", name) != 1)
			continue;

		/* Check the more specific word first: "disconnected" contains
		 * "connected" as a substring.
		 */
		if (strstr(line, " disconnected"))
			continue;
		if (!strstr(line, " connected"))
			continue;

		for (const char *const *ic = internal_connectors; *ic; ic++)
			if (strstr(name, *ic))
				internal = 1;
		if (!internal)
			count++;
	}

	/* A non-zero exit means xrandr could not run at all (missing, or no
	 * reachable X server despite DISPLAY being set) - the count gathered
	 * up to that point is not meaningful, so the caller must fall back
	 * rather than trust a possibly-partial zero.
	 */
	if (pclose(p) != 0)
		return -1;

	return count;
}

/* --- the xorg fragment ---------------------------------------------- */

static int
snippet_write(const Gpu *gpu, const char *driver)
{
	FILE *f;
	char tmp[PATH_MAX];

	if (!gpu->found || !gpu->busid[0]) {
		warn_("refusing to write a layout with no GPU BusID");
		return -1;
	}

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", SNIPPET_PATH) >= (int)sizeof(tmp))
		return -1;

	if (!(f = fopen(tmp, "w"))) {
		warn_("cannot write %s:", tmp);
		return -1;
	}

	fprintf(f,
	        "# Generated by xgpuprofile. Do not edit; it is rewritten.\n"
	        "\n"
	        "Section \"ServerLayout\"\n"
	        "\tIdentifier \"layout\"\n"
	        "\tScreen 0 \"xgpuprofile\"\n");

	/* The built-in panel hangs off the integrated GPU, so it has to stay
	 * in the layout even though it drives no screen of its own. Without
	 * this it is not a provider at all, and the internal display goes
	 * dark the moment the discrete GPU takes over.
	 */
	if (gpu->igpu_found)
		fprintf(f, "\tInactive \"igpu\"\n");

	fprintf(f,
	        "EndSection\n"
	        "\n"
	        "Section \"Device\"\n"
	        "\tIdentifier \"xgpuprofile\"\n"
	        "\tDriver \"%s\"\n"
	        "\tBusID \"%s\"\n"
	        "EndSection\n"
	        "\n"
	        "Section \"Screen\"\n"
	        "\tIdentifier \"xgpuprofile\"\n"
	        "\tDevice \"xgpuprofile\"\n"
	        "\tOption \"AllowEmptyInitialConfiguration\"\n"
	        "EndSection\n",
	        driver, gpu->busid);

	if (gpu->igpu_found)
		fprintf(f,
		        "\n"
		        "Section \"Device\"\n"
		        "\tIdentifier \"igpu\"\n"
		        "\tDriver \"modesetting\"\n"
		        "\tBusID \"%s\"\n"
		        "EndSection\n"
		        "\n"
		        "Section \"Screen\"\n"
		        "\tIdentifier \"igpu\"\n"
		        "\tDevice \"igpu\"\n"
		        "EndSection\n",
		        gpu->igpu_busid);

	/* Rename rather than write in place, so Xorg can never read a
	 * half-written config if it starts while this runs.
	 */
	if (fclose(f) != 0 || rename(tmp, SNIPPET_PATH) != 0) {
		warn_("cannot install %s:", SNIPPET_PATH);
		unlink(tmp);
		return -1;
	}

	vinfo("wrote %s (Driver \"%s\", BusID \"%s\")", SNIPPET_PATH, driver,
	      gpu->busid);
	return 0;
}

static int
snippet_remove(void)
{
	if (unlink(SNIPPET_PATH) != 0 && errno != ENOENT) {
		warn_("cannot remove %s:", SNIPPET_PATH);
		return -1;
	}

	vinfo("%s absent", SNIPPET_PATH);
	return 0;
}

static int
snippet_present(void)
{
	return access(SNIPPET_PATH, F_OK) == 0;
}

/* --- display manager ------------------------------------------------ */

/* Display managers that do not install the display-manager.service alias
 * have to be looked up by name.
 */
static const char *const known_dms[] = {
	"ly.service", "greetd.service", "sddm.service", "gdm.service",
	"lightdm.service", "lxdm.service", "xdm.service", "emptty.service",
	NULL,
};

static int
dm_resolve(const char *override, char *unit, size_t unitsz)
{
	char link[PATH_MAX], target[PATH_MAX];
	ssize_t n;

	if (override && *override && strcmp(override, "auto")) {
		return copy_str(unit, unitsz, override);
	}

	/* The standard mechanism: exactly one unit answers to this alias. */
	if (joinpath(link, sizeof(link), SYSTEMD_SYSTEM_DIR,
	             "display-manager.service") == 0
	    && (n = readlink(link, target, sizeof(target) - 1)) > 0) {
		const char *base;

		target[n] = '\0';
		base = strrchr(target, '/');
		return copy_str(unit, unitsz, base ? base + 1 : target);
	}

	for (const char *const *u = known_dms; *u; u++) {
		char cmd[PATH_MAX], out[64] = "";
		FILE *p;

		snprintf(cmd, sizeof(cmd), "systemctl is-enabled %s 2>/dev/null", *u);
		if (!(p = popen(cmd, "r")))
			continue;
		if (fgets(out, sizeof(out), p))
			out[strcspn(out, "\n")] = '\0';
		pclose(p);

		if (!strncmp(out, "enabled", 7))
			return copy_str(unit, unitsz, *u);
	}

	return -1;
}

static int
dm_restart(const char *unit)
{
	pid_t pid;
	int status;

	if ((pid = fork()) < 0) {
		warn_("fork:");
		return -1;
	}

	if (pid == 0) {
		execlp("systemctl", "systemctl", "restart", unit, (char *)NULL);
		_exit(127);
	}

	if (waitpid(pid, &status, 0) < 0)
		return -1;

	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return 0;

	warn_("systemctl restart %s failed", unit);
	return -1;
}

/* --- actions --------------------------------------------------------- */

/* `ac` is -1 where the machine has no battery, which trivially satisfies
 * any mains condition.
 */
static int
rule_satisfied(Rule r, int ac, int ext)
{
	int on_ac = (ac != 0);
	int has_ext = (ext > 0);

	switch (r) {
	case RULE_ALWAYS:   return 1;
	case RULE_AC:       return on_ac;
	case RULE_EXTERNAL: return has_ext;
	case RULE_EITHER:   return on_ac || has_ext;
	default:            return on_ac && has_ext;
	}
}

static int
evaluate(const Config *cfg, Gpu *gpu, int *ac, int *ext)
{
	detect_gpu(gpu);

	if (cfg->busid[0] && strcmp(cfg->busid, "auto")) {
		if (copy_str(gpu->busid, sizeof(gpu->busid), cfg->busid) == 0)
			gpu->found = 1;
	}

	*ac = detect_ac();

	*ext = count_external_xrandr();
	if (*ext >= 0) {
		vinfo("external displays: %d (from the running X session)", *ext);
	} else {
		*ext = gpu->found ? count_external(gpu->slot) : 0;
		vinfo("external displays: %d (no X session reachable; used sysfs)", *ext);
	}

	if (!gpu->found)
		return 0;

	switch (cfg->mode) {
	case MODE_HYBRID: return 0;
	case MODE_DGPU:   return 1;
	default:          return rule_satisfied(cfg->rule, *ac, *ext);
	}
}

static int
act_refresh(const Config *cfg, int dry_run)
{
	Gpu gpu;
	int ac, ext, engage;

	engage = evaluate(cfg, &gpu, &ac, &ext);

	vinfo("mode=%s rule=%s ac=%d external=%d -> %s", mode_name(cfg->mode),
	      rule_name(cfg->rule), ac, ext, engage ? "dgpu" : "hybrid");

	if (dry_run) {
		printf("Mode        %s\n", mode_name(cfg->mode));
		if (cfg->mode == MODE_AUTO)
			printf("Rule        %s (%s)\n", rule_name(cfg->rule),
			       rule_blurb(cfg->rule));
		printf("Power       %s\n",
		       ac < 0 ? "no battery" : (ac ? "on mains" : "on battery"));
		printf("External    %d display%s\n", ext, ext == 1 ? "" : "s");
		printf("\nWould use   %s\n",
		       engage ? "discrete GPU" : "integrated GPU (hybrid)");
		return 0;
	}

	if (engage) {
		const char *driver = cfg->driver;

		if (!driver[0] || !strcmp(driver, "auto"))
			driver = gpu.driver[0] ? gpu.driver : "nvidia";

		return snippet_write(&gpu, driver);
	}

	return snippet_remove();
}

static int
act_status(const Config *cfg, const char *path)
{
	Gpu gpu;
	char unit[STRMAX];
	int ac, ext, want, have;

	want = evaluate(cfg, &gpu, &ac, &ext);
	have = snippet_present();

	printf("Configuration  (%s)\n", path);
	printf("  Mode         %-8s %s\n", mode_name(cfg->mode),
	       mode_blurb(cfg->mode));
	if (cfg->mode == MODE_AUTO)
		printf("  Rule         %-8s use the discrete GPU %s\n",
		       rule_name(cfg->rule), rule_blurb(cfg->rule));

	printf("\nDetected\n");
	if (gpu.found)
		printf("  Discrete     %-14s %s\n", gpu.busid,
		       gpu.driver[0] ? gpu.driver : "no driver bound");
	else
		printf("  Discrete     none found\n");

	if (gpu.igpu_found)
		printf("  Integrated   %s\n", gpu.igpu_busid);
	else
		printf("  Integrated   none found "
		       "(the built-in panel may stay dark)\n");

	printf("  Power        %s\n",
	       ac < 0 ? "no battery" : (ac ? "on mains" : "on battery"));
	printf("  External     %d display%s connected\n", ext, ext == 1 ? "" : "s");

	if (dm_resolve(cfg->display_manager, unit, sizeof(unit)) == 0)
		printf("  Session      %s\n", unit);
	else
		printf("  Session      no display manager found\n");

	printf("\nLayout\n");
	printf("  Running      %s\n",
	       have ? "discrete GPU" : "integrated GPU (hybrid)");
	printf("  Should be    %s\n",
	       want ? "discrete GPU" : "integrated GPU (hybrid)");

	if (want == have) {
		printf("\nIn sync.\n");
	} else {
		/* The fragment on disk is whatever was last staged. If it no
		 * longer matches the decision - because the hardware or the
		 * config changed since - it has to be rewritten before an X
		 * restart would pick the right one up.
		 */
		printf("\nOut of sync: this session is not using the layout the\n"
		       "configuration now calls for. To bring it in line:\n"
		       "    sudo %s --refresh      stage the right layout\n"
		       "    sudo %s --restart-x    apply it now (ends the session)\n"
		       "\nA reboot does both by itself.\n", prog, prog);
	}

	return 0;
}

/* Every staged change needs an X restart, so say so in one place. */
static void
hint_apply(void)
{
	printf("Takes effect at your next login, or immediately with:\n"
	       "    sudo %s --restart-x\n", prog);
}

/*
 * Applies a layout right now, ignoring mode and rule, and deliberately
 * writes nothing to the config file. That is what makes it a one-off: the
 * next --refresh - including the one the next normal boot runs - decides
 * again from the unchanged config and undoes this. There is nothing to
 * remember to reset.
 */
static int
act_once(const Config *cfg, const char *want)
{
	Gpu gpu;
	const char *driver;
	Mode m;

	if (parse_mode(want, &m) < 0 || m == MODE_AUTO) {
		warn_("--once takes dgpu or hybrid, not \"%s\"", want);
		return 1;
	}

	if (need_root("--once") < 0)
		return 1;

	if (m == MODE_HYBRID) {
		if (snippet_remove() < 0)
			return 1;
		printf("Hybrid staged for this session only; %s is unchanged.\n",
		       CONFIG_PATH);
		hint_apply();
		return 0;
	}

	detect_gpu(&gpu);

	if (cfg->busid[0] && strcmp(cfg->busid, "auto")) {
		if (copy_str(gpu.busid, sizeof(gpu.busid), cfg->busid) < 0) {
			warn_("the configured busid is too long");
			return 1;
		}
		gpu.found = 1;
	}

	if (!gpu.found) {
		warn_("no discrete GPU found");
		return 1;
	}

	driver = cfg->driver;
	if (!driver[0] || !strcmp(driver, "auto"))
		driver = gpu.driver[0] ? gpu.driver : "nvidia";

	if (snippet_write(&gpu, driver) < 0)
		return 1;

	printf("Discrete GPU staged for this session only; %s is unchanged.\n",
	       CONFIG_PATH);
	hint_apply();
	return 0;
}

/* Sets the saved mode and stages the layout that follows from it. */
static int
act_mode(Config *cfg, const char *path, const char *want)
{
	Mode m;

	if (parse_mode(want, &m) < 0) {
		warn_("unknown mode \"%s\" (want hybrid, auto or dgpu)", want);
		return 1;
	}

	if (need_root("--mode") < 0)
		return 1;

	if (config_set_mode(path, m) < 0)
		return 1;

	cfg->mode = m;

	if (act_refresh(cfg, 0) < 0)
		return 1;

	printf("Mode set to %s - %s.\n", mode_name(m), mode_blurb(m));
	if (m == MODE_AUTO)
		printf("Rule is %s: use the discrete GPU %s.\n",
		       rule_name(cfg->rule), rule_blurb(cfg->rule));
	hint_apply();

	return 0;
}

static int
act_restart_x(const Config *cfg)
{
	char unit[STRMAX], reply[16] = "";

	if (need_root("--restart-x") < 0)
		return 1;

	if (dm_resolve(cfg->display_manager, unit, sizeof(unit)) < 0) {
		warn_("cannot determine the display manager; set display_manager in %s",
		      CONFIG_PATH);
		return 1;
	}

	printf("This restarts %s, closing everything in your current session.\n"
	       "Unsaved work will be lost.\n\n", unit);
	printf("Continue? [y/N] ");
	fflush(stdout);

	if (!fgets(reply, sizeof(reply), stdin)
	    || (reply[0] != 'y' && reply[0] != 'Y')) {
		printf("Cancelled; nothing was restarted.\n");
		return 1;
	}

	return dm_restart(unit) < 0 ? 1 : 0;
}

static int
need_root(const char *what)
{
	if (geteuid() != 0) {
		warn_("%s needs root (try: sudo %s %s)", what, prog, what);
		return -1;
	}
	return 0;
}

static void
usage(void)
{
	printf(
"Usage: %s [action] [options]\n"
"\n"
"Chooses which GPU drives the X display. Changes are staged into an Xorg\n"
"config fragment and take effect the next time Xorg starts.\n"
"\n"
"Actions (one at a time; --status if none given):\n"
"  --status              Show the mode, what was detected, and whether the\n"
"                        running session matches\n"
"  --mode MODE           Set and save the mode:\n"
"                          hybrid  always keep the integrated GPU\n"
"                          auto    let the rule in the config decide\n"
"                          dgpu    always use the discrete GPU\n"
"  --once MODE           Stage hybrid or dgpu for this session only, without\n"
"                        saving it; the next --refresh undoes it\n"
"  --refresh             Re-decide from the saved config and update the\n"
"                        fragment (this is what runs at boot)\n"
"  --restart-x           Restart the display manager so a staged change\n"
"                        applies now, ending the current session\n"
"\n"
"Options:\n"
"  --config PATH         Use a different configuration file\n"
"  --dry-run             With --refresh, show the decision and change nothing\n"
"  --verbose             Explain each step on stderr\n"
"  --version             Print version and exit\n"
"  --help                Print this message and exit\n"
"\n"
"Examples:\n"
"  sudo %s --mode auto        Follow the rule from now on\n"
"  sudo %s --once dgpu        Use the discrete GPU for this session only\n"
"  %s --refresh --dry-run     See what would be chosen, changing nothing\n",
	       prog, prog, prog, prog);
}

enum {
	OPT_STATUS = 1000, OPT_REFRESH, OPT_MODE, OPT_ONCE, OPT_RESTART_X,
	OPT_CONFIG, OPT_DRY_RUN, OPT_VERBOSE, OPT_VERSION, OPT_HELP,
};

int
main(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "status",    no_argument,       0, OPT_STATUS    },
		{ "refresh",   no_argument,       0, OPT_REFRESH   },
		{ "mode",      required_argument, 0, OPT_MODE      },
		{ "once",      required_argument, 0, OPT_ONCE      },
		{ "restart-x", no_argument,       0, OPT_RESTART_X },
		{ "config",    required_argument, 0, OPT_CONFIG    },
		{ "dry-run",   no_argument,       0, OPT_DRY_RUN   },
		{ "verbose",   no_argument,       0, OPT_VERBOSE   },
		{ "version",   no_argument,       0, OPT_VERSION   },
		{ "help",      no_argument,       0, OPT_HELP      },
		{ 0, 0, 0, 0 },
	};

	Config cfg;
	const char *path = CONFIG_PATH;
	const char *arg = NULL;
	int action = OPT_STATUS, nactions = 0, dry_run = 0, opt;

	while ((opt = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
		switch (opt) {
		case OPT_STATUS: case OPT_REFRESH: case OPT_RESTART_X:
			nactions++;
			action = opt;
			break;

		case OPT_MODE: case OPT_ONCE:
			nactions++;
			action = opt;
			arg = optarg;
			break;

		case OPT_CONFIG:  path = optarg;  break;
		case OPT_DRY_RUN: dry_run = 1;    break;
		case OPT_VERBOSE: verbose = 1;    break;

		case OPT_VERSION:
			printf("%s %s\n", prog, VERSION);
			return 0;

		case OPT_HELP:
			usage();
			return 0;

		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", prog);
			return 1;
		}
	}

	if (optind < argc) {
		warn_("unexpected argument \"%s\"", argv[optind]);
		return 1;
	}
	if (nactions > 1) {
		warn_("only one action at a time");
		return 1;
	}

	config_defaults(&cfg);
	config_load(&cfg, path);

	switch (action) {
	case OPT_REFRESH:
		/* --dry-run only reports, so it stays usable without root. */
		if (!dry_run && need_root("--refresh") < 0)
			return 1;
		return act_refresh(&cfg, dry_run) < 0 ? 1 : 0;

	case OPT_MODE:
		return act_mode(&cfg, path, arg);

	case OPT_ONCE:
		return act_once(&cfg, arg);

	case OPT_RESTART_X:
		return act_restart_x(&cfg);

	default:
		return act_status(&cfg, path);
	}
}
