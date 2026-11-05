/**
 * Raven Charts
 *
 * A desktop chart viewer: open CSV / TSV / JSON files (Epoch AI's ECI export
 * among them), pick a chart style and the fields, and get a Vega-Lite chart.
 *
 *   Raven                 the window, the controls, the files, the spec editor
 *   raven_web             a <WebView> that shows web/chart.html
 *   web/chart.js          parses the files, builds the Vega-Lite spec, draws it
 *
 * The application talks to the page with Rweb_view_run_javascript (calls of
 * window.RC); the page answers with window.raven.postMessage("kind\nbody").
 *
 *   raven_charts [files...]
 *
 * Environment: RAVEN_CHARTS_DIR (folder holding ui/ and web/),
 * RAVEN_CHARTS_FRAMES and RAVEN_CHARTS_CAPTURE (automated runs: render, save
 * the window as a PPM image, exit), RAVEN_CHARTS_STYLE (style at start).
 */

#include <Raven.h>
#include <raven_web.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef RAVEN_CHARTS_DIR
#define RAVEN_CHARTS_DIR "."
#endif

#define MAX_DATASETS 64
#define MAX_FIELDS   256

/* ---------------------------------------------------------------------------
 *  Small growing string
 * ------------------------------------------------------------------------- */

typedef struct { char* data; size_t length, capacity; } Str;

static void str_reserve(Str* s, size_t extra) {
	if (s->length + extra + 1 <= s->capacity) return;
	size_t capacity = s->capacity ? s->capacity : 256;
	while (capacity < s->length + extra + 1) capacity *= 2;
	char* data = (char*)realloc(s->data, capacity);
	if (!data) { fprintf(stderr, "out of memory\n"); exit(1); }
	s->data = data;
	s->capacity = capacity;
}

static void str_add(Str* s, const char* text) {
	size_t n = strlen(text);
	str_reserve(s, n);
	memcpy(s->data + s->length, text, n + 1);
	s->length += n;
}

static void str_char(Str* s, char c) {
	str_reserve(s, 1);
	s->data[s->length++] = c;
	s->data[s->length] = '\0';
}

/* A JavaScript / JSON string literal, quotes included. */
static void str_quoted(Str* s, const char* text) {
	static const char hex[] = "0123456789abcdef";
	const unsigned char* p = (const unsigned char*)(text ? text : "");
	str_char(s, '"');
	for (; *p; p++) {
		unsigned char c = *p;
		switch (c) {
		case '"': str_add(s, "\\\""); break;
		case '\\': str_add(s, "\\\\"); break;
		case '\n': str_add(s, "\\n"); break;
		case '\r': str_add(s, "\\r"); break;
		case '\t': str_add(s, "\\t"); break;
		default:
			if (c < 0x20) {
				char u[7] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15], 0 };
				str_add(s, u);
			} else if (c == 0xE2 && p[1] == 0x80 && (p[2] == 0xA8 || p[2] == 0xA9)) {
				/* U+2028 / U+2029 end a line in older JavaScript */
				str_add(s, p[2] == 0xA8 ? "\\u2028" : "\\u2029");
				p += 2;
			} else {
				str_char(s, (char)c);
			}
		}
	}
	str_char(s, '"');
}

static void str_free(Str* s) { free(s->data); memset(s, 0, sizeof(*s)); }

/* ---------------------------------------------------------------------------
 *  State
 * ------------------------------------------------------------------------- */

typedef struct {
	char name[256];
	char path[1024];
	int rows;
	int parsed;   /* the page answered */
	int field_count;
	char* fields[MAX_FIELDS];
	char types[MAX_FIELDS];   /* 'q' quantitative, 't' temporal, 'n' nominal */
	/* what the controls showed for it, given back when it is selected again */
	int sel_x, sel_y, sel_color, sel_label, sel_size, sel_low, sel_high;
} Dataset;

