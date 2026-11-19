/* ST 2110 Monitor – Oberfläche. Fragt /api/snapshot zweimal je Sekunde ab und zeichnet die aktive Ansicht. */
'use strict';

(() => {
  const $ = (id) => document.getElementById(id);
  const fmt = Charts.fmt;
  const C = (name) => Charts.css(name);

  const state = {
    tab: 'overview',
    seg: { videoPath: 'P', profilePath: 'P', profileKind: 'last', audioPath: 'P', eventFilter: 'all' },
    window: 60,
    paused: false,
    logY: true,
    snap: null,
    prevCounters: null,
    rates: { video: null, audio: null },
    lastEventCount: 0,
    seenEvents: 0,
    info: null,
  };

  const charts = {};
  document.querySelectorAll('canvas[data-chart]').forEach(c => { charts[c.dataset.chart] = c; });

  // ---------- Hilfsfunktionen ----------
  const verdictLabel = { narrow: 'narrow', wide: 'wide', failed: 'failed' };
  const ptpLabel = { locked: 'eingerastet', syncing: 'nicht eingerastet', nosync: 'kein Sync' };
  const us = (ns) => (ns === null || ns === undefined) ? null : ns / 1000;

  function esc(s) {
    return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }

  function kv(table, rows) {
    table.innerHTML = rows.map(([k, v, cls]) =>
      `<tr><td>${esc(k)}</td><td${cls ? ` class="${cls}"` : ''}>${v === null || v === undefined ? '–' : esc(v)}</td></tr>`).join('');
  }

  function verdictSpan(v) {
    return v ? `<span class="verdict ${v}">${verdictLabel[v] || v}</span>` : '<span class="empty">–</span>';
  }

  function rangeText(r, scale = 1, digits) {
    if (!r) return '–';
    return `${fmt(r.min * scale, digits)} / ${fmt(r.mean * scale, digits)} / ${fmt(r.max * scale, digits)}`;
  }

  function setSegEnabled(name, value, enabled) {
    const btn = document.querySelector(`[data-seg="${name}"] button[data-v="${value}"]`);
    if (!btn) return;
    btn.disabled = !enabled;
    if (!enabled && state.seg[name] === value) {
      state.seg[name] = 'P';
      document.querySelectorAll(`[data-seg="${name}"] button`).forEach(b => b.classList.toggle('active', b.dataset.v === 'P'));
    }
  }

  function pathColor(p) { return p === 'R' ? C('--path-r') : C('--path-p'); }

  // ---------- Bedienung ----------
  $('tabs').addEventListener('click', (e) => {
    const btn = e.target.closest('button[data-tab]');
    if (!btn) return;
    state.tab = btn.dataset.tab;
    document.querySelectorAll('#tabs button').forEach(b => b.classList.toggle('active', b === btn));
    document.querySelectorAll('.view').forEach(v => v.classList.toggle('active', v.dataset.view === state.tab));
    if (state.tab === 'events') { state.seenEvents = state.lastEventCount; $('eventBadge').textContent = ''; }
    render();
  });

  document.querySelectorAll('[data-seg]').forEach(seg => {
    seg.addEventListener('click', (e) => {
      const btn = e.target.closest('button');
      if (!btn || btn.disabled) return;
      state.seg[seg.dataset.seg] = btn.dataset.v;
      seg.querySelectorAll('button').forEach(b => b.classList.toggle('active', b === btn));
      render();
    });
  });

  $('window').addEventListener('change', (e) => { state.window = +e.target.value; poll(true); });
  $('logY').addEventListener('change', (e) => { state.logY = e.target.checked; render(); });
  $('pause').addEventListener('click', () => {
    state.paused = !state.paused;
    $('pause').textContent = state.paused ? 'Fortsetzen' : 'Anhalten';
    $('pause').classList.toggle('on', state.paused);
  });
  $('reset').addEventListener('click', async () => {
    if (!confirm('Alle Histogramme, Gesamtwerte, Profile und Ereignisse zurücksetzen?')) return;
    await fetch('api/reset', { method: 'POST' });
    state.prevCounters = null;
    poll(true);
  });
  $('demoPreset').addEventListener('change', async (e) => {
    await fetch('api/demo', { method: 'POST', headers: { 'Content-Type': 'application/json' },
                              body: JSON.stringify({ preset: e.target.value }) });
  });
  window.addEventListener('resize', () => render());
  Charts.attachTooltip($('tooltip'));

  // ---------- Abfrage ----------
  let inflight = false;
  async function poll(force) {
    if ((state.paused && !force) || inflight) return;
    inflight = true;
    try {
      const res = await fetch(`api/snapshot?window=${state.window}`, { cache: 'no-store' });
      if (!res.ok) throw new Error('HTTP ' + res.status);
      const snap = await res.json();
      updateRates(snap);
      state.snap = snap;
      $('banner').hidden = true;
      render();
    } catch (err) {
      const b = $('banner');
      b.hidden = false;
      b.className = 'banner error';
      b.textContent = 'Keine Verbindung zum Messgerät (' + err.message + ').';
    } finally {
      inflight = false;
    }
  }

  async function init() {
    try {
      state.info = await (await fetch('api/info')).json();
      if (state.info.demo) {
        const d = await (await fetch('api/demo')).json();
        const sel = $('demoPreset');
        sel.innerHTML = d.presets.map(p => `<option value="${esc(p.id)}">${esc(p.label)}</option>`).join('');
        sel.value = d.current;
        $('demoCtl').hidden = false;
      }
    } catch (_) { /* Abfrage meldet den Fehler */ }
    poll(true);
    setInterval(poll, 500);
  }

  function updateRates(snap) {
    const now = performance.now() / 1000;
    const cur = snap.counters || {};
    if (state.prevCounters) {
      const dt = now - state.prevCounters.time;
      for (const m of ['video', 'audio']) {
        const a = cur[m], b = state.prevCounters[m];
        if (a && b && dt > 0.2) {
          const rate = {};
          for (const p of Object.keys(a.paths)) {
            rate[p] = {
              pps: (a.paths[p].packets - b.paths[p].packets) / dt,
              bps: (a.paths[p].bytes - b.paths[p].bytes) * 8 / dt,
            };
          }
          state.rates[m] = rate;
        }
      }
    }
    state.prevCounters = { time: now, video: cur.video, audio: cur.audio };
  }

  // ---------- Darstellung ----------
  function render() {
    const s = state.snap;
    if (!s) return;
    renderHeader(s);
    switch (state.tab) {
      case 'overview': renderOverview(s); break;
      case 'video': renderVideo(s); break;
      case 'profile': renderProfile(s); break;
      case 'audio': renderAudio(s); break;
      case 'ptp': renderPtp(s); break;
      case 'redundancy': renderRedundancy(s); break;
      case 'counters': renderCounters(s); break;
      case 'events': renderEvents(s); break;
    }
  }

  function renderHeader(s) {
    const parts = [];
    if (s.demo) parts.push('DEMO');
    if (s.video) parts.push(s.video.format);
    if (s.audio) parts.push(s.audio.format);
    parts.push(`Laufzeit ${fmtDuration(s.uptimeSec)}`);
    $('subtitle').textContent = parts.join(' · ');

    const chips = [];
    if (s.video) for (const p of Object.keys(s.video.paths)) {
      const v = s.video.paths[p].verdict;
      chips.push(chip(v || '', `Video ${p}`, v ? verdictLabel[v] : '–'));
    }
    if (s.audio) for (const p of Object.keys(s.audio.paths)) {
      const v = s.audio.paths[p].verdict;
      chips.push(chip(v || '', `Ton ${p}`, v ? verdictLabel[v] : '–'));
    }
    if (s.ptpEnabled || s.demo) {
      const off = s.ptp.lastDeltaNs;
      chips.push(chip(s.ptp.state, 'PTP', `${ptpLabel[s.ptp.state]}${off !== null ? ' · ' + fmt(off, 0) + ' ns' : ''}`));
    }
    if (s.video && s.video.skew && s.video.skew.lastNs !== null) {
      chips.push(chip('ok', '2022-7', `Versatz ${fmt(us(s.video.skew.lastNs), 1)} µs`));
    }
    const lost = totalLost(s);
    chips.push(chip(lost.unrecovered > 0 ? 'error' : lost.lost > 0 ? 'warn' : 'ok', 'Verlust',
                    `${fmt(lost.lost, 0)} / ${fmt(lost.unrecovered, 0)}`));
    $('chips').innerHTML = chips.join('');

    // Hinweise
    const warn = [];
    if (!s.hwTimestamp && !s.demo) warn.push('Ohne Hardware-Zeitstempel: Cinst, VRX, IPT und FPT sind nicht aussagekräftig.');
    if (!s.ptpEnabled && !s.demo) warn.push('Ohne PTP: FPT, Latenz und RTP-Offset sind nicht aussagekräftig.');
    if (s.video && s.video.packets > 0 && s.video.swTimestampPackets > 0) {
      warn.push(`${fmt(s.video.swTimestampPackets / s.video.packets * 100, 2)} % der Videopakete ohne Hardware-Zeitstempel.`);
    }
    if (state.info && !state.info.packetHook && !s.demo) warn.push('MTL ohne Paket-Rückruf: Histogramme und Paketprofile fehlen (patches/mtl anwenden).');
    const b = $('banner');
    if (warn.length && b.hidden) { b.hidden = false; b.className = 'banner'; b.textContent = warn.join(' '); }

    const n = (s.events || []).length;
    state.lastEventCount = n;
    const errors = (s.events || []).slice(0, Math.max(0, n - state.seenEvents)).filter(e => e.level !== 'info').length;
    $('eventBadge').textContent = state.tab === 'events' || !errors ? '' : String(errors);
  }

  function chip(cls, label, value) {
    return `<span class="chip ${esc(cls)}"><span class="dot"></span>${esc(label)} <b>${esc(value)}</b></span>`;
  }

  function fmtDuration(sec) {
    sec = Math.floor(sec || 0);
    const h = Math.floor(sec / 3600), m = Math.floor(sec / 60) % 60, s = sec % 60;
    return (h ? h + ':' + String(m).padStart(2, '0') : m) + ':' + String(s).padStart(2, '0');
  }

  function totalLost(s) {
    let lost = 0, unrecovered = 0;
    for (const m of ['video', 'audio']) {
      const c = s.counters && s.counters[m];
      if (c) { lost += c.lost; unrecovered += c.unrecovered; }
    }
    return { lost, unrecovered };
  }

  function videoPass(s) { return (s.video && s.video.pass) || null; }

  // ---------- Übersicht ----------
  function renderOverview(s) {
    const tiles = [];
    if (s.video) {
      for (const p of Object.keys(s.video.paths)) {
        const path = s.video.paths[p];
        const f = path.lastFrame;
        const h = path.histograms;
        tiles.push(tile(path.verdict || '', `Video ${p} · ${s.video.format}`, path.verdict ? verdictLabel[path.verdict] : '–', [
          ['Cinst max', f ? fmt(f.cinstMax, 0) : '–'],
          ['VRX min/max', f ? `${fmt(f.vrxMin, 0)} / ${fmt(f.vrxMax, 0)}` : '–'],
          ['FPT', f ? fmt(us(f.fptNs), 1) + ' µs' : '–'],
          ['Latenz', f ? fmt(us(f.latencyNs), 1) + ' µs' : '–'],
          ['RTP-Offset', f ? fmt(f.rtpOffset, 0) : '–'],
          ['IPT p50/p99', h && h.iptNs && h.iptNs.count ? `${fmt(us(h.iptNs.p50), 3)} / ${fmt(us(h.iptNs.p99), 3)} µs` : '–'],
        ]));
      }
    }
    if (s.audio) {
      for (const p of Object.keys(s.audio.paths)) {
        const path = s.audio.paths[p];
        const a = path.lastPeriod;
        tiles.push(tile(path.verdict || '', `Ton ${p} · ${s.audio.format}`, path.verdict ? verdictLabel[path.verdict] : '–', [
          ['DPVR min/max', a ? `${fmt(a.dpvrMinUs, 0)} / ${fmt(a.dpvrMaxUs, 0)} µs` : '–'],
          ['TSDF', a ? fmt(a.tsdfUs, 0) + ' µs' : '–'],
          ['IPT mittel', a ? fmt(us(a.iptAvgNs), 1) + ' µs' : '–'],
          ['Pakete/200 ms', a ? fmt(a.packets, 0) : '–'],
        ]));
      }
    }
    const ptp = s.ptp;
    tiles.push(tile(ptp.state, 'PTP', ptpLabel[ptp.state], [
      ['Korrektur', ptp.lastDeltaNs !== null ? fmt(ptp.lastDeltaNs, 0) + ' ns' : '–'],
      ['min/mittel/max', ptp.deltaNs ? rangeText(ptp.deltaNs, 1, 0) + ' ns' : '–'],
      ['UTC-Versatz', ptp.utcOffset !== null ? ptp.utcOffset + ' s' : '–'],
      ['Syncs', fmt(ptp.syncs, 0)],
    ]));
    if (s.video && s.video.skew) {
      const sk = s.video.skew;
      tiles.push(tile(sk.lastNs !== null ? 'ok' : '', 'ST 2022-7 Pfadversatz', sk.lastNs !== null ? fmt(us(sk.lastNs), 1) + ' µs' : '–', [
        ['min/mittel/max', sk.totalNs ? rangeText(sk.totalNs, 1e-3, 1) + ' µs' : '–'],
        ['Bilder rekonstruiert', fmt(s.video.frames.reconstructed, 0)],
      ]));
    }
    const lost = totalLost(s);
    tiles.push(tile(lost.unrecovered ? 'error' : lost.lost ? 'warn' : 'ok', 'Paketverlust', fmt(lost.lost, 0), [
      ['nach 2022-7 verloren', fmt(lost.unrecovered, 0)],
      ['Bilder beschädigt', s.video ? fmt(s.video.frames.corrupted, 0) : '–'],
      ['Bilder gesamt', s.video ? fmt(s.video.frames.total, 0) : '–'],
    ]));
    $('overviewTiles').innerHTML = tiles.join('');

    const pass = videoPass(s);
    if (s.video) {
      const vp = s.video.paths;
      const list = Object.keys(vp).map(p => ({ data: vp[p].series.cinstMax, color: pathColor(p), label: p, use: 'max' }));
      Charts.series(charts.ovCinst, list, { window: state.window, markers: pass ? [
        { y: pass.cinstMaxNarrow, label: 'narrow', color: C('--narrow') },
        { y: pass.cinstMaxWide, label: 'wide', color: C('--wide'), include: false }] : [], yMin: 0 });
      const vl = [];
      for (const p of Object.keys(vp)) {
        vl.push({ data: mergeMinMax(vp[p].series.vrxMin, vp[p].series.vrxMax), color: pathColor(p), label: p, use: 'band' });
      }
      Charts.series(charts.ovVrx, vl, { window: state.window, markers: pass ? [
        { y: pass.vrxMaxNarrow, label: 'narrow', color: C('--narrow') }, { y: 0, label: '', color: C('--failed') }] : [] });
    } else { Charts.noData(charts.ovCinst, 'kein Video'); Charts.noData(charts.ovVrx, 'kein Video'); }
    if (s.audio) {
      const ap = s.audio.paths;
      const list = [];
      for (const p of Object.keys(ap)) {
        list.push({ data: ap[p].series.dpvrMaxUs, color: pathColor(p), label: `DPVR ${p}`, use: 'max' });
        list.push({ data: ap[p].series.tsdfUs, color: p === 'R' ? '#f0abfc' : '#a5f3fc', label: `TSDF ${p}`, use: 'max' });
      }
      const ps = s.audio.pass;
      Charts.series(charts.ovAudio, list, { window: state.window, unit: 'µs', yMin: 0,
        markers: ps ? [{ y: ps.tsdfMaxNarrowUs, label: 'TSDF narrow', color: C('--narrow'), include: false }] : [] });
    } else Charts.noData(charts.ovAudio, 'kein Ton');
    Charts.series(charts.ovPtp, [{ data: s.ptp.series, color: C('--accent'), label: 'Korrektur', use: 'max' }],
      { window: state.window, unit: 'ns', markers: [{ y: 0, color: C('--ideal'), label: '' }] });
  }

  function mergeMinMax(minSeries, maxSeries) {
    if (!minSeries || !maxSeries) return null;
    return { t: maxSeries.t, min: minSeries.min, max: maxSeries.max };
  }

  function tile(cls, label, value, rows) {
    return `<div class="tile ${esc(cls)}"><div class="label">${esc(label)}</div><div class="value">${esc(value)}</div>` +
      `<div class="rows">${rows.map(([k, v]) => `<span>${esc(k)}</span><span>${esc(v)}</span>`).join('')}</div></div>`;
  }

  // ---------- Video ----------
  function renderVideo(s) {
    if (!s.video) { for (const k of ['iptHist', 'cinstHist', 'vrxHist', 'vCinstVrx', 'vFptLat', 'vRtp', 'vIptMax']) Charts.noData(charts[k], 'kein Video'); return; }
    setSegEnabled('videoPath', 'R', !!s.video.paths.R);
    const p = state.seg.videoPath;
    const path = s.video.paths[p];
    const pass = videoPass(s);
    const params = s.video.params;
    $('videoSource').textContent = s.video.frameSource === 'mtl'
      ? 'Bildwerte und Urteil: Timing-Parser von MTL · Histogramme: eigene Paketauswertung'
      : 'Bildwerte, Urteil und Histogramme: eigene Paketauswertung';
    const col = pathColor(p);
    const h = path.histograms || {};
    const trsUs = params ? params.trsNs / 1000 : null;
    Charts.histogram(charts.iptHist, h.iptNs, { scale: 1e-3, unit: 'µs', logY: state.logY, color: col,
      markers: trsUs ? [{ x: trsUs, label: 'TRS', color: C('--ideal') }] : [] });
    Charts.histogram(charts.cinstHist, h.cinst, { unit: 'Pakete', logY: state.logY, color: col, minSpan: 8,
      markers: pass ? [{ x: pass.cinstMaxNarrow + 1, label: 'narrow', color: C('--narrow') },
                       { x: pass.cinstMaxWide + 1, label: 'wide', color: C('--wide'), include: false }] : [] });
    Charts.histogram(charts.vrxHist, h.vrx, { unit: 'Pakete', logY: state.logY, color: col, minSpan: 12,
      markers: pass ? [{ x: 0, label: '0', color: C('--failed') },
                       { x: pass.vrxMaxNarrow + 1, label: 'narrow', color: C('--narrow') }] : [] });

    const se = path.series;
    Charts.series(charts.vCinstVrx, [
      { data: se.cinstMax, color: col, label: 'Cinst max', use: 'max' },
      { data: mergeMinMax(se.vrxMin, se.vrxMax), color: C('--ideal'), label: 'VRX min…max', use: 'band' },
    ], { window: state.window, markers: pass ? [{ y: pass.cinstMaxNarrow, label: 'Cinst narrow', color: C('--narrow') }] : [] });
    Charts.series(charts.vFptLat, [
      { data: se.fptUs, color: col, label: 'FPT', use: 'max' },
      { data: se.latencyUs, color: C('--wide'), label: 'Latenz', use: 'max' },
    ], { window: state.window, unit: 'µs', markers: pass ? [{ y: pass.trOffsetNs / 1000, label: 'TR_OFFSET', color: C('--failed') }] : [] });
    Charts.series(charts.vRtp, [{ data: se.rtpOffset, color: col, label: 'RTP-Offset', use: 'band' }],
      { window: state.window, markers: pass ? [
        { y: pass.rtpOffsetMax, label: 'max', color: C('--failed'), include: false },
        { y: pass.rtpOffsetMin, label: 'min', color: C('--failed') }] : [] });
    Charts.series(charts.vIptMax, [{ data: se.iptMaxNs, color: col, label: 'IPT max', use: 'max' }],
      { window: state.window, unit: 'µs', scale: 1e-3, markers: trsUs ? [{ y: trsUs, label: 'TRS', color: C('--ideal') }] : [] });

    const f = path.lastFrame;
    kv($('videoLast'), f ? [
      ['Urteil', f.verdict], ['Ursache', f.cause],
      ['Cinst max / mittel', `${fmt(f.cinstMax, 0)} / ${fmt(f.cinstAvg, 2)}`],
      ['VRX min / max', `${fmt(f.vrxMin, 0)} / ${fmt(f.vrxMax, 0)}`],
      ['IPT min / mittel / max', `${fmt(us(f.iptMinNs), 3)} / ${fmt(us(f.iptAvgNs), 3)} / ${fmt(us(f.iptMaxNs), 3)} µs`],
      ['FPT', fmt(us(f.fptNs), 2) + ' µs'], ['Latenz', fmt(us(f.latencyNs), 2) + ' µs'],
      ['RTP-Offset / Δ', `${fmt(f.rtpOffset, 0)} / ${fmt(f.rtpTsDelta, 0)}`], ['Pakete', fmt(f.packets, 0)],
    ] : [['Status', 'noch kein Bild']]);
    kv($('videoParams'), [
      ['Format', s.video.format], ['Payload-Typ', s.video.payloadType],
      ['Strom P', flowText(s.video.flows.P)], ['Strom R', s.video.flows.R ? flowText(s.video.flows.R) : null],
      ['Pakete je Bild', params ? fmt(params.packetsPerFrame, 0) : null],
      ['TRS', params ? fmt(params.trsNs / 1000, 4) + ' µs' : null],
      ['TR_OFFSET', pass ? fmt(pass.trOffsetNs / 1000, 2) + ' µs' : null],
      ['Cinst narrow / wide', pass ? `${pass.cinstMaxNarrow} / ${pass.cinstMaxWide}` : null],
      ['VRX narrow / wide', pass ? `${pass.vrxMaxNarrow} / ${pass.vrxMaxWide}` : null],
      ['RTP-Offset min / max', pass ? `${pass.rtpOffsetMin} / ${pass.rtpOffsetMax}` : null],
      ['Latenz max', pass ? fmt(pass.latencyMaxNs / 1000, 0) + ' µs' : null],
    ]);
    const t = path.total;
    kv($('videoTotal'), [
      ['Bilder', fmt(t.frames, 0)],
      ['narrow / wide / failed', `${fmt(t.counts.narrow, 0)} / ${fmt(t.counts.wide, 0)} / ${fmt(t.counts.failed, 0)}`],
      ['Cinst min/mittel/max', rangeText(t.cinst, 1, 2)], ['VRX min/mittel/max', rangeText(t.vrx, 1, 2)],
      ['FPT min/mittel/max', rangeText(t.fptNs, 1e-3, 1) + ' µs'], ['Latenz min/mittel/max', rangeText(t.latencyNs, 1e-3, 1) + ' µs'],
      ['RTP-Offset min/max', t.rtpOffset ? `${fmt(t.rtpOffset.min, 0)} / ${fmt(t.rtpOffset.max, 0)}` : '–'],
    ]);
    const causes = Object.entries(t.failedCauses || {}).sort((a, b) => b[1] - a[1]);
    $('videoCauses').innerHTML = causes.length
      ? causes.map(([c, n]) => `<tr><td>${esc(c)}</td><td>${fmt(n, 0)}</td></tr>`).join('')
      : '<tr><td class="empty">keine</td><td></td></tr>';
  }

  function flowText(f) {
    if (!f) return null;
    return `${f.ip}:${f.udpPort}${f.sourceIp ? ' (Quelle ' + f.sourceIp + ')' : ''}`;
  }

  // ---------- Paketprofil ----------
  function renderProfile(s) {
    const ids = ['profDev', 'profCinst', 'profVrx', 'profShape'];
    if (!s.video) { ids.forEach(k => Charts.noData(charts[k], 'kein Video')); return; }
    setSegEnabled('profilePath', 'R', !!s.video.paths.R);
    const path = s.video.paths[state.seg.profilePath];
    const prof = path.profiles && path.profiles[state.seg.profileKind];
    const params = s.video.params;
    const pass = videoPass(s);
    if (!prof || !params) {
      ids.forEach(k => Charts.noData(charts[k], state.info && !state.info.packetHook ? 'Paketprofil braucht MTL mit Patch' : 'noch kein Profil'));
      kv($('profileTiming'), [['Status', 'noch kein Profil']]);
      $('profileInfo').textContent = '';
      return;
    }
    const col = pathColor(state.seg.profilePath);
    const trs = params.trsNs / 1000;
    const tro = params.trOffsetNs / 1000;
    const ideal = prof.index.map(i => tro + trs * (i + 1));
    const dev = prof.arrivalUs.map((a, k) => a - ideal[k]);
    $('profileInfo').textContent = `RTP ${prof.rtpTimestamp} · ${fmt(prof.packets, 0)} Pakete` +
      (prof.stride > 1 ? ` · Darstellung jedes ${prof.stride}. Pakets (Spitzen erhalten)` : '');
    const devMarkers = [{ y: 0, label: 'Soll', color: C('--ideal') }];
    if (pass) devMarkers.push({ y: -pass.vrxMaxNarrow * trs, label: 'VRX narrow', color: C('--narrow'), include: false });
    Charts.xy(charts.profDev, [{ x: prof.index, y: dev, color: col, mode: 'points', label: 'Abweichung' }], {
      xLabel: 'Paketindex', yLabel: 'µs',
      markersY: devMarkers,
      hover: (x) => nearest(prof.index, x, k => `Paket ${prof.index[k]}\nAnkunft ${fmt(prof.arrivalUs[k], 3)} µs\nSoll ${fmt(ideal[k], 3)} µs\nAbweichung ${fmt(dev[k], 3)} µs`),
    });
    Charts.xy(charts.profCinst, [{ x: prof.index, y: prof.cinst, color: col, mode: 'step', label: 'Cinst' }], {
      xLabel: 'Paketindex', yMin: 0,
      markersY: pass ? [{ y: pass.cinstMaxNarrow, label: 'narrow', color: C('--narrow') }] : [],
      hover: (x) => nearest(prof.index, x, k => `Paket ${prof.index[k]}\nCinst ${prof.cinst[k]}`),
    });
    Charts.xy(charts.profVrx, [{ x: prof.index, y: prof.vrx, color: col, mode: 'step', label: 'VRX' }], {
      xLabel: 'Paketindex',
      markersY: pass ? [{ y: pass.vrxMaxNarrow, label: 'narrow', color: C('--narrow') }, { y: 0, label: '0', color: C('--failed') }] : [],
      hover: (x) => nearest(prof.index, x, k => `Paket ${prof.index[k]}\nVRX ${prof.vrx[k]}`),
    });
    Charts.xy(charts.profShape, [
      { x: prof.arrivalUs, y: prof.index, color: col, mode: 'points', label: 'Ankunft' },
      { x: ideal, y: prof.index, color: C('--ideal'), mode: 'line', dash: [4, 4], label: 'Soll', fit: false },
    ], { xLabel: 'µs seit Bildbeginn', yLabel: 'Paket', xMin: 0,
         markersX: [{ x: tro, label: 'TR_OFFSET', color: C('--failed') }] });
    const f = prof.timing;
    kv($('profileTiming'), [
      ['Urteil', f.verdict], ['Ursache', f.cause],
      ['Cinst max', fmt(f.cinstMax, 0)], ['VRX min / max', `${fmt(f.vrxMin, 0)} / ${fmt(f.vrxMax, 0)}`],
      ['FPT', fmt(us(f.fptNs), 2) + ' µs'], ['Latenz', fmt(us(f.latencyNs), 2) + ' µs'],
      ['IPT min / max', `${fmt(us(f.iptMinNs), 3)} / ${fmt(us(f.iptMaxNs), 3)} µs`],
      ['RTP-Offset', fmt(f.rtpOffset, 0)],
    ]);
  }

  function nearest(xs, x, make) {
    let best = 0, bd = Infinity;
    for (let i = 0; i < xs.length; i++) { const d = Math.abs(xs[i] - x); if (d < bd) { bd = d; best = i; } }
    return make(best);
  }

  // ---------- Ton ----------
  function renderAudio(s) {
    const ids = ['dpvrHist', 'aIptHist', 'aDpvr', 'aTsdf'];
    if (!s.audio) { ids.forEach(k => Charts.noData(charts[k], 'kein Ton')); return; }
    setSegEnabled('audioPath', 'R', !!s.audio.paths.R);
    const p = state.seg.audioPath;
    const path = s.audio.paths[p];
    const pass = s.audio.pass;
    const col = pathColor(p);
    const h = path.histograms || {};
    $('audioInfo').textContent = s.audio.periodSource === 'mtl'
      ? 'Zeiträume und Urteil: Timing-Parser von MTL · Histogramme: eigene Paketauswertung'
      : 'Zeiträume, Urteil und Histogramme: eigene Paketauswertung';
    Charts.histogram(charts.dpvrHist, h.dpvrUs, { unit: 'µs', logY: state.logY, color: col,
      markers: pass ? [{ x: pass.dpvrMaxNarrowUs, label: 'narrow', color: C('--narrow') },
                       { x: 0, label: '0', color: C('--failed') }] : [] });
    Charts.histogram(charts.aIptHist, h.iptNs, { scale: 1e-3, unit: 'µs', logY: state.logY, color: col,
      markers: [{ x: s.audio.packetTimeUs, label: 'Paketzeit', color: C('--ideal') }] });
    const se = path.series;
    Charts.series(charts.aDpvr, [{ data: mergeMinMax(se.dpvrMinUs, se.dpvrMaxUs), color: col, label: 'DPVR min…max', use: 'band' }],
      { window: state.window, unit: 'µs', markers: pass ? [{ y: pass.dpvrMaxNarrowUs, label: 'narrow', color: C('--narrow') }] : [] });
    Charts.series(charts.aTsdf, [{ data: se.tsdfUs, color: col, label: 'TSDF', use: 'max' }],
      { window: state.window, unit: 'µs', yMin: 0, markers: pass ? [
        { y: pass.tsdfMaxNarrowUs, label: 'narrow', color: C('--narrow') },
        { y: pass.tsdfMaxWideUs, label: 'wide', color: C('--wide'), include: false }] : [] });
    const a = path.lastPeriod;
    kv($('audioLast'), a ? [
      ['Urteil', a.verdict], ['Ursache', a.cause],
      ['DPVR min / mittel / max', `${fmt(a.dpvrMinUs, 0)} / ${fmt(a.dpvrAvgUs, 1)} / ${fmt(a.dpvrMaxUs, 0)} µs`],
      ['TSDF', fmt(a.tsdfUs, 0) + ' µs'],
      ['IPT min / mittel / max', `${fmt(us(a.iptMinNs), 1)} / ${fmt(us(a.iptAvgNs), 1)} / ${fmt(us(a.iptMaxNs), 1)} µs`],
      ['Pakete', fmt(a.packets, 0)],
    ] : [['Status', 'noch kein Zeitraum']]);
    kv($('audioParams'), [
      ['Format', s.audio.format], ['Payload-Typ', s.audio.payloadType],
      ['Strom P', flowText(s.audio.flows.P)], ['Strom R', s.audio.flows.R ? flowText(s.audio.flows.R) : null],
      ['Paketzeit', fmt(s.audio.packetTimeUs, 1) + ' µs'], ['Abtastwerte je Paket', s.audio.samplesPerPacket],
      ['DPVR narrow / wide', pass ? `${pass.dpvrMaxNarrowUs} / ${pass.dpvrMaxWideUs} µs` : null],
      ['TSDF narrow / wide', pass ? `${pass.tsdfMaxNarrowUs} / ${pass.tsdfMaxWideUs} µs` : null],
    ]);
  }

  // ---------- PTP ----------
  function renderPtp(s) {
    const p = s.ptp;
    $('ptpTiles').innerHTML = [
      tile(p.state, 'Zustand', ptpLabel[p.state], [
        ['letzter Sync vor', p.sinceLastSyncSec !== null ? fmt(p.sinceLastSyncSec, 2) + ' s' : '–'],
        ['Schwelle eingerastet', '< ' + fmt(p.lockThresholdNs, 0) + ' ns']]),
      tile(p.state, 'Letzte Korrektur', p.lastDeltaNs !== null ? fmt(p.lastDeltaNs, 0) + ' ns' : '–', [
        ['min / mittel / max', p.deltaNs ? rangeText(p.deltaNs, 1, 0) + ' ns' : '–']]),
      tile('', 'UTC-Versatz Grandmaster', p.utcOffset !== null ? p.utcOffset + ' s' : '–', [['Syncs', fmt(p.syncs, 0)]]),
    ].join('');
    Charts.series(charts.ptpSeries, [{ data: p.series, color: C('--accent'), label: 'Korrektur', use: 'max' }], {
      window: state.window, unit: 'ns',
      markers: [{ y: p.lockThresholdNs, label: 'Schwelle', color: C('--wide'), include: false },
                { y: -p.lockThresholdNs, label: '', color: C('--wide'), include: false }, { y: 0, label: '', color: C('--ideal') }] });
    Charts.histogram(charts.ptpHist, p.histogram, { unit: 'ns', logY: false, color: C('--accent'),
      markers: [{ x: 0, label: '0', color: C('--ideal') }] });
  }

  // ---------- 2022-7 ----------
  function renderRedundancy(s) {
    const sk = s.video && s.video.skew;
    if (!sk) {
      $('redTiles').innerHTML = tile('', 'ST 2022-7', 'nicht aktiv', [['Hinweis', '--video-red und --red-port angeben']]);
      Charts.noData(charts.skewSeries, 'kein redundanter Pfad'); Charts.noData(charts.skewHist, 'kein redundanter Pfad');
      return;
    }
    const cv = s.counters.video;
    $('redTiles').innerHTML = [
      tile('ok', 'Pfadversatz R − P', sk.lastNs !== null ? fmt(us(sk.lastNs), 1) + ' µs' : '–', [
        ['min / mittel / max', sk.totalNs ? rangeText(sk.totalNs, 1e-3, 1) + ' µs' : '–']]),
      tile(cv && cv.paths.P.lost ? 'warn' : 'ok', 'Pfad P', cv ? fmt(cv.paths.P.packets, 0) + ' Pakete' : '–', [
        ['verloren', cv ? fmt(cv.paths.P.lost, 0) : '–'], ['umgeordnet', cv ? fmt(cv.paths.P.reordered, 0) : '–']]),
      tile(cv && cv.paths.R.lost ? 'warn' : 'ok', 'Pfad R', cv ? fmt(cv.paths.R.packets, 0) + ' Pakete' : '–', [
        ['verloren', cv ? fmt(cv.paths.R.lost, 0) : '–'], ['umgeordnet', cv ? fmt(cv.paths.R.reordered, 0) : '–']]),
      tile(cv && cv.unrecovered ? 'error' : 'ok', 'Nach Zusammenführung', cv ? fmt(cv.unrecovered, 0) + ' verloren' : '–', [
        ['Bilder rekonstruiert', fmt(s.video.frames.reconstructed, 0)], ['Bilder beschädigt', fmt(s.video.frames.corrupted, 0)]]),
    ].join('');
    Charts.series(charts.skewSeries, [{ data: sk.series, color: C('--path-r'), label: 'R − P', use: 'band' }],
      { window: state.window, unit: 'µs', scale: 1e-3, markers: [{ y: 0, label: '', color: C('--ideal') }] });
    Charts.histogram(charts.skewHist, sk.histogram, { scale: 1e-3, unit: 'µs', logY: false, color: C('--path-r'),
      markers: [{ x: 0, label: '0', color: C('--ideal') }] });
  }

  // ---------- Zähler ----------
  function renderCounters(s) {
    for (const [m, id] of [['video', 'countersVideo'], ['audio', 'countersAudio']]) {
      const c = s.counters[m];
      const t = $(id);
      if (!c) { t.innerHTML = '<tr><td class="empty">keine Daten</td></tr>'; continue; }
      const paths = Object.keys(c.paths).filter(p => m === 'video' ? (s.video && s.video.paths[p]) : (s.audio && s.audio.paths[p]));
      const rate = state.rates[m] || {};
      const row = (label, fn) => `<tr><td>${esc(label)}</td>${paths.map(p => `<td>${fn(p)}</td>`).join('')}</tr>`;
      t.innerHTML = `<tr><th></th>${paths.map(p => `<th>Pfad ${p}</th>`).join('')}</tr>` +
        row('Pakete', p => fmt(c.paths[p].packets, 0)) +
        row('Pakete/s', p => rate[p] ? fmt(rate[p].pps, 0) : '–') +
        row('Datenrate', p => rate[p] ? fmt(rate[p].bps / 1e6, 1) + ' Mbit/s' : '–') +
        row('Verloren', p => fmt(c.paths[p].lost, 0)) +
        row('Umgeordnet', p => fmt(c.paths[p].reordered, 0)) +
        row('Doppelt (gleicher Pfad)', p => fmt(c.paths[p].duplicates, 0)) +
        row('Fehlerhaft', p => fmt(c.paths[p].errors, 0)) +
        `<tr><th>Strom</th>${paths.map(() => '<th></th>').join('')}</tr>` +
        `<tr><td>Angenommen</td><td colspan="${paths.length}">${fmt(c.received, 0)}</td></tr>` +
        `<tr><td>Unwiederbringlich verloren</td><td colspan="${paths.length}">${fmt(c.unrecovered, 0)}</td></tr>` +
        `<tr><td>Redundante Kopien</td><td colspan="${paths.length}">${fmt(c.redundant, 0)}</td></tr>` +
        `<tr><td>Falscher Payload-Typ</td><td colspan="${paths.length}">${fmt(c.wrongPayloadType, 0)}</td></tr>` +
        `<tr><td>Falsche SSRC</td><td colspan="${paths.length}">${fmt(c.wrongSsrc, 0)}</td></tr>` +
        `<tr><td>Unvollständige Bilder</td><td colspan="${paths.length}">${fmt(c.framesIncomplete, 0)}</td></tr>`;
    }
  }

  // ---------- Ereignisse ----------
  function renderEvents(s) {
    const f = state.seg.eventFilter;
    const list = (s.events || []).filter(e => f === 'all' || (f === 'warn' ? e.level !== 'info' : e.level === 'error'));
    const label = { info: 'Info', warn: 'Warnung', error: 'Fehler' };
    $('eventTable').innerHTML = list.length
      ? list.map(e => `<tr><td class="time">${esc(e.time.replace('T', ' ').replace('Z', ''))}</td>` +
          `<td class="level"><span class="lvl ${esc(e.level)}">${label[e.level] || esc(e.level)}</span></td><td>${esc(e.text)}</td></tr>`).join('')
      : '<tr><td class="empty">keine Ereignisse</td></tr>';
  }

  init();
})();
