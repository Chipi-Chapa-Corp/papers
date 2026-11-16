// SPDX-License-Identifier: GPL-2.0-or-later
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sane/sane.h>
#include <sane/saneopts.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <unistd.h>
#endif

static volatile sig_atomic_t stopped;
static SANE_Handle scanner;
static gchar *device, *directory, *kind = NULL, *paper = NULL, *sides = NULL;
static gint resolution = 150, mode, delay = 3, brightness, contrast;
static gboolean list_devices, color, single, feeder_done;

static void
message (const char *type, const char *text)
{
	g_autofree char *escaped = g_uri_escape_string (text, NULL, TRUE);
	g_print ("%s\t%s\n", type, escaped);
	fflush (stdout);
}

/* Airscan numbers devices independently in each process. Match protocol and
 * advertised name instead of retaining a discovery process's numeric index. */
static gboolean
same_device (const char *a, const char *b)
{
	if (g_strcmp0 (a, b) == 0)
		return TRUE;
	if (!g_str_has_prefix (a, "airscan:") || !g_str_has_prefix (b, "airscan:"))
		return FALSE;
	const char *a_name = strchr (a + 8, ':'), *b_name = strchr (b + 8, ':');
	return a[8] == b[8] && a_name && b_name && strcmp (a_name, b_name) == 0;
}

static SANE_Status
open_scanner (void)
{
	const SANE_Device **devices;
	/* Enumerate in this worker before opening: network discovery IDs can change. */
	SANE_Status status = sane_get_devices (&devices, SANE_FALSE);
	if (status != SANE_STATUS_GOOD)
		return status;
	for (int i = 0; devices[i]; i++) {
		if (same_device (device, devices[i]->name)) {
			g_free (device);
			device = g_strdup (devices[i]->name);
			return sane_open (device, &scanner);
		}
	}
	return sane_open (device, &scanner);
}

static void
scanner_error (const char *step, SANE_Status status)
{
	const char *hint = "Check the scanner connection, then refresh scanners and try again.";
	switch (status) {
	case SANE_STATUS_DEVICE_BUSY:
		hint = "The scanner is busy. Close other scanning applications and try again.";
		break;
	case SANE_STATUS_NO_DOCS:
		hint = "The feeder is empty. Load pages or choose Single Page for the flatbed.";
		break;
	case SANE_STATUS_JAMMED:
		hint = "Clear the paper jam and try again.";
		break;
	case SANE_STATUS_COVER_OPEN:
		hint = "Close the scanner cover and try again.";
		break;
	case SANE_STATUS_INVAL:
		hint = "The scanner rejected the device or scan settings. Refresh scanners or try Automatic page size and another resolution.";
		break;
	case SANE_STATUS_ACCESS_DENIED:
		hint = "Access was denied. Check scanner permissions and authentication.";
		break;
	default:
		break;
	}
	g_autofree char *text = g_strdup_printf ("Could not %s. %s\nScanner: %s · %s · %d dpi · page size: %s. Driver: %s (SANE %d).",
	                                         step, hint,
	                                         device ? device : "unavailable", color ? "image" : "text", resolution,
	                                         paper ? paper : "auto", sane_strstatus (status), status);
	message ("ERROR", text);
}

static void
interrupt_scan (int sig)
{
	(void) sig;
	stopped = 1;
	if (scanner)
		sane_cancel (scanner);
}

static const SANE_Option_Descriptor *
option (const char *name, int *index)
{
	for (int i = 1;; i++) {
		const SANE_Option_Descriptor *o = sane_get_option_descriptor (scanner, i);
		if (!o)
			break;
		if (o->name && strcmp (o->name, name) == 0 && SANE_OPTION_IS_ACTIVE (o->cap) && SANE_OPTION_IS_SETTABLE (o->cap)) {
			*index = i;
			return o;
		}
	}
	return NULL;
}