static const struct { const char* id; const char* name; const char* help; } STYLES[] = {
	{ "frontier",    "Frontier (best so far)",  "Every point faded, the running best of Y over X as a step line, and the record-setting points labelled. The classic capabilities-over-time view." },
	{ "labeled",     "Scatter + labels",        "Every record as a point; the Top N by Y get their Label." },
	{ "scatter",     "Scatter",                 "Every record as a point, colored by Color." },
	{ "bubble",      "Bubble",                  "A scatter with the point area given by the Size field." },
	{ "trend",       "Scatter + linear trend",  "A least-squares line (exponential when Y is logarithmic), per color group if asked." },
	{ "loess",       "Scatter + smooth (LOESS)","A locally weighted smooth curve through the points." },
	{ "errorbar",    "Points + error bars",     "Each value with its interval: needs Error low and Error high." },
	{ "line",        "Line",                    "One line per color group; Aggregate combines records sharing an X." },
	{ "linepoint",   "Line + points",           "A line with a point on every record." },
	{ "step",        "Step line",               "A line that holds its value until the next record." },
	{ "area",        "Area",                    "Filled areas, overlaid per color group." },
	{ "bar",         "Bar",                     "Bars by X: months for dates, bins for numbers. Aggregate sets the height (Count without Y)." },
	{ "leaderboard", "Leaderboard (top N)",     "Horizontal bars of the Top N records by Y, named by Label." },
	{ "histogram",   "Histogram",               "How the values of X are spread, stacked by Color." },
	{ "boxplot",     "Box plot",                "Y summarised per category (a text X, or Color)." },
	{ "strip",       "Strip plot",              "Every value of Y as a tick, per category." },
	{ "heatmap",     "Heatmap (2D bins)",       "Records counted in a grid of X and Y bins." },
};
#define STYLE_COUNT ((int)(sizeof(STYLES) / sizeof(STYLES[0])))

static const char* AGGREGATES[] = { "none", "count", "sum", "mean", "median", "min", "max" };

static struct {
	char dir[1024];
	Rid window, view, files, style, style_help, status, tabs, spec, light;
	Rid x, y, color, label, size, low, high, aggregate, topn, logy, logx, pergroup, title, filter;
	Dataset datasets[MAX_DATASETS];
	int dataset_count;
	int active;
	int ready;               /* the page is loaded */
	Str* pending;            /* scripts given before the page was ready */
	int pending_count;
	int updating;            /* the controls are being filled: no redraw */
	char export_path[1024];
	int drawn;               /* automated runs: a chart was drawn */
} app;

/* ---------------------------------------------------------------------------
 *  Talking to the page
 * ------------------------------------------------------------------------- */

static void run(Str* script) {
	if (!app.ready) {
		Str* grown = (Str*)realloc(app.pending, sizeof(Str) * (size_t)(app.pending_count + 1));
		if (!grown) return;
		app.pending = grown;
		app.pending[app.pending_count++] = *script;
		memset(script, 0, sizeof(*script));
		return;
	}
	Rweb_view_run_javascript(app.view, script->data, NULL, NULL);
	str_free(script);
}

static void run_text(const char* text) {
	Str s = { 0 };
	str_add(&s, text);
	run(&s);
}

static void set_status(const char* text) { Rlabel_set_text(app.status, text); }

static Dataset* active_dataset(void) {
	return app.active >= 0 && app.active < app.dataset_count ? &app.datasets[app.active] : NULL;
}

/* The field chosen in a combo box (item 0 is "(none)"). */
static const char* chosen(Rid combo) {
	Dataset* d = active_dataset();
	int index = Rcombo_box_get_selected(combo) - 1;
	return d && index >= 0 && index < d->field_count ? d->fields[index] : "";
}

