// Browser side of the Peter runtime port: DOM windows/controls, canvases, text rendering, input, audio, dialogs.
'use strict';

var VK = (function () {
	var m = {
		Backspace: 8, Tab: 9, Enter: 13, NumpadEnter: 13, ShiftLeft: 0xA0, ShiftRight: 0xA1, ControlLeft: 0xA2, ControlRight: 0xA3,
		AltLeft: 0xA4, AltRight: 0xA5, Pause: 19, CapsLock: 20, Escape: 27, Space: 32, PageUp: 33, PageDown: 34, End: 35, Home: 36,
		ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40, PrintScreen: 44, Insert: 45, Delete: 46,
		MetaLeft: 91, MetaRight: 92, ContextMenu: 93, NumLock: 144, ScrollLock: 145,
		Numpad0: 96, Numpad1: 97, Numpad2: 98, Numpad3: 99, Numpad4: 100, Numpad5: 101, Numpad6: 102, Numpad7: 103, Numpad8: 104, Numpad9: 105,
		NumpadMultiply: 106, NumpadAdd: 107, NumpadSubtract: 109, NumpadDecimal: 110, NumpadDivide: 111,
		Semicolon: 186, Equal: 187, Comma: 188, Minus: 189, Period: 190, Slash: 191, Backquote: 192,
		BracketLeft: 219, Backslash: 220, BracketRight: 221, Quote: 222, IntlBackslash: 226
	};
	for (var i = 0; i < 26; i++) m['Key' + String.fromCharCode(65 + i)] = 65 + i;
	for (var d = 0; d < 10; d++) m['Digit' + d] = 48 + d;
	for (var f = 1; f <= 24; f++) m['F' + f] = 111 + f;
	return m;
})();
var SCAN = { Escape: 1, Digit1: 2, Digit2: 3, Digit3: 4, Digit4: 5, Digit5: 6, Digit6: 7, Digit7: 8, Digit8: 9, Digit9: 10, Digit0: 11,
	Minus: 12, Equal: 13, Backspace: 14, Tab: 15, KeyQ: 16, KeyW: 17, KeyE: 18, KeyR: 19, KeyT: 20, KeyY: 21, KeyU: 22, KeyI: 23, KeyO: 24,
	KeyP: 25, BracketLeft: 26, BracketRight: 27, Enter: 28, ControlLeft: 29, KeyA: 30, KeyS: 31, KeyD: 32, KeyF: 33, KeyG: 34, KeyH: 35,
	KeyJ: 36, KeyK: 37, KeyL: 38, Semicolon: 39, Quote: 40, Backquote: 41, ShiftLeft: 42, Backslash: 43, KeyZ: 44, KeyX: 45, KeyC: 46,
	KeyV: 47, KeyB: 48, KeyN: 49, KeyM: 50, Comma: 51, Period: 52, Slash: 53, ShiftRight: 54, AltLeft: 56, Space: 57, CapsLock: 58,
	F1: 59, F2: 60, F3: 61, F4: 62, F5: 63, F6: 64, F7: 65, F8: 66, F9: 67, F10: 68, ArrowUp: 72, ArrowLeft: 75, ArrowRight: 77, ArrowDown: 80,
	PageUp: 73, PageDown: 81, Home: 71, End: 79, Insert: 82, Delete: 83 };

function charToAnsi(ch) {
	var c = ch.charCodeAt(0);
	if (c < 0x80 || (c >= 0xa0 && c < 0x100)) return c;
	var hi = [0x20AC,0x0081,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,0x02C6,0x2030,0x0160,0x2039,0x0152,0x008D,0x017D,0x008F,
		0x0090,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x02DC,0x2122,0x0161,0x203A,0x0153,0x009D,0x017E,0x0178];
	var i = hi.indexOf(c);
	return i >= 0 ? 0x80 + i : 0;
}

