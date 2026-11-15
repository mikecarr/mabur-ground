// Pre-flight channel scan: parse the core's `SCAN <kind> <json>` lines
// (web/src/scan_json.h), keep the scan's state, and turn its recommendation
// into the GS link fragment, the drone's config lines and a page config.

const KINDS = new Set(['caps', 'dwell', 'result']);

export function parseScanLine(text) {
  const t = String(text ?? '');
  if (t === 'SCAN done') return { kind: 'done', data: null };
  if (t.startsWith('SCAN error ')) return { kind: 'error', data: t.slice(11) };
  const m = /^SCAN (\w+) (.*)$/.exec(t);
  if (!m || !KINDS.has(m[1])) return null;
  try { return { kind: m[1], data: JSON.parse(m[2]) }; } catch { return null; }
}

export class ScanState {
  constructor() {
    this.caps = null;
    this.result = null;
    this.round = 0;
    this.dwells = 0;
    this.lastDwell = null;
    this.done = false;
    this.error = null;
  }

  apply(kind, data) {
    if (kind === 'caps') this.caps = data;
    else if (kind === 'dwell') { this.dwells++; this.round = data.round ?? this.round; this.lastDwell = data; }
    else if (kind === 'result') this.result = data;
    else if (kind === 'done') this.done = true;
    else if (kind === 'error') this.error = data;
  }

  get view() {
    return { caps: this.caps, result: this.result, round: this.round, dwells: this.dwells,
             lastDwell: this.lastDwell, done: this.done, error: this.error };
  }
}

const baselineKey = (chip) => 'webgs.scanBaseline.' + chip;

// Quiet-spot counter rates for a counter-only card, per chip. Storage can
// throw or be absent (private mode, blocked site data): no baseline then.
export function loadBaseline(storage, chip) {
  try {
    const t = storage && storage.getItem(baselineKey(chip));
    if (!t) return null;
    const v = JSON.parse(t);
    return v && typeof v === 'object' ? v : null;
  } catch { return null; }
}

export function saveBaseline(storage, chip, rates) {
  try { storage && storage.setItem(baselineKey(chip), JSON.stringify(rates)); } catch { /* page works without storage */ }
}

export function gsFragment(advice) {
  return `?chs=${advice.set.join(',')}&w=${advice.width}&ch=auto`;
}

export function droneLines(advice) {
  return `channels = [${advice.set.join(', ')}]\nwidth = ${advice.width}`;
}

export function canUseAdvice(advice) {
  return !!advice && Array.isArray(advice.set) && advice.set.length > 0;
}

// The page config for a recommendation. In GS mode with an adaptive ladder
// the receiver width is derived from the rungs (config.js pageWidth), so
// every rung flies the recommended width too. An empty set changes nothing.
export function applyAdvice(cfg, advice, mode = 'gs') {
  if (!canUseAdvice(advice)) return cfg;
  const out = { ...cfg, channels: [...advice.set], width: advice.width, link: 'auto' };
  if (mode === 'gs' && Array.isArray(cfg.ladder))
    out.ladder = cfg.ladder.map((r) => ({ ...r, bw: advice.width }));
  return out;
}

// The panel stays up while a scan runs and after it ends, but never over a
// link: a Connect pressed with the panel open wins the screen.
export function showScanPanel(scanView, sess) {
  if (!scanView) return false;
  if (sess.mode === 'scan') return true;
  return sess.state !== 'live' && sess.state !== 'connecting';
}

// What the panel says went wrong: the core's own SCAN error first, else the
// session's error (WebUSB, card busy, module start) or notice.
export function scanMessage(view, sess) {
  if (view?.error) return view.error;
  if (sess.state === 'error' && sess.error) return sess.error;
  if (sess.state !== 'live' && sess.notice) return sess.notice;
  return null;
}

// One row per scanned channel for the bar list. pct drives the bar width:
// busy % (nhm/estimate), or the counter rate relative to the busiest
// channel (rank: counter-only card without a baseline, so no % exists).
export function scanRows(view, setChannels = []) {
  const chans = view?.result?.chans ?? [];
  const source = view?.caps?.source ?? 'nhm';
  const read = chans.filter((c) => c.reading);
  const max = Math.max(0, ...read.map((c) => c.worst));
  const order = [...read].sort((a, b) => a.worst - b.worst).map((c) => c.ch);
  return chans.map((c) => {
    const base = { ch: c.ch, star: setChannels.includes(c.ch), dfs: !!c.dfs, noReading: !c.reading };
    if (!c.reading) return { ...base, pct: 0, label: '—' };
    if (source === 'rank')
      return { ...base, pct: max > 0 ? (100 * c.worst) / max : 0, label: '#' + (order.indexOf(c.ch) + 1) };
    return { ...base, pct: c.worst, label: (source === 'estimate' ? '~' : '') + Math.round(c.worst) + '%' };
  });
}

const PAIR_TEXT = { ok40: '40 MHz OK', busy: 'busy', rank: 'no verdict' };

export function pairRows(view) {
  const est = view?.caps?.source === 'estimate';
  return (view?.result?.pairs ?? []).map((p) => {
    let text = p.verdict === 'only20' ? `20 MHz on ${p.clean} only` : PAIR_TEXT[p.verdict] ?? 'no verdict';
    if (est && p.verdict !== 'rank') text += ' (estimate)';
    return { name: `${p.lo}+${p.hi}`, verdict: p.verdict, text, spur: !!p.spur };
  });
}

// The recommendation for the Include DFS switch (the core sends both).
export function adviceFor(view, includeDfs) {
  const r = view?.result;
  if (!r) return null;
  return (includeDfs ? r.advice : r.advice_no_dfs) ?? null;
}

export function confirmText(advice) {
  return `${droneLines(advice)}\n\nSet the same channels on the drone too ([radio] in /etc/mabur.toml), or the link may not form.`;
}