static void render(void) {
	Dataset* d = active_dataset();
	int style = Rcombo_box_get_selected(app.style);
	int aggregate = Rcombo_box_get_selected(app.aggregate);
	char number[64];
	Str s = { 0 };
	if (app.updating || !d || !d->parsed) return;
	if (style < 0 || style >= STYLE_COUNT) style = 0;
	if (aggregate < 0 || aggregate >= (int)(sizeof(AGGREGATES) / sizeof(AGGREGATES[0]))) aggregate = 0;

	str_add(&s, "RC.render({dataset: ");
	snprintf(number, sizeof(number), "%d", app.active);
	str_add(&s, number);
	str_add(&s, ", style: "); str_quoted(&s, STYLES[style].id);
	str_add(&s, ", x: "); str_quoted(&s, chosen(app.x));
	str_add(&s, ", y: "); str_quoted(&s, chosen(app.y));
	str_add(&s, ", color: "); str_quoted(&s, chosen(app.color));
	str_add(&s, ", label: "); str_quoted(&s, chosen(app.label));
	str_add(&s, ", size: "); str_quoted(&s, chosen(app.size));
	str_add(&s, ", low: "); str_quoted(&s, chosen(app.low));
	str_add(&s, ", high: "); str_quoted(&s, chosen(app.high));
	str_add(&s, ", aggregate: "); str_quoted(&s, AGGREGATES[aggregate]);
	snprintf(number, sizeof(number), ", topN: %d", (int)Rspin_box_get_value(app.topn));
	str_add(&s, number);
	str_add(&s, Rcheckbox_is_checked(app.logy) ? ", logY: true" : ", logY: false");
	str_add(&s, Rcheckbox_is_checked(app.logx) ? ", logX: true" : ", logX: false");
	str_add(&s, Rcheckbox_is_checked(app.pergroup) ? ", perGroup: true" : ", perGroup: false");
	str_add(&s, ", title: "); str_quoted(&s, Rtext_input_get_text(app.title));
	str_add(&s, ", filter: "); str_quoted(&s, Rtext_input_get_text(app.filter));
	str_add(&s, "})");
	run(&s);
}

/* ---------------------------------------------------------------------------
 *  Files
 * ------------------------------------------------------------------------- */

static const char* base_name(const char* path) {
	const char* slash = strrchr(path, '/');
	const char* back = strrchr(path, '\\');
	if (back && (!slash || back > slash)) slash = back;
	return slash ? slash + 1 : path;
}

static const char* extension(const char* path) {
	const char* name = base_name(path);
	const char* dot = strrchr(name, '.');
	return dot ? dot + 1 : "";
}

static void send_file(int index) {
	Dataset* d = &app.datasets[index];
	size_t size = 0;
	char* text = Rfile_read(d->path, &size);
	char number[32];
	Str s = { 0 };
	if (!text) {
		char message[1200];
		snprintf(message, sizeof(message), "Cannot read %s", d->path);
		set_status(message);
		return;
	}
	snprintf(number, sizeof(number), "%d", index);
	str_add(&s, "RC.loadDataset(");
	str_add(&s, number);
	str_add(&s, ", ");
	str_quoted(&s, d->name);
	str_add(&s, ", ");
	str_quoted(&s, text);
	str_add(&s, ", ");
	str_quoted(&s, extension(d->path));
	str_add(&s, ")");
	Rmemory_free(text);
	run(&s);
}

static void open_path(const char* path) {
	int i;
	char message[1200];
	/* opening a file again reloads it */
	for (i = 0; i < app.dataset_count; i++) {
		if (strcmp(app.datasets[i].path, path) == 0) {
			app.datasets[i].parsed = 0;
			send_file(i);
			return;
		}
	}
	if (app.dataset_count >= MAX_DATASETS) {
		set_status("Too many files open: remove one first.");
		return;
	}
	Dataset* d = &app.datasets[app.dataset_count];
	memset(d, 0, sizeof(*d));
	snprintf(d->path, sizeof(d->path), "%s", path);
	snprintf(d->name, sizeof(d->name), "%s", base_name(path));
	d->sel_x = d->sel_y = -1;
	app.dataset_count++;
	snprintf(message, sizeof(message), "Reading %s...", d->name);
	set_status(message);
	send_file(app.dataset_count - 1);
}

static void free_fields(Dataset* d) {
	int i;
	for (i = 0; i < d->field_count; i++) free(d->fields[i]);
	d->field_count = 0;
}

