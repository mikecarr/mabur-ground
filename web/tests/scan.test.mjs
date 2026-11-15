import test from 'node:test';
import assert from 'node:assert/strict';
import {
  parseScanLine, ScanState, loadBaseline, saveBaseline, gsFragment, droneLines, applyAdvice,
  scanRows, pairRows, confirmText, adviceFor, showScanPanel, scanMessage, canUseAdvice,
} from '../ui/src/lib/scan.js';
import { defaultConfig, pageWidth } from '../ui/src/lib/config.js';

const caps = { blocked_pct: 30, chip: 'RTL8812AU', gen: 'jaguar1', plan: [36, 40], rounds: 3, source: 'rank' };
const advice = { width: 40, set: [64, 112, 128, 136], flagged: false, reason: '5 clean pairs → 40 MHz has room' };

test('parseScanLine reads every kind', () => {
  assert.deepEqual(parseScanLine('SCAN caps ' + JSON.stringify(caps)), { kind: 'caps', data: caps });
  assert.deepEqual(parseScanLine('SCAN dwell {"ch":36,"round":0}'), { kind: 'dwell', data: { ch: 36, round: 0 } });
  assert.deepEqual(parseScanLine('SCAN result {"advice":{}}'), { kind: 'result', data: { advice: {} } });
  assert.deepEqual(parseScanLine('SCAN done'), { kind: 'done', data: null });
  assert.deepEqual(parseScanLine('SCAN error no device'), { kind: 'error', data: 'no device' });
});

test('parseScanLine ignores other and malformed lines', () => {
  assert.equal(parseScanLine('CHANNEL 40'), null);
  assert.equal(parseScanLine('SCAN dwell {not json'), null);
  assert.equal(parseScanLine('SCAN bogus {}'), null);
  assert.equal(parseScanLine(''), null);
});

test('ScanState tracks round, dwells and the last result', () => {
  const s = new ScanState();
  s.apply('caps', caps);
  s.apply('dwell', { ch: 36, round: 0, busy: null, rate: 9 });
  s.apply('dwell', { ch: 40, round: 1, busy: null, rate: 3 });
  s.apply('result', { chans: [{ ch: 36 }], pairs: [], advice, advice_no_dfs: advice, rates: { 36: 9 } });
  s.apply('result', { chans: [{ ch: 36 }, { ch: 40 }], pairs: [], advice, advice_no_dfs: advice, rates: { 36: 9, 40: 3 } });
  const v = s.view;
  assert.equal(v.caps.chip, 'RTL8812AU');
  assert.equal(v.round, 1);
  assert.equal(v.dwells, 2);
  assert.equal(v.result.chans.length, 2);
  assert.equal(v.done, false);
  s.apply('done', null);
  assert.equal(s.view.done, true);
  s.apply('error', 'no device');
  assert.equal(s.view.error, 'no device');
});

test('gsFragment and droneLines', () => {
  assert.equal(gsFragment(advice), '?chs=64,112,128,136&w=40&ch=auto');
  assert.equal(droneLines(advice), 'channels = [64, 112, 128, 136]\nwidth = 40');
});

test('applyAdvice sets channels, width and Auto', () => {
  const cfg = { channels: [40], link: 40, width: 20, nack: true };
  const out = applyAdvice(cfg, advice);
  assert.deepEqual(out.channels, [64, 112, 128, 136]);
  assert.equal(out.width, 40);
  assert.equal(out.link, 'auto');
  assert.equal(out.nack, true);
  assert.deepEqual(cfg.channels, [40]);   // input untouched
});

test('baseline roundtrip keyed by chip', () => {
  const m = new Map();
  const st = { getItem: (k) => (m.has(k) ? m.get(k) : null), setItem: (k, v) => m.set(k, v) };
  assert.equal(loadBaseline(st, 'RTL8812AU'), null);
  saveBaseline(st, 'RTL8812AU', { 36: 9, 40: 3 });
  assert.ok(m.has('webgs.scanBaseline.RTL8812AU'));
  assert.deepEqual(loadBaseline(st, 'RTL8812AU'), { 36: 9, 40: 3 });
  assert.equal(loadBaseline(st, 'RTL8812EU'), null);
});

test('baseline survives a throwing or missing storage', () => {
  const bad = { getItem: () => { throw new Error('denied'); }, setItem: () => { throw new Error('denied'); } };
  assert.equal(loadBaseline(bad, 'x'), null);
  assert.doesNotThrow(() => saveBaseline(bad, 'x', { 36: 1 }));
  assert.equal(loadBaseline(null, 'x'), null);
  const junk = { getItem: () => '{oops', setItem: () => {} };
  assert.equal(loadBaseline(junk, 'x'), null);
});

const view = (source, chans, pairs = []) => ({ caps: { ...caps, source }, result: { chans, pairs, advice, advice_no_dfs: { ...advice, set: [36, 40, 149, 161], width: 20 } } });

