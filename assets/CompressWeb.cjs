const fs = require('node:fs');
const zlib = require('node:zlib');
const input = fs.readFileSync(process.argv[1]);
const output = zlib.brotliCompressSync(input, {params: {
  [zlib.constants.BROTLI_PARAM_QUALITY]: 11,
  [zlib.constants.BROTLI_PARAM_LGWIN]: 24,
  [zlib.constants.BROTLI_PARAM_SIZE_HINT]: input.length,
}});
if (!zlib.brotliDecompressSync(output).equals(input)) throw Error('Brotli round-trip failed');
fs.writeFileSync(process.argv[2], output);
console.log(`Browser assets: ${(input.length / 1048576).toFixed(2)} MiB -> ${(output.length / 1048576).toFixed(2)} MiB Brotli`);
fs.writeFileSync(require('node:path').join(require('node:path').dirname(process.argv[2]), 'index.data.json'), JSON.stringify({size: input.length, compressedSize: output.length, sha256: require('node:crypto').createHash('sha256').update(input).digest('hex')}));