static void remember_selection(void) {
	Dataset* d = active_dataset();
	if (!d) return;
	d->sel_x = Rcombo_box_get_selected(app.x);
	d->sel_y = Rcombo_box_get_selected(app.y);
	d->sel_color = Rcombo_box_get_selected(app.color);
	d->sel_label = Rcombo_box_get_selected(app.label);
	d->sel_size = Rcombo_box_get_selected(app.size);
	d->sel_low = Rcombo_box_get_selected(app.low);
	d->sel_high = Rcombo_box_get_selected(app.high);
}

static void fill_combos(void) {
	static Rid* combos[] = { &app.x, &app.y, &app.color, &app.label, &app.size, &app.low, &app.high };
	Dataset* d = active_dataset();
	int c, i;
	app.updating++;
	for (c = 0; c < (int)(sizeof(combos) / sizeof(combos[0])); c++) {
		Rid combo = *combos[c];
		Rcombo_box_clear_items(combo);
		Rcombo_box_add_item(combo, "(none)");
		if (d) {
			for (i = 0; i < d->field_count; i++) {
				char item[300];
				const char* kind = d->types[i] == 'q' ? "number" : d->types[i] == 't' ? "date" : "text";
				snprintf(item, sizeof(item), "%s  \xC2\xB7 %s", d->fields[i], kind);
				Rcombo_box_add_item(combo, item);
			}
		}
	}
	if (d) {
		Rcombo_box_set_selected(app.x, d->sel_x);
		Rcombo_box_set_selected(app.y, d->sel_y);
		Rcombo_box_set_selected(app.color, d->sel_color);
		Rcombo_box_set_selected(app.label, d->sel_label);
		Rcombo_box_set_selected(app.size, d->sel_size);
		Rcombo_box_set_selected(app.low, d->sel_low);
		Rcombo_box_set_selected(app.high, d->sel_high);
	} else {
		for (c = 0; c < (int)(sizeof(combos) / sizeof(combos[0])); c++) Rcombo_box_set_selected(*combos[c], 0);
	}
	app.updating--;
}

static void refresh_list(void) {
	int i;
	Rlist_view_clear_items(app.files);
	for (i = 0; i < app.dataset_count; i++) {
		char item[320];
		Dataset* d = &app.datasets[i];
		if (d->parsed) snprintf(item, sizeof(item), "%s  (%d rows)", d->name, d->rows);
		else snprintf(item, sizeof(item), "%s", d->name);
		Rlist_view_add_item(app.files, item);
	}
	if (app.active >= 0) Rlist_view_set_selected(app.files, app.active);
}

static void select_dataset(int index) {
	char script[64];
	if (index == app.active) return;
	remember_selection();
	app.active = index;
	fill_combos();
	snprintf(script, sizeof(script), "RC.setActive(%d)", index);
	run_text(script);
	render();
}

static int field_index(Dataset* d, const char* name) {
	int i;
	if (!name || !name[0]) return 0;
	for (i = 0; i < d->field_count; i++) if (strcmp(d->fields[i], name) == 0) return i + 1;
	return 0;
}

