/* Kleine Diagrammbibliothek auf Canvas, ohne externe Abhängigkeiten (Messnetz ohne Internet).
 * Histogramme, Zeitverläufe und XY-Darstellungen mit Grenzlinien und Tooltip. */
'use strict';

const Charts = (() => {
  const PAD = { l: 52, r: 12, t: 22, b: 26 };

  function css(name) {
    return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  }

  function setup(canvas) {
    const dpr = window.devicePixelRatio || 1;
    const w = canvas.clientWidth || 300;
    const h = canvas.clientHeight || 200;
    if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
      canvas.width = Math.round(w * dpr);
      canvas.height = Math.round(h * dpr);
    }
    const ctx = canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    ctx.font = '11px ' + css('--sans');
    ctx.textBaseline = 'middle';
    return { ctx, w, h, pw: w - PAD.l - PAD.r, ph: h - PAD.t - PAD.b };
  }

  function fmt(v, digits) {
    if (v === null || v === undefined || Number.isNaN(v)) return '–';
    const a = Math.abs(v);
    if (digits === undefined) digits = a >= 100 ? 0 : a >= 10 ? 1 : a >= 1 ? 2 : 3;
    return v.toLocaleString('de-DE', { minimumFractionDigits: 0, maximumFractionDigits: digits });
  }

  function niceStep(span, count) {
    const raw = span / Math.max(1, count);
    const mag = Math.pow(10, Math.floor(Math.log10(raw || 1)));
    const n = raw / mag;
    return (n < 1.5 ? 1 : n < 3 ? 2 : n < 7 ? 5 : 10) * mag;
  }

  function ticks(min, max, count) {
    if (!(max > min)) { max = min + 1; }
    const step = niceStep(max - min, count);
    const out = [];
    for (let v = Math.ceil(min / step) * step; v <= max + step * 1e-9; v += step) out.push(+v.toPrecision(12));
    return out;
  }

  function noData(canvas, text) {
    const { ctx, w, h } = setup(canvas);
    ctx.fillStyle = css('--muted');
    ctx.textAlign = 'center';
    ctx.fillText(text || 'keine Daten', w / 2, h / 2);
    canvas._hover = null;
  }

  function frame(ctx, g, xs, ys, xLabel, yLabel) {
    ctx.strokeStyle = css('--grid');
    ctx.lineWidth = 1;
    ctx.fillStyle = css('--muted');
    ctx.textAlign = 'right';
    for (const t of ys.ticks) {
      const y = ys.map(t);
      ctx.beginPath(); ctx.moveTo(PAD.l, y + .5); ctx.lineTo(PAD.l + g.pw, y + .5); ctx.stroke();
      ctx.fillText(ys.label(t), PAD.l - 6, y);
    }
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    for (const t of xs.ticks) {
      const x = xs.map(t);
      ctx.beginPath(); ctx.moveTo(x + .5, PAD.t); ctx.lineTo(x + .5, PAD.t + g.ph); ctx.stroke();
      ctx.fillText(xs.label(t), x, PAD.t + g.ph + 5);
    }
    ctx.textBaseline = 'middle';
    ctx.strokeStyle = css('--line');
    ctx.strokeRect(PAD.l + .5, PAD.t + .5, g.pw, g.ph);
    if (yLabel) { ctx.textAlign = 'left'; ctx.fillText(yLabel, 4, PAD.t - 10 < 9 ? 9 : PAD.t - 10); }
    if (xLabel) { ctx.textAlign = 'right'; ctx.fillText(xLabel, PAD.l + g.pw, 9); }
  }

  function linearScale(min, max, from, to, count, label) {
    const tk = ticks(min, max, count);
    const step = tk.length > 1 ? tk[1] - tk[0] : 1;
    const digits = Math.max(0, Math.min(6, -Math.floor(Math.log10(step) + 1e-9)));
    const auto = v => fmt(Math.abs(v) < step * 1e-6 ? 0 : v, digits);
    return {
      min, max, ticks: tk,
      map: v => from + (v - min) / ((max - min) || 1) * (to - from),
      inv: p => min + (p - from) / ((to - from) || 1) * (max - min),
      label: label || auto,
    };
  }

  function logScale(maxCount, from, to) {
    const top = Math.max(1, Math.ceil(Math.log10(maxCount + 1)));
    const tk = [];
    for (let e = 0; e <= top; e++) tk.push(Math.pow(10, e));
    return {
      ticks: tk,
      map: v => from + (Math.log10(Math.max(v, 1) ) / top) * (to - from),
      label: v => v >= 1e6 ? (v / 1e6) + ' Mio' : v >= 1e3 ? (v / 1e3) + 'k' : String(v),
    };
  }

  function vLine(ctx, g, x, color, label, dash, bottom) {
    if (x < PAD.l - 0.5 || x > PAD.l + g.pw + 0.5) return;
    ctx.save();
    ctx.strokeStyle = color; ctx.setLineDash(dash || [5, 4]); ctx.lineWidth = 1.2;
    ctx.beginPath(); ctx.moveTo(x, PAD.t); ctx.lineTo(x, PAD.t + g.ph); ctx.stroke();
    ctx.setLineDash([]); ctx.fillStyle = color; ctx.textAlign = 'left';
    if (label) ctx.fillText(label, Math.min(x + 4, PAD.l + g.pw - 60), bottom ? PAD.t + g.ph - 9 : PAD.t + 9);
    ctx.restore();
  }

  function hLine(ctx, g, y, color, label, dash) {
    if (y < PAD.t - 0.5 || y > PAD.t + g.ph + 0.5) return;
    ctx.save();
    ctx.strokeStyle = color; ctx.setLineDash(dash || [5, 4]); ctx.lineWidth = 1.2;
    ctx.beginPath(); ctx.moveTo(PAD.l, y); ctx.lineTo(PAD.l + g.pw, y); ctx.stroke();
    ctx.setLineDash([]); ctx.fillStyle = color; ctx.textAlign = 'right';
    if (label) ctx.fillText(label, PAD.l + g.pw - 4, y - 7);
    ctx.restore();
  }

  /* Histogramm: h = {lower, upper, width, bins[], underflow, overflow, count, min, max, p50, p99}.
   * opts: scale (Faktor für x), unit, logY, color, markers [{x, label, color}], minSpan */
  function histogram(canvas, h, opts = {}) {
    if (!h || !h.count) { noData(canvas, opts.empty || 'noch keine Pakete'); return; }
    const g = setup(canvas);
    const { ctx } = g;
    const s = opts.scale || 1;
    const bins = h.bins;
    let first = bins.findIndex(c => c > 0);
    let last = bins.length - 1 - [...bins].reverse().findIndex(c => c > 0);
    if (first < 0) { first = 0; last = bins.length - 1; }
    let xmin = h.lower + first * h.width;
    let xmax = h.lower + (last + 1) * h.width;
    for (const m of opts.markers || []) {
      if (m.x !== null && m.x !== undefined && m.include !== false) { xmin = Math.min(xmin, m.x / s * 1); xmax = Math.max(xmax, m.x / s * 1); }
    }
    // Marker kommen bereits skaliert: Bereich in Rohwerten führen.
    const span = Math.max(xmax - xmin, (opts.minSpan || 0) / s, h.width * 8);
    const pad = span * 0.08;
    xmin = Math.max(h.lower, xmin - pad);
    xmax = Math.min(h.upper, xmin + span + 2 * pad);
    const i0 = Math.max(0, Math.floor((xmin - h.lower) / h.width));
    const i1 = Math.min(bins.length - 1, Math.ceil((xmax - h.lower) / h.width));
    let maxCount = 1;
    for (let i = i0; i <= i1; i++) maxCount = Math.max(maxCount, bins[i]);

    const xs = linearScale(xmin * s, xmax * s, PAD.l, PAD.l + g.pw, Math.max(3, Math.floor(g.pw / 70)));
    const ys = opts.logY ? logScale(maxCount, PAD.t + g.ph, PAD.t)
                         : linearScale(0, maxCount * 1.08, PAD.t + g.ph, PAD.t, 4, v => fmt(v, 0));
    frame(ctx, g, xs, ys, '', '');

    ctx.fillStyle = opts.color || css('--bar');
    const base = PAD.t + g.ph;
    for (let i = i0; i <= i1; i++) {
      const c = bins[i];
      if (!c) continue;
      const xa = xs.map((h.lower + i * h.width) * s);
      const xb = xs.map((h.lower + (i + 1) * h.width) * s);
      const y = ys.map(c);
      ctx.fillRect(Math.max(PAD.l, xa), y, Math.max(1, Math.min(PAD.l + g.pw, xb) - Math.max(PAD.l, xa) - (xb - xa > 3 ? 1 : 0)), base - y);
    }
    for (const m of opts.markers || []) {
      if (m.x !== null && m.x !== undefined) vLine(ctx, g, xs.map(m.x), m.color, m.label, m.dash);
    }
    // Statistik und Über-/Unterlauf
    ctx.fillStyle = css('--muted');
    ctx.textAlign = 'left';
    const u = opts.unit ? ' ' + opts.unit : '';
    const stats = `n ${fmt(h.count, 0)} · p50 ${fmt(h.p50 * s)} · p99 ${fmt(h.p99 * s)} · max ${fmt(h.max * s)}${u}`;
    ctx.fillText(stats, PAD.l + 4, 9);
    if (h.underflow || h.overflow) {
      ctx.textAlign = 'right';
      ctx.fillStyle = css('--wide');
      ctx.fillText(`außerhalb: ${fmt(h.underflow, 0)} ↓  ${fmt(h.overflow, 0)} ↑`, PAD.l + g.pw - 4, PAD.t + g.ph - 10);
    }
    canvas._hover = (mx) => {
      if (mx < PAD.l || mx > PAD.l + g.pw) return null;
      const v = xs.inv(mx) / s;
      const i = Math.floor((v - h.lower) / h.width);
      if (i < 0 || i >= bins.length) return null;
      const a = (h.lower + i * h.width) * s;
      const b = (h.lower + (i + 1) * h.width) * s;
      const share = h.count ? (bins[i] / h.count * 100) : 0;
      return `${fmt(a)} … ${fmt(b)}${u}\n${fmt(bins[i], 0)} (${fmt(share, 3)} %)`;
    };
  }

  /* Zeitverlauf: list = [{data: {t, min, max}, color, label, use: 'max'|'min'|'band'}].
   * opts: window (s), unit, markers [{y, label, color}], yMin, yMax, scale */
  // Zusammenhängende Abschnitte einer Zeitreihe [von, bis). Eine Lücke (z. B. Sender gestoppt und neu gestartet,
  // keine Bilder dazwischen) wird nicht überbrückt: Die Linie bricht ab, statt einen Verlauf vorzutäuschen.
  function segments(t) {
    const n = t.length;
    if (n < 3) return [[0, n]];
    const dts = [];
    for (let i = 1; i < n; i++) dts.push(t[i] - t[i - 1]);
    const sorted = dts.slice().sort((a, b) => a - b);
    const limit = Math.max(sorted[Math.floor(sorted.length / 2)] * 4, 0.5);
    const out = [];
    let a = 0;
    for (let i = 1; i < n; i++) {
      if (dts[i - 1] > limit) { out.push([a, i]); a = i; }
    }
    out.push([a, n]);
    return out;
  }

  function series(canvas, list, opts = {}) {
    const items = list.filter(it => it.data && it.data.t && it.data.t.length);
    if (!items.length) { noData(canvas, opts.empty || 'noch keine Werte'); return; }
    const g = setup(canvas);
    const { ctx } = g;
    const s = opts.scale || 1;
    let tmax = -Infinity;
    let ymin = Infinity, ymax = -Infinity;
    for (const it of items) {
      tmax = Math.max(tmax, it.data.t[it.data.t.length - 1]);
      for (let i = 0; i < it.data.t.length; i++) {
        const lo = (it.use === 'max' ? it.data.max[i] : it.data.min[i]) * s;
        const hi = (it.use === 'min' ? it.data.min[i] : it.data.max[i]) * s;
        ymin = Math.min(ymin, lo); ymax = Math.max(ymax, hi);
      }
    }
    for (const m of opts.markers || []) {
      if (m.y !== null && m.y !== undefined && m.include !== false) { ymin = Math.min(ymin, m.y); ymax = Math.max(ymax, m.y); }
    }
    if (opts.yMin !== undefined) ymin = Math.min(ymin, opts.yMin);
    if (opts.yMax !== undefined) ymax = Math.max(ymax, opts.yMax);
    if (ymax - ymin < 1e-9) { ymax += 1; ymin -= 1; }
    const yp = (ymax - ymin) * 0.08;
    const win = opts.window || 60;
    const xs = linearScale(tmax - win, tmax, PAD.l, PAD.l + g.pw, Math.max(3, Math.floor(g.pw / 80)),
                           v => { const d = Math.round(v - tmax); return (d === 0 ? '0' : fmt(d, 0)) + ' s'; });
    const ys = linearScale(ymin - yp, ymax + yp, PAD.t + g.ph, PAD.t, 4);
    frame(ctx, g, xs, ys, '', opts.unit || '');
    ctx.save();
    ctx.beginPath(); ctx.rect(PAD.l, PAD.t, g.pw, g.ph); ctx.clip();
    for (const it of items) {
      const d = it.data;
      ctx.strokeStyle = it.color; ctx.fillStyle = it.color; ctx.lineWidth = 1.4;
      const parts = segments(d.t);
      if (it.use === 'band') {
        ctx.globalAlpha = 0.25;
        for (const [a, b] of parts) {
          ctx.beginPath();
          for (let i = a; i < b; i++) ctx.lineTo(xs.map(d.t[i]), ys.map(d.max[i] * s));
          for (let i = b - 1; i >= a; i--) ctx.lineTo(xs.map(d.t[i]), ys.map(d.min[i] * s));
          ctx.closePath(); ctx.fill();
        }
        ctx.globalAlpha = 1;
      }
      const field = it.use === 'min' ? d.min : d.max;
      ctx.beginPath();
      for (const [a, b] of parts) {
        for (let i = a; i < b; i++) {
          const x = xs.map(d.t[i]), y = ys.map(field[i] * s);
          if (i === a) ctx.moveTo(x, y); else ctx.lineTo(x, y);
        }
        if (b - a === 1) { const x = xs.map(d.t[a]), y = ys.map(field[a] * s); ctx.moveTo(x, y); ctx.lineTo(x + 1, y); }
      }
      ctx.stroke();
    }
    ctx.restore();
    for (const m of opts.markers || []) {
      if (m.y !== null && m.y !== undefined) hLine(ctx, g, ys.map(m.y), m.color, m.label, m.dash);
    }
    // Legende
    ctx.textAlign = 'left';
    let lx = PAD.l + 6;
    for (const it of items) {
      if (!it.label) continue;
      ctx.fillStyle = it.color; ctx.fillRect(lx, PAD.t + 6, 10, 3);
      ctx.fillStyle = css('--text'); ctx.fillText(it.label, lx + 14, PAD.t + 8);
      lx += ctx.measureText(it.label).width + 30;
    }
    canvas._hover = (mx) => {
      if (mx < PAD.l || mx > PAD.l + g.pw) return null;
      const t = xs.inv(mx);
      const lines = [];
      for (const it of items) {
        const d = it.data;
        let best = 0, bd = Infinity;
        for (let i = 0; i < d.t.length; i++) { const dd = Math.abs(d.t[i] - t); if (dd < bd) { bd = dd; best = i; } }
        const lo = d.min[best] * s, hi = d.max[best] * s;
        const val = it.use === 'min' ? fmt(lo) : it.use === 'max' ? fmt(hi) : (lo === hi ? fmt(hi) : `${fmt(lo)} … ${fmt(hi)}`);
        lines.push(`${it.label || ''} ${val}${opts.unit ? ' ' + opts.unit : ''}`.trim());
      }
      return `${fmt(t - tmax, 1)} s\n` + lines.join('\n');
    };
  }

  /* XY: list = [{x[], y[], color, mode: 'points'|'line'|'step', dash, label}].
   * opts: xLabel, yLabel, markersY [{y,label,color}], markersX [{x,label,color}], yMin, yMax, hoverX */
  function xy(canvas, list, opts = {}) {
    const items = list.filter(it => it.x && it.x.length);
    if (!items.length) { noData(canvas, opts.empty || 'kein Profil'); return; }
    const g = setup(canvas);
    const { ctx } = g;
    let xmin = Infinity, xmax = -Infinity, ymin = Infinity, ymax = -Infinity;
    for (const it of items) {
      for (let i = 0; i < it.x.length; i++) {
        xmin = Math.min(xmin, it.x[i]); xmax = Math.max(xmax, it.x[i]);
        if (it.fit !== false) { ymin = Math.min(ymin, it.y[i]); ymax = Math.max(ymax, it.y[i]); }
      }
    }
    for (const m of opts.markersY || []) if (m.include !== false) { ymin = Math.min(ymin, m.y); ymax = Math.max(ymax, m.y); }
    for (const m of opts.markersX || []) if (m.include !== false) { xmin = Math.min(xmin, m.x); xmax = Math.max(xmax, m.x); }
    if (opts.xMin !== undefined) xmin = Math.min(xmin, opts.xMin);
    if (opts.yMin !== undefined) ymin = Math.min(ymin, opts.yMin);
    if (opts.yMax !== undefined) ymax = Math.max(ymax, opts.yMax);
    if (!(ymax > ymin)) { ymax = ymin + 1; }
    const yp = (ymax - ymin) * 0.08;
    const xs = linearScale(xmin, xmax, PAD.l, PAD.l + g.pw, Math.max(3, Math.floor(g.pw / 80)));
    const ys = linearScale(ymin - yp, ymax + yp, PAD.t + g.ph, PAD.t, 4);
    frame(ctx, g, xs, ys, opts.xLabel || '', opts.yLabel || '');
    ctx.save();
    ctx.beginPath(); ctx.rect(PAD.l, PAD.t, g.pw, g.ph); ctx.clip();
    for (const it of items) {
      ctx.strokeStyle = it.color; ctx.fillStyle = it.color; ctx.lineWidth = 1.3;
      ctx.setLineDash(it.dash || []);
      if (it.mode === 'points') {
        const r = it.x.length > 1500 ? 1 : 1.5;
        for (let i = 0; i < it.x.length; i++) ctx.fillRect(xs.map(it.x[i]) - r, ys.map(it.y[i]) - r, 2 * r, 2 * r);
      } else {
        ctx.beginPath();
        for (let i = 0; i < it.x.length; i++) {
          const x = xs.map(it.x[i]), y = ys.map(it.y[i]);
          if (i === 0) ctx.moveTo(x, y);
          else if (it.mode === 'step') { ctx.lineTo(x, ys.map(it.y[i - 1])); ctx.lineTo(x, y); }
          else ctx.lineTo(x, y);
        }
        ctx.stroke();
      }
      ctx.setLineDash([]);
    }
    ctx.restore();
    for (const m of opts.markersY || []) hLine(ctx, g, ys.map(m.y), m.color, m.label, m.dash);
    for (const m of opts.markersX || []) vLine(ctx, g, xs.map(m.x), m.color, m.label, m.dash, true);
    ctx.textAlign = 'left';
    let lx = PAD.l + 6;
    for (const it of items) {
      if (!it.label) continue;
      ctx.fillStyle = it.color; ctx.fillRect(lx, PAD.t + 6, 10, 3);
      ctx.fillStyle = css('--text'); ctx.fillText(it.label, lx + 14, PAD.t + 8);
      lx += ctx.measureText(it.label).width + 30;
    }
    canvas._hover = opts.hover ? (mx) => {
      if (mx < PAD.l || mx > PAD.l + g.pw) return null;
      return opts.hover(xs.inv(mx));
    } : null;
  }

  // Tooltip für alle Diagramme
  function attachTooltip(tooltip) {
    document.addEventListener('mousemove', (e) => {
      const c = e.target;
      if (!(c instanceof HTMLCanvasElement) || !c._hover) { tooltip.hidden = true; return; }
      const r = c.getBoundingClientRect();
      const text = c._hover(e.clientX - r.left, e.clientY - r.top);
      if (!text) { tooltip.hidden = true; return; }
      tooltip.textContent = text;
      tooltip.hidden = false;
      const tw = tooltip.offsetWidth;
      tooltip.style.left = Math.min(window.innerWidth - tw - 8, e.clientX + 14) + 'px';
      tooltip.style.top = (e.clientY + 14) + 'px';
    });
  }

  return { histogram, series, xy, noData, fmt, css, attachTooltip };
})();