static gboolean
set_number (const char *name, double value)
{
	int i;
	const SANE_Option_Descriptor *o = option (name, &i);
	if (!o || (o->type != SANE_TYPE_INT && o->type != SANE_TYPE_FIXED))
		return FALSE;
	SANE_Word v = o->type == SANE_TYPE_FIXED ? SANE_FIX (value) : (SANE_Word) value;
	if (o->constraint_type == SANE_CONSTRAINT_RANGE) {
		const SANE_Range *range = o->constraint.range;
		v = CLAMP (v, range->min, range->max);
		if (range->quant > 0)
			v = MIN (range->max, range->min + ((v - range->min + range->quant / 2) / range->quant) * range->quant);
	} else if (o->constraint_type == SANE_CONSTRAINT_WORD_LIST) {
		const SANE_Word *values = o->constraint.word_list;
		if (values[0] == 0)
			return FALSE;
		SANE_Word nearest = values[1];
		for (int j = 2; j <= values[0]; j++) {
			if (llabs ((gint64) values[j] - v) < llabs ((gint64) nearest - v) ||
			    (llabs ((gint64) values[j] - v) == llabs ((gint64) nearest - v) && values[j] > nearest))
				nearest = values[j];
		}
		v = nearest;
	}
	return sane_control_option (scanner, i, SANE_ACTION_SET_VALUE, &v, NULL) == SANE_STATUS_GOOD;
}

static gboolean
set_choice (const char *name, const char *const *choices)
{
	int i;
	const SANE_Option_Descriptor *o = option (name, &i);
	if (!o || o->type != SANE_TYPE_STRING || o->constraint_type != SANE_CONSTRAINT_STRING_LIST)
		return FALSE;
	for (int c = 0; choices[c]; c++) {
		for (int j = 0; o->constraint.string_list[j]; j++) {
			if (g_ascii_strcasecmp (o->constraint.string_list[j], choices[c]) == 0) {
				g_autofree char *value = g_malloc0 (o->size);
				g_strlcpy (value, o->constraint.string_list[j], o->size);
				return sane_control_option (scanner, i, SANE_ACTION_SET_VALUE, value, NULL) == SANE_STATUS_GOOD;
			}
		}
	}
	return FALSE;
}

static double
maximum (const char *name, double fallback)
{
	int i;
	const SANE_Option_Descriptor *o = option (name, &i);
	if (!o || o->constraint_type != SANE_CONSTRAINT_RANGE)
		return fallback;
	return o->type == SANE_TYPE_FIXED ? SANE_UNFIX (o->constraint.range->max) : o->constraint.range->max;
}

static gboolean
configure (void)
{
	const char *flatbed[] = { "Flatbed", "FlatBed", "Normal", NULL };
	const char *front[] = { "ADF Front", "ADF Frontside", "ADF Simplex", "ADF", "Automatic Document Feeder", "Feeder", "Document Feeder", NULL };
	const char *back[] = { "ADF Back", "ADF Backside", NULL };
	const char *duplex[] = { "ADF Duplex", "Duplex", "Automatic Document Feeder (Duplex)", NULL };
	const char *gray[] = { "Gray", "Grayscale", "Grey", "True Gray", "8bit Gray", NULL };
	const char *rgb[] = { "Color", "Colour", "RGB", "24bit Color", NULL };
	if (mode == 1) {
		gboolean selected = FALSE;
		if (g_strcmp0 (sides, "both") == 0) {
			selected = set_choice (SANE_NAME_SCAN_SOURCE, duplex);
			if (!selected && set_choice (SANE_NAME_SCAN_SOURCE, front)) {
				int i;
				const SANE_Option_Descriptor *o = option ("duplex", &i);
				if (o && o->type == SANE_TYPE_BOOL) {
					SANE_Word enabled = SANE_TRUE;
					selected = sane_control_option (scanner, i, SANE_ACTION_SET_VALUE, &enabled, NULL) == SANE_STATUS_GOOD;
				}
				const char *both[] = { "Duplex", "Both", NULL };
				selected = set_choice ("adf-mode", both) || selected;
			}
		} else {
			selected = set_choice (SANE_NAME_SCAN_SOURCE, g_strcmp0 (sides, "back") == 0 ? back : front);
		}
		if (!selected) {
			message ("ERROR", "The selected feeder or scan sides are not supported by this scanner.");
			return FALSE;
		}
	} else {
		gboolean selected = set_choice (SANE_NAME_SCAN_SOURCE, flatbed);
		int i;
		if (mode == 2 && !selected && option (SANE_NAME_SCAN_SOURCE, &i)) {
			message ("ERROR", "This scanner does not support scanning multiple pages from a flatbed.");
			return FALSE;
		}
	}
	if (!set_choice (SANE_NAME_SCAN_MODE, color ? rgb : gray)) {
		/* Some devices only offer color. Convert text pages in software. */
		set_choice (SANE_NAME_SCAN_MODE, rgb);
	}
	set_number (SANE_NAME_BIT_DEPTH, 8);
	if (!set_number (SANE_NAME_SCAN_RESOLUTION, resolution)) {
		set_number (SANE_NAME_SCAN_X_RESOLUTION, resolution);
		set_number (SANE_NAME_SCAN_Y_RESOLUTION, resolution);
	}
	int idx;
	const SANE_Option_Descriptor *o = option (SANE_NAME_SCAN_RESOLUTION, &idx);
	if (!o)
		o = option (SANE_NAME_SCAN_X_RESOLUTION, &idx);
	if (o) {
		SANE_Word actual;
		if (sane_control_option (scanner, idx, SANE_ACTION_GET_VALUE, &actual, NULL) == SANE_STATUS_GOOD)
			resolution = o->type == SANE_TYPE_FIXED ? (int) SANE_UNFIX (actual) : actual;
	}
	resolution = MAX (resolution, 1);
	double width = maximum (SANE_NAME_SCAN_BR_X, 210), height = maximum (SANE_NAME_SCAN_BR_Y, 297);
	if (g_strcmp0 (paper, "a4") == 0) {
		width = 210;
		height = 297;
	} else if (g_strcmp0 (paper, "a5") == 0) {
		width = 148;
		height = 210;
	} else if (g_strcmp0 (paper, "a6") == 0) {
		width = 105;
		height = 148;
	} else if (g_strcmp0 (paper, "a3") == 0) {
		width = 297;
		height = 420;
	} else if (g_strcmp0 (paper, "4x6") == 0) {
		width = 101.6;
		height = 152.4;
	} else if (g_strcmp0 (paper, "letter") == 0) {
		width = 215.9;
		height = 279.4;
	} else if (g_strcmp0 (paper, "legal") == 0) {
		width = 215.9;
		height = 355.6;
	}
	set_number (SANE_NAME_SCAN_TL_X, 0);
	set_number (SANE_NAME_SCAN_TL_Y, 0);
	set_number ("page-width", width);
	set_number ("page-height", height);
	set_number (SANE_NAME_SCAN_BR_X, width);
	set_number (SANE_NAME_SCAN_BR_Y, height);
	/* Automatic length is a device capability, not a fixed A4 assumption. */
	if (g_strcmp0 (paper, "auto") == 0) {
		const char *automatic[] = { "Auto", "Automatic", NULL };
		set_choice ("page-size", automatic);
	}
	return TRUE;
}

