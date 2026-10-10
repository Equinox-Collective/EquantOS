// Site script runtime for NetSurf (EquantOS port).
//
// Loaded first; every other *.js in this directory registers a site with
// registerSite({ name, hosts: [...], transform(req) }).  transform() gets
//   req.url     address of the document
//   req.loc     parsed form: { scheme, host, path, query: {k: v}, hash }
//   req.html    document as the server sent it
//   req.status  HTTP status
// and returns the HTML NetSurf should show, or null to leave the page alone.
// Adding "ns_raw=1" to a query string always shows the original page.
'use strict';

var __sites = [];

function registerSite(site) {
    __sites.push(site);
}

function log() {
    __native.log(Array.prototype.map.call(arguments, function (a) {
        return typeof a === 'string' ? a : JSON.stringify(a);
    }).join(' '));
}

// ---- URLs ------------------------------------------------------------------

function decodeQueryPart(s) {
    try {
        return decodeURIComponent(s.replace(/\+/g, ' '));
    } catch (e) {
        return s;
    }
}

function parseQuery(qs) {
    var out = {};
    if (!qs) return out;
    qs.split('&').forEach(function (kv) {
        if (!kv) return;
        var i = kv.indexOf('=');
        var k = decodeQueryPart(i < 0 ? kv : kv.slice(0, i));
        if (!(k in out)) out[k] = i < 0 ? '' : decodeQueryPart(kv.slice(i + 1));
    });
    return out;
}