/* "dataset" message: idx\trows\tname / guesses / one field\ttype per line. */
static void on_dataset(char* body) {
	char* lines[MAX_FIELDS + 2];
	char* guess[7] = { 0 };
	int line_count = 0, index, i;
	char* p = body;
	while (p && line_count < MAX_FIELDS + 2) {
		char* end = strchr(p, '\n');
		lines[line_count++] = p;
		if (end) *end = '\0';
		p = end ? end + 1 : NULL;
	}
	if (line_count < 2) return;
	index = atoi(lines[0]);
	if (index < 0 || index >= app.dataset_count) return;
	Dataset* d = &app.datasets[index];
	{
		char* tab = strchr(lines[0], '\t');
		d->rows = tab ? atoi(tab + 1) : 0;
	}
	free_fields(d);
	for (i = 2; i < line_count && d->field_count < MAX_FIELDS; i++) {
		char* tab = strchr(lines[i], '\t');
		if (!tab) continue;
		*tab = '\0';
		size_t n = strlen(lines[i]);
		d->fields[d->field_count] = (char*)malloc(n + 1);
		memcpy(d->fields[d->field_count], lines[i], n + 1);
		d->types[d->field_count] = tab[1] == 'q' ? 'q' : tab[1] == 't' ? 't' : 'n';
		d->field_count++;
	}
	/* the guesses of the page: x, y, color, label, low, high, size */
	p = lines[1];
	for (i = 0; i < 7 && p; i++) {
		char* tab = strchr(p, '\t');
		guess[i] = p;
		if (tab) *tab = '\0';
		p = tab ? tab + 1 : NULL;
	}
	if (!d->parsed || d->sel_x < 0) {
		d->sel_x = field_index(d, guess[0]);
		d->sel_y = field_index(d, guess[1]);
		d->sel_color = field_index(d, guess[2]);
		d->sel_label = field_index(d, guess[3]);
		d->sel_low = field_index(d, guess[4]);
		d->sel_high = field_index(d, guess[5]);
		d->sel_size = field_index(d, guess[6]);
	}
	d->parsed = 1;
	if (app.active != index) remember_selection();
	app.active = index;
	refresh_list();
	fill_combos();
	{
		char message[400];
		snprintf(message, sizeof(message), "%s: %d rows, %d fields", d->name, d->rows, d->field_count);
		set_status(message);
	}
	render();
}

/* ---------------------------------------------------------------------------
 *  Exports
 * ------------------------------------------------------------------------- */

static int base64_value(char c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+' || c == '-') return 62;
	if (c == '/' || c == '_') return 63;
	return -1;
}

static void save_export(const char* body, int png) {
	char message[1200];
	if (!app.export_path[0]) return;
	if (png) {
		const char* comma = strchr(body, ',');
		const char* p = comma ? comma + 1 : body;
		size_t length = strlen(p), n = 0;
		unsigned char* bytes = (unsigned char*)malloc(length / 4 * 3 + 4);
		unsigned int buffer = 0;
		int bits = 0;
		if (!bytes) return;
		for (; *p; p++) {
			int v = base64_value(*p);
			if (v < 0) continue;
			buffer = (buffer << 6) | (unsigned int)v;
			bits += 6;
			if (bits >= 8) {
				bits -= 8;
				bytes[n++] = (unsigned char)((buffer >> bits) & 0xFF);
			}
		}
		snprintf(message, sizeof(message), Rfile_write(app.export_path, bytes, n) ? "Saved %s" : "Cannot write %s", app.export_path);
		free(bytes);
	} else {
		snprintf(message, sizeof(message), Rfile_write(app.export_path, body, strlen(body)) ? "Saved %s" : "Cannot write %s", app.export_path);
	}
	set_status(message);
	app.export_path[0] = '\0';
}

static void action_export(Rid sender, const char* action, void* user_data) {
	int png = strcmp(action, "export.png") == 0;
	RFileDialogDesc desc;
	RFileFilter filter;
	const char* path;
	(void)sender;
	(void)user_data;
	if (strcmp(action, "export.copy") == 0) {
		Rclipboard_set_text(Rtext_editor_get_text(app.spec));
		set_status("The Vega-Lite spec is on the clipboard.");
		return;
	}
	memset(&desc, 0, sizeof(desc));
	filter.name = png ? "PNG image" : "SVG image";
	filter.patterns = png ? "*.png" : "*.svg";
	desc.parent = app.window;
	desc.title = png ? "Export the chart as PNG" : "Export the chart as SVG";
	desc.file_name = png ? "chart.png" : "chart.svg";
	desc.default_extension = png ? "png" : "svg";
	desc.filters = &filter;
	desc.filter_count = 1;
	path = Rfile_dialog_save(&desc);
	if (!path) return;
	snprintf(app.export_path, sizeof(app.export_path), "%s", path);
	run_text(png ? "RC.exportPNG(2)" : "RC.exportSVG()");
}

/* ---------------------------------------------------------------------------
 *  Messages of the page
 * ------------------------------------------------------------------------- */

