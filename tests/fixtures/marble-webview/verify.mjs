import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {readFileSync} from 'node:fs';
import {Script} from 'node:vm';

const read = name => readFileSync(new URL(name, import.meta.url), 'utf8');
const html = read('fixture.html');
const negative = read('no-js.html');
assert.equal(negative, html.replace("script-src 'unsafe-inline'", "script-src 'none'"));
assert.notEqual(negative, html);
assert.match(html, /<p id="state"[^>]*>STATIC \/ JS NOT RUN<\/p>/);
assert.match(html, /<canvas[^>]*width="192" height="96"/);
assert.match(html, /<button id="tap"[^>]* disabled>/);
assert.match(html, /default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src 'self'/);
assert.equal([...html.matchAll(/<script[ >]/g)].length, 1);
const script = new Script(html.match(/<script>\s*([\s\S]*?)<\/script>/)[1], {filename: 'fixture-inline.js'});

const rgba = {'#ff0000': [255,0,0,255], '#00ff00': [0,255,0,255], '#0000ff': [0,0,255,255], '#ffff00': [255,255,0,255]};
const digest = data => createHash('sha256').update(data).digest('hex');
function load(url, hasCanvas = true) {
  const state = {textContent: 'STATIC / JS NOT RUN'};
  let click;
  const tap = {disabled: true, addEventListener(type, fn) { assert.equal(type, 'click'); click = fn; }};
  const pixels = Buffer.alloc(192 * 96 * 4);
  const requests = [];
  const canvas = {getContext(type) {
    assert.equal(type, '2d');
    return hasCanvas ? {fillRect(x, y, w, h) {
      assert.ok(rgba[this.fillStyle]);
      assert.ok(x >= 0 && y >= 0 && x + w <= 192 && y + h <= 96);
      for (let yy = y; yy < y + h; yy++) {
        for (let xx = x; xx < x + w; xx++) pixels.set(rgba[this.fillStyle], 4 * (yy * 192 + xx));
      }
    }} : null;
  }};
  const document = {getElementById(id) { return {state, grid: canvas, tap}[id]; }};
  class Image { set src(path) { requests.push(path); } }
  script.runInNewContext({URLSearchParams, document, Image, location: new URL(url)}, {timeout: 500});
  return {state, tap, pixels, requests, click: isTrusted => click({isTrusted})};
}

const base = 'http://127.0.0.1:18719/fixture.html';
const page = load(base + '?run=h19p1');
const at = (x, y) => [...page.pixels.subarray(4 * (y * 192 + x), 4 * (y * 192 + x) + 4)];
const samples = () => [[48,24], [144,24], [48,72], [144,72]].map(([x,y]) => at(x,y));
assert.equal(page.state.textContent, 'JS READY / h19p1');
assert.equal(page.tap.disabled, false);
assert.deepEqual(samples(), [rgba['#ff0000'], rgba['#00ff00'], rgba['#0000ff'], rgba['#ffff00']]);
assert.deepEqual(page.requests, ['mark-h19p1-ready.gif']);
const ready = digest(page.pixels);
page.click(false);
assert.equal(page.state.textContent, 'JS READY / h19p1');
assert.equal(digest(page.pixels), ready);
assert.equal(page.requests.length, 1);
page.click(true);
assert.equal(page.state.textContent, 'JS TAP 1 / h19p1');
assert.equal(page.tap.disabled, true);
assert.deepEqual(samples(), [rgba['#00ff00'], rgba['#0000ff'], rgba['#ffff00'], rgba['#ff0000']]);
assert.deepEqual(page.requests, ['mark-h19p1-ready.gif', 'mark-h19p1-tap1.gif']);
const tapped = digest(page.pixels);
page.click(true);
assert.equal(digest(page.pixels), tapped);
assert.equal(page.requests.length, 2);
for (const url of [base + '?run=bad%2Frun', 'file:///fixture.html?run=ok', 'http://example.invalid/fixture.html?run=ok']) {
  const other = load(url);
  other.click(true);
  assert.deepEqual(other.requests, []);
}
const missing = load(base + '?run=h19p1', false);
assert.equal(missing.state.textContent, 'JS READY / h19p1 / CANVAS UNAVAILABLE');
assert.deepEqual(missing.requests, []);
assert.equal(missing.tap.disabled, true);

console.log('PASS: inline JS parses; static markup and CSP-only negative difference; mocked DOM/Canvas READY, untrusted click, TAP 1, repeat click, marker origin/input guards, Canvas unavailable path.');
console.log('RGBA 192x96 row-major SHA256 READY:', ready);
console.log('RGBA 192x96 row-major SHA256 TAP 1:', tapped);
console.log('No browser, HTTP listener or target device used by this test.');
