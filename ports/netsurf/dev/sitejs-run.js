// Development helper: runs the site scripts under Node.js against a saved or
// freshly fetched page, without building NetSurf.
//   node ports/netsurf/dev/sitejs-run.js <url> [saved.html] > out.html
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const { spawnSync } = require('child_process');

const dir = path.join(__dirname, '..', 'overlay', 'netsurf', 'frontends', 'framebuffer', 'res', 'sitejs');
const UA = 'Mozilla/5.0 (X11; Linux) NetSurf/3.11';
const state = {};

function curl(url, method, headers, body, timeout) {
    const args = ['-s', '-L', '--compressed', '-m', String(Math.ceil((timeout || 25000) / 1000)),
        '-D', '-', '-X', method || 'GET'];
    if (!(headers || []).some((h) => /^user-agent:/i.test(h))) args.push('-A', UA);
    (headers || []).forEach((h) => args.push('-H', h));
    if (typeof body === 'string') args.push('--data-binary', '@-');
    args.push(url);
    const r = spawnSync('curl', args, { input: typeof body === 'string' ? body : undefined, maxBuffer: 1 << 28 });
    const out = r.stdout || Buffer.alloc(0);
    let at = 0, head = '', status = 0;
    for (;;) { // skip the header block of every redirect hop
        const end = out.indexOf('\r\n\r\n', at);
        if (end < 0 || out.slice(at, at + 5).toString() !== 'HTTP/') break;
        head = out.slice(at, end).toString();
        status = parseInt(head.split(' ')[1], 10) || 0;
        at = end + 4;
    }
    const res = { status, headers: head, body: out.slice(at).toString('utf8'), url };
    if (r.status !== 0) res.error = 'curl exit ' + r.status;
    return res;
}

const sandbox = {
    __native: {
        http: curl,
        log: (s) => process.stderr.write('sitejs: ' + s + '\n'),
        readFile: (n) => { try { return fs.readFileSync(path.join(dir, n), 'utf8'); } catch (e) { return null; } },
        loadState: (k) => (k in state ? state[k] : null),
        saveState: (k, v) => { state[k] = v; return true; }
    }
};
vm.createContext(sandbox);
fs.readdirSync(dir).filter((f) => f.endsWith('.js')).sort().forEach((f) => {
    vm.runInContext(fs.readFileSync(path.join(dir, f), 'utf8'), sandbox, { filename: f });
});

const url = process.argv[2];
if (!url) { console.error('usage: sitejs-run.js <url> [saved.html]'); process.exit(2); }
const wants = vm.runInContext('__siteWants', sandbox)(url);
process.stderr.write('wants: ' + wants + '\n');
let html, status = 200;
if (process.argv[3]) {
    html = fs.readFileSync(process.argv[3], 'utf8');
} else {
    const r = curl(url, 'GET', [], undefined, 30000);
    html = r.body; status = r.status;
}
const t0 = Date.now();
const out = vm.runInContext('__siteTransform', sandbox)(url, html, status);
process.stderr.write('transform: ' + (Date.now() - t0) + ' ms, ' + (out === null ? 'declined' : out.length + ' chars') + '\n');
if (out !== null) process.stdout.write(out);
