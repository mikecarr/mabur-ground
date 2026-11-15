<script>
  import Icon from './Icon.svelte';
  import { scanRows, pairRows, adviceFor, gsFragment, droneLines, confirmText, canUseAdvice } from '../lib/scan.js';
  let { view, running = false, setChannels = [], mobile = false, message = null,
        onRescan = () => {}, onStop = () => {}, onClose = () => {}, onUse = () => {}, onSaveBaseline = () => {} } = $props();

  let includeDfs = $state(true);
  let confirm = $state(false);
  let copied = $state('');

  const SOURCE = {
    nhm: 'Busy % from the card\'s noise histogram.',
    estimate: 'Estimated from counters against your quiet baseline (this card has no busy %).',
    rank: 'Ranked by activity only. This card has no busy %: save a quiet baseline for estimates.',
  };
  const rows = $derived(scanRows(view, setChannels));
  const pairs = $derived(pairRows(view));
  const advice = $derived(adviceFor(view, includeDfs));
  const source = $derived(view?.caps?.source ?? 'nhm');
  const total = $derived((view?.caps?.plan?.length ?? 0) * (view?.caps?.rounds ?? 0));
  const counterCard = $derived(source !== 'nhm');

  async function copy(text, what) {
    try { await navigator.clipboard.writeText(text); copied = what; } catch { copied = 'copy failed'; }
    setTimeout(() => (copied = ''), 1500);
  }
</script>

<div class="scan" class:mobile>
  <div class="head">
    <span class="title"><Icon name="broadcast" />Channel scan</span>
    {#if view?.caps}<span class="dim5">{view.caps.chip} · {running ? `dwell ${view.dwells}/${total}` : view.done ? 'done' : 'stopped'}</span>{/if}
    <span class="grow"></span>
    {#if running}
      <button class="btn btn-ghost" type="button" onclick={onStop}>Stop</button>
    {:else}
      <button class="btn btn-secondary" type="button" onclick={onRescan}>Rescan</button>
      <button class="btn btn-ghost" type="button" onclick={onClose}>Close</button>
    {/if}
  </div>
  {#if message}<span class="msg err">{message}</span>{/if}
  {#if view?.caps}<span class="dim5 note">{SOURCE[source]}</span>{/if}

  <div class="bars">
    {#each rows as r (r.ch)}
      <div class="bar" class:none={r.noReading} title={r.noReading ? 'no reading' : ''}>
        <span class="ch">{r.ch}</span>
        <span class="track"><span class="fill" class:hot={source !== 'rank' && r.pct >= (view?.caps?.blocked_pct ?? 30)}
          style="width:{Math.min(100, r.pct)}%"></span></span>
        <span class="val">{r.label}</span>
        <span class="tags">{#if r.star}<span class="tag" title="in your channel set">★</span>{/if}{#if r.dfs}<span class="tag dim5">DFS</span>{/if}</span>
      </div>
    {:else}
      <span class="dim5">{running ? 'Measuring the first round…' : 'No results.'}</span>
    {/each}
  </div>

  {#if pairs.length}
    <div class="pairs">
      <span class="sub">40 MHz pairs</span>
      {#each pairs as p (p.name)}
        <div class="pair v-{p.verdict}"><span>{p.name}</span><span>{p.text}</span>
          {#if p.spur}<span class="dim5" title="slow retune on the drone's 8822E">slow retune</span>{/if}</div>
      {/each}
    </div>
  {/if}

  {#if canUseAdvice(advice)}
    <div class="advice">
      <span class="sub">Recommendation{counterCard ? ' (estimate)' : ''}</span>
      <span class="big">{advice.width} MHz · {advice.set.join(', ')}</span>
      <span class="dim5">{advice.reason}{advice.flagged ? ' — not all of these are clean' : ''}</span>
      <label class="dim5"><input type="checkbox" bind:checked={includeDfs} /> Include DFS channels</label>
      <div class="btns">
        <button class="btn btn-ghost" type="button" onclick={() => copy(gsFragment(advice), 'GS link copied')}>Copy GS link</button>
        <button class="btn btn-ghost" type="button" onclick={() => copy(droneLines(advice), 'drone lines copied')}>Copy drone lines</button>
        {#if !running}<button class="btn btn-primary" type="button" onclick={() => (confirm = true)}>Use this</button>{/if}
        {#if !running && counterCard}<button class="btn btn-ghost" type="button" onclick={onSaveBaseline}
          title="Scan somewhere quiet first: later scans with this card are estimated against it">Save as quiet baseline</button>{/if}
      </div>
      {#if copied}<span class="dim5">{copied}</span>{/if}
      {#if confirm}
        <div class="confirm">
          <pre>{confirmText(advice)}</pre>
          <div class="btns">
            <button class="btn btn-primary" type="button" onclick={() => { confirm = false; onUse(advice); }}>Apply to this GS</button>
            <button class="btn btn-ghost" type="button" onclick={() => (confirm = false)}>Cancel</button>
          </div>
        </div>
      {/if}
    </div>
  {/if}
</div>

<style>
  .scan { position: absolute; inset: 0; z-index: 3; overflow: auto; display: flex; flex-direction: column; gap: 10px;
          padding: 16px 24px; background: var(--color-bg); text-align: left; font-size: 13px; }
  .scan.mobile { padding: 12px 16px; }
  .head { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
  .title { display: inline-flex; align-items: center; gap: 6px; font-size: 16px; }
  .grow { flex: 1; }
  .note { font-size: 12px; }
  .msg.err { color: var(--color-accent-200); background: var(--color-accent-900); padding: 6px 10px; border-radius: var(--radius-md); }
  .bars { display: grid; gap: 3px; }
  .bar { display: grid; grid-template-columns: 3ch 1fr 5ch 7ch; gap: 8px; align-items: center; font-variant-numeric: tabular-nums; }
  .bar.none { opacity: 0.5; }
  .track { height: 10px; background: var(--color-neutral-800); border-radius: 3px; overflow: hidden; }
  .fill { display: block; height: 100%; background: var(--color-neutral-400); }
  .fill.hot { background: var(--color-accent-400); }
  .val { text-align: right; }
  .tags { display: inline-flex; gap: 4px; font-size: 11px; }
  .sub { font-size: 12px; text-transform: uppercase; letter-spacing: 0.04em; color: var(--color-neutral-400); }
  .pairs { display: grid; gap: 2px; }
  .pair { display: grid; grid-template-columns: 8ch 1fr auto; gap: 8px; }
  .pair.v-ok40 span:nth-child(2) { color: var(--color-neutral-100); }
  .pair.v-busy span:nth-child(2) { color: var(--color-accent-300); }
  .advice { display: flex; flex-direction: column; gap: 6px; padding: 10px; border-radius: var(--radius-md);
            background: var(--color-neutral-900); }
  .big { font-size: 18px; }
  .btns { display: flex; gap: 8px; flex-wrap: wrap; }
  .confirm pre { margin: 0; padding: 8px; white-space: pre-wrap; background: var(--color-neutral-800); border-radius: var(--radius-md); }
</style>
