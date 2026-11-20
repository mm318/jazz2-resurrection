// Run with Node and the decoder produced by the web build.
const fs = require('node:fs');
const vm = require('node:vm');
const zlib = require('node:zlib');
const crypto = require('node:crypto');
const assert = require('node:assert/strict');
const source = fs.readFileSync('web_assets/asset-worker.js', 'utf8');
const decoder = fs.readFileSync(process.argv[2]);
const raw = Buffer.from('browser asset integrity test\n'.repeat(10000));
const compressed = zlib.brotliCompressSync(raw);
const manifest = {size: raw.length, compressedSize: compressed.length, sha256: crypto.createHash('sha256').update(raw).digest('hex')};
async function check(data, metadata = manifest, missing = false) {
  const messages = [], requests = [];
  const self = {location: {href: 'https://example.test/project/asset-worker.js'}, postMessage: m => messages.push(m), close() {}};
  const context = {self, URL, crypto: crypto.webcrypto, WebAssembly, Uint8Array, DataView, Number, Array, Error, fetch: async url => {
    requests.push(url.href);
    assert(url.pathname.startsWith('/project/'));
    const name = url.pathname.split('/').pop();
    const bytes = name === 'brotli.wasm' ? decoder : data;
    return {ok: !missing, status: missing ? 404 : 200, json: async () => metadata,
      arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength)};
  }};
  vm.runInNewContext(source, context);
  await self.onmessage();
  return {result: messages.at(-1), requests};
}
(async () => {
  let test = await check(compressed);
  assert.equal(test.result.type, 'ready');
  assert(Buffer.from(test.result.data).equals(raw));
  assert(test.requests.some(x => x.endsWith('brotli.wasm')));
  test = await check(raw);
  assert.equal(test.result.type, 'ready');
  assert(!test.requests.some(x => x.endsWith('brotli.wasm')));
  assert.equal((await check(compressed.subarray(1))).result.type, 'error');
  assert.equal((await check(compressed, {...manifest, sha256: '0'.repeat(64)})).result.type, 'error');
  assert.equal((await check(compressed, {...manifest, size: -1})).result.type, 'error');
  assert.equal((await check(compressed, manifest, true)).result.type, 'error');
  if (process.argv[3]) {
    const compressed = fs.readFileSync(process.argv[3]);
    const expected = zlib.brotliDecompressSync(compressed);
    const metadata = {size: expected.length, compressedSize: compressed.length, sha256: crypto.createHash('sha256').update(expected).digest('hex')};
    const full = await check(compressed, metadata);
    assert.equal(full.result.type, 'ready', full.result.message);
    assert(Buffer.from(full.result.data).equals(expected));
    console.log('Full game bundle decoded byte-for-byte with WebAssembly memory growth.');
  }
  const window = {};
  const module = {setStatus() {}};
  const document = {baseURI: 'https://example.test/project/', createElement: () => ({}), body: {appendChild(script) {
    assert.equal(script.src, 'https://example.test/project/index.js'); script.onload();
  }}};
  class Worker {
    constructor(url) { assert.equal(url.href, 'https://example.test/project/asset-worker.js'); }
    terminate() {}
    postMessage() { queueMicrotask(() => this.onmessage({data: {type: 'ready', data: new ArrayBuffer(4)}})); }
  }
  vm.runInNewContext(fs.readFileSync('web_assets/asset-loader.js', 'utf8'), {window, document, Worker, URL, Promise, Error});
  await window.JazzAssets.start(module, 'index.js');
  assert.equal(module.getPreloadedPackage('index.data', 4).byteLength, 4);
  assert.throws(() => module.getPreloadedPackage('index.data', 4));
  console.log('Browser worker tests passed: raw Brotli, HTTP-decoded data, project URLs, truncation, checksum, invalid metadata, and HTTP errors.');
})().catch(error => { console.error(error); process.exitCode = 1; });