static void on_message(Rid view, const char* message, void* user_data) {
	const char* newline = strchr(message, '\n');
	size_t kind_length = newline ? (size_t)(newline - message) : strlen(message);
	const char* body = newline ? newline + 1 : "";
	char kind[16];
	(void)view;
	(void)user_data;
	if (kind_length >= sizeof(kind)) return;
	memcpy(kind, message, kind_length);
	kind[kind_length] = '\0';

	if (strcmp(kind, "ready") == 0) {
		int i;
		app.ready = 1;
		if (Rcheckbox_is_checked(app.light)) run_text("RC.setTheme('light')");
		for (i = 0; i < app.pending_count; i++) {
			Rweb_view_run_javascript(app.view, app.pending[i].data, NULL, NULL);
			str_free(&app.pending[i]);
		}
		free(app.pending);
		app.pending = NULL;
		app.pending_count = 0;
	} else if (strcmp(kind, "dataset") == 0) {
		size_t n = strlen(body);
		char* copy = (char*)malloc(n + 1);
		if (!copy) return;
		memcpy(copy, body, n + 1);
		on_dataset(copy);
		free(copy);
	} else if (strcmp(kind, "spec") == 0) {
		Rtext_editor_set_text(app.spec, body);
	} else if (strcmp(kind, "specfile") == 0) {
		Rtext_editor_set_text(app.spec, body);
		set_status("Opened a Vega-Lite spec: edit it in the Vega-Lite spec tab.");
	} else if (strcmp(kind, "drawn") == 0) {
		Dataset* d = active_dataset();
		int style = Rcombo_box_get_selected(app.style);
		char text[400];
		app.drawn++;
		if (d && style >= 0 && style < STYLE_COUNT) {
			snprintf(text, sizeof(text), "%s  \xC2\xB7  %s  \xC2\xB7  %d rows", STYLES[style].name, d->name, d->rows);
			set_status(text);
		}
	} else if (strcmp(kind, "error") == 0) {
		char text[600];
		snprintf(text, sizeof(text), "Problem: %.560s", body);
		set_status(text);
		app.export_path[0] = '\0';
	} else if (strcmp(kind, "svg") == 0) {
		save_export(body, 0);
	} else if (strcmp(kind, "png") == 0) {
		save_export(body, 1);
	}
}

/* ---------------------------------------------------------------------------
 *  Actions
 * ------------------------------------------------------------------------- */

static void action_file(Rid sender, const char* action, void* user_data) {
	(void)sender;
	(void)user_data;
	if (strcmp(action, "file.open") == 0) {
		static const RFileFilter filters[] = {
			{ "Data and specs (CSV, TSV, JSON)", "*.csv;*.tsv;*.tab;*.txt;*.json;*.jsonl;*.ndjson;*.vl" },
			{ "All files", "*" },
		};
		RFileDialogDesc desc;
		int i;
		memset(&desc, 0, sizeof(desc));
		desc.parent = app.window;
		desc.title = "Open data files";
		desc.filters = filters;
		desc.filter_count = 2;
		desc.flags = R_FILE_DIALOG_MULTIPLE;
		if (!Rfile_dialog_open(&desc)) return;
		for (i = 0; i < Rfile_dialog_get_result_count(); i++) open_path(Rfile_dialog_get_result(i));
	} else if (strcmp(action, "file.demo") == 0) {
		char path[1200];
		snprintf(path, sizeof(path), "%s/data/demo_models_FICTIONAL.csv", app.dir);
		open_path(path);
	} else if (strcmp(action, "file.reload") == 0) {
		Dataset* d = active_dataset();
		if (d) send_file(app.active);
	} else if (strcmp(action, "file.remove") == 0) {
		Dataset* d = active_dataset();
		char script[64];
		int i;
		if (!d) return;
		/* the page keys its datasets by index: they are all sent again, renumbered */
		snprintf(script, sizeof(script), "RC.datasets = {}");
		run_text(script);
		free_fields(d);
		for (i = app.active; i < app.dataset_count - 1; i++) app.datasets[i] = app.datasets[i + 1];
		app.dataset_count--;
		app.active = app.dataset_count > 0 ? 0 : -1;
		for (i = 0; i < app.dataset_count; i++) send_file(i);
		refresh_list();
		fill_combos();
		if (app.dataset_count == 0) {
			Rtext_editor_set_text(app.spec, "");
			run_text("location.reload()");
			app.ready = 0;
			set_status("Open a CSV, TSV or JSON file, or drop one on the window.");
		}
	}
}