var PeterWin = {
	els: {}, desk: null, root: null, sw: 1024, sh: 768, scale: 1, offX: 0, offY: 0, fx: 0, fy: 0, lastWin: 0,
	cursorCss: 'default', modals: {}, nextModal: 1, warpCount: 0, locked: false, vx: 0, vy: 0, buttons: 0,
	textCanvas: null, textCtx: null,

	init: function (root) {
		this.root = root;
		var d = document.createElement('div');
		d.className = 'pdesk';
		root.appendChild(d);
		this.desk = d;
		var self = this;
		window.addEventListener('resize', function () { self.fit(); });
		this.installInput();
		this.fit();
	},
	screen: function (w, h) { this.sw = w; this.sh = h; this.desk.style.width = w + 'px'; this.desk.style.height = h + 'px'; this.fit(); },
	// scale & translate the desktop so the visible top-level windows fill the container
	fit: function () {
		if (!this.root) return;
		var W = this.root.clientWidth, H = this.root.clientHeight;
		var l = Infinity, t = Infinity, r = -Infinity, b = -Infinity;
		for (var id in this.els) {
			var e = this.els[id];
			if (e.parent || e.div.style.display === 'none') continue;
			l = Math.min(l, e.x); t = Math.min(t, e.y); r = Math.max(r, e.x + e.w); b = Math.max(b, e.y + e.h);
		}
		if (!isFinite(l)) { l = 0; t = 0; r = this.sw; b = this.sh; }
		l = Math.max(0, l); t = Math.max(0, t); r = Math.min(this.sw, r); b = Math.min(this.sh, b);
		var bw = Math.max(1, r - l), bh = Math.max(1, b - t);
		var s = Math.min(W / bw, H / bh);
		if (s > 1) s = Math.floor(s * 4) / 4 || s;	// prefer clean upscales
		this.scale = s;
		this.offX = (W - bw * s) / 2 - l * s; this.offY = (H - bh * s) / 2 - t * s;
		this.desk.style.transform = 'translate(' + this.offX + 'px,' + this.offY + 'px) scale(' + s + ')';
	},
	fitSoon: function () { var self = this; if (this._fitT) return; this._fitT = requestAnimationFrame(function () { self._fitT = 0; self.fit(); }); },

	create: function (id, parent, kind, flags, text) {
		var div = document.createElement('div');
		div.className = 'pw k' + kind + ((flags & 1) ? ' cap' : '') + ((flags & 4) ? ' cedge' : '') + ((flags & 8) ? ' bord' : '') + ((flags & 16) ? ' thick' : '');
		div.dataset.wid = id;
		div.style.display = 'none';
		var e = { id: id, div: div, parent: parent, kind: kind, flags: flags, x: 0, y: 0, w: 0, h: 0, client: null, ctl: null, canvas: null, gl: null, text: text };
		var cl = document.createElement('div'); cl.className = 'pcl'; e.client = cl;
		if (flags & 1) {
			var cap = document.createElement('div'); cap.className = 'pcap';
			var tt = document.createElement('span'); tt.className = 'ptitle'; tt.textContent = text; cap.appendChild(tt); e.title = tt;
			if (flags & 2) {
				var x = document.createElement('button'); x.className = 'pclose'; x.textContent = '✕'; x.tabIndex = -1;
				x.addEventListener('mousedown', function (ev) { ev.stopPropagation(); });
				x.addEventListener('click', function (ev) { ev.stopPropagation(); Module._shim_on_nc(id, 1); });
				cap.appendChild(x);
			}
			this.dragCaption(cap, e);
			div.appendChild(cap); e.cap = cap;
		}
		div.appendChild(cl);
		this.buildControl(e, text);
		var p = parent ? this.els[parent] : null;
		(p ? p.client : this.desk).appendChild(div);
		this.els[id] = e;
	},
	buildControl: function (e, text) {
		var id = e.id, self = this, c = null, k = e.kind, f = e.flags;
		var notify = function (what, val, txt) { Module.ccall('shim_on_ctrl', null, ['number', 'number', 'number', 'string'], [id, what, val || 0, txt || '']); };
		var focus = function () { notify(6); };
		switch (k) {
		case 1: c = document.createElement('button'); c.className = 'pbtn' + ((f & 32) ? ' def' : ''); c.textContent = text.replace(/&(.)/g, '$1');
			c.addEventListener('click', function () { notify(1); }); break;
		case 2: case 3:
			c = document.createElement('label'); c.className = 'pchk';
			var inp = document.createElement('input'); inp.type = k === 2 ? 'checkbox' : 'radio';
			inp.addEventListener('click', function (ev) { ev.preventDefault(); notify(1); });
			var sp = document.createElement('span'); sp.textContent = text.replace(/&(.)/g, '$1');
			c.appendChild(inp); c.appendChild(sp); e.inp = inp; e.lbl = sp; break;
		case 4: c = document.createElement('fieldset'); c.className = 'pgrp'; var lg = document.createElement('legend'); lg.textContent = text.replace(/&(.)/g, '$1'); c.appendChild(lg); e.lbl = lg; break;
		case 5: c = document.createElement('input'); c.type = (f & 1024) ? 'password' : 'text'; c.className = 'pedit'; c.value = text; c.spellcheck = false;
			c.addEventListener('input', function () { notify(2, 0, c.value); }); break;
		case 6: c = document.createElement('textarea'); c.className = 'pedit pmemo'; c.value = text; c.spellcheck = false;
			c.addEventListener('input', function () { notify(2, 0, c.value.replace(/\n/g, '\r\n')); }); break;
		case 7: c = document.createElement('select'); c.size = 2; c.className = 'plist';
			c.addEventListener('change', function () { notify(3, c.selectedIndex); });
			c.addEventListener('dblclick', function () { notify(4); }); break;
		case 8: c = document.createElement('select'); c.className = 'pcombo';
			c.addEventListener('change', function () { notify(3, c.selectedIndex); }); break;
		case 9: c = document.createElement('input'); c.className = 'pedit'; c.value = text;
			var dl = document.createElement('datalist'); dl.id = 'pdl' + id; c.setAttribute('list', dl.id); e.dl = dl;
			c.addEventListener('input', function () { notify(2, 0, c.value); }); break;
		case 10: if (!(f & 256)) { c = document.createElement('div'); c.className = 'pstatic' + ((f & 64) ? ' center' : (f & 128) ? ' right' : ''); c.textContent = text; } break;
		case 11: case 12: c = document.createElement('input'); c.type = 'range'; c.className = 'prange' + ((f & 512) ? ' vert' : '');
			c.addEventListener('input', function () { notify(5, parseInt(c.value, 10)); }); break;
		case 13: c = document.createElement('progress'); c.className = 'pprog'; c.max = 100; c.value = 0; break;
		case 14: c = document.createElement('div'); c.className = 'pstatus'; c.textContent = text; break;
		case 15: c = document.createElement('div'); c.className = 'ptabs'; break;
		case 16: c = document.createElement('div'); c.className = 'plistview'; break;
		}
		if (c) {
			c.dataset.ctl = '1';
			c.addEventListener('focus', focus, true);
			e.client.appendChild(c); e.ctl = c;
			if (e.dl) e.client.appendChild(e.dl);
		}
	},
	dragCaption: function (cap, e) {
		var self = this;
		cap.addEventListener('mousedown', function (ev) {
			if (ev.button !== 0) return;
			ev.preventDefault();
			Module._shim_on_nc(e.id, 2);
			var sx = ev.clientX, sy = ev.clientY, ox = e.x, oy = e.y;
			function mv(m) { Module._shim_on_move(e.id, Math.round(ox + (m.clientX - sx) / self.scale), Math.round(oy + (m.clientY - sy) / self.scale)); }
			function up() { window.removeEventListener('mousemove', mv); window.removeEventListener('mouseup', up); }
			window.addEventListener('mousemove', mv); window.addEventListener('mouseup', up);
		});
	},
	destroy: function (id) { var e = this.els[id]; if (!e) return; if (e.div.parentNode) e.div.parentNode.removeChild(e.div); delete this.els[id]; this.fitSoon(); },
	rect: function (id, x, y, w, h, cx, cy, cw, ch) {
		var e = this.els[id]; if (!e) return;
		e.x = x; e.y = y; e.w = w; e.h = h;
		var s = e.div.style; s.left = x + 'px'; s.top = y + 'px'; s.width = w + 'px'; s.height = h + 'px';
		var c = e.client.style; c.left = cx + 'px'; c.top = cy + 'px'; c.width = cw + 'px'; c.height = ch + 'px';
		if (e.cap) { e.cap.style.left = cx + 'px'; e.cap.style.width = cw + 'px'; e.cap.style.top = Math.max(0, cy - 19) + 'px'; }
		if (e.canvas && (e.canvas.width !== cw || e.canvas.height !== ch)) { e.canvas.width = Math.max(1, cw); e.canvas.height = Math.max(1, ch); e.ctx = null; }
		if (!e.parent) this.fitSoon();
	},
	frame: function (id, flags) {
		var e = this.els[id]; if (!e) return;
		e.flags = (e.flags & ~31) | flags;
		e.div.classList.toggle('cap', !!(flags & 1)); e.div.classList.toggle('bord', !!(flags & 8)); e.div.classList.toggle('thick', !!(flags & 16));
		if (e.cap) e.cap.style.display = (flags & 1) ? '' : 'none';
	},
	show: function (id, v) { var e = this.els[id]; if (!e) return; e.div.style.display = v ? '' : 'none'; if (!e.parent) this.fitSoon(); },
	enable: function (id, en) {
		var e = this.els[id]; if (!e) return;
		e.div.classList.toggle('disabled', !en);
		if (e.ctl) { if ('disabled' in e.ctl) e.ctl.disabled = !en; if (e.inp) e.inp.disabled = !en; }
	},
	text: function (id, t) {
		var e = this.els[id]; if (!e) return;
		e.text = t;
		if (e.title) e.title.textContent = t;
		if (e.parent === 0 && e.title === undefined && e.id === this.mainId) document.title = t;
		if (!e.ctl) return;
		switch (e.kind) {
		case 1: e.ctl.textContent = t.replace(/&(.)/g, '$1'); break;
		case 2: case 3: case 4: e.lbl.textContent = t.replace(/&(.)/g, '$1'); break;
		case 5: case 9: if (e.ctl.value !== t) e.ctl.value = t; break;
		case 6: var v = t.replace(/\r\n/g, '\n'); if (e.ctl.value !== v) e.ctl.value = v; break;
		case 10: case 14: e.ctl.textContent = t; break;
		}
	},
	check: function (id, st) { var e = this.els[id]; if (!e) return; if (e.inp) { e.inp.checked = st === 1; e.inp.indeterminate = st === 2; } else if (e.ctl && e.kind === 1) e.ctl.classList.toggle('pressed', st === 1); },
	items: function (id, arr, sel) {
		var e = this.els[id]; if (!e || !e.ctl) return;
		if (e.kind === 7 || e.kind === 8) {
			e.ctl.innerHTML = '';
			arr.forEach(function (t) { var o = document.createElement('option'); o.textContent = t; e.ctl.appendChild(o); });
			e.ctl.selectedIndex = sel;
		} else if (e.kind === 9) {
			e.dl.innerHTML = ''; arr.forEach(function (t) { var o = document.createElement('option'); o.value = t; e.dl.appendChild(o); });
		} else if (e.kind === 15) {
			var self = this; e.ctl.innerHTML = '';
			arr.forEach(function (t, i) {
				var b = document.createElement('button'); b.className = 'ptab' + (i === sel ? ' on' : ''); b.textContent = t;
				b.addEventListener('click', function () { Module.ccall('shim_on_ctrl', null, ['number', 'number', 'number', 'string'], [id, 3, i, '']); self.sel(id, i); });
				e.ctl.appendChild(b);
			});
		}
	},
	sel: function (id, s) {
		var e = this.els[id]; if (!e || !e.ctl) return;
		if (e.kind === 7 || e.kind === 8) e.ctl.selectedIndex = s;
		else if (e.kind === 15) Array.prototype.forEach.call(e.ctl.children, function (b, i) { b.classList.toggle('on', i === s); });
	},
	range: function (id, mn, mx, pos) {
		var e = this.els[id]; if (!e || !e.ctl) return;
		if (e.kind === 13) { e.ctl.max = Math.max(1, mx - mn); e.ctl.value = pos - mn; return; }
		e.ctl.min = mn; e.ctl.max = Math.max(mn, mx); e.ctl.value = pos;
	},
	focus: function (id) { var e = this.els[id]; if (e && e.ctl && e.ctl.focus) { try { e.ctl.focus({ preventScroll: true }); } catch (x) {} } else if (document.activeElement && document.activeElement.blur && document.activeElement !== document.body) document.activeElement.blur(); },
	front: function (id) { var e = this.els[id]; if (!e || e.parent) return; this.desk.appendChild(e.div); },
	font: function (id, css) { var e = this.els[id]; if (!e) return; var f = css.split('|'); e.div.style.font = f[f.length - 1]; },
	readonly: function (id, ro) { var e = this.els[id]; if (e && e.ctl && 'readOnly' in e.ctl) e.ctl.readOnly = !!ro; },
	cursor: function (css) {
		this.cursorCss = css; this.desk.style.cursor = css;
		// program shows the pointer again (menus): leave mouse-look so absolute clicks work
		if (css !== 'none' && this.locked && document.exitPointerLock) document.exitPointerLock();
	},

	// ---- GDI canvas
	blit: function (id, ptr, w, h, l, t, r, b) {
		var e = this.els[id]; if (!e || w <= 0 || h <= 0) return;
		if (!e.canvas) {
			var c = document.createElement('canvas'); c.className = 'pgdi'; c.width = w; c.height = h;
			e.client.insertBefore(c, e.client.firstChild); e.canvas = c;
		}
		if (e.canvas.width !== w || e.canvas.height !== h) { e.canvas.width = w; e.canvas.height = h; e.ctx = null; l = 0; t = 0; r = w; b = h; }
		if (!e.ctx) e.ctx = e.canvas.getContext('2d');
		var rw = r - l, rh = b - t;
		if (rw <= 0 || rh <= 0) return;
		var img = e.ctx.createImageData(rw, rh), dst = img.data;
		var src = HEAPU8;
		for (var y = 0; y < rh; y++) {
			var so = ptr + ((t + y) * w + l) * 4;
			dst.set(src.subarray(so, so + rw * 4), y * rw * 4);
		}
		e.ctx.putImageData(img, l, t);
	},
	glCanvas: function (id, on) {
		var e = this.els[id]; if (!e) return;
		if (on) {
			if (!e.gl) { var c = document.createElement('canvas'); c.className = 'pgl'; c.id = 'peter-gl-' + id; e.client.appendChild(c); e.gl = c; }
		} else if (e.gl) { e.gl.parentNode.removeChild(e.gl); e.gl = null; }
	},
	glSize: function (id, w, h) { var e = this.els[id]; if (e && e.gl) { e.gl.width = Math.max(1, w); e.gl.height = Math.max(1, h); } },

	// ---- text rendering: returns malloc'd alpha buffer, out = [w,h,ascent,ox,oy,advance]
	renderText: function (fontSpec, text, sx, angle, measureOnly, out) {
		if (!this.textCanvas) { this.textCanvas = document.createElement('canvas'); this.textCtx = this.textCanvas.getContext('2d', { willReadFrequently: true }); }
		var parts = fontSpec.split('|'), flags = parts.length > 1 ? parts[0] : '', css = parts[parts.length - 1];
		var ctx = this.textCtx; ctx.font = css;
		var o32 = out >> 2;
		if (text === '\x01avg') { HEAP32[o32] = Math.round(ctx.measureText('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ').width / 52 * 100); return 0; }
		var m = ctx.measureText(text);
		var asc = Math.ceil(m.fontBoundingBoxAscent || parseFloat(css) * 0.9), desc = Math.ceil(m.fontBoundingBoxDescent || parseFloat(css) * 0.25);
		var lineH = asc + desc, adv = Math.round(m.width * sx);
		if (measureOnly) { HEAP32[o32] = adv; HEAP32[o32 + 1] = lineH; HEAP32[o32 + 2] = asc; HEAP32[o32 + 3] = 0; HEAP32[o32 + 4] = 0; HEAP32[o32 + 5] = adv; return 0; }
		var a = -angle * Math.PI / 180, ca = Math.cos(a), sa = Math.sin(a);
		var pts = [[0, 0], [adv + 2, 0], [0, lineH], [adv + 2, lineH]];
		var minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
		pts.forEach(function (p) { var x = p[0] * ca - p[1] * sa, y = p[0] * sa + p[1] * ca; minx = Math.min(minx, x); miny = Math.min(miny, y); maxx = Math.max(maxx, x); maxy = Math.max(maxy, y); });
		var ox = Math.floor(minx), oy = Math.floor(miny), W = Math.max(1, Math.ceil(maxx) - ox + 1), H = Math.max(1, Math.ceil(maxy) - oy + 1);
		var cv = this.textCanvas;
		if (cv.width < W || cv.height < H) { cv.width = Math.max(cv.width, W); cv.height = Math.max(cv.height, H); }
		ctx.setTransform(1, 0, 0, 1, 0, 0);
		ctx.clearRect(0, 0, W, H);
		ctx.font = css; ctx.fillStyle = '#fff'; ctx.textBaseline = 'alphabetic';
		ctx.setTransform(ca * sx, sa * sx, -sa, ca, -ox, -oy);
		ctx.fillText(text, 0, asc);
		if (flags.indexOf('u') >= 0) ctx.fillRect(0, asc + Math.max(1, desc / 3), m.width, Math.max(1, lineH / 16));
		if (flags.indexOf('s') >= 0) ctx.fillRect(0, asc - asc / 3, m.width, Math.max(1, lineH / 16));
		ctx.setTransform(1, 0, 0, 1, 0, 0);
		var d = ctx.getImageData(0, 0, W, H).data;
		var p = Module._malloc(W * H);
		for (var i = 0, n = W * H; i < n; i++) HEAPU8[p + i] = d[i * 4 + 3];
		HEAP32[o32] = W; HEAP32[o32 + 1] = H; HEAP32[o32 + 2] = asc; HEAP32[o32 + 3] = ox; HEAP32[o32 + 4] = oy; HEAP32[o32 + 5] = adv;
		return p;
	},

	// ---- input
	// Relative mouse mode: the program hid the cursor and has warped it (mouse-look). Browsers
	// cannot warp the real pointer, so the virtual cursor moves by movementX/Y from the warp point.
	relative: function () { return this.locked || (this.cursorCss === 'none' && this.warpCount > 0); },
	tryLock: function (id) {
		if (id > 0 && this.cursorCss === 'none' && this.warpCount > 0 && !this.locked && this.root.requestPointerLock) {
			this.lockWin = id;
			try { var r = this.root.requestPointerLock(); if (r && r.catch) r.catch(function () {}); } catch (x) {}
		}
	},
	deskPos: function (ev) {
		if (this.relative()) return [this.vx, this.vy];
		var r = this.root.getBoundingClientRect();
		return [Math.round((ev.clientX - r.left - this.offX) / this.scale), Math.round((ev.clientY - r.top - this.offY) / this.scale)];
	},
	targetWin: function (ev) {
		var t = ev.target;
		if (t && t.closest) {
			if (t.closest('[data-ctl]') && !t.closest('.pgl') && !t.closest('.pgdi')) return -1;
			var w = t.closest('[data-wid]');
			if (w) return parseInt(w.dataset.wid, 10);
		}
		return 0;
	},
	installInput: function () {
		var self = this, root = this.root;
		var send = function (ev, msg) {
			var p = self.deskPos(ev); self.vx = p[0]; self.vy = p[1];
			if (!self.relative()) { self.fx = p[0]; self.fy = p[1]; }
			var id = self.locked ? (self.lockWin || 0) : self.targetWin(ev);
			if (id > 0) self.lastWin = id;
			if (id <= 0) { Module._shim_on_mouse(0, 0, p[0], p[1], self.buttons); return; }
			Module._shim_on_mouse(id, msg, p[0], p[1], self.buttons);
		};
		root.addEventListener('mousedown', function (ev) {
			PeterAudio.unlock();
			self.buttons |= (ev.button === 0 ? 1 : ev.button === 2 ? 2 : 4);
			var id = self.targetWin(ev);
			if (id > 0) ev.preventDefault();
			self.tryLock(id);
			var dbl = ev.detail === 2;
			var msg = ev.button === 0 ? (dbl ? 0x203 : 0x201) : ev.button === 2 ? (dbl ? 0x206 : 0x204) : (dbl ? 0x209 : 0x207);
			send(ev, msg);
			if (id > 0 && document.activeElement && document.activeElement !== document.body && !ev.target.closest('[data-ctl]')) document.activeElement.blur();
		});
		window.addEventListener('mouseup', function (ev) {
			self.buttons &= ~(ev.button === 0 ? 1 : ev.button === 2 ? 2 : 4);
			send(ev, ev.button === 0 ? 0x202 : ev.button === 2 ? 0x205 : 0x208);
		});
		window.addEventListener('mousemove', function (ev) {
			if (self.relative()) {
				// keep the fractional part so slow movements are not lost to rounding
				self.fx += (ev.movementX || 0) / self.scale; self.fy += (ev.movementY || 0) / self.scale;
				self.vx = Math.round(self.fx); self.vy = Math.round(self.fy);
			}
			send(ev, 0x200);
		});
		root.addEventListener('contextmenu', function (ev) { ev.preventDefault(); });
		root.addEventListener('wheel', function (ev) { if (self.targetWin(ev) > 0) ev.preventDefault(); }, { passive: false });
		document.addEventListener('pointerlockchange', function () { self.locked = document.pointerLockElement === root; });
		var key = function (ev, down) {
			if (self.modalOpen) return;
			var vk = VK[ev.code] || (ev.keyCode > 0 && ev.keyCode < 256 ? ev.keyCode : 0);
			if (!vk) return;
			var t = ev.target, inText = t && (t.tagName === 'INPUT' && t.type !== 'checkbox' && t.type !== 'radio' && t.type !== 'range' || t.tagName === 'TEXTAREA' || t.tagName === 'SELECT');
			var ch = 0;
			if (down) {
				if (ev.key.length === 1) ch = charToAnsi(ev.key);
				else if (vk === 13) ch = 13; else if (vk === 8) ch = 8; else if (vk === 9) ch = 9; else if (vk === 27) ch = 27;
				if (ev.ctrlKey && ev.key.length === 1) { var u = ev.key.toUpperCase().charCodeAt(0); if (u >= 64 && u < 96) ch = u - 64; }
			}
			var targetId = 0;
			if (inText) { var w = t.closest('[data-wid]'); if (w) targetId = parseInt(w.dataset.wid, 10); }
			Module._shim_on_key(vk, down ? 1 : 0, ev.repeat ? 1 : 0, SCAN[ev.code] || 0, inText ? 0 : ch, targetId);
			if (!inText && !(ev.ctrlKey && (ev.key === 'r' || ev.key === 'R')) && ev.key !== 'F5' && ev.key !== 'F11' && ev.key !== 'F12') ev.preventDefault();
		};
		window.addEventListener('keydown', function (ev) { PeterAudio.unlock(); if (ev.key !== 'Escape') self.tryLock(self.lastWin || 0); key(ev, true); });
		window.addEventListener('keyup', function (ev) { key(ev, false); });
		window.addEventListener('blur', function () { if (Module._shim_on_blur) Module._shim_on_blur(); });
	},
	warp: function (x, y) { this.warpCount++; this.vx = this.fx = x; this.vy = this.fy = y; },
	pointerLock: function (on) {},

	// ---- modal dialogs (message boxes, file dialogs)
	modalBox: function (title) {
		var ov = document.createElement('div'); ov.className = 'pmodal';
		var box = document.createElement('div'); box.className = 'pmbox';
		var cap = document.createElement('div'); cap.className = 'pcap static'; cap.textContent = title;
		var body = document.createElement('div'); body.className = 'pmbody';
		var btns = document.createElement('div'); btns.className = 'pmbtns';
		box.appendChild(cap); box.appendChild(body); box.appendChild(btns); ov.appendChild(box);
		this.root.appendChild(ov);
		this.modalOpen = true;
		return { ov: ov, body: body, btns: btns };
	},
	finishModal: function (h, result, text) {
		var m = this.modals[h]; if (!m || m.result) return;
		m.result = result; m.text = text;
		if (m.ui && m.ui.ov.parentNode) m.ui.ov.parentNode.removeChild(m.ui.ov);
		this.modalOpen = false;
	},
	msgBox: function (title, text, type) {
		var h = this.nextModal++, self = this;
		var ui = this.modalBox(title || 'Message');
		var icon = (type >> 4) & 7;
		var p = document.createElement('div'); p.className = 'pmtext' + (icon === 1 ? ' err' : icon === 2 ? ' ask' : icon === 3 ? ' warn' : icon === 4 ? ' info' : '');
		p.textContent = text; ui.body.appendChild(p);
		var sets = { 0: [['OK', 1]], 1: [['OK', 1], ['Cancel', 2]], 2: [['Abort', 3], ['Retry', 4], ['Ignore', 5]], 3: [['Yes', 6], ['No', 7], ['Cancel', 2]],
			4: [['Yes', 6], ['No', 7]], 5: [['Retry', 4], ['Cancel', 2]] };
		var list = sets[type & 15] || sets[0];
		var def = (type >> 8) & 3;
		list.forEach(function (b, i) {
			var e = document.createElement('button'); e.className = 'pbtn'; e.textContent = b[0];
			e.addEventListener('click', function () { self.finishModal(h, b[1]); });
			ui.btns.appendChild(e);
			if (i === def) setTimeout(function () { e.focus(); }, 0);
		});
		ui.ov.addEventListener('keydown', function (ev) { if (ev.key === 'Escape') { var c = list.filter(function (b) { return b[1] === 2 || b[1] === 7; }); self.finishModal(h, c.length ? c[0][1] : list[0][1]); } });
		this.modals[h] = { ui: ui, result: 0 };
		return h;
	},
	pollModal: function (h) { var m = this.modals[h]; return m ? m.result : 2; },
	modalText: function (h) { var m = this.modals[h]; return m ? m.text : null; },
	fileDialog: function (save, title, dir, files, deflt) {
		var h = this.nextModal++, self = this;
		var ui = this.modalBox(title);
		var where = document.createElement('div'); where.className = 'pmdir'; where.textContent = dir; ui.body.appendChild(where);
		var list = document.createElement('select'); list.size = 8; list.className = 'plist pmlist';
		(files ? files.split('\n') : []).forEach(function (f) { var o = document.createElement('option'); o.textContent = f; list.appendChild(o); });
		ui.body.appendChild(list);
		var inp = document.createElement('input'); inp.className = 'pedit pmname'; inp.value = deflt; inp.placeholder = 'File name';
		ui.body.appendChild(inp);
		list.addEventListener('change', function () { inp.value = list.value; });
		list.addEventListener('dblclick', function () { if (list.value) self.finishModal(h, 1, list.value); });
		var ok = document.createElement('button'); ok.className = 'pbtn def'; ok.textContent = save ? 'Save' : 'Open';
		ok.addEventListener('click', function () { if (inp.value) self.finishModal(h, 1, inp.value); });
		var cancel = document.createElement('button'); cancel.className = 'pbtn'; cancel.textContent = 'Cancel';
		cancel.addEventListener('click', function () { self.finishModal(h, 2); });
		if (!save) {
			var up = document.createElement('button'); up.className = 'pbtn'; up.textContent = 'Upload…';
			up.addEventListener('click', function () {
				var fi = document.createElement('input'); fi.type = 'file';
				fi.onchange = function () {
					var f = fi.files[0]; if (!f) return;
					f.arrayBuffer().then(function (buf) {
						var udir = '/' + dir[0].toLowerCase() + dir.slice(2).replace(/\\/g, '/').toLowerCase();
						try { FS.writeFile(udir + '/' + f.name.toLowerCase(), new Uint8Array(buf)); } catch (x) { console.error(x); }
						if (Module.persistFS) Module.persistFS();
						self.finishModal(h, 1, f.name.toLowerCase());
					});
				};
				fi.click();
			});
			ui.btns.appendChild(up);
		}
		ui.btns.appendChild(ok); ui.btns.appendChild(cancel);
		inp.addEventListener('keydown', function (ev) { if (ev.key === 'Enter') ok.click(); if (ev.key === 'Escape') cancel.click(); });
		setTimeout(function () { inp.focus(); }, 0);
		this.modals[h] = { ui: ui, result: 0 };
		return h;
	},
	fatal: function (msg) {
		var d = document.createElement('div'); d.className = 'pfatal'; d.textContent = msg; this.root.appendChild(d);
	}
};

