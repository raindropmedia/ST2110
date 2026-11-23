/* ST 2110 Monitor – Oberfläche. Fragt /api/snapshot zweimal je Sekunde ab und zeichnet die aktive Ansicht. */
'use strict';

(() => {
  const $ = (id) => document.getElementById(id);
  const fmt = Charts.fmt;
  const C = (name) => Charts.css(name);

  const state = {
    tab: 'flows',
    flowId: '',
    flowsData: null,
    seg: { videoPath: 'P', profilePath: 'P', profileKind: 'last', audioPath: 'P', eventFilter: 'all',
           eventScope: 'all', discKind: 'all' },
    window: 60,
    paused: false,
    logY: true,
    snap: null,
    prevCounters: null,
    rates: { video: null, audio: null },
    lastEventCount: 0,
    seenEvents: 0,
    info: null,
    tx: null,        // letzte Antwort von /api/tx
    txEditing: null, // ID des Eintrags, dessen Einstellungen gerade im Formular stehen
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
    if (!btn || btn.disabled) return;
    showTab(btn.dataset.tab);
  });

  function showTab(tab) {
    state.tab = tab;
    document.querySelectorAll('#tabs button').forEach(b => b.classList.toggle('active', b.dataset.tab === tab));
    document.querySelectorAll('.view').forEach(v => v.classList.toggle('active', v.dataset.view === tab));
    if (tab === 'events') { state.seenEvents = state.lastEventCount; $('eventBadge').textContent = ''; }
    if (tab === 'flows') pollFlows();
    if (tab === 'txgen') pollTx();
    if (tab !== 'preview') leavePreview();
    writeHash();
    render();
  }

  function writeHash() {
    const h = `#tab=${state.tab}${state.flowId ? '&flow=' + encodeURIComponent(state.flowId) : ''}`;
    if (location.hash !== h) history.replaceState(null, '', h);
  }

  function readHash() {
    const p = new URLSearchParams(location.hash.slice(1));
    if (p.get('flow')) state.flowId = p.get('flow');
    if (p.get('tab') && document.querySelector(`.view[data-view="${p.get('tab')}"]`)) state.tab = p.get('tab');
  }

  function selectFlow(id) {
    if (id === state.flowId) return;
    state.flowId = id;
    state.prevCounters = null;
    state.rates = { video: null, audio: null };
    $('flowSelect').value = id;
    writeHash();
    poll(true);
  }

  $('flowSelect').addEventListener('change', (e) => selectFlow(e.target.value));

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
      const res = await fetch(`api/snapshot?window=${state.window}&flow=${encodeURIComponent(state.flowId)}`,
                              { cache: 'no-store' });
      if (!res.ok) throw new Error('HTTP ' + res.status);
      const snap = await res.json();
      // Ansichten arbeiten mit dem gewählten Flow wie bisher mit s.video / s.audio / s.counters.
      const f = snap.flow;
      if (f && !state.flowId) { state.flowId = f.id; writeHash(); }
      if (!f && state.flowId && snap.flows.length && !snap.flows.some(x => x.id === state.flowId)) {
        state.flowId = snap.flows[0].id; // gewählter Flow wurde entfernt
      }
      snap.video = f && f.video ? f.video : null;
      snap.audio = f && f.audio ? f.audio : null;
      snap.counters = { video: f && f.kind === 'video' ? f.counters : null, audio: f && f.kind === 'audio' ? f.counters : null };
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
    readHash();
    showTab(state.tab);
    poll(true);
    pollFlows();
    setInterval(poll, 500);
    setInterval(() => { if (state.tab === 'flows' && !state.paused) pollFlows(); }, 1000);
    setInterval(() => { if (state.tab === 'txgen') pollTx(); }, 1000);
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
    updateFlowSelect(s);
    updateTabs(s);
    switch (state.tab) {
      case 'flows': renderFlows(); break;
      case 'txgen': renderTx(); break;
      case 'overview': renderOverview(s); break;
      case 'preview': renderPreviewCards(s); break;
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
    if (s.flow) parts.push(s.flow.label);
    if (s.video) parts.push(s.video.format);
    if (s.audio) parts.push(s.audio.format);
    parts.push(`Laufzeit ${fmtDuration(s.uptimeSec)}`);
    $('subtitle').textContent = parts.join(' · ');

    const chips = [];
    const counts = { narrow: 0, wide: 0, failed: 0, none: 0 };
    for (const f of s.flows) counts[worstVerdict(f) || 'none']++;
    const flowsCls = counts.failed ? 'failed' : counts.wide ? 'wide' : counts.narrow ? 'narrow' : '';
    chips.push(chip(flowsCls, 'Flows', `${s.flows.length}` + (counts.failed ? ` · ${counts.failed} failed` : '') +
                                        (counts.wide ? ` · ${counts.wide} wide` : '')));
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
      $('ovCap1').textContent = 'Cinst max je Bild';
      $('ovCap2').textContent = 'VRX je Bild (min/max)';
      $('ovCap3').textContent = 'FPT und Latenz je Bild (µs)';
      Charts.series(charts.ov1, Object.keys(vp).map(p => ({ data: vp[p].series.cinstMax, color: pathColor(p), label: p, use: 'max' })),
        { window: state.window, yMin: 0, markers: pass ? [
          { y: pass.cinstMaxNarrow, label: 'narrow', color: C('--narrow') },
          { y: pass.cinstMaxWide, label: 'wide', color: C('--wide'), include: false }] : [] });
      Charts.series(charts.ov2, Object.keys(vp).map(p => ({ data: mergeMinMax(vp[p].series.vrxMin, vp[p].series.vrxMax),
                                                         color: pathColor(p), label: p, use: 'band' })),
        { window: state.window, markers: pass ? [
          { y: pass.vrxMaxNarrow, label: 'narrow', color: C('--narrow') }, { y: 0, label: '', color: C('--failed') }] : [] });
      const fl = [];
      for (const p of Object.keys(vp)) {
        fl.push({ data: vp[p].series.fptUs, color: pathColor(p), label: `FPT ${p}`, use: 'max' });
      }
      fl.push({ data: vp.P.series.latencyUs, color: C('--wide'), label: 'Latenz P', use: 'max' });
      Charts.series(charts.ov3, fl, { window: state.window, unit: 'µs',
        markers: pass ? [{ y: pass.trOffsetNs / 1000, label: 'TR_OFFSET', color: C('--failed') }] : [] });
    } else if (s.audio) {
      const ap = s.audio.paths;
      const ps = s.audio.pass;
      $('ovCap1').textContent = 'DPVR min/max je 200 ms (µs)';
      $('ovCap2').textContent = 'TSDF je 200 ms (µs)';
      $('ovCap3').textContent = 'IPT max je 200 ms (µs)';
      Charts.series(charts.ov1, Object.keys(ap).map(p => ({ data: mergeMinMax(ap[p].series.dpvrMinUs, ap[p].series.dpvrMaxUs),
                                                         color: pathColor(p), label: p, use: 'band' })),
        { window: state.window, unit: 'µs', markers: ps ? [{ y: ps.dpvrMaxNarrowUs, label: 'narrow', color: C('--narrow'), include: false }] : [] });
      Charts.series(charts.ov2, Object.keys(ap).map(p => ({ data: ap[p].series.tsdfUs, color: pathColor(p), label: p, use: 'max' })),
        { window: state.window, unit: 'µs', yMin: 0, markers: ps ? [{ y: ps.tsdfMaxNarrowUs, label: 'narrow', color: C('--narrow') }] : [] });
      Charts.series(charts.ov3, Object.keys(ap).map(p => ({ data: ap[p].series.iptMaxNs, color: pathColor(p), label: p, use: 'max' })),
        { window: state.window, unit: 'µs', scale: 1e-3, markers: [{ y: s.audio.packetTimeUs, label: 'Paketzeit', color: C('--ideal') }] });
    } else {
      for (const k of ['ov1', 'ov2', 'ov3']) Charts.noData(charts[k], 'kein Flow gewählt');
    }
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
        ['Einrasten / Ausrasten', '< ' + fmt(p.lockThresholdNs, 0) + ' ns (8 Syncs) / ≥ ' + fmt(p.unlockThresholdNs, 0) + ' ns'],
        ['Zeitsprünge (≥ 1 ms)', p.steps ? `${fmt(p.steps, 0)} · zuletzt ${fmt(p.lastStepNs, 0)} ns` : 'keine']]),
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
    const scope = state.seg.eventScope;
    const list = (s.events || []).filter(e => (f === 'all' || (f === 'warn' ? e.level !== 'info' : e.level === 'error')) &&
                                              (scope === 'all' || e.flow === state.flowId));
    const label = { info: 'Info', warn: 'Warnung', error: 'Fehler' };
    $('eventTable').innerHTML = list.length
      ? list.map(e => `<tr><td class="time">${esc(e.time.replace('T', ' ').replace('Z', ''))}</td>` +
          `<td class="level"><span class="lvl ${esc(e.level)}">${label[e.level] || esc(e.level)}</span></td><td>${esc(e.text)}</td></tr>`).join('')
      : '<tr><td class="empty">keine Ereignisse</td></tr>';
  }

  // ---------- Flows ----------
  const kindLabel = { video: 'Video', audio: 'Ton', anc: 'Zusatzdaten', ptp: 'PTP', sap: 'SAP', rtp: 'RTP', udp: 'UDP' };
  const order = { failed: 3, wide: 2, narrow: 1 };

  function worstVerdict(f) {
    let worst = null;
    for (const v of Object.values(f.verdict || {})) {
      if (v && (!worst || order[v] > order[worst])) worst = v;
    }
    return worst;
  }

  function mbit(bps) { return bps === null || bps === undefined ? '–' : fmt(bps / 1e6, bps >= 1e8 ? 0 : 1); }

  function updateFlowSelect(s) {
    const sel = $('flowSelect');
    const key = s.flows.map(f => f.id + f.label).join('|');
    if (sel.dataset.key !== key) {
      sel.innerHTML = s.flows.length
        ? s.flows.map(f => `<option value="${esc(f.id)}">${esc(f.label)} · ${esc(f.format)}</option>`).join('')
        : '<option value="">keine Flows</option>';
      sel.dataset.key = key;
    }
    sel.value = state.flowId;
  }

  function updateTabs(s) {
    const kind = s.flow ? s.flow.kind : null;
    const allowed = {
      flows: true, txgen: true, ptp: true, events: true,
      overview: !!kind, counters: !!kind, redundancy: !!kind, preview: !!kind,
      video: kind === 'video', profile: kind === 'video', audio: kind === 'audio',
    };
    document.querySelectorAll('#tabs button[data-tab]').forEach(b => {
      b.disabled = !allowed[b.dataset.tab];
      b.title = b.disabled ? (kind ? 'passt nicht zum gewählten Flow' : 'kein Flow gewählt') : '';
    });
    if (!allowed[state.tab]) showTab(kind ? 'overview' : 'flows');
  }

  async function pollFlows() {
    try {
      const res = await fetch('api/flows', { cache: 'no-store' });
      if (!res.ok) return;
      state.flowsData = await res.json();
      if (state.tab === 'flows') renderFlows();
    } catch (_) { /* Abfrage des Snapshots meldet Verbindungsfehler */ }
  }

  function addrText(a) { return a ? `${a.ip}:${a.udpPort}` : '–'; }

  function renderFlows() {
    const d = state.flowsData;
    if (!d) return;
    // Überwachte Flows
    const m = d.monitored;
    $('monitoredInfo').textContent = `${m.length} von höchstens ${d.maxFlows}`;
    $('monitoredTable').innerHTML = `<tr><th></th><th>Name</th><th>Art</th><th>Format</th><th>Ziel P</th>
      <th>Ziel R</th><th>PT</th><th>Mbit/s P / R</th><th>Pakete/s</th><th>Verlust</th><th>Urteil P / R</th><th></th></tr>` +
      (m.length ? m.map(f => {
        const w = worstVerdict(f);
        const rp = f.rates.P, rr = f.rates.R;
        const v = (p) => f.verdict[p] ? `<span class="verdict ${f.verdict[p]}">${f.verdict[p]}</span>` : '–';
        return `<tr class="${f.id === state.flowId ? 'selected' : ''}" data-id="${esc(f.id)}">
          <td><span class="dot ${esc(w || '')}"></span></td><td>${esc(f.label)}</td>
          <td><span class="tag ${f.kind}">${kindLabel[f.kind]}</span></td><td class="mono">${esc(f.format)}</td>
          <td class="mono">${esc(addrText(f.flows.P))}${f.flows.P.sourceIp ? `<span class="sub">Quelle ${esc(f.flows.P.sourceIp)}</span>` : ''}</td>
          <td class="mono">${esc(addrText(f.flows.R))}</td><td class="num">${f.payloadType}</td>
          <td class="num">${rp ? mbit(rp.bps) : '–'}${f.flows.R ? ' / ' + (rr ? mbit(rr.bps) : '–') : ''}</td>
          <td class="num">${rp ? fmt(rp.pps, 0) : '–'}</td><td class="num">${f.lost === null ? '–' : fmt(f.lost, 0)}</td>
          <td>${v('P')}${f.flows.R ? ' / ' + v('R') : ''}</td>
          <td class="actions"><button class="btn small" data-action="select" data-id="${esc(f.id)}">Anzeigen</button>
              <button class="btn small danger" data-action="remove" data-id="${esc(f.id)}">Entfernen</button></td></tr>`;
      }).join('') : '<tr><td colspan="12" class="empty">Noch kein Flow überwacht – aus der Erkennung übernehmen oder „Flow hinzufügen“.</td></tr>');

    // Erkennung
    const st = d.discovery || {};
    let info = st.interface ? `Schnittstelle ${st.interface}` : 'aus (--discover <schnittstelle>)';
    if (st.frames !== undefined) info += ` · ${fmt(st.frames, 0)} Rahmen`;
    if (st.drops) info += ` · ${fmt(st.drops, 0)} beim Mitlesen verworfen (Sequenzfehler können daher stammen)`;
    if (st.error) info += ` · ${st.error}`;
    $('discoveryInfo').textContent = info;
    const k = state.seg.discKind;
    const list = d.discovered.filter(f => k === 'all' || (k === 'other' ? !['video', 'audio'].includes(f.kind) : f.kind === k));
    $('discoveredTable').innerHTML = `<tr><th></th><th>Ziel</th><th>Quelle</th><th>Art</th><th>Format (geschätzt)</th><th>PT</th>
      <th>SSRC</th><th>Mbit/s</th><th>Pakete/s</th><th>Seq.-Fehler</th><th>DSCP</th><th>VLAN</th><th></th></tr>` +
      (list.length ? list.map((f, i) => {
        const can = (f.kind === 'video' || f.kind === 'audio') && !f.monitored;
        const status = f.monitored ? `<span class="tag on">überwacht</span>` :
          `<button class="btn small" data-action="monitor" data-index="${d.discovered.indexOf(f)}" ${can ? '' : 'disabled title="Art wird nicht unterstützt"'}>Überwachen</button>`;
        return `<tr class="${f.active ? '' : 'inactive'}"><td><span class="dot ${f.active ? 'ok' : ''}"></span></td>
          <td class="mono">${esc(f.dstIp)}:${f.dstPort}</td><td class="mono">${esc(f.srcIp)}:${f.srcPort}</td>
          <td><span class="tag ${f.kind}">${kindLabel[f.kind] || f.kind}</span></td><td class="mono">${esc(f.format || '–')}</td>
          <td class="num">${f.payloadType ?? '–'}</td><td class="mono">${f.ssrc !== null ? '0x' + f.ssrc.toString(16).padStart(8, '0') : '–'}</td>
          <td class="num">${mbit(f.bps)}</td><td class="num">${fmt(f.pps, 0)}</td>
          <td class="num">${fmt(f.sequenceErrors, 0)}</td><td class="num">${f.dscp}</td><td class="num">${f.vlan ?? '–'}</td>
          <td class="actions">${status}${f.monitored && f.monitored !== state.flowId ? ` <button class="btn small" data-action="select" data-id="${esc(f.monitored)}">Anzeigen</button>` : ''}</td></tr>`;
      }).join('') : `<tr><td colspan="13" class="empty">${st.interface ? 'noch keine Pakete' : 'Erkennung nicht aktiv'}</td></tr>`);

    // SAP
    const a = d.announced;
    $('announcedTable').innerHTML = `<tr><th>Name</th><th>Quelle</th><th>Medien</th><th>zuletzt</th><th></th></tr>` +
      (a.length ? a.map(e => {
        const media = e.media.map(x => `<span class="tag ${x.kind}">${kindLabel[x.kind]}</span> ${esc(x.format)} · ${esc(x.address)}${x.redundant ? ' (+R)' : ''}${x.monitored ? ' <span class="tag on">überwacht</span>' : ''}`).join('<br>');
        const all = e.media.length && e.media.every(x => x.monitored);
        return `<tr><td>${esc(e.name || '–')}</td><td class="mono">${esc(e.origin)}</td><td>${media || esc(e.errors.join('; '))}</td>
          <td class="num">vor ${fmt(e.lastSeenSec, 0)} s</td>
          <td><button class="btn small" data-action="sap" data-key="${esc(e.key)}" ${all || !e.media.length ? 'disabled' : ''}>Überwachen</button></td></tr>`;
      }).join('') : '<tr><td colspan="5" class="empty">keine Ankündigungen</td></tr>');
  }

  // ---------- Testsender ----------
  const txStateLabel = { stopped: 'gestoppt', starting: 'startet …', sending: 'sendet', stopping: 'stoppt …', error: 'Fehler' };
  const ptimePerSecond = { '125us': 8000, '250us': 4000, '333us': 3000, '1ms': 1000, '4ms': 250 };

  async function pollTx() {
    try {
      const res = await fetch('api/tx', { cache: 'no-store' });
      if (!res.ok) return;
      state.tx = await res.json();
      if (state.tab === 'txgen') renderTx();
    } catch (_) { /* Abfrage des Snapshots meldet Verbindungsfehler */ }
  }

  // Beschreibung wie bei --tx-video / --tx-audio
  function txSpec(f) {
    const spec = [`${f.ip}:${f.udpPort}`];
    if (f.kind === 'video') {
      spec.push(`format=${f.video.width}x${f.video.height}${f.video.interlaced ? 'i' : 'p'}${f.video.rate}`);
    } else {
      spec.push(`format=L${f.audio.bitDepth}/${f.audio.sampleRate}/${f.audio.channels}`, `ptime=${f.audio.ptime}`);
    }
    spec.push(`pt=${f.payloadType}`);
    if (f.label) spec.push('name=' + f.label.replace(/,/g, ' '));
    return spec.join(',');
  }

  // Nutzdatenrate in Gbit/s (4:2:2, 10 Bit = 20 Bit je Bildpunkt; bei Zeilensprung halbe Höhe je Halbbild)
  function txVideoGbit(width, height, rate, interlaced) {
    return width * (interlaced ? height / 2 : height) * 20 * rate / 1e9;
  }

  function txFormValues() {
    const f = $('txForm').elements;
    const kind = f.kind.value;
    const out = { kind, address: f.address.value.trim(), pt: f.pt.value, name: f.name.value.trim().replace(/,/g, ' ') };
    if (kind === 'video') {
      out.resolution = f.resolution.value.trim();
      out.rate = f.rate.value;
      out.interlaced = f.interlaced.checked;
    } else {
      out.sampleRate = +f.sampleRate.value;
      out.bitDepth = +f.bitDepth.value;
      out.channels = +f.channels.value;
      out.ptime = f.ptime.value;
    }
    return out;
  }

  function txFormSpec() {
    const v = txFormValues();
    const spec = [v.address];
    if (v.kind === 'video') spec.push(`format=${v.resolution}${v.interlaced ? 'i' : 'p'}${v.rate}`);
    else spec.push(`format=L${v.bitDepth}/${v.sampleRate}/${v.channels}`, `ptime=${v.ptime}`);
    if (v.pt !== '') spec.push('pt=' + v.pt);
    if (v.name) spec.push('name=' + v.name);
    return { kind: v.kind, spec: spec.join(',') };
  }

  function updateTxForm() {
    const form = $('txForm');
    const kind = form.elements.kind.value;
    form.querySelectorAll('[data-kind]').forEach(el => { el.hidden = el.dataset.kind !== kind; });
    // Hinweis zur Last bzw. Paketgröße
    const v = txFormValues();
    const est = $('txEstimate');
    est.className = 'hint';
    if (kind === 'video') {
      const m = /^(\d+)x(\d+)$/.exec(v.resolution);
      if (!m) { est.textContent = 'Auflösung als Breite x Höhe, z. B. 1920x1080.'; return; }
      const gbit = txVideoGbit(+m[1], +m[2], parseFloat(v.rate), v.interlaced);
      est.textContent = `Nutzdaten ca. ${fmt(gbit, 2)} Gbit/s (auf der Leitung rund 5 % mehr).`;
      if (gbit * 1.05 > 9.5) { est.className = 'hint warn'; est.textContent += ' Mehr, als ein 10G-Link trägt.'; }
    } else {
      const perSecond = ptimePerSecond[v.ptime];
      const samples = Math.round(v.sampleRate / perSecond);
      const bytes = samples * v.channels * (v.bitDepth / 8);
      const maxChannels = Math.min(64, Math.floor(1440 / (samples * (v.bitDepth / 8))));
      est.textContent = `${samples} Abtastwerte je Kanal und Paket, Nutzlast ${bytes} Byte (höchstens 1440); mit dieser Paketzeit höchstens ${maxChannels} Kanäle.`;
      if (bytes > 1440) est.className = 'hint warn';
    }
  }

  // Standardziel für einen neuen Eintrag: erste freie Adresse in 239.211.0.x (Video) bzw. 239.211.1.x (Ton)
  function nextTxAddress(kind) {
    const used = new Set(((state.tx && state.tx.flows) || []).map(f => `${f.ip}:${f.udpPort}`));
    for (let i = 1; i < 250; ++i) {
      const a = kind === 'video' ? `239.211.0.${i}:20000` : `239.211.1.${i}:30000`;
      if (!used.has(a)) return a;
    }
    return '';
  }

  function txMessage(text, error) {
    const msg = $('txMsg');
    msg.hidden = !text;
    msg.className = 'msg' + (error ? ' error' : '');
    msg.textContent = text || '';
  }

  function txResetForm() {
    state.txEditing = null;
    const f = $('txForm').elements;
    f.kind.disabled = false;
    f.address.value = nextTxAddress(f.kind.value);
    f.name.value = '';
    f.pt.value = f.kind.value === 'video' ? 96 : 97;
    $('txFormTitle').textContent = 'Testsender anlegen';
    $('txSubmit').textContent = 'Anlegen und senden';
    $('txSave').hidden = false;
    $('txCancel').hidden = true;
    updateTxForm();
  }

  function txEdit(id) {
    const f = ((state.tx && state.tx.flows) || []).find(x => x.id === id);
    if (!f) return;
    state.txEditing = id;
    const e = $('txForm').elements;
    e.kind.value = f.kind;
    e.kind.disabled = true;
    e.address.value = `${f.ip}:${f.udpPort}`;
    e.name.value = f.label;
    e.pt.value = f.payloadType;
    if (f.kind === 'video') {
      e.resolution.value = `${f.video.width}x${f.video.height}`;
      e.rate.value = f.video.rate;
      e.interlaced.checked = f.video.interlaced;
    } else {
      e.sampleRate.value = f.audio.sampleRate;
      e.bitDepth.value = f.audio.bitDepth;
      e.channels.value = f.audio.channels;
      e.ptime.value = f.audio.ptime;
    }
    $('txFormTitle').textContent = `Einstellungen von ${f.label} ändern`;
    $('txSubmit').textContent = 'Übernehmen';
    $('txSave').hidden = true;
    $('txCancel').hidden = false;
    updateTxForm();
    txMessage('');
    $('txFormCard').scrollIntoView({ behavior: 'smooth', block: 'nearest' });
  }

  async function txRequest(method, path, body) {
    try {
      const res = await fetch(path, { method, headers: { 'Content-Type': 'application/json' }, body: body ? JSON.stringify(body) : undefined });
      const r = await res.json();
      if (r.flows) { state.tx = r; renderTx(); }
      if (!res.ok || r.ok === false) { txMessage(r.error || `Fehler ${res.status}`, true); return null; }
      return r;
    } catch (err) {
      txMessage('Fehler: ' + err.message, true);
      return null;
    }
  }

  function renderTx() {
    const d = state.tx;
    if (!d) return;
    $('txUnavailable').hidden = d.available !== false ? true : false;
    $('txFormCard').hidden = d.available === false;
    if (d.available === false) { $('txTable').innerHTML = ''; $('txInfo').textContent = ''; return; }
    $('txInfo').textContent = `Port ${d.port}${d.localIp ? ' · ' + d.localIp : ''} · ${d.flows.length} von höchstens ${d.maxFlows}`;
    $('txTable').innerHTML = `<tr><th>Zustand</th><th>Name</th><th>Art</th><th>Format</th><th>Ziel</th><th>PT</th><th>Nutzdaten</th><th></th></tr>` +
      (d.flows.length ? d.flows.map(f => {
        const busy = f.state === 'starting' || f.state === 'stopping';
        const running = f.state === 'sending' || f.state === 'starting';
        const rate = f.kind === 'video'
          ? fmt(txVideoGbit(f.video.width, f.video.height, parseFloat(f.video.rate), f.video.interlaced) * 1000, 0) + ' Mbit/s'
          : fmt(f.audio.sampleRate * f.audio.channels * f.audio.bitDepth / 1e6, 2) + ' Mbit/s';
        return `<tr><td><span class="txstate ${f.state}">${txStateLabel[f.state] || f.state}</span>${f.state === 'starting' ? '<span class="sub">wartet ggf. auf PTP</span>' : ''}</td>
          <td>${esc(f.label)}</td><td><span class="tag ${f.kind}">${kindLabel[f.kind]}</span></td><td class="mono">${esc(f.format)}</td>
          <td class="mono">${esc(f.ip)}:${f.udpPort}</td><td class="num">${f.payloadType}</td><td class="num">${rate}</td>
          <td class="actions">
            ${running ? `<button class="btn small" data-tx="stop" data-id="${esc(f.id)}" ${f.state === 'stopping' ? 'disabled' : ''}>Stoppen</button>`
                      : `<button class="btn small primary" data-tx="start" data-id="${esc(f.id)}" ${busy ? 'disabled' : ''}>Starten</button>`}
            <button class="btn small" data-tx="edit" data-id="${esc(f.id)}" ${f.state === 'stopped' || f.state === 'error' ? '' : 'disabled title="Nur im gestoppten Zustand"'}>Ändern</button>
            <button class="btn small" data-tx="monitor" data-id="${esc(f.id)}" title="Den Strom über den Empfangsport überwachen">Überwachen</button>
            <button class="btn small danger" data-tx="remove" data-id="${esc(f.id)}" ${busy ? 'disabled' : ''}>Löschen</button></td></tr>
          ${f.message ? `<tr><td></td><td colspan="7" class="wrap">${esc(f.message)}</td></tr>` : ''}`;
      }).join('') : '<tr><td colspan="8" class="empty">Noch kein Testsender – unten einen anlegen.</td></tr>');
    if (state.txEditing === null && !$('txForm').elements.address.dataset.touched) {
      $('txForm').elements.address.value = nextTxAddress($('txForm').elements.kind.value);
    }
  }

  $('txTable').addEventListener('click', async (e) => {
    const btn = e.target.closest('button[data-tx]');
    if (!btn || btn.disabled) return;
    const id = btn.dataset.id;
    txMessage('');
    switch (btn.dataset.tx) {
      case 'start': await txRequest('POST', `api/tx/${id}/start`); break;
      case 'stop': await txRequest('POST', `api/tx/${id}/stop`); break;
      case 'remove':
        if (state.txEditing === id) txResetForm();
        await txRequest('DELETE', `api/tx/${id}`);
        break;
      case 'edit': txEdit(id); break;
      case 'monitor': {
        const f = (state.tx.flows || []).find(x => x.id === id);
        if (!f) break;
        $('addFlowMsg').hidden = true;
        if (await postFlow({ kind: f.kind, spec: txSpec(f) })) showTab('overview');
        else txMessage(($('addFlowMsg').textContent || 'Flow lässt sich nicht überwachen.'), true);
        break;
      }
    }
  });

  async function txSubmit(start) {
    const body = txFormSpec();
    let r;
    if (state.txEditing) {
      r = await txRequest('PUT', `api/tx/${state.txEditing}`, { spec: body.spec });
      if (r) { txMessage('Einstellungen übernommen.'); txResetForm(); }
    } else {
      r = await txRequest('POST', 'api/tx', { ...body, start });
      if (r) {
        txMessage(start ? `${r.id} wird gestartet.` : `${r.id} angelegt.`);
        const address = $('txForm').elements.address;
        delete address.dataset.touched;
        txResetForm();
      }
    }
  }
  $('txForm').addEventListener('submit', (e) => { e.preventDefault(); txSubmit(true); });
  $('txSave').addEventListener('click', () => txSubmit(false));
  $('txCancel').addEventListener('click', () => { txResetForm(); txMessage(''); });
  $('txForm').addEventListener('input', (e) => {
    if (e.target.name === 'address') e.target.dataset.touched = '1';
    updateTxForm();
  });
  $('txForm').elements.kind.addEventListener('change', (e) => {
    const f = $('txForm').elements;
    delete f.address.dataset.touched;
    f.address.value = nextTxAddress(e.target.value);
    f.pt.value = e.target.value === 'video' ? 96 : 97;
    updateTxForm();
  });
  updateTxForm();

  async function postFlow(body) {
    const msg = $('addFlowMsg');
    try {
      const res = await fetch('api/flows', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
      const r = await res.json();
      msg.hidden = false;
      msg.className = 'msg' + (res.ok ? '' : ' error');
      const parts = [];
      if (r.ids && r.ids.length) parts.push(`Überwacht: ${r.ids.join(', ')}`);
      if (r.errors && r.errors.length) parts.push(...r.errors);
      if (r.error) parts.push(r.error);
      msg.textContent = parts.join('\n');
      if (r.ids && r.ids.length) selectFlow(r.ids[0]);
      pollFlows();
      return res.ok;
    } catch (err) {
      msg.hidden = false;
      msg.className = 'msg error';
      msg.textContent = 'Fehler: ' + err.message;
      return false;
    }
  }

  function openForm(prefill) {
    const form = $('addFlowForm');
    $('addFlowCard').hidden = false;
    $('addFlowMsg').hidden = true;
    if (prefill) {
      for (const [k, v] of Object.entries(prefill)) if (form.elements[k] && v !== undefined && v !== null) form.elements[k].value = v;
    }
    updateFormKind();
    $('addFlowCard').scrollIntoView({ behavior: 'smooth', block: 'nearest' });
  }

  function updateFormKind() {
    const form = $('addFlowForm');
    const kind = form.elements.kind.value;
    form.querySelectorAll('[data-kind]').forEach(el => { el.hidden = el.dataset.kind !== kind; });
  }

  $('addFlowBtn').addEventListener('click', () => openForm());
  $('addFlowClose').addEventListener('click', () => { $('addFlowCard').hidden = true; });
  $('addFlowForm').elements.kind.addEventListener('change', (e) => {
    $('addFlowForm').elements.pt.value = e.target.value === 'video' ? 96 : 97;
    updateFormKind();
  });
  $('addFlowForm').addEventListener('submit', (e) => {
    e.preventDefault();
    const f = e.target.elements;
    const kind = f.kind.value;
    const spec = [f.address.value.trim()];
    if (f.red.value.trim()) spec.push('red=' + f.red.value.trim());
    if (f.source.value.trim()) spec.push('source=' + f.source.value.trim());
    if (f.redSource.value.trim()) spec.push('red-source=' + f.redSource.value.trim());
    if (kind === 'video') spec.push('format=' + f.videoFormat.value.trim());
    else { spec.push('format=' + f.audioFormat.value.trim()); spec.push('ptime=' + f.ptime.value); }
    if (f.pt.value) spec.push('pt=' + f.pt.value);
    if (f.name.value.trim()) spec.push('name=' + f.name.value.trim().replace(/,/g, ' '));
    postFlow({ kind, spec: spec.join(',') });
  });
  $('sdpSubmit').addEventListener('click', () => {
    const sdp = $('sdpText').value.trim();
    if (sdp) postFlow({ sdp });
  });

  // Aktionen in den Tabellen
  document.querySelector('[data-view="flows"]').addEventListener('click', async (e) => {
    const btn = e.target.closest('button[data-action]');
    if (!btn || btn.disabled) return;
    const action = btn.dataset.action;
    if (action === 'select') {
      selectFlow(btn.dataset.id);
      showTab('overview');
    } else if (action === 'remove') {
      if (!confirm('Flow ' + btn.dataset.id + ' nicht mehr überwachen?')) return;
      await fetch('api/flows/' + encodeURIComponent(btn.dataset.id), { method: 'DELETE' });
      if (state.flowId === btn.dataset.id) state.flowId = '';
      pollFlows();
      poll(true);
    } else if (action === 'monitor') {
      const f = state.flowsData.discovered[+btn.dataset.index];
      if (!f) return;
      const prefill = { kind: f.kind, address: `${f.dstIp}:${f.dstPort}`, source: f.multicast ? f.srcIp : '',
                        name: `${f.kind === 'video' ? 'Video' : 'Ton'} ${f.dstIp}`, pt: f.payloadType, red: '', redSource: '' };
      if (f.kind === 'video' && f.format) prefill.videoFormat = f.format;
      if (f.kind === 'audio' && f.format) {
        const [enc, pt] = f.format.split(' ');
        prefill.audioFormat = enc;
        prefill.ptime = { '125us': '0.125', '250us': '0.25', '333us': '0.333', '1ms': '1', '4ms': '4' }[pt] || '1';
      }
      openForm(prefill);
    } else if (action === 'sap') {
      postFlow({ sap: btn.dataset.key });
    }
  });

  // ---------- Vorschau ----------
  const preview = {
    timer: null, audioTimer: null, flowKey: '', ctx: null, gain: null, listening: false,
    nextTime: 0, from: 0, holds: [], holdUntil: [], lastSeq: 0, videoRun: 0, shown: [], bytes: [],
  };

  function renderPreviewCards(s) {
    const kind = s.flow ? s.flow.kind : null;
    $('videoPreviewCard').hidden = kind !== 'video';
    $('audioPreviewCard').hidden = kind !== 'audio';
    $('noPreview').hidden = !!kind;
    const key = `${state.tab}|${state.flowId}|${kind}`;
    if (key !== preview.flowKey) {
      preview.flowKey = key;
      restartPreview(kind, s);
    }
  }

  function restartPreview(kind, s) {
    clearInterval(preview.audioTimer);
    preview.audioTimer = null;
    preview.videoRun++; // laufende Bildschleife beendet sich
    stopListening();
    preview.from = 0;
    preview.lastSeq = 0;
    if (state.tab !== 'preview') return;
    if (kind === 'video') {
      $('videoPreviewInfo').textContent = `${s.flow.label} · ${s.video.format}`;
      preview.shown = [];
      preview.bytes = [];
      videoLoop(preview.videoRun);
    } else if (kind === 'audio') {
      $('audioPreviewInfo').textContent = `${s.flow.label} · ${s.audio.format}`;
      buildMeters(s.audio.format);
      fetchAudio();
      preview.audioTimer = setInterval(fetchAudio, 150);
    }
  }

  // Die Ansicht verlassen beendet die Vorschau (das Messgerät hört 3 s nach der letzten Abfrage auf).
  function leavePreview() {
    preview.flowKey = '';
    restartPreview(null);
  }

  for (const id of ['previewFps', 'previewWidth', 'previewQuality']) {
    $(id).addEventListener('change', () => { preview.shown = []; preview.bytes = []; });
  }
  $('previewFullscreen').addEventListener('click', () => {
    const el = $('previewFrame');
    if (document.fullscreenElement) document.exitFullscreen(); else if (el.requestFullscreen) el.requestFullscreen();
  });

  // Bildschleife mit Gegendruck: das nächste Bild wird erst abgerufen, wenn das vorige gezeichnet ist.
  async function videoLoop(run) {
    // Das Messgerät antwortet erst mit einem neueren Bild (Long Polling) und gibt so den Takt vor.
    while (run === preview.videoRun) {
      const ok = await fetchFrame();
      if (!ok) await new Promise(r => setTimeout(r, 200));
    }
  }

  async function fetchFrame() {
    const id = state.flowId;
    const overlay = $('previewOverlay');
    try {
      const q = `flow=${encodeURIComponent(id)}&width=${$('previewWidth').value}&fps=${$('previewFps').value}` +
                `&quality=${$('previewQuality').value}&after=${preview.lastSeq}`;
      const res = await fetch(`api/preview/video?${q}`, { cache: 'no-store' });
      if (id !== state.flowId) return true;
      if (res.status === 204) { overlay.textContent = 'warte auf erstes Bild …'; return false; }
      if (!res.ok) { overlay.textContent = 'keine Vorschau'; return false; }
      const w = +res.headers.get('X-Preview-Width');
      const h = +res.headers.get('X-Preview-Height');
      const seq = +res.headers.get('X-Preview-Sequence');
      const age = +res.headers.get('X-Preview-Age-Ms');
      const blob = await res.blob();
      if (seq === preview.lastSeq) { // in 250 ms kein neues Bild
        overlay.className = 'preview-overlay' + (age > 1500 ? ' stale' : '');
        return true;
      }
      const bitmap = await createImageBitmap(blob);
      const canvas = $('previewCanvas');
      const dpr = window.devicePixelRatio || 1;
      const cw = Math.round(canvas.clientWidth * dpr), ch = Math.round(canvas.clientHeight * dpr);
      if (canvas.width !== cw || canvas.height !== ch) { canvas.width = cw; canvas.height = ch; }
      const c = canvas.getContext('2d');
      c.fillStyle = '#000';
      c.fillRect(0, 0, canvas.width, canvas.height);
      const scale = Math.min(canvas.width / w, canvas.height / h);
      const dw = w * scale, dh = h * scale;
      c.imageSmoothingEnabled = true;
      c.drawImage(bitmap, (canvas.width - dw) / 2, (canvas.height - dh) / 2, dw, dh);
      bitmap.close();
      preview.lastSeq = seq;
      // gemessene Bildrate und Datenrate über die letzten 2 s
      const now = performance.now();
      preview.shown.push(now);
      preview.bytes.push(blob.size);
      while (preview.shown.length > 1 && now - preview.shown[0] > 2000) { preview.shown.shift(); preview.bytes.shift(); }
      const span = (now - preview.shown[0]) / 1000;
      const fps = span > 0 ? (preview.shown.length - 1) / span : 0;
      const kb = preview.bytes.reduce((a, b) => a + b, 0) / preview.bytes.length / 1024;
      const stale = age > 1500;
      overlay.className = 'preview-overlay' + (stale ? ' stale' : '');
      overlay.textContent = `${state.snap && state.snap.flow ? state.snap.flow.label : ''} · ${w}×${h} · ${fmt(fps, 1)} Bilder/s · ` +
        `${fmt(kb, 0)} kB/Bild · ${fmt(kb * 8 * fps / 1024, 1)} Mbit/s · Alter ${fmt(age, 0)} ms${stale ? ' · kein neues Bild' : ''}`;
      return true;
    } catch (_) { return false; /* nächster Versuch */ }
  }

  const dbToPct = (db) => Math.max(0, Math.min(100, (db + 60) / 60 * 100));

  function buildMeters(format) {
    const channels = +((format.match(/\/(\d+) /) || [])[1] || 2);
    const sel = $('pairSelect');
    const pairs = Math.ceil(channels / 2);
    const keep = sel.value;
    sel.innerHTML = Array.from({ length: pairs }, (_, i) =>
      `<option value="${i}">${channels === 1 ? '1' : `${i * 2 + 1}/${Math.min(i * 2 + 2, channels)}`}</option>`).join('');
    if (keep && +keep < pairs) sel.value = keep;
    preview.holds = new Array(channels).fill(-120);
    preview.holdUntil = new Array(channels).fill(0);
    const scale = [0, -9, -18, -30, -40, -60].map(db =>
      `<span style="bottom:${dbToPct(db)}%">${db === 0 ? '0' : '−' + Math.abs(db)}</span>`).join('');
    $('meters').innerHTML = `<div class="meter scale"><span class="val">dBFS</span><div class="scalebar">${scale}</div><span class="ch">&nbsp;</span></div>` +
      Array.from({ length: channels }, (_, i) =>
      `<div class="meter" data-ch="${i}"><span class="val">–</span><div class="barwrap"><div class="bar"></div>` +
      `<div class="fill-wrap"><div class="fill" style="height:0%"></div><div class="hold" style="bottom:0%"></div></div></div>` +
      `<span class="ch">${i + 1}</span></div>`).join('');
    markSelectedPair();
  }

  function markSelectedPair() {
    const pair = +$('pairSelect').value;
    document.querySelectorAll('#meters .meter[data-ch]').forEach(m => {
      const ch = +m.dataset.ch;
      m.classList.toggle('selected', ch === pair * 2 || ch === pair * 2 + 1);
    });
  }

  function updateMeters(peaks) {
    const now = performance.now();
    document.querySelectorAll('#meters .meter[data-ch]').forEach(m => {
      const ch = +m.dataset.ch;
      const db = peaks[ch] ?? -120;
      if (db >= preview.holds[ch] || now > preview.holdUntil[ch]) {
        preview.holds[ch] = db;
        preview.holdUntil[ch] = now + 2000;
      }
      m.querySelector('.fill').style.height = dbToPct(db) + '%';
      m.querySelector('.hold').style.bottom = dbToPct(preview.holds[ch]) + '%';
      m.querySelector('.val').textContent = preview.holds[ch] <= -100 ? '−∞' : fmt(preview.holds[ch], 1);
    });
  }

  async function fetchAudio() {
    const id = state.flowId;
    const pair = +$('pairSelect').value || 0;
    try {
      const res = await fetch(`api/preview/audio?flow=${encodeURIComponent(id)}&from=${preview.from}&pair=${pair}`, { cache: 'no-store' });
      if (id !== state.flowId || !res.ok) return;
      const start = +res.headers.get('X-Audio-Start');
      const frames = +res.headers.get('X-Audio-Frames');
      const rate = +res.headers.get('X-Audio-Rate');
      const peaks = (res.headers.get('X-Audio-Peaks') || '').split(',').map(Number);
      const data = new Int16Array(await res.arrayBuffer());
      updateMeters(peaks);
      if (frames > 0) preview.from = start + frames;
      if (preview.listening && frames > 0) playChunk(data, frames, rate);
    } catch (_) { /* nächster Versuch */ }
  }

  function playChunk(data, frames, rate) {
    const ctx = preview.ctx;
    const buffer = ctx.createBuffer(2, frames, rate);
    const l = buffer.getChannelData(0), r = buffer.getChannelData(1);
    for (let i = 0; i < frames; i++) { l[i] = data[i * 2] / 32768; r[i] = data[i * 2 + 1] / 32768; }
    const src = ctx.createBufferSource();
    src.buffer = buffer;
    src.connect(preview.gain);
    // Abspielzeitpunkt fortschreiben; bei Lücken oder Rückstand neu mit 0,3 s Vorlauf beginnen.
    if (preview.nextTime < ctx.currentTime + 0.05 || preview.nextTime > ctx.currentTime + 1.0) {
      preview.nextTime = ctx.currentTime + 0.3;
    }
    src.start(preview.nextTime);
    preview.nextTime += frames / rate;
  }

  function startListening() {
    if (!preview.ctx) {
      preview.ctx = new (window.AudioContext || window.webkitAudioContext)();
      preview.gain = preview.ctx.createGain();
      preview.gain.connect(preview.ctx.destination);
    }
    preview.ctx.resume();
    preview.gain.gain.value = +$('volume').value;
    preview.listening = true;
    preview.nextTime = 0;
    preview.from = 0; // mit aktuellem Ton beginnen
    $('listenBtn').textContent = 'Stumm';
    $('listenBtn').classList.add('on');
  }

  function stopListening() {
    preview.listening = false;
    if (preview.ctx) preview.ctx.suspend();
    $('listenBtn').textContent = 'Anhören';
    $('listenBtn').classList.remove('on');
  }

  $('listenBtn').addEventListener('click', () => { if (preview.listening) stopListening(); else startListening(); });
  $('volume').addEventListener('input', (e) => { if (preview.gain) preview.gain.gain.value = +e.target.value; });
  $('pairSelect').addEventListener('change', () => { markSelectedPair(); preview.from = 0; preview.nextTime = 0; });

  init();
})();