function parseUrl(url) {
    var m = /^([a-z][a-z0-9+.-]*):\/\/([^\/?#]*)([^?#]*)(?:\?([^#]*))?(?:#(.*))?$/i.exec(url) || [];
    var host = (m[2] || '').replace(/^[^@]*@/, '').replace(/:\d+$/, '').toLowerCase();
    return {
        scheme: (m[1] || '').toLowerCase(),
        host: host,
        path: m[3] || '/',
        search: m[4] || '',
        query: parseQuery(m[4]),
        hash: m[5] || ''
    };
}

function buildQuery(obj) {
    return Object.keys(obj).filter(function (k) {
        return obj[k] !== undefined && obj[k] !== null;
    }).map(function (k) {
        return encodeURIComponent(k) + '=' + encodeURIComponent(obj[k]);
    }).join('&');
}

// ---- HTML ------------------------------------------------------------------

function esc(s) {
    if (s === undefined || s === null) return '';
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;')
        .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function unesc(s) {
    return String(s).replace(/&lt;/g, '<').replace(/&gt;/g, '>')
        .replace(/&quot;/g, '"').replace(/&#39;/g, "'").replace(/&#x27;/g, "'")
        .replace(/&#(\d+);/g, function (_, n) { return String.fromCodePoint(+n); })
        .replace(/&#x([0-9a-f]+);/gi, function (_, n) { return String.fromCodePoint(parseInt(n, 16)); })
        .replace(/&nbsp;/g, ' ').replace(/&amp;/g, '&');
}

// Plain text with URLs turned into links and newlines kept.
function linkify(text) {
    return esc(text).replace(/(https?:\/\/[^\s<]+[^\s<.,;:!?)\]'"])/g, '<a href="$1">$1</a>')
        .replace(/\n/g, '<br>');
}

function stripTags(html) {
    return unesc(String(html).replace(/<[^>]*>/g, '')).replace(/\s+/g, ' ').trim();
}

function page(opts) {
    return '<!DOCTYPE html>\n<html><head><meta charset="utf-8"><title>' + esc(opts.title || '') +
        '</title><style>' + (opts.css || '') + '</style></head><body>' + opts.body + '</body></html>';
}

// ---- a small HTML parser for pages that carry no data but their markup -------
//
// parseHTML(html) -> tree of { tag, attrs: {name: value}, children: [...], parent }
// with text as { text }.  Good enough for well-formed generated pages; it does
// not implement the HTML5 error recovery rules.

var __VOID = { area: 1, base: 1, br: 1, col: 1, embed: 1, hr: 1, img: 1, input: 1,
               link: 1, meta: 1, source: 1, track: 1, wbr: 1 };
var __RAWTEXT = { script: 1, style: 1, textarea: 1, title: 1 };

function parseHTML(html) {
    var root = { tag: '#root', attrs: {}, children: [], parent: null };
    var cur = root, i = 0, n = html.length, lower = null;

    function isSpace(c) { return c === 32 || c === 10 || c === 9 || c === 13 || c === 12; }

    while (i < n) {
        var lt = html.indexOf('<', i);
        if (lt < 0) lt = n;
        if (lt > i) cur.children.push({ text: html.slice(i, lt), parent: cur });
        if (lt >= n) break;

        var c1 = html.charCodeAt(lt + 1), end;
        if (c1 === 33 || c1 === 63) { // <!-- -->, <!doctype>, <?...>
            var comment = html.startsWith('<!--', lt);
            end = comment ? html.indexOf('-->', lt + 4) : html.indexOf('>', lt);
            i = end < 0 ? n : end + (comment ? 3 : 1);
            continue;
        }
        if (c1 === 47) { // </tag>
            end = html.indexOf('>', lt);
            if (end < 0) break;
            var closing = html.slice(lt + 2, end).trim().toLowerCase();
            for (var p = cur; p && p.parent; p = p.parent) {
                if (p.tag === closing) { cur = p.parent; break; }
            }
            i = end + 1;
            continue;
        }
        if (!((c1 >= 65 && c1 <= 90) || (c1 >= 97 && c1 <= 122))) { // a stray "<"
            cur.children.push({ text: '&lt;', parent: cur });
            i = lt + 1;
            continue;
        }

        var j = lt + 1;
        while (j < n && !isSpace(html.charCodeAt(j)) && html[j] !== '>' && html[j] !== '/') j++;
        var node = { tag: html.slice(lt + 1, j).toLowerCase(), attrs: {}, children: [], parent: cur };
        var selfClose = false;
        for (;;) {
            while (j < n && isSpace(html.charCodeAt(j))) j++;
            if (j >= n) break;
            if (html[j] === '>') { j++; break; }
            if (html[j] === '/') { selfClose = html[j + 1] === '>'; j++; continue; }
            var k = j;
            while (k < n && !isSpace(html.charCodeAt(k)) && html[k] !== '=' && html[k] !== '>' && html[k] !== '/') k++;
            var name = html.slice(j, k).toLowerCase(), value = '';
            j = k;
            while (j < n && isSpace(html.charCodeAt(j))) j++;
            if (html[j] === '=') {
                j++;
                while (j < n && isSpace(html.charCodeAt(j))) j++;
                var q = html[j];
                if (q === '"' || q === "'") {
                    k = html.indexOf(q, j + 1);
                    if (k < 0) k = n;
                    value = html.slice(j + 1, k);
                    j = k + 1;
                } else {
                    k = j;
                    while (k < n && !isSpace(html.charCodeAt(k)) && html[k] !== '>') k++;
                    value = html.slice(j, k);
                    j = k;
                }
            }
            if (name && !(name in node.attrs)) node.attrs[name] = value.indexOf('&') < 0 ? value : unesc(value);
        }
        cur.children.push(node);
        i = j;

        if (__RAWTEXT[node.tag]) {
            if (lower === null) lower = html.toLowerCase();
            var close = lower.indexOf('</' + node.tag, i);
            if (close < 0) close = n;
            node.children.push({ text: html.slice(i, close), parent: node, raw: true });
            end = html.indexOf('>', close);
            i = end < 0 ? n : end + 1;
        } else if (!selfClose && !__VOID[node.tag]) {
            cur = node;
        }
    }
    return root;
}

function __compileSelector(sel) {
    return sel.trim().split(/\s+/).map(function (part) {
        var c = { tag: null, id: null, classes: [], attrs: [] };
        part.replace(/^[a-zA-Z][\w-]*|\*|#[\w-]+|\.[\w-]+|\[[^\]]+\]/g, function (tok) {
            if (tok[0] === '#') c.id = tok.slice(1);
            else if (tok[0] === '.') c.classes.push(tok.slice(1));
            else if (tok[0] === '[') {
                var m = /^\[([\w:-]+)(?:([~^$*]?=)["']?([^"'\]]*)["']?)?\]$/.exec(tok);
                if (m) c.attrs.push({ name: m[1].toLowerCase(), op: m[2], value: m[3] });
            } else if (tok !== '*') c.tag = tok.toLowerCase();
            return '';
        });
        return c;
    });
}

function __matches(node, c) {
    if (!node.tag || node.tag === '#root') return false;
    if (c.tag && node.tag !== c.tag) return false;
    if (c.id && node.attrs.id !== c.id) return false;
    if (c.classes.length) {
        var cls = ' ' + (node.attrs['class'] || '').replace(/\s+/g, ' ') + ' ';
        for (var i = 0; i < c.classes.length; i++)
            if (cls.indexOf(' ' + c.classes[i] + ' ') < 0) return false;
    }
    for (var j = 0; j < c.attrs.length; j++) {
        var a = c.attrs[j], v = node.attrs[a.name];
        if (v === undefined) return false;
        if (!a.op) continue;
        if (a.op === '=' && v !== a.value) return false;
        if (a.op === '^=' && v.indexOf(a.value) !== 0) return false;
        if (a.op === '$=' && v.slice(-a.value.length) !== a.value) return false;
        if (a.op === '*=' && v.indexOf(a.value) < 0) return false;
        if (a.op === '~=' && (' ' + v + ' ').indexOf(' ' + a.value + ' ') < 0) return false;
    }
    return true;
}

// queryAll(node, 'div.box a[href^="/"]'): descendant selectors made of tag,
// #id, .class and [attr], [attr=v], [attr^=v], [attr$=v], [attr*=v], [attr~=v]
function queryAll(node, selector, limit) {
    var chain = __compileSelector(selector), out = [];
    function ancestorsMatch(n, idx) {
        if (idx < 0) return true;
        for (var p = n.parent; p; p = p.parent)
            if (__matches(p, chain[idx]) && ancestorsMatch(p, idx - 1)) return true;
        return false;
    }
    (function walk(n) {
        var kids = n.children;
        if (!kids) return;
        for (var i = 0; i < kids.length; i++) {
            if (limit && out.length >= limit) return;
            var k = kids[i];
            if (!k.tag) continue;
            if (__matches(k, chain[chain.length - 1]) && ancestorsMatch(k, chain.length - 2)) out.push(k);
            walk(k);
        }
    })(node);
    return out;
}

function query(node, selector) {
    return node ? (queryAll(node, selector, 1)[0] || null) : null;
}

function textOf(node) {
    if (!node) return '';
    if (node.text !== undefined) return node.raw ? node.text : unesc(node.text);
    var s = '';
    for (var i = 0; i < node.children.length; i++) s += textOf(node.children[i]);
    return s;
}

function cleanText(node) {
    return textOf(node).replace(/\s+/g, ' ').trim();
}

// Serialises a node; drop(node) returning true leaves an element out.
function outerHTML(node, drop) {
    if (node.text !== undefined) return node.text;
    if (drop && node.tag !== '#root' && drop(node)) return '';
    var inner = '';
    for (var i = 0; i < node.children.length; i++) inner += outerHTML(node.children[i], drop);
    if (node.tag === '#root') return inner;
    var s = '<' + node.tag;
    for (var a in node.attrs) s += ' ' + a + '="' + esc(node.attrs[a]) + '"';
    return __VOID[node.tag] ? s + '>' : s + '>' + inner + '</' + node.tag + '>';
}

function innerHTML(node, drop) {
    var s = '';
    for (var i = 0; node && i < node.children.length; i++) s += outerHTML(node.children[i], drop);
    return s;
}

// ---- data ------------------------------------------------------------------

// Parses the JSON object or array that starts at the first { or [ after
// `marker` (as in "var data = {...};" inside a <script>).
function jsonAfter(text, marker, from) {
    var at = text.indexOf(marker, from || 0);
    if (at < 0) return undefined;
    var i = at + marker.length;
    while (i < text.length && text[i] !== '{' && text[i] !== '[') i++;
    var start = i, depth = 0, inStr = false;
    for (; i < text.length; i++) {
        var c = text.charCodeAt(i);
        if (inStr) {
            if (c === 92) i++;
            else if (c === 34) inStr = false;
        } else if (c === 34) {
            inStr = true;
        } else if (c === 123 || c === 91) {
            depth++;
        } else if (c === 125 || c === 93) {
            if (--depth === 0) {
                try {
                    return JSON.parse(text.slice(start, i + 1));
                } catch (e) {
                    return undefined;
                }
            }
        }
    }
    return undefined;
}

// Safe deep access: dig(obj, 'a', 0, 'b')
function dig(o) {
    for (var i = 1; i < arguments.length; i++) {
        if (o === undefined || o === null) return undefined;
        o = o[arguments[i]];
    }
    return o;
}

// Depth-first search for every value stored under one of `keys`; fn(key, value)
// returning true stops the walk below that value.
function walkKeys(o, keys, fn) {
    if (!o || typeof o !== 'object') return;
    if (Array.isArray(o)) {
        for (var i = 0; i < o.length; i++) walkKeys(o[i], keys, fn);
        return;
    }
    for (var k in o) {
        var v = o[k];
        if (keys[k] === true && v && typeof v === 'object') {
            if (fn(k, v) === true) continue;
        }
        if (v && typeof v === 'object') walkKeys(v, keys, fn);
    }
}

function findKey(o, key) {
    var found;
    var keys = {};
    keys[key] = true;
    walkKeys(o, keys, function (k, v) {
        if (found === undefined) found = v;
        return true;
    });
    return found;
}

// ---- network ---------------------------------------------------------------

// http(url, { method, headers: {Name: value}, body, timeout }) ->
//   { status, body, headers, url, error }
function http(url, opts) {
    opts = opts || {};
    var headers = [];
    for (var k in (opts.headers || {})) headers.push(k + ': ' + opts.headers[k]);
    return __native.http(url, opts.method || (opts.body !== undefined ? 'POST' : 'GET'),
        headers, opts.body, opts.timeout || 15000);
}

function httpJson(url, opts) {
    var r = http(url, opts);
    if (r.error) throw new Error('Network error: ' + r.error + ' (' + url.replace(/\?.*/, '') + ')');
    try {
        r.json = JSON.parse(r.body);
    } catch (e) {
        throw new Error('HTTP ' + r.status + ' from ' + url.replace(/\?.*/, '') + ': not JSON');
    }
    return r;
}

function loadState(key, fallback) {
    var s = __native.loadState(key);
    if (s === null) return fallback;
    try {
        return JSON.parse(s);
    } catch (e) {
        return fallback;
    }
}

function saveState(key, value) {
    return __native.saveState(key, JSON.stringify(value));
}

// ---- entry points called by NetSurf -----------------------------------------

function __findSite(host) {
    for (var i = 0; i < __sites.length; i++) {
        var hosts = __sites[i].hosts || [];
        for (var j = 0; j < hosts.length; j++) {
            var h = hosts[j];
            if (host === h || (host.length > h.length && host.slice(-h.length - 1) === '.' + h))
                return __sites[i];
        }
    }
    return null;
}

function __siteWants(url) {
    var loc = parseUrl(url);
    if (loc.scheme !== 'http' && loc.scheme !== 'https') return false;
    if (loc.query.ns_raw === '1') return false;
    return __findSite(loc.host) !== null;
}

function __siteTransform(url, html, status) {
    var loc = parseUrl(url);
    var site = __findSite(loc.host);
    if (!site) return null;
    var out = site.transform({ url: url, loc: loc, html: html, status: status });
    return typeof out === 'string' ? out : null;
}
