/*
 * Raven Charts - the chart page.
 *
 * The Raven side (src/main.c) owns the controls and the files. It calls the
 * functions of RC below with Rweb_view_run_javascript; the page answers with
 * window.raven.postMessage("<kind>\n<body>"):
 *
 *   ready                       the page is loaded
 *   dataset  idx\trows\tname    a file was parsed; then a line of guesses
 *            x\ty\tcolor\tlabel\tlow\thigh\tsize, then one "field\ttype" per line
 *   specfile <json>             the file was a Vega-Lite spec: it is shown as it is
 *   spec     <json>             the spec just drawn (for the Spec tab)
 *   error    <text>             parsing or drawing failed
 *   svg      <svg>              answer to RC.exportSVG()
 *   png      <data url>         answer to RC.exportPNG()
 */
(function () {
	"use strict";

	const RC = (window.RC = {
		datasets: {},   // idx -> { name, alias, rows, fields: [{name, type}] }
		active: null,   // idx of the dataset the controls build charts for
		view: null,
		theme: "dark",
	});

	function post(kind, body) {
		const text = kind + "\n" + (body == null ? "" : String(body));
		if (window.raven && window.raven.postMessage) window.raven.postMessage(text);
		else console.log(text);
	}

	/* ------------------------------------------------------------------ */
	/*  Parsing and field types                                            */
	/* ------------------------------------------------------------------ */

	const NUMBER = /^\s*[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?\s*%?\s*$/;
	const DATE = /^\s*(\d{4}-\d{1,2}(-\d{1,2})?([T ]\d{1,2}:\d{2}(:\d{2}(\.\d+)?)?(Z|[+-]\d{2}:?\d{2})?)?|\d{1,2}\/\d{1,2}\/\d{4}|\d{4}\/\d{1,2}\/\d{1,2}|[A-Za-z]{3,9}\.? \d{1,2},? \d{4}|\d{1,2} [A-Za-z]{3,9}\.? \d{4})\s*$/;

	function isSpec(json) {
		return json && !Array.isArray(json) && typeof json === "object" &&
			(typeof json.$schema === "string" && /vega/.test(json.$schema) ||
			 "mark" in json || "layer" in json || "hconcat" in json || "vconcat" in json ||
			 "concat" in json || "facet" in json || "repeat" in json);
	}

	function sniffDelimiter(text) {
		const head = text.slice(0, 4000).split(/\r?\n/).slice(0, 5).join("\n");
		const count = (c) => head.split(c).length - 1;
		const options = [[",", count(",")], ["\t", count("\t")], [";", count(";")], ["|", count("|")]];
		options.sort((a, b) => b[1] - a[1]);
		return options[0][1] > 0 ? options[0][0] : ",";
	}

	function rowsFromJSON(json) {
		if (Array.isArray(json)) return json;
		if (json && typeof json === "object") {
			for (const key of ["data", "values", "rows", "records", "items", "results"]) {
				if (Array.isArray(json[key])) return json[key];
			}
			for (const key in json) if (Array.isArray(json[key])) return json[key];
		}
		throw new Error("This JSON file holds no array of records.");
	}

	/* Records with nested objects are flattened one level: {a: {b: 1}} -> "a.b". */
	function flatten(rows) {
		return rows.filter((r) => r && typeof r === "object").map((r) => {
			const out = {};
			for (const k in r) {
				const v = r[k];
				if (v && typeof v === "object" && !Array.isArray(v)) {
					for (const k2 in v) if (typeof v[k2] !== "object") out[k + "." + k2] = v[k2];
				} else if (!Array.isArray(v)) out[k] = v;
			}
			return out;
		});
	}

	function inferType(name, rows) {
		let seen = 0, numbers = 0, dates = 0;
		const limit = Math.min(rows.length, 5000);
		for (let i = 0; i < limit; i++) {
			const v = rows[i][name];
			if (v === null || v === undefined || v === "") continue;
			seen++;
			if (typeof v === "number" || typeof v === "boolean") { numbers += typeof v === "number" ? 1 : 0; continue; }
			const s = String(v);
			if (NUMBER.test(s)) numbers++;
			else if (DATE.test(s) && !isNaN(Date.parse(s))) dates++;
		}
		if (!seen) return "nominal";
		if (dates / seen >= 0.9) return "temporal";
		if (numbers / seen >= 0.9) return "quantitative";
		return "nominal";
	}

	function convert(rows, fields) {
		for (const row of rows) {
			for (const f of fields) {
				const v = row[f.name];
				if (v === "" || v === undefined) { row[f.name] = null; continue; }
				if (v === null) continue;
				if (f.type === "quantitative" && typeof v !== "number") {
					const n = parseFloat(String(v).replace("%", ""));
					row[f.name] = isFinite(n) ? n : null;
				} else if (f.type === "temporal" && typeof v !== "number") {
					const t = Date.parse(String(v).trim());
					row[f.name] = isNaN(t) ? null : t;  // epoch milliseconds: a temporal scale reads them as dates
				}
			}
		}
	}

	/* A field, with its distinct values when there are few (most common first): the "Show only" list. */
	function describe(name, type, rows) {
		const counts = new Map();
		for (const r of rows) {
			const v = r[name];
			if (v === null || v === undefined || v === "") continue;
			counts.set(v, (counts.get(v) || 0) + 1);
		}
		const sorted = [...counts.entries()].sort((a, b) => b[1] - a[1]);
		return { name, type, distinct: counts.size, top: sorted.length ? String(sorted[0][0]) : "",
			values: type === "nominal" && counts.size <= 300 ? sorted.map((e) => String(e[0])) : [] };
	}

	function guess(fields) {
		const of = (type) => fields.filter((f) => f.type === type);
		const pick = (re, types, not) => {
			const f = fields.find((f) => re.test(f.name) && (!types || types.includes(f.type)) && !(not || []).includes(f.name));
			return f ? f.name : "";
		};
		const temporal = of("temporal"), quantitative = of("quantitative"), nominal = of("nominal");
		/* the date of the record itself, not of something it refers to (benchmark_release_date) */
		const x = pick(/^(date|release[ _]?date|model[ _]?release[ _]?date|publication[ _]?date|published)$/i, ["temporal"]) ||
			pick(/^(?!.*(benchmark|dataset|task)).*(release|publication|published|date)/i, ["temporal"]) ||
			pick(/release|publication|published|date/i, ["temporal"]) ||
			(temporal[0] && temporal[0].name) || (quantitative[0] && quantitative[0].name) || (fields[0] && fields[0].name) || "";
		const y = pick(/(^|[^a-z])eci([^a-z]|$)|capabilit/i, ["quantitative"], [x]) ||
			pick(/score|accuracy|performance|value|mean/i, ["quantitative"], [x]) ||
			((quantitative.find((f) => f.name !== x) || {}).name) || "";
		const root = y.toLowerCase().replace(/[^a-z0-9]/g, "");
		const bound = (re) => {
			const candidates = quantitative.filter((f) => f.name !== y && f.name !== x && re.test(f.name));
			const near = candidates.find((f) => root && f.name.toLowerCase().replace(/[^a-z0-9]/g, "").includes(root));
			return (near || candidates[0] || {}).name || "";
		};
		const low = bound(/(lower|low|_lo$|ci_?l|p0?5\b|5%|2\.5%)/i);
		const high = bound(/(upper|high|_hi$|ci_?h|ci_?u|p95\b|95%|97\.5%)/i);
		/* who made it; else, in "long" files (one row per model and benchmark), what was measured */
		const fewValues = (f) => f.type === "nominal" && f.distinct > 1 && f.distinct <= 80;
		const color = pick(/organi[sz]ation|^org|^lab$|developer|company|country|publisher|provider|vendor/i, ["nominal"]) ||
			((fields.find((f) => /^(benchmark|task|dataset|category|metric)$/i.test(f.name) && fewValues(f)) || {}).name) || "";
		const label = pick(/^(model|model[ _]?name|name|system|display[ _]?name)$/i, ["nominal"], [color]) ||
			pick(/^(?!.*(_id|id$|version)).*(model|system|display)/i, ["nominal"], [color]) ||
			pick(/model|system|^name$|display/i, ["nominal"], [color]) ||
			((nominal.find((f) => f.name !== color) || {}).name) || "";
		const colorField = fields.find((f) => f.name === color);
		const isOrganization = /organi[sz]ation|^org|^lab$|developer|company|country|publisher|provider|vendor/i.test(color);
		const only = color && !isOrganization && colorField && colorField.distinct > 12 ? colorField.top : "";
		return { x, y, color, label, low: low && high ? low : "", high: low && high ? high : "", size: "", only };
	}

	RC.loadDataset = function (idx, name, text, ext) {
		try {
			if (text.charCodeAt(0) === 0xfeff) text = text.slice(1);
			ext = String(ext || "").toLowerCase();
			let rows;
			if (ext === "json" || ext === "vl" || ((ext === "" || ext === "txt") && /^\s*[[{]/.test(text))) {
				const json = JSON.parse(text);
				if (isSpec(json)) {
					post("specfile", JSON.stringify(json, null, 2));
					RC.renderSpec(JSON.stringify(json));
					return;
				}
				rows = flatten(rowsFromJSON(json));
			} else if (ext === "ndjson" || ext === "jsonl") {
				rows = flatten(text.split(/\r?\n/).filter((l) => l.trim()).map((l) => JSON.parse(l)));
			} else {
				const delimiter = ext === "tsv" || ext === "tab" ? "\t" : sniffDelimiter(text);
				rows = vega.read(text, { type: "dsv", delimiter: delimiter });
			}
			if (!rows.length) throw new Error("The file has no rows.");
			const names = [];
			for (const r of rows.slice(0, 200)) for (const k in r) if (!names.includes(k)) names.push(k);
			const fields = names.filter((n) => n.trim() !== "").map((n) => describe(n, inferType(n, rows), rows));
			convert(rows, fields);
			const alias = String(name).replace(/\.[^.]*$/, "").replace(/[^A-Za-z0-9_]+/g, "_").replace(/^(\d)/, "_$1") || "data";
			RC.datasets[idx] = { name, alias, rows, fields };
			const g = guess(fields);
			const clean = (s) => String(s).replace(/[\t\r\n]+/g, " ");
			post("dataset", [
				[idx, rows.length, clean(name)].join("\t"),
				[g.x, g.y, g.color, g.label, g.low, g.high, g.size, g.only].map(clean).join("\t"),
				...fields.map((f) => [clean(f.name), f.type].concat(f.values.map(clean)).join("\t")),
			].join("\n"));
		} catch (e) {
			post("error", name + ": " + (e && e.message ? e.message : e));
		}
	};

	RC.removeDataset = function (idx) { delete RC.datasets[idx]; };

	/* ------------------------------------------------------------------ */
	/*  Look                                                               */
	/* ------------------------------------------------------------------ */

	const PALETTE = {
		dark: ["#7aa2ff", "#ff9f5a", "#56d4a4", "#f472b6", "#c4a7ff", "#ffd166", "#4cc9f0", "#ef6b6b", "#a3d977", "#d4b483", "#9aa0b4", "#ff8fab"],
		light: ["#2f5fd0", "#d9622b", "#16996b", "#c2347e", "#7a52cc", "#b88a00", "#0f8fb5", "#c63d3d", "#5a9a2a", "#8a6a3a", "#5d6275", "#d65a7a"],
	};

	function themeConfig(name) {
		const dark = name !== "light";
		const fg = dark ? "#e6e6e8" : "#1d1d22";
		const muted = dark ? "#a3a3ad" : "#5b5b66";
		const line = dark ? "#45454d" : "#c9c9d1";
		const grid = dark ? "#26262c" : "#ececf0";
		const font = "system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif";
		return {
			background: dark ? "#161618" : "#ffffff",
			font: font,
			view: { stroke: null },
			padding: 8,
			axis: { domainColor: line, tickColor: line, gridColor: grid, labelColor: muted, titleColor: fg,
				labelFontSize: 11, titleFontSize: 12, titleFontWeight: 500, labelPadding: 4, titlePadding: 10 },
			legend: { labelColor: muted, titleColor: fg, labelFontSize: 11, titleFontSize: 12, titleFontWeight: 500,
				symbolLimit: 24, labelLimit: 200, symbolOpacity: 1 },
			title: { color: fg, subtitleColor: muted, fontSize: 17, subtitleFontSize: 12, fontWeight: 600, anchor: "start", offset: 14 },
			header: { labelColor: muted, titleColor: fg },
			range: { category: PALETTE[dark ? "dark" : "light"] },
			mark: { color: PALETTE[dark ? "dark" : "light"][0] },
			text: { color: fg },
			rule: { color: line },
			boxplot: { median: { color: dark ? "#161618" : "#ffffff" } },
		};
	}

	RC.setTheme = function (name) {
		RC.theme = name === "light" ? "light" : "dark";
		document.documentElement.style.setProperty("--bg", RC.theme === "light" ? "#ffffff" : "#161618");
		document.documentElement.style.setProperty("--fg", RC.theme === "light" ? "#1d1d22" : "#e6e6e8");
		if (RC.lastSpec) embed(RC.lastSpec, false);
	};

	/* ------------------------------------------------------------------ */
	/*  Chart styles                                                       */
	/* ------------------------------------------------------------------ */

	const json = (s) => JSON.stringify(s);
	const ref = (f) => "datum[" + json(f) + "]";

	function build(cfg) {
		const ds = RC.datasets[cfg.dataset];
		if (!ds) throw new Error("Open a file first.");
		const types = Object.fromEntries(ds.fields.map((f) => [f.name, f.type]));
		const has = (f) => f && f in types;
		const T = (f) => types[f];
		const x = has(cfg.x) ? cfg.x : "", y = has(cfg.y) ? cfg.y : "";
		const color = has(cfg.color) ? cfg.color : "", label = has(cfg.label) ? cfg.label : "";
		const low = has(cfg.low) ? cfg.low : "", high = has(cfg.high) ? cfg.high : "";
		const size = has(cfg.size) ? cfg.size : "";
		const topN = Math.max(1, cfg.topN | 0 || 10);
		const agg = cfg.aggregate && cfg.aggregate !== "none" ? cfg.aggregate : "";
		const style = cfg.style || "scatter";
		const needs = (...list) => { for (const [f, what] of list) if (!f) throw new Error("This style needs " + what + "."); };

		const fmt = (f) => (T(f) === "temporal" ? { format: "%Y-%m-%d" } : T(f) === "quantitative" ? { format: ",.4~g" } : {});
		const enc = (f, extra) => Object.assign({ field: f, type: T(f), title: f }, extra || {});
		const yScale = cfg.logY ? { type: "log" } : { zero: false };
		const xScale = cfg.logX && T(x) === "quantitative" ? { type: "log" } : T(x) === "quantitative" ? { zero: false } : undefined;
		const xEnc = () => enc(x, xScale ? { scale: xScale } : {});
		const yEnc = () => enc(y, { scale: yScale });
		const colorEnc = () => color ? { field: color, type: T(color) === "quantitative" ? "quantitative" : "nominal", title: color, legend: cfg.only ? null : undefined } : undefined;
		const tooltip = () => {
			const list = [];
			for (const f of [label, x, y, low, high, color, size]) {
				if (f && !list.some((t) => t.field === f)) list.push(Object.assign({ field: f, type: T(f) }, fmt(f)));
			}
			return list;
		};
		const valid = (...fs) => fs.filter(Boolean).map((f) => {
			const q = T(f) !== "nominal";
			const pos = cfg.logY && f === y || cfg.logX && f === x;
			return { filter: q ? "isValid(" + ref(f) + ") && isFinite(" + ref(f) + ")" + (pos ? " && " + ref(f) + " > 0" : "") : "isValid(" + ref(f) + ")" };
		});
		const transforms = [];
		if (color && cfg.only) transforms.push({ filter: ref(color) + " == " + json(cfg.only) });
		if (cfg.filter && cfg.filter.trim()) transforms.push({ filter: cfg.filter.trim() });

		const point = (extra) => ({
			mark: Object.assign({ type: "point", filled: true, size: 70, opacity: 0.85, stroke: null }, extra || {}),
			encoding: Object.assign({ x: xEnc(), y: yEnc(), tooltip: tooltip() },
				color ? { color: colorEnc() } : {}, size ? { size: enc(size, { scale: { range: [20, 600] } }) } : {}),
		});
		const labels = (filterExpr) => label ? {
			transform: filterExpr ? [{ filter: filterExpr }] : [],
			mark: { type: "text", align: "left", baseline: "bottom", dx: 7, dy: -4, fontSize: 11, opacity: 0.9 },
			encoding: Object.assign({ x: xEnc(), y: yEnc(), text: { field: label, type: "nominal" } }, color ? { color: colorEnc() } : {}),
		} : null;
		const groupby = cfg.perGroup && color ? [color] : [];

		let body;
		switch (style) {
			case "scatter":
			case "bubble": {
				needs([x, "an X field"], [y, "a Y field"]);
				if (style === "bubble") needs([size, "a Size field"]);
				transforms.push(...valid(x, y));
				body = point();
				break;
			}
			case "labeled": {
				needs([x, "an X field"], [y, "a Y field"]);
				transforms.push(...valid(x, y),
					{ window: [{ op: "row_number", as: "__rank" }], sort: [{ field: y, order: "descending" }] });
				body = { layer: [point(), labels("datum.__rank <= " + topN)].filter(Boolean) };
				break;
			}
			case "frontier": {
				needs([x, "an X field"], [y, "a Y field"]);
				transforms.push(...valid(x, y),
					{ window: [{ op: "max", field: y, as: "__best" }], sort: [{ field: x }, { field: y, order: "descending" }], frame: [null, 0], groupby },
					{ window: [{ op: "lag", field: "__best", as: "__previous" }], sort: [{ field: x }, { field: y, order: "descending" }], groupby },
					{ calculate: "!isValid(datum.__previous) || " + ref(y) + " > datum.__previous", as: "__record" });
				const record = "datum.__record";
				body = { layer: [
					point({ opacity: 0.28, size: 45 }),
					{ mark: { type: "line", interpolate: "step-after", strokeWidth: 2 },
						encoding: Object.assign({ x: xEnc(), y: { field: "__best", type: "quantitative", title: y, scale: yScale } },
							color && groupby.length ? { color: colorEnc() } : {}) },
					Object.assign(point({ size: 110, opacity: 1, strokeWidth: 1.5 }), { transform: [{ filter: record }] }),
					labels(record),
				].filter(Boolean) };
				break;
			}
			case "trend":
			case "loess": {
				needs([x, "an X field"], [y, "a Y field"]);
				transforms.push(...valid(x, y));
				const fit = style === "trend"
					? { regression: y, on: x, method: cfg.logY ? "exp" : "linear", groupby }
					: { loess: y, on: x, bandwidth: 0.4, groupby };
				body = { layer: [
					point({ opacity: 0.7 }),
					{ transform: [fit], mark: { type: "line", strokeWidth: 2.5 },
						encoding: Object.assign({ x: xEnc(), y: yEnc() }, groupby.length ? { color: colorEnc() } : {}) },
				] };
				if (!groupby.length) body.layer[1].mark.color = RC.theme === "light" ? "#1d1d22" : "#f2f2f4";
				break;
			}
			case "line":
			case "linepoint":
			case "step":
			case "area": {
				needs([x, "an X field"], [y, "a Y field"]);
				transforms.push(...valid(x, y));
				const mark = style === "area" ? { type: "area", opacity: color ? 0.35 : 0.85, line: true, interpolate: "monotone" }
					: { type: "line", strokeWidth: 2, point: style === "linepoint", interpolate: style === "step" ? "step-after" : "monotone" };
				const yy = agg ? enc(y, { aggregate: agg, scale: yScale, title: agg + " of " + y }) : yEnc();
				if (style === "area") yy.stack = null;  /* overlaid: records rarely share their X values */
				body = { mark, encoding: Object.assign({ x: xEnc(), y: yy, tooltip: agg ? undefined : tooltip() }, color ? { color: colorEnc() } : {}) };
				if (agg) body.encoding.tooltip = [enc(x, fmt(x)), Object.assign(enc(y, { aggregate: agg }), fmt(y))];
				break;
			}
			case "bar": {
				needs([x, "an X field"]);
				transforms.push(...valid(x));
				const yy = !y || agg === "count" ? { aggregate: "count", type: "quantitative", title: "Count" }
					: agg ? enc(y, { aggregate: agg, title: agg + " of " + y }) : enc(y);
				const xx = T(x) === "temporal" ? enc(x, { timeUnit: "yearmonth", title: x }) : T(x) === "quantitative" ? enc(x, { bin: { maxbins: 40 } }) : enc(x, { sort: "-y" });
				body = { mark: { type: "bar", cornerRadiusEnd: 2 }, encoding: Object.assign({ x: xx, y: yy, tooltip: [xx, yy] }, color ? { color: colorEnc() } : {}) };
				break;
			}
			case "leaderboard": {
				const category = label || (T(x) === "nominal" ? x : "") || color;
				needs([category, "a Label (or a text X) field"], [y, "a Y field"]);
				transforms.push(...valid(category, y));
				if (agg && agg !== "count") {
					transforms.push({ aggregate: [{ op: agg, field: y, as: y }], groupby: [category].concat(color && color !== category ? [color] : []) });
				}
				transforms.push({ window: [{ op: "row_number", as: "__rank" }], sort: [{ field: y, order: "descending" }] }, { filter: "datum.__rank <= " + topN });
				const cat = { field: category, type: "nominal", sort: "-x", title: null, axis: { labelLimit: 260 } };
				const val = { field: y, type: "quantitative", title: agg && agg !== "count" ? agg + " of " + y : y, scale: cfg.logY ? { type: "log" } : { zero: true } };
				body = { layer: [
					{ mark: { type: "bar", cornerRadiusEnd: 3 },
						encoding: Object.assign({ y: cat, x: val, tooltip: [{ field: category, type: "nominal" }, Object.assign({ field: y, type: "quantitative" }, fmt(y))] }, color ? { color: colorEnc() } : {}) },
					{ mark: { type: "text", align: "left", dx: 5, fontSize: 11 },
						encoding: { y: cat, x: val, text: { field: y, type: "quantitative", format: ",.4~g" } } },
				] };
				break;
			}
			case "histogram": {
				const f = x || y;
				needs([f, "an X field"]);
				transforms.push(...valid(f));
				const xx = T(f) === "nominal" ? enc(f, { sort: "-y" }) : T(f) === "temporal" ? enc(f, { timeUnit: "yearquarter" }) : enc(f, { bin: { maxbins: 40 } });
				body = { mark: { type: "bar", cornerRadiusEnd: 2, binSpacing: 1 },
					encoding: Object.assign({ x: xx, y: { aggregate: "count", type: "quantitative", title: "Count" } }, color ? { color: colorEnc() } : {}) };
				break;
			}
			case "boxplot":
			case "strip": {
				needs([y, "a Y field"]);
				const category = (T(x) === "nominal" ? x : "") || color;
				transforms.push(...valid(y, category));
				const value = enc(y, { scale: cfg.logY ? { type: "log" } : { zero: false } });
				const enc2 = category
					? { x: { field: category, type: "nominal", title: category, sort: { op: "median", field: y, order: "descending" } }, y: value }
					: { y: value };
				if (style === "boxplot") {
					body = { mark: { type: "boxplot", extent: 1.5, size: 28 }, encoding: Object.assign(enc2, category ? { color: { field: category, type: "nominal", legend: null } } : {}) };
				} else {
					body = { mark: { type: "tick", thickness: 2, opacity: 0.75 }, encoding: Object.assign(enc2, { tooltip: tooltip() }, color ? { color: colorEnc() } : {}) };
				}
				break;
			}
			case "heatmap": {
				needs([x, "an X field"], [y, "a Y field"]);
				transforms.push(...valid(x, y));
				const axis = (f, channel) => T(f) === "nominal" ? enc(f) : T(f) === "temporal" ? enc(f, { timeUnit: "yearquarter" }) : enc(f, { bin: { maxbins: channel === "x" ? 30 : 20 } });
				const value = color && T(color) === "quantitative" && agg && agg !== "count"
					? { field: color, aggregate: agg, type: "quantitative", title: agg + " of " + color }
					: { aggregate: "count", type: "quantitative", title: "Count" };
				body = { mark: { type: "rect", cornerRadius: 2 },
					encoding: { x: axis(x, "x"), y: axis(y, "y"), color: Object.assign(value, { scale: { scheme: RC.theme === "light" ? "blues" : "viridis" } }), tooltip: [value] } };
				break;
			}
			case "errorbar": {
				needs([x, "an X field"], [y, "a Y field"], [low, "an Error low field"], [high, "an Error high field"]);
				transforms.push(...valid(x, y, low, high));
				body = { layer: [
					{ mark: { type: "rule", strokeWidth: 1.5, opacity: 0.55 },
						encoding: Object.assign({ x: xEnc(), y: enc(low, { scale: yScale, title: y }), y2: { field: high } }, color ? { color: colorEnc() } : {}) },
					point({ size: 60, opacity: 1 }),
					labels(null) && Object.assign(labels(null), {
						transform: [{ window: [{ op: "row_number", as: "__rank" }], sort: [{ field: y, order: "descending" }] }, { filter: "datum.__rank <= " + topN }] }),
				].filter(Boolean) };
				break;
			}
			default:
				throw new Error("Unknown style " + style);
		}

		const ds2 = RC.datasets[cfg.dataset];
		const titleText = cfg.title && cfg.title.trim() ? cfg.title.trim()
			: style === "histogram" ? "Distribution of " + (x || y)
			: style === "leaderboard" ? "Top " + topN + " by " + y
			: (y || "Count") + (x ? " by " + x : "");
		const shownTitle = color && cfg.only && !(cfg.title && cfg.title.trim()) ? cfg.only + ": " + titleText : titleText;
		return Object.assign({
			$schema: "https://vega.github.io/schema/vega-lite/v5.json",
			title: { text: shownTitle, subtitle: ds2.name + "  ·  " + ds2.rows.length + " rows" },
			width: "container",
			height: "container",
			autosize: { type: "fit", contains: "padding" },
			data: { name: "table" },
			transform: transforms.concat(body.transform || []),
		}, body, { transform: transforms.concat(body.transform || []) });
	}

	/* ------------------------------------------------------------------ */
	/*  Drawing                                                            */
	/* ------------------------------------------------------------------ */

	function showError(text) {
		const box = document.getElementById("error");
		box.textContent = text;
		box.style.display = text ? "block" : "none";
	}

	/* Every loaded file is available to a spec by name: "table" is the active one, the others by file name. */
	function datasetsFor() {
		const out = {};
		for (const idx in RC.datasets) out[RC.datasets[idx].alias] = RC.datasets[idx].rows;
		if (RC.active != null && RC.datasets[RC.active]) out.table = RC.datasets[RC.active].rows;
		return out;
	}

	async function embed(spec, announce) {
		showError("");
		const full = Object.assign({}, spec);
		const provided = datasetsFor();
		full.datasets = Object.assign({}, provided, spec.datasets || {});
		try {
			const result = await vegaEmbed("#chart", full, {
				actions: false,
				renderer: "canvas",
				config: themeConfig(RC.theme),
				tooltip: { theme: RC.theme },
			});
			if (RC.view && RC.view !== result.view) try { RC.view.finalize(); } catch (e) { /* already gone */ }
			RC.view = result.view;
			RC.lastSpec = spec;
			document.getElementById("empty").style.display = "none";
			if (announce) post("spec", JSON.stringify(spec, null, 2));
			post("drawn", "");
		} catch (e) {
			const text = e && e.message ? e.message : String(e);
			showError(text);
			post("error", text);
		}
	}

	RC.render = function (cfgText) {
		try {
			const cfg = typeof cfgText === "string" ? JSON.parse(cfgText) : cfgText;
			RC.active = cfg.dataset;
			return embed(build(cfg), true);
		} catch (e) {
			const text = e && e.message ? e.message : String(e);
			showError(text);
			post("error", text);
		}
	};

	RC.renderSpec = function (text) {
		try {
			return embed(JSON.parse(text), false);
		} catch (e) {
			const message = "Spec: " + (e && e.message ? e.message : e);
			showError(message);
			post("error", message);
		}
	};

	RC.setActive = function (idx) { RC.active = idx; };

	RC.exportSVG = async function () {
		if (!RC.view) return post("error", "Nothing to export yet.");
		post("svg", await RC.view.toSVG());
	};

	RC.exportPNG = async function (scale) {
		if (!RC.view) return post("error", "Nothing to export yet.");
		post("png", await RC.view.toImageURL("png", scale || 2));
	};

	/* For the tests and the console: the spec the controls would build. */
	RC.build = build;

	window.addEventListener("load", () => post("ready", ""));
})();