static guint8
adjust (guint8 p)
{
	double factor = (100.0 + contrast) / (100.0 - MIN (contrast, 99));
	return (guint8) CLAMP (factor * (p - 127.5) + 127.5 + brightness * 2.55, 0, 255);
}

static void
preview (GdkPixbuf *pixbuf, const char *path)
{
	int w = gdk_pixbuf_get_width (pixbuf), h = gdk_pixbuf_get_height (pixbuf);
	double scale = MIN (1.0, MIN (900.0 / w, 900.0 / h));
	g_autoptr (GdkPixbuf) small = gdk_pixbuf_scale_simple (pixbuf, MAX (1, (int) (w * scale)), MAX (1, (int) (h * scale)), GDK_INTERP_BILINEAR);
	g_autofree char *temp = g_strconcat (path, ".tmp", NULL);
	if (gdk_pixbuf_save (small, temp, "png", NULL, NULL) && g_rename (temp, path) == 0)
		message ("PREVIEW", path);
}

static gboolean
read_page (int page)
{
	SANE_Parameters p;
	SANE_Status status = sane_get_parameters (scanner, &p);
	if (status != SANE_STATUS_GOOD) {
		scanner_error ("read the image parameters", status);
		return FALSE;
	}
	if (p.pixels_per_line <= 0 || p.bytes_per_line <= 0 ||
	    (p.format != SANE_FRAME_GRAY && p.format != SANE_FRAME_RGB) ||
	    (p.depth != 1 && p.depth != 8 && p.depth != 16)) {
		message ("ERROR", "The scanner returned an unsupported image format. Try another scan type.");
		return FALSE;
	}
	int channels = p.format == SANE_FRAME_RGB ? 3 : 1;
	if ((guint64) p.pixels_per_line * channels * p.depth > (guint64) p.bytes_per_line * 8) {
		message ("ERROR", "The scanner returned invalid image dimensions.");
		return FALSE;
	}
	int height = p.lines > 0 ? p.lines : MAX (1, (int) (maximum (SANE_NAME_SCAN_BR_Y, 355.6) * resolution / 25.4));
	if ((guint64) p.pixels_per_line * height * 3 > 512 * 1024 * 1024 || (guint64) p.bytes_per_line > 32 * 1024 * 1024) {
		message ("ERROR", "This scan is too large. Choose a lower resolution or smaller page size.");
		return FALSE;
	}
	g_autoptr (GdkPixbuf) image = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, p.pixels_per_line, height);
	if (!image) {
		message ("ERROR", "Not enough memory for this scan.");
		return FALSE;
	}
	gdk_pixbuf_fill (image, 0xffffffff);
	g_autofree guint8 *buffer = g_malloc (p.bytes_per_line);
	guint8 *pixels = gdk_pixbuf_get_pixels (image);
	int stride = gdk_pixbuf_get_rowstride (image), row = 0, used = 0;
	gint64 last_preview = 0, last_progress = 0;
	g_autofree char *preview_path = g_build_filename (directory, "preview.png", NULL);
	while (!stopped) {
		SANE_Int n = 0;
		status = sane_read (scanner, buffer + used, p.bytes_per_line - used, &n);
		if (status == SANE_STATUS_EOF)
			break;
		if (status == SANE_STATUS_NO_DOCS && mode == 1 && page > 1 && row == 0) {
			feeder_done = TRUE;
			return FALSE;
		}
		if (status != SANE_STATUS_GOOD) {
			if (!stopped)
				scanner_error ("read the scanned page", status);
			return FALSE;
		}
		if (n == 0) {
			g_usleep (1000);
			continue;
		}
		used += n;
		if (used < p.bytes_per_line)
			continue;
		if (row >= height) {
			message ("ERROR", "The scanned page exceeds the supported page length.");
			return FALSE;
		}
		for (int x = 0; x < p.pixels_per_line; x++) {
			guint8 rgb[3];
			for (int c = 0; c < 3; c++) {
				int sample = x * channels + (channels == 1 ? 0 : c);
				guint8 value;
				if (p.depth == 1)
					value = (buffer[sample / 8] & (0x80 >> (sample % 8))) ? 255 : 0;
				else if (p.depth == 16) {
					guint16 word;
					memcpy (&word, buffer + sample * 2, sizeof word);
					value = word >> 8;
				} else
					value = buffer[sample];
				rgb[c] = value;
			}
			if (!color) {
				guint8 gray = (rgb[0] * 77 + rgb[1] * 150 + rgb[2] * 29) >> 8;
				rgb[0] = rgb[1] = rgb[2] = gray;
			}
			for (int c = 0; c < 3; c++)
				pixels[row * stride + x * 3 + c] = adjust (rgb[c]);
		}
		row++;
		used = 0;
		gint64 now = g_get_monotonic_time ();
		if (now - last_progress >= 50000) {
			g_print ("PROGRESS\t%.6f\n", MIN (1.0, (double) row / height));
			fflush (stdout);
			last_progress = now;
		}
		if (now - last_preview >= 250000) {
			preview (image, preview_path);
			g_print ("PROGRESS\t%.6f\n", (double) row / height);
			fflush (stdout);
			last_preview = now;
		}
	}
	if (stopped || row == 0)
		return FALSE;
	g_autoptr (GdkPixbuf) cropped = gdk_pixbuf_new_subpixbuf (image, 0, 0, p.pixels_per_line, row);
	g_autofree char *name = g_strdup_printf ("page-%06d.png", page);
	g_autofree char *path = g_build_filename (directory, name, NULL);
	g_autoptr (GError) error = NULL;
	if (!gdk_pixbuf_save (cropped, path, "png", &error, NULL)) {
		message ("ERROR", error->message);
		return FALSE;
	}
	g_autofree char *escaped = g_uri_escape_string (path, NULL, TRUE);
	g_print ("PROGRESS\t1\nPAGE\t%s\t%d\n", escaped, resolution);
	fflush (stdout);
	return TRUE;
}

