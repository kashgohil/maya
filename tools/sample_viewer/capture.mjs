// Renders views.json's views with the Khronos glTF Sample Renderer in headless Chromium, and writes each
// as an 8-bit RGB PNG (tests/references/sample-viewer). capture.sh prepares `site` and runs this.
//   node capture.mjs <site folder> <output folder> [--sky]
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { chromium } from 'playwright-core';

// An 8-bit RGB PNG with filter 0 on every row, as tests/support/png.hpp reads, from RGBA rows bottom up.
function rgbPng(width, height, rgba) {
    const raw = Buffer.alloc((width * 3 + 1) * height);
    for (let y = 0; y < height; ++y) {
        const source = (height - 1 - y) * width * 4, target = y * (width * 3 + 1);
        for (let x = 0; x < width; ++x)
            for (let c = 0; c < 3; ++c) raw[target + 1 + x * 3 + c] = rgba[source + x * 4 + c];
    }
    const crcTable = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
    const crc = (buffer) => { let c = 0xffffffff; for (const b of buffer) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
    const chunk = (type, data) => {
        const length = Buffer.alloc(4); length.writeUInt32BE(data.length);
        const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
        const sum = Buffer.alloc(4); sum.writeUInt32BE(crc(body));
        return Buffer.concat([length, body, sum]);
    };
    const header = Buffer.alloc(13);
    header.writeUInt32BE(width, 0); header.writeUInt32BE(height, 4); header[8] = 8; header[9] = 2;
    return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', header),
                          chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]);
}
const here = path.dirname(new URL(import.meta.url).pathname);
const [site, output] = process.argv.slice(2, 4).map((p) => path.resolve(p));
const sky = process.argv.includes('--sky');
const config = JSON.parse(fs.readFileSync(path.join(here, 'views.json'), 'utf8'));
const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.png': 'image/png', '.jpg': 'image/jpeg' };
const server = http.createServer((request, response) => {
    const file = path.join(site, decodeURIComponent(new URL(request.url, 'http://x').pathname));
    fs.readFile(file, (error, data) => {
        if (error) { response.writeHead(404); response.end(); return; }
        response.writeHead(200, { 'Content-Type': types[path.extname(file)] ?? 'application/octet-stream' });
        response.end(data);
    });
});
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const port = server.address().port;
// The GPU through ANGLE on Metal, as the Sample Viewer runs in Chrome on a Mac.
const browser = await chromium.launch({ headless: true, args: ['--use-angle=metal', '--enable-gpu', '--ignore-gpu-blocklist'] });
const page = await browser.newPage();
page.on('console', (message) => { if (message.type() === 'error') console.log('page:', message.text()); });
page.on('pageerror', (error) => console.log('page error:', error.message));
await page.goto(`http://127.0.0.1:${port}/index.html`);
await page.waitForFunction(() => window.ready === true);
console.log('renderer:', await page.evaluate(() => {
    const gl = document.createElement('canvas').getContext('webgl2');
    const info = gl.getExtension('WEBGL_debug_renderer_info');
    return info ? gl.getParameter(info.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER);
}));
fs.mkdirSync(output, { recursive: true });
for (const view of config.views) {
    const pixels = await page.evaluate((o) => window.renderView(o), {
        width: config.width, height: config.height, exposure: config.exposure, hdr: config.hdr, model: view.model,
        from: view.from, to: view.to, yfov: view.yfov, rotation: config.rotation, background: sky });
    const file = path.join(output, `${view.name}${sky ? '-sky' : ''}.png`);
    fs.writeFileSync(file, rgbPng(config.width, config.height, pixels));
    console.log('wrote', file);
}
await browser.close();
server.close();