test('scanRows: nhm percentages, set stars, DFS tag, no reading', () => {
  const rows = scanRows(view('nhm', [
    { ch: 36, reading: true, worst: 64.2, dfs: false },
    { ch: 64, reading: true, worst: 4, dfs: true },
    { ch: 144, reading: false, worst: 0, dfs: true },
  ]), [64, 112]);
  assert.deepEqual(rows.map((r) => r.label), ['64%', '4%', '—']);
  assert.deepEqual(rows.map((r) => r.pct), [64.2, 4, 0]);
  assert.deepEqual(rows.map((r) => r.star), [false, true, false]);
  assert.deepEqual(rows.map((r) => r.dfs), [false, true, true]);
  assert.equal(rows[2].noReading, true);
});

test('scanRows: estimate marks ~, rank-only ranks by rate', () => {
  assert.equal(scanRows(view('estimate', [{ ch: 36, reading: true, worst: 12 }]), [])[0].label, '~12%');
  const r = scanRows(view('rank', [
    { ch: 36, reading: true, worst: 900 }, { ch: 40, reading: true, worst: 100 }, { ch: 44, reading: true, worst: 450 },
  ]), []);
  assert.deepEqual(r.map((x) => x.label), ['#3', '#1', '#2']);
  assert.deepEqual(r.map((x) => x.pct), [100, 100 / 9, 50]);
});

test('pairRows texts and estimate suffix', () => {
  const pairs = [
    { lo: 100, hi: 104, verdict: 'ok40', clean: 0, spur: true },
    { lo: 157, hi: 161, verdict: 'only20', clean: 161, spur: true },
    { lo: 36, hi: 40, verdict: 'busy', clean: 0, spur: false },
    { lo: 140, hi: 144, verdict: 'rank', clean: 0, spur: false },
  ];
  assert.deepEqual(pairRows(view('nhm', [], pairs)).map((p) => p.text),
    ['40 MHz OK', '20 MHz on 161 only', 'busy', 'no verdict']);
  assert.equal(pairRows(view('nhm', [], pairs))[0].name, '100+104');
  assert.equal(pairRows(view('nhm', [], pairs))[0].spur, true);
  assert.equal(pairRows(view('estimate', [], pairs))[0].text, '40 MHz OK (estimate)');
});

test('adviceFor follows the Include DFS switch', () => {
  const v = view('nhm', []);
  assert.deepEqual(adviceFor(v, true).set, [64, 112, 128, 136]);
  assert.deepEqual(adviceFor(v, false).set, [36, 40, 149, 161]);
  assert.equal(adviceFor({ result: null }, true), null);
});

test('use this shows drone lines and reminder', () => {
  const t = confirmText(advice);
  assert.ok(t.includes('channels = [64, 112, 128, 136]\nwidth = 40'));
  assert.ok(t.includes('Set the same channels on the drone'));
});

test('applyAdvice sets the GS receiver width through the ladder', () => {
  const adv20 = { ...advice, width: 20, set: [36, 44, 149, 165] };
  const forty = applyAdvice(defaultConfig(), advice, 'gs');
  assert.equal(pageWidth(forty, 'gs'), 40);
  const withWide = { ...defaultConfig(), ladder: defaultConfig().ladder.map((r) => ({ ...r, bw: 40 })) };
  assert.equal(pageWidth(applyAdvice(withWide, adv20, 'gs'), 'gs'), 20);
  assert.equal(pageWidth(applyAdvice(defaultConfig(), advice, 'spotter'), 'spotter'), 40);
});

test('an empty recommendation is never applied', () => {
  const cfg = { ...defaultConfig(), channels: [40, 64] };
  const empty = { width: 20, set: [], flagged: true, reason: 'only 0 usable channels' };
  assert.equal(canUseAdvice(empty), false);
  assert.equal(canUseAdvice(advice), true);
  assert.equal(canUseAdvice(null), false);
  assert.deepEqual(applyAdvice(cfg, empty, 'gs').channels, [40, 64]);
});

test('scan panel yields to a live link', () => {
  const v = { result: null };
  assert.equal(showScanPanel(null, { mode: 'scan', state: 'live' }), false);
  assert.equal(showScanPanel(v, { mode: 'scan', state: 'live' }), true);
  assert.equal(showScanPanel(v, { mode: 'scan', state: 'idle' }), true);
  assert.equal(showScanPanel(v, { mode: 'gs', state: 'live' }), false);
  assert.equal(showScanPanel(v, { mode: 'gs', state: 'connecting' }), false);
});

test('scan panel shows session errors and notices', () => {
  assert.equal(scanMessage({ error: 'no device' }, { state: 'error', error: 'x' }), 'no device');
  assert.equal(scanMessage({ error: null }, { state: 'error', error: 'Card busy — close that' }), 'Card busy — close that');
  assert.equal(scanMessage({ error: null }, { state: 'idle', notice: 'No device selected' }), 'No device selected');
  assert.equal(scanMessage({ error: null }, { state: 'live', error: 'stale' }), null);
});