static void update_style_help(void) {
	int style = Rcombo_box_get_selected(app.style);
	if (style >= 0 && style < STYLE_COUNT) Rlabel_set_text(app.style_help, STYLES[style].help);
}

static void action_chart(Rid sender, const char* action, void* user_data) {
	(void)sender;
	(void)user_data;
	if (strcmp(action, "chart.style") == 0) update_style_help();
	render();
}

static void action_spec(Rid sender, const char* action, void* user_data) {
	(void)sender;
	(void)user_data;
	if (strcmp(action, "spec.apply") == 0) {
		Str s = { 0 };
		str_add(&s, "RC.renderSpec(");
		str_quoted(&s, Rtext_editor_get_text(app.spec));
		str_add(&s, ")");
		run(&s);
		Rtab_view_set_active(app.tabs, 0);
	} else {
		render();
		Rtab_view_set_active(app.tabs, 0);
	}
}

static void action_theme(Rid sender, const char* action, void* user_data) {
	int light = Rcheckbox_is_checked(sender);
	(void)action;
	(void)user_data;
	Rtheme_apply(light ? R_THEME_LIGHT : R_THEME_DARK);
	run_text(light ? "RC.setTheme('light')" : "RC.setTheme('dark')");
}

static void on_file_selected(Rid list, int index, void* user_data) {
	(void)list;
	(void)user_data;
	if (index >= 0 && index < app.dataset_count && app.datasets[index].parsed) select_dataset(index);
}

static int on_drop(Rid target, const RDropEvent* event, void* user_data) {
	int i;
	(void)target;
	(void)user_data;
	if (event->phase != R_DROP_DROPPED) return event->path_count > 0 || event->drag_type == NULL;
	for (i = 0; i < event->path_count; i++) open_path(event->paths[i]);
	return R_TRUE;
}

static void on_error(RError code, RSeverity severity, const char* detail) {
	fprintf(stderr, "[Raven] %s (%s): %s\n", Rerror_to_string(code), Rseverity_to_string(severity), detail);
}

/* ---------------------------------------------------------------------------
 *  Automated runs: RAVEN_CHARTS_FRAMES=n [RAVEN_CHARTS_CAPTURE=shot.ppm]
 * ------------------------------------------------------------------------- */

static void capture(const char* path) {
	int width = 0, height = 0, i;
	uint8_t* pixels = Rwindow_capture(app.window, &width, &height);
	FILE* file = pixels ? fopen(path, "wb") : NULL;
	if (file) {
		fprintf(file, "P6\n%d %d\n255\n", width, height);
		for (i = 0; i < width * height; i++) fwrite(pixels + i * 4, 1, 3, file);
		fclose(file);
	}
	Rpixels_free(pixels);
}

static int automated(void) {
	const char* frames = getenv("RAVEN_CHARTS_FRAMES");
	const char* shot = getenv("RAVEN_CHARTS_CAPTURE");
	double timeout = frames ? atof(frames) : 0.0;
	int settle = 0;
	if (!frames) return 0;
	/* waits for the browser and a drawn chart (up to RAVEN_CHARTS_FRAMES seconds), then a few frames more */
	{
		double start = Rget_time();
		while (Ris_running() && Rget_time() - start < timeout) {
			Rwindow_request_redraw(app.window);
			Rwait_frame(0.02);
			if ((app.drawn || app.dataset_count == 0) && app.ready && ++settle > 60) break;
		}
	}
	if (shot) capture(shot);
	printf("ready=%d datasets=%d drawn=%d\n", app.ready, app.dataset_count, app.drawn);
	return 1;
}