// ---------------------------------------------------------------- audio
var PeterAudio = {
	ctx: null, streams: {}, master: null,
	ensure: function () {
		if (this.ctx) return this.ctx;
		var AC = window.AudioContext || window.webkitAudioContext;
		if (!AC) return null;
		this.ctx = new AC();
		this.master = this.ctx.createGain(); this.master.gain.value = 0.8; this.master.connect(this.ctx.destination);
		return this.ctx;
	},
	unlock: function () { var c = this.ensure(); if (c && c.state !== 'running') c.resume(); },
	write: function (stream, ptr, bytes, channels, rate, bits, gain) {
		this.writes = (this.writes || 0) + 1; this.last = [stream, bytes, channels, rate, bits];
		var now = performance.now();
		var frames = Math.floor(bytes / (channels * bits / 8));
		var dur = frames / rate * 1000;
		var s = this.streams[stream] || (this.streams[stream] = { endWall: 0, endCtx: 0, srcs: [] });
		var c = this.ensure();
		if (!c || c.state !== 'running' || frames <= 0) {
			// no audible output yet (autoplay policy): keep the timing so the program's buffers complete
			s.endWall = Math.max(s.endWall, now) + dur;
			return s.endWall;
		}
		var buf = c.createBuffer(channels, frames, rate);
		for (var ch = 0; ch < channels; ch++) {
			var d = buf.getChannelData(ch);
			if (bits === 8) { for (var i = 0; i < frames; i++) d[i] = (HEAPU8[ptr + i * channels + ch] - 128) / 128; }
			else { var b16 = ptr >> 1; for (var j = 0; j < frames; j++) d[j] = HEAP16[b16 + j * channels + ch] / 32768; }
		}
		var src = c.createBufferSource(); src.buffer = buf;
		var g = c.createGain(); g.gain.value = gain; src.connect(g); g.connect(this.master);
		var t = Math.max(c.currentTime + 0.02, s.endCtx);
		src.start(t);
		s.endCtx = t + frames / rate;
		s.srcs.push(src);
		src.onended = function () { var k = s.srcs.indexOf(src); if (k >= 0) s.srcs.splice(k, 1); };
		s.endWall = now + (s.endCtx - c.currentTime) * 1000;
		return s.endWall;
	},
	reset: function (stream) {
		var s = this.streams[stream]; if (!s) return;
		s.srcs.forEach(function (x) { try { x.stop(); } catch (e) {} });
		s.srcs = []; s.endCtx = 0; s.endWall = 0;
	}
};
PeterWin.colors = function (id, bg, fg) {
	var e = this.els[id]; if (!e) return;
	var hex = function (c) { return 'rgb(' + (c & 255) + ',' + ((c >> 8) & 255) + ',' + ((c >> 16) & 255) + ')'; };
	var el = e.ctl || e.client;
	if (e.kind === 1) { el.style.color = hex(fg); return; }	// push buttons keep their face colour
	if (e.kind === 4) { el.style.color = hex(fg); if (e.lbl && bg >= 0) e.lbl.style.background = hex(bg); return; }
	if (bg >= 0) { el.style.background = hex(bg); e.client.style.background = hex(bg); }
	el.style.color = hex(fg);
};