int
main (int argc, char **argv)
{
#ifdef __linux__
	/* A driver blocked in I/O must not outlive the application that owns it. */
	pid_t parent = getppid ();
	prctl (PR_SET_PDEATHSIG, SIGKILL);
	if (getppid () != parent)
		return 1;
#endif
	GOptionEntry entries[] = {
		{ "list", 0, 0, G_OPTION_ARG_NONE, &list_devices, NULL, NULL },
		{ "single", 0, 0, G_OPTION_ARG_NONE, &single, NULL, NULL },
		{ "device", 0, 0, G_OPTION_ARG_STRING, &device, NULL, NULL },
		{ "directory", 0, 0, G_OPTION_ARG_STRING, &directory, NULL, NULL },
		{ "kind", 0, 0, G_OPTION_ARG_STRING, &kind, NULL, NULL },
		{ "paper", 0, 0, G_OPTION_ARG_STRING, &paper, NULL, NULL },
		{ "sides", 0, 0, G_OPTION_ARG_STRING, &sides, NULL, NULL },
		{ "dpi", 0, 0, G_OPTION_ARG_INT, &resolution, NULL, NULL },
		{ "mode", 0, 0, G_OPTION_ARG_INT, &mode, NULL, NULL },
		{ "delay", 0, 0, G_OPTION_ARG_INT, &delay, NULL, NULL },
		{ "brightness", 0, 0, G_OPTION_ARG_INT, &brightness, NULL, NULL },
		{ "contrast", 0, 0, G_OPTION_ARG_INT, &contrast, NULL, NULL },
		{ NULL }
	};
	g_autoptr (GOptionContext) context = g_option_context_new (NULL);
	g_option_context_add_main_entries (context, entries, NULL);
	g_autoptr (GError) error = NULL;
	if (!g_option_context_parse (context, &argc, &argv, &error)) {
		message ("ERROR", error->message);
		return 1;
	}
	brightness = CLAMP (brightness, -100, 100);
	contrast = CLAMP (contrast, -100, 100);
	signal (SIGINT, interrupt_scan);
	signal (SIGTERM, interrupt_scan);
	/* Toolbx exposes the host system bus here rather than at the usual path.
	 * SANE network backends need Avahi; this changes only the worker's bus. */
	if (!g_getenv ("DBUS_SYSTEM_BUS_ADDRESS") &&
	    !g_file_test ("/run/dbus/system_bus_socket", G_FILE_TEST_EXISTS) &&
	    g_file_test ("/run/host/run/dbus/system_bus_socket", G_FILE_TEST_EXISTS))
		g_setenv ("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/run/host/run/dbus/system_bus_socket", FALSE);
	SANE_Int version;
	SANE_Status status = sane_init (&version, NULL);
	if (status != SANE_STATUS_GOOD) {
		scanner_error ("initialize scanner support", status);
		return 1;
	}
	int result = 0;
	if (list_devices) {
		const SANE_Device **devices;
		status = sane_get_devices (&devices, SANE_FALSE);
		if (status == SANE_STATUS_GOOD) {
			for (int i = 0; devices[i]; i++) {
				g_autofree char *id = g_uri_escape_string (devices[i]->name, NULL, TRUE);
				g_autofree char *label = g_strdup_printf ("%s %s", devices[i]->vendor, devices[i]->model);
				g_autofree char *escaped = g_uri_escape_string (label, NULL, TRUE);
				g_print ("DEVICE\t%s\t%s\n", id, escaped);
				fflush (stdout);
			}
		} else {
			scanner_error ("find scanners", status);
			result = 1;
		}
	} else if (!device || !directory || resolution < 50 || resolution > 2400 || delay < 0 || delay > 3600 || mode < 0 || mode > 2) {
		message ("ERROR", "Invalid scan settings.");
		result = 1;
	} else {
		color = g_strcmp0 (kind, "image") == 0;
		status = open_scanner ();
		if (status != SANE_STATUS_GOOD) {
			scanner_error ("open the selected scanner", status);
			result = 1;
		} else {
			if (!configure ()) {
				sane_close (scanner);
				scanner = NULL;
				sane_exit ();
				return 1;
			}
			g_print ("SETTINGS\t%d\n", resolution);
			fflush (stdout);
			for (int page = 1; !stopped; page++) {
				g_print ("START\t%d\nPROGRESS\t0\n", page);
				fflush (stdout);
				status = sane_start (scanner);
				if (status == SANE_STATUS_NO_DOCS && mode == 1 && page > 1)
					break;
				if (status != SANE_STATUS_GOOD) {
					if (!stopped)
						scanner_error ("start scanning", status);
					result = stopped || feeder_done ? 0 : 1;
					break;
				}
				if (!read_page (page)) {
					result = stopped || feeder_done ? 0 : 1;
					break;
				}
				if (mode == 0 || (single && page >= (g_strcmp0 (sides, "both") == 0 ? 2 : 1)))
					break;
				if (mode == 2) {
					sane_cancel (scanner);
					message ("WAIT", "Place the next page on the flatbed");
					for (int tick = 0; tick < delay * 10 && !stopped; tick++)
						g_usleep (100000);
				}
			}
		}
		if (scanner) {
			sane_cancel (scanner);
			sane_close (scanner);
			scanner = NULL;
		}
	}
	sane_exit ();
	g_print ("DONE\n");
	return result;
}