/* ---------------------------------------------------------------------------
 *  Start
 * ------------------------------------------------------------------------- */

int main(int argc, char** argv) {
	RInitDesc desc;
	char path[1200];
	const char* dir = getenv("RAVEN_CHARTS_DIR");
	const char* start_style = getenv("RAVEN_CHARTS_STYLE");
	int i;

	snprintf(app.dir, sizeof(app.dir), "%s", dir && dir[0] ? dir : RAVEN_CHARTS_DIR);
	app.active = -1;

	memset(&desc, 0, sizeof(desc));
	desc.backend = R_BACKEND_VULKAN;
	desc.app_name = "Raven Charts";
	Rerror_set_callback(on_error);
	if (!Rinit_ex(&desc)) {
		fprintf(stderr, "Raven Charts: cannot start Raven (is a Vulkan driver installed?)\n");
		return 1;
	}
	Rtheme_apply(R_THEME_DARK);
	Rweb_register_xml();
	if (!Rweb_is_available()) {
		fprintf(stderr, "Raven Charts: no browser of the Chromium family was found (Edge, Chrome, Chromium).\n"
			"Install one, or set RAVEN_WEB_BROWSER to the path of its program.\n");
	}

	Raction_bind("file.open", action_file, NULL);
	Raction_bind("file.demo", action_file, NULL);
	Raction_bind("file.reload", action_file, NULL);
	Raction_bind("file.remove", action_file, NULL);
	Raction_bind("chart.style", action_chart, NULL);
	Raction_bind("chart.update", action_chart, NULL);
	Raction_bind("spec.apply", action_spec, NULL);
	Raction_bind("spec.reset", action_spec, NULL);
	Raction_bind("export.png", action_export, NULL);
	Raction_bind("export.svg", action_export, NULL);
	Raction_bind("export.copy", action_export, NULL);
	Raction_bind("view.theme", action_theme, NULL);

	snprintf(path, sizeof(path), "%s/ui/main.xml", app.dir);
	app.window = Rwindow_create_from_xml_file(path);
	if (app.window == R_INVALID_ID) {
		fprintf(stderr, "Raven Charts: cannot load %s (set RAVEN_CHARTS_DIR to the folder holding ui/ and web/)\n", path);
		Rshutdown();
		return 1;
	}
#define FIND(field, name) app.field = Rwidget_find(app.window, name)
	FIND(view, "chart"); FIND(files, "files"); FIND(style, "style"); FIND(style_help, "style_help");
	FIND(status, "status"); FIND(tabs, "tabs"); FIND(spec, "spec"); FIND(light, "light");
	FIND(x, "x"); FIND(y, "y"); FIND(color, "color"); FIND(label, "label"); FIND(size, "size");
	FIND(low, "low"); FIND(high, "high"); FIND(aggregate, "aggregate"); FIND(topn, "topn");
	FIND(logy, "logy"); FIND(logx, "logx"); FIND(pergroup, "pergroup"); FIND(title, "title"); FIND(filter, "filter");
#undef FIND

	app.updating++;
	for (i = 0; i < STYLE_COUNT; i++) {
		Rcombo_box_add_item(app.style, STYLES[i].name);
		if (start_style && strcmp(start_style, STYLES[i].id) == 0) Rcombo_box_set_selected(app.style, i);
	}
	if (Rcombo_box_get_selected(app.style) < 0) Rcombo_box_set_selected(app.style, 0);
	app.updating--;
	update_style_help();
	fill_combos();

	Rweb_view_set_message_callback(app.view, on_message, NULL);
	Rlist_view_set_selection_changed_callback(app.files, on_file_selected, NULL);
	Rwindow_set_drop_callback(app.window, on_drop, NULL);
	Rwindow_show(app.window);

	for (i = 1; i < argc; i++) open_path(argv[i]);

	if (!automated()) {
		while (Rwait_frame(-1.0)) {
		}
	}
	for (i = 0; i < app.dataset_count; i++) free_fields(&app.datasets[i]);
	Rshutdown();
	return Rget_exit_code();
}
