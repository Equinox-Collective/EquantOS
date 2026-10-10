// GitHub for NetSurf.
//
// github.com pages are React applications styled with CSS NetSurf cannot
// follow (custom properties, grid, cascade layers), but every page embeds
// its data as JSON or is plain server-side HTML underneath.  This script
// lays that data out with simple markup and its own stylesheet.  Pages it has
// no layout for are shown cleaned up: scripts, icons, menus and hidden
// dialogs removed, forms (sign in, search) left working.
'use strict';

(function () {

var CSS = [
    'body{margin:0;font-family:sans-serif;font-size:14px;color:#1f2328;background:#fff;line-height:1.5}',
    'a{color:#0969da;text-decoration:none}',
    'a:hover{text-decoration:underline}',
    '[hidden],template,dialog,.sr-only,.show-on-focus{display:none}',
    '.top{background:#24292f;color:#fff;padding:12px 24px;height:32px}',
    '.top a{color:#fff;font-weight:bold}',
    '.top .logo{float:left;font-size:20px;margin-right:24px;line-height:32px}',
    '.top .nav{float:left;line-height:32px}',
    '.top .nav a{margin-right:16px;font-size:14px}',
    '.top form{float:right;margin-top:2px}',
    '.top input.q{width:300px;height:24px;border:1px solid #57606a;background:#fff;color:#1f2328;padding:1px 8px;font-size:14px}',
    '.top input.go{height:28px;background:#32383f;color:#fff;border:1px solid #57606a;padding:0 12px}',
    '.repohead{background:#f6f8fa;border-bottom:1px solid #d0d7de;padding:16px 24px 0 24px}',
    '.repohead .name{font-size:20px}',
    '.repohead .name b a{font-weight:bold}',
    '.badge{display:inline-block;border:1px solid #d0d7de;color:#57606a;font-size:12px;padding:0 7px;margin-left:8px;vertical-align:middle}',
    '.tabs{margin-top:12px}',
    '.tabs a{display:inline-block;color:#1f2328;padding:8px 14px;border-bottom:2px solid #f6f8fa}',
    '.tabs a.on{font-weight:bold;border-bottom:2px solid #fd8c73}',
    '.main{padding:20px 24px 40px 24px}',
    'h1{font-size:24px;font-weight:normal;margin:0 0 12px 0}',
    'h2{font-size:18px;margin:20px 0 8px 0}',
    '.cols{border-collapse:collapse;width:100%}',
    '.cols td{vertical-align:top;padding:0}',
    '.side{width:296px;padding-left:24px !important}',
    '.side h3{font-size:16px;margin:0 0 8px 0}',
    '.side p{margin:0 0 10px 0}',
    '.topic{display:inline-block;background:#ddf4ff;color:#0969da;padding:0 10px;margin:0 4px 4px 0;font-size:12px}',
    '.muted{color:#57606a}',
    '.small{font-size:12px}',
    '.box{border:1px solid #d0d7de;margin-bottom:16px}',
    '.boxhead{background:#f6f8fa;border-bottom:1px solid #d0d7de;padding:10px 16px;font-weight:bold}',
    '.boxhead .r{float:right;font-weight:normal}',
    '.files{border-collapse:collapse;width:100%}',
    '.files td{padding:7px 16px;border-top:1px solid #d0d7de}',
    '.files tr.first td{border-top:0}',
    '.ico{display:inline-block;width:14px;height:11px;margin-right:10px;vertical-align:middle}',
    '.dir{background:#54aeff}',
    '.file{border:1px solid #8c959f;width:9px;height:12px;margin-left:2px;margin-right:12px}',
    '.md{padding:24px 32px}',
    '.markdown-body{font-size:16px;line-height:1.5}',
    '.markdown-body h1{font-size:30px;font-weight:bold;border-bottom:1px solid #d0d7de;padding-bottom:8px;margin:24px 0 16px 0}',
    '.markdown-body h2{font-size:24px;border-bottom:1px solid #d0d7de;padding-bottom:6px;margin:24px 0 16px 0}',
    '.markdown-body h3{font-size:20px;margin:24px 0 16px 0}',
    '.markdown-body h4{font-size:16px;margin:24px 0 16px 0}',
    '.markdown-body p{margin:0 0 16px 0}',
    '.markdown-body img{max-width:100%}',
    '.markdown-body pre{background:#f6f8fa;padding:16px;font-size:13px;line-height:1.45;overflow:auto;margin:0 0 16px 0}',
    '.markdown-body code{background:#eff1f3;font-size:85%;padding:2px 5px}',
    '.markdown-body pre code{background:#f6f8fa;padding:0;font-size:100%}',
    '.markdown-body blockquote{border-left:4px solid #d0d7de;color:#57606a;margin:0 0 16px 0;padding:0 16px}',
    '.markdown-body table{border-collapse:collapse;margin:0 0 16px 0}',
    '.markdown-body th,.markdown-body td{border:1px solid #d0d7de;padding:6px 13px}',
    '.markdown-body th{background:#f6f8fa}',
    '.markdown-body ul,.markdown-body ol{margin:0 0 16px 0;padding-left:32px}',
    '.markdown-body hr{border:0;border-top:3px solid #d0d7de;margin:24px 0}',
    '.crumbs{font-size:16px;margin-bottom:14px}',
    '.crumbs b{font-weight:bold}',
    '.code{border-collapse:collapse;width:100%}',
    '.code td{vertical-align:top;padding:0}',
    '.code pre{margin:0;font-family:monospace;font-size:12px;line-height:20px}',
    '.code .ln{width:1%;text-align:right;color:#8c959f;padding:8px 12px 8px 16px;background:#fff}',
    '.code .src{padding:8px 16px 8px 0}',
    '.pl-c{color:#6e7781}', '.pl-k,.pl-kos{color:#cf222e}', '.pl-s,.pl-pds,.pl-sr{color:#0a3069}',
    '.pl-c1,.pl-v{color:#0550ae}', '.pl-en,.pl-e{color:#8250df}', '.pl-smi,.pl-s1{color:#1f2328}',
    '.pl-ent{color:#116329}', '.pl-mh,.pl-md{color:#82071e}', '.pl-mi1{color:#116329}',
    '.item{border-top:1px solid #d0d7de;padding:10px 16px}',
    '.item.first{border-top:0}',
    '.item .t{font-size:16px;font-weight:bold;color:#1f2328}',
    '.dot{display:inline-block;width:10px;height:10px;margin-right:8px}',
    '.open{background:#1a7f37}', '.closed{background:#8250df}', '.draft{background:#8c959f}', '.red{background:#cf222e}',
    '.state{display:inline-block;color:#fff;font-weight:bold;padding:3px 12px;margin-right:8px}',
    '.label{display:inline-block;border:1px solid #d0d7de;font-size:12px;padding:0 7px;margin-left:6px}',
    '.comment{border:1px solid #d0d7de;margin:0 0 16px 0}',
    '.comment .who{background:#f6f8fa;border-bottom:1px solid #d0d7de;padding:8px 16px;color:#57606a}',
    '.comment .who b{color:#1f2328}',
    '.comment .who img{width:20px;height:20px;vertical-align:middle;margin-right:8px}',
    '.comment .body{padding:16px}',
    '.comment .body.markdown-body{font-size:14px}',
    '.event{color:#57606a;margin:0 0 16px 16px;font-size:13px}',
    '.pager{text-align:center;padding:16px 0}',
    '.pager a,.pager span{display:inline-block;border:1px solid #d0d7de;padding:5px 12px;margin:0 4px}',
    '.btn{display:inline-block;border:1px solid #d0d7de;background:#f6f8fa;color:#1f2328;padding:4px 12px;font-size:13px}',
    '.hero{text-align:center;padding:48px 0 32px 0}',
    '.hero h1{font-size:40px;font-weight:bold}',
    '.hero input.q{width:520px;height:34px;font-size:16px;border:1px solid #d0d7de;padding:1px 10px}',
    '.hero input.go{height:38px;font-size:16px;background:#1f883d;color:#fff;border:1px solid #1a7f37;padding:0 20px}',
    '.profile{min-height:100px;margin-bottom:20px}',
    '.profile img{float:left;width:96px;height:96px;margin-right:20px}',
    '.profile .n{font-size:24px;font-weight:bold;display:block}',
    '.profile .l{font-size:18px;color:#57606a;display:block}',
    '.lang{display:inline-block;margin-right:16px}',
    '.diff{font-family:monospace;font-size:12px;line-height:18px;margin:0;padding:8px 0}',
    '.diff span{display:block;padding:0 16px;white-space:pre}',
    '.diff .a{background:#e6ffec}', '.diff .d{background:#ffebe9}', '.diff .h{background:#ddf4ff;color:#57606a}',
    '.diff .f{background:#f6f8fa;font-weight:bold;border-top:1px solid #d0d7de;padding-top:6px;padding-bottom:6px}',
    '.note{background:#fff8c5;border:1px solid #d4a72c;padding:10px 16px;margin-bottom:16px}',
    '.generic img{max-width:100%}',
    '.foot{color:#8c959f;font-size:12px;padding:16px 24px;border-top:1px solid #d0d7de}',
    '.foot a{color:#57606a}'
].join('\n');

var MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

function fmtDate(iso) {
    var m = /^(\d{4})-(\d\d)-(\d\d)/.exec(iso || '');
    return m ? MONTHS[+m[2] - 1] + ' ' + (+m[3]) + ', ' + m[1] : '';
}

function fmtCount(n) {
    if (typeof n !== 'number') return n === undefined || n === null ? '' : String(n);
    if (n >= 1000000) return (n / 1000000).toFixed(1).replace(/\.0$/, '') + 'M';
    if (n >= 1000) return (n / 1000).toFixed(1).replace(/\.0$/, '') + 'k';
    return String(n);
}

function encPath(p) {
    return String(p).split('/').map(encodeURIComponent).join('/');
}

// ---- page frame --------------------------------------------------------------

function shell(opts) {
    var repo = opts.repo, head = '';
    if (repo) {
        var base = '/' + repo.owner + '/' + repo.name;
        var tabs = [['code', 'Code', base], ['issues', 'Issues', base + '/issues'],
                    ['pulls', 'Pull requests', base + '/pulls'], ['commits', 'Commits', base + '/commits'],
                    ['releases', 'Releases', base + '/releases'], ['tags', 'Tags', base + '/tags']];
        head = '<div class="repohead"><div class="name"><a href="/' + esc(repo.owner) + '">' + esc(repo.owner) +
            '</a> / <b><a href="' + esc(base) + '">' + esc(repo.name) + '</a></b>' +
            (repo.badge ? '<span class="badge">' + esc(repo.badge) + '</span>' : '') + '</div><div class="tabs">' +
            tabs.map(function (t) {
                return '<a href="' + esc(t[2]) + '"' + (opts.tab === t[0] ? ' class="on"' : '') + '>' + t[1] + '</a>';
            }).join('') + '</div></div>';
    }
    return page({
        title: (opts.title ? opts.title + ' · ' : '') + 'GitHub',
        css: CSS,
        body: '<div class="top"><a class="logo" href="/">GitHub</a><div class="nav"><a href="/trending">Trending</a>' +
            '<a href="/explore?ns_raw=1">Explore</a><a href="/login">Sign in</a></div>' +
            '<form action="/search" method="get"><input class="q" type="text" name="q" value="' + esc(opts.query || '') +
            '"><input type="hidden" name="type" value="repositories"><input class="go" type="submit" value="Search"></form></div>' +
            head + '<div class="main">' + opts.body + '</div>' +
            '<div class="foot">GitHub through NetSurf site scripts &middot; <a href="' +
            esc(opts.rawUrl || '?ns_raw=1') + '">original page</a></div>'
    });
}

function embedded(html, target) {
    var marker = 'data-target="' + (target || 'react-app.embeddedData') + '">';
    var at = html.indexOf(marker);
    while (at >= 0) {
        var end = html.indexOf('</script>', at);
        if (end < 0) return null;
        try {
            var json = JSON.parse(html.slice(at + marker.length, end));
            if (json && json.payload) return json.payload;
        } catch (e) { /* try the next one */ }
        at = html.indexOf(marker, end);
    }
    return null;
}

// Rendered markdown from GitHub: drop the octicons and anchors NetSurf would show as noise
function cleanRich(html) {
    return String(html || '')
        .replace(/<svg[\s\S]*?<\/svg>/g, '')
        .replace(/<clipboard-copy[\s\S]*?<\/clipboard-copy>/g, '')
        .replace(/<a [^>]*class="anchor"[^>]*>\s*<\/a>/g, '')
        .replace(/<(\/?)(markdown-accessiblity-table|g-emoji|task-lists)[^>]*>/g, '');
}

function pager(loc, page, pages) {
    if (!pages || pages < 2) return '';
    function link(p, label) {
        var q = Object.assign({}, loc.query, { page: p });
        return '<a href="' + esc(loc.path + '?' + buildQuery(q)) + '">' + label + '</a>';
    }
    return '<div class="pager">' + (page > 1 ? link(page - 1, '&laquo; Previous') : '') +
        '<span>Page ' + page + ' of ' + pages + '</span>' + (page < pages ? link(page + 1, 'Next &raquo;') : '') + '</div>';
}

// ---- code: repository, directory, file ------------------------------------------

function fileTable(repo, ref, items, parentPath) {
    var base = '/' + repo.owner + '/' + repo.name;
    var rows = '';
    if (parentPath !== null && parentPath !== undefined) {
        rows += '<tr class="first"><td><span class="ico dir"></span><a href="' +
            esc(base + (parentPath ? '/tree/' + encPath(ref) + '/' + encPath(parentPath) : (ref ? '/tree/' + encPath(ref) : ''))) +
            '">..</a></td></tr>';
    }
    (items || []).forEach(function (it, i) {
        var dir = it.contentType === 'directory' || it.contentType === 'submodule';
        rows += '<tr' + (!rows && i === 0 ? ' class="first"' : '') + '><td><span class="ico ' + (dir ? 'dir' : 'file') +
            '"></span><a href="' + esc(base + (dir ? '/tree/' : '/blob/') + encPath(ref) + '/' + encPath(it.path)) + '">' +
            esc(it.name) + '</a></td></tr>';
    });
    return '<table class="files">' + rows + '</table>';
}

function crumbs(repo, ref, path, isFile) {
    var base = '/' + repo.owner + '/' + repo.name;
    var out = '<a href="' + esc(base + '/tree/' + encPath(ref)) + '">' + esc(repo.name) + '</a>';
    var parts = path ? path.split('/') : [], acc = '';
    parts.forEach(function (p, i) {
        acc += (i ? '/' : '') + p;
        var last = i === parts.length - 1;
        out += ' / ' + (last ? '<b>' + esc(p) + '</b>'
            : '<a href="' + esc(base + '/tree/' + encPath(ref) + '/' + encPath(acc)) + '">' + esc(p) + '</a>');
    });
    return '<div class="crumbs">' + out + ' <span class="badge">' + esc(ref) + '</span></div>';
}

function repoOf(payload, loc) {
    var r = dig(payload, 'codeViewLayoutRoute', 'repo') || findKey(payload, 'repo') || {};
    var parts = loc.path.split('/');
    return {
        owner: r.ownerLogin || parts[1], name: r.name || parts[2],
        badge: r.isArchived ? 'Archived' : (r.isFork ? 'Fork' : (r.private ? 'Private' : 'Public')),
        defaultBranch: r.defaultBranch
    };
}

function repoPage(req, payload) {
    var route = payload.codeViewRepoRoute;
    var repo = repoOf(payload, req.loc);
    var ref = dig(route, 'refInfo', 'name') || repo.defaultBranch || 'HEAD';
    var about = dig(route, 'overview', 'sidebarAboutData') || payload.sidebarAbout || {};
    var base = '/' + repo.owner + '/' + repo.name;

    var files = '<div class="box"><div class="boxhead"><span class="r"><a href="' + esc(base + '/commits/' + encPath(ref)) + '">' +
        esc(dig(route, 'overview', 'commitCount') || '') + ' commits</a></span>Branch: ' + esc(ref) + '</div>' +
        fileTable(repo, ref, dig(route, 'tree', 'items')) + '</div>';

    var readme = '';
    (dig(route, 'overview', 'overviewFiles') || []).forEach(function (f) {
        if (!f.richText) return;
        readme += '<div class="box"><div class="boxhead">' + esc(f.displayName || f.tabName || 'README') +
            '</div><div class="md">' + cleanRich(f.richText) + '</div></div>';
    });

    var side = '<h3>About</h3><p>' + (about.formattedDescription || esc(about.description) || '<span class="muted">No description.</span>') + '</p>' +
        (about.website ? '<p><a href="' + esc(about.website) + '">' + esc(about.website) + '</a></p>' : '') +
        '<p>' + (about.topics || []).map(function (t) {
            var name = typeof t === 'string' ? t : t.name;
            return '<a class="topic" href="/topics/' + esc(name) + '">' + esc(name) + '</a>';
        }).join('') + '</p>' +
        '<p class="muted">&#9733; ' + fmtCount(about.stargazerCount) + ' stars<br>' + fmtCount(about.watcherCount) +
        ' watching<br>' + fmtCount(about.forksCount) + ' forks</p>' +
        '<p><a class="btn" href="' + esc(base + '/archive/refs/heads/' + encPath(ref) + '.zip') + '">Download ZIP</a></p>';

    return shell({ title: repo.owner + '/' + repo.name, repo: repo, tab: 'code',
        body: '<table class="cols"><tr><td>' + files + readme + '</td><td class="side">' + side + '</td></tr></table>' });
}

function treePage(req, payload) {
    var route = payload.codeViewTreeRoute;
    var repo = repoOf(payload, req.loc);
    var ref = dig(route, 'refInfo', 'name') || 'HEAD';
    var path = route.path || '';
    var parent = path.indexOf('/') >= 0 ? path.replace(/\/[^\/]*$/, '') : '';
    var readme = dig(route, 'tree', 'readme', 'richText');
    return shell({ title: path + ' · ' + repo.owner + '/' + repo.name, repo: repo, tab: 'code',
        body: crumbs(repo, ref, path) + '<div class="box">' + fileTable(repo, ref, dig(route, 'tree', 'items'), parent) + '</div>' +
            (readme ? '<div class="box"><div class="boxhead">README</div><div class="md">' + cleanRich(readme) + '</div></div>' : '') });
}

// One source line with GitHub's syntax spans applied
function styledLine(line, spans) {
    if (!spans || !spans.length) return esc(line);
    var out = '', at = 0;
    spans.map(function (s) { // [start, end, class] (older pages: {start, end, cssClass})
        return Array.isArray(s) ? { start: s[0], end: s[1], cls: s[2] } : { start: s.start, end: s.end, cls: s.cssClass };
    }).sort(function (a, b) { return a.start - b.start; }).forEach(function (s) {
        if (!(s.start >= at) || !(s.end > s.start)) return;
        out += esc(line.slice(at, s.start)) + '<span class="' + esc(s.cls) + '">' + esc(line.slice(s.start, s.end)) + '</span>';
        at = s.end;
    });
    return out + esc(line.slice(at));
}

function blobPage(req, payload) {
    var layout = payload.codeViewBlobLayoutRoute || {};
    var styled = payload['codeViewBlobLayoutRoute.StyledBlob'] || {};
    var route = payload.codeViewBlobRoute || {};
    var blob = layout.blob || {};
    var repo = repoOf(payload, req.loc);
    var ref = dig(layout, 'refInfo', 'name') || 'HEAD';
    var path = layout.path || '';
    var lines = styled.rawLines || blob.rawLines;
    var info = blob.headerInfo || {};
    var facts = [dig(info, 'lineInfo', 'truncatedLoc') ? info.lineInfo.truncatedLoc + ' lines' : '', info.blobSize || '', blob.language || '']
        .filter(Boolean).join(' · ');
    var raw = blob.rawBlobUrl || ('/' + repo.owner + '/' + repo.name + '/raw/' + encPath(ref) + '/' + encPath(path));

    var body;
    var rich = route.richText || blob.richText;
    if (rich) {
        body = '<div class="md">' + cleanRich(rich) + '</div>';
    } else if (blob.displayUrl && (blob.image || /\.(png|jpe?g|gif|webp|bmp|svg|ico)$/i.test(path))) {
        body = '<div class="md"><img src="' + esc(blob.displayUrl) + '" alt=""></div>';
    } else if (lines) {
        var max = 6000, shown = lines.slice(0, max), nums = '', code = '';
        var spans = styled.stylingDirectives || [];
        for (var i = 0; i < shown.length; i++) {
            nums += (i + 1) + '\n';
            code += styledLine(shown[i], spans[i]) + '\n';
        }
        body = '<table class="code"><tr><td class="ln"><pre>' + nums + '</pre></td><td class="src"><pre>' + code + '</pre></td></tr></table>' +
            (lines.length > max ? '<div class="note">Showing the first ' + max + ' of ' + lines.length + ' lines. <a href="' + esc(raw) + '">Raw file</a></div>' : '');
    } else {
        body = '<div class="md"><p class="muted">This file is not shown here (binary or too large).</p><p><a class="btn" href="' + esc(raw) + '">Download</a></p></div>';
    }
    return shell({ title: path + ' · ' + repo.owner + '/' + repo.name, repo: repo, tab: 'code',
        body: crumbs(repo, ref, path, true) + '<div class="box"><div class="boxhead"><span class="r"><a href="' + esc(raw) +
            '">Raw</a> &nbsp; <a href="' + esc('/' + repo.owner + '/' + repo.name + '/commits/' + encPath(ref) + '/' + encPath(path)) +
            '">History</a></span>' + esc(facts || path.replace(/^.*\//, '')) + '</div>' + body + '</div>' });
}

// ---- issues and pull requests ---------------------------------------------------

function stateClass(state) {
    state = String(state || '').toUpperCase();
    if (/DRAFT/.test(state)) return 'draft';
    if (/OPEN/.test(state)) return 'open';
    if (/CLOSED/.test(state) && /PULL/.test(state)) return 'red';
    return 'closed';
}

function issueRows(results) {
    return results.map(function (r, i) {
        var it = r.issue || r.pullRequest || r;
        if (!it || !it.number) return '';
        var isPr = !!r.pullRequest || /PULL/.test(r.itemType || '') || /\/pull\//.test(it.permalink || it.url || '');
        var href = (it.permalink || it.url || '').replace(/^https?:\/\/github\.com/, '') ||
            ('/' + it.repoNameWithOwner + (isPr ? '/pull/' : '/issues/') + it.number);
        var labels = (dig(it, 'labels', 'nodes') || dig(it, 'labels', 'edges') || []).map(function (l) {
            l = l.node || l;
            return '<span class="label">' + esc(l.name) + '</span>';
        }).join('');
        return '<div class="item' + (i === 0 ? ' first' : '') + '"><span class="dot ' + stateClass(it.state + (isPr ? ' PULL' : '')) +
            '"></span><a class="t" href="' + esc(href) + '">' + (dig(it, 'titleHtml', 'value') || it.titleHTML || esc(it.title)) + '</a>' + labels +
            '<div class="muted small">#' + it.number + ' ' + (isPr ? 'pull request' : 'issue') + ' opened ' + fmtDate(it.createdAt) +
            (dig(it, 'author', 'login') ? ' by ' + esc(it.author.login) : '') +
            (it.commentCount || dig(it, 'comments', 'totalCount') ? ' · ' + (it.commentCount || it.comments.totalCount) + ' comments' : '') +
            '</div></div>';
    }).join('');
}

function issuesPage(req, payload, tab) {
    var parts = req.loc.path.split('/');
    var repo = { owner: parts[1], name: parts[2] };
    var content = payload && payload.issuesIndexContentRoute;
    if (!content) {
        // the pull request dashboard loads its list later: ask the issue search for it
        var q = req.loc.query.q || ('is:pr is:open');
        var r = http('https://github.com/' + repo.owner + '/' + repo.name + '/issues?' +
            buildQuery({ q: q, page: req.loc.query.page }));
        content = dig(embedded(r.body), 'issuesIndexContentRoute');
    }
    var results = (content && content.results) || [];
    var info = (content && content.pageInfo) || {};
    var base = '/' + repo.owner + '/' + repo.name + (tab === 'pulls' ? '/pulls' : '/issues');
    var kind = tab === 'pulls' ? 'pr' : 'issue';
    var filters = '<p><a class="btn" href="' + esc(base + '?q=' + encodeURIComponent('is:' + kind + ' is:open')) + '">Open</a> ' +
        '<a class="btn" href="' + esc(base + '?q=' + encodeURIComponent('is:' + kind + ' is:closed')) + '">Closed</a> ' +
        '<span class="muted">' + (info.totalCount !== undefined ? fmtCount(info.totalCount) + ' results' : '') + '</span></p>' +
        '<form action="' + esc(base) + '" method="get"><input type="text" name="q" size="50" value="' +
        esc(req.loc.query.q || 'is:' + kind + ' is:open') + '"> <input type="submit" value="Filter"></form><br>';
    return shell({ title: (tab === 'pulls' ? 'Pull requests' : 'Issues') + ' · ' + repo.owner + '/' + repo.name, repo: repo, tab: tab,
        body: filters + '<div class="box">' + (issueRows(results) || '<div class="item first muted">Nothing matches.</div>') + '</div>' +
            pager(req.loc, info.currentPage || +req.loc.query.page || 1, info.totalPages) });
}

function commentHtml(author, when, bodyHtml, avatar) {
    return '<div class="comment"><div class="who">' + (avatar ? '<img src="' + esc(avatar) + '" alt="">' : '') +
        '<b>' + esc(author || 'ghost') + '</b> commented' + (when ? ' on ' + fmtDate(when) : '') +
        '</div><div class="body markdown-body">' + (cleanRich(bodyHtml) || '<span class="muted">No description provided.</span>') + '</div></div>';
}

function issuePage(req, payload) {
    var parts = req.loc.path.split('/');
    var repo = { owner: parts[1], name: parts[2] };
    var issue = dig(payload, 'issueViewerRoute', 'data', 'repository', 'issue') || findKey(payload, 'issue');
    if (!issue || !issue.title) return null;
    var open = /OPEN/i.test(issue.state);
    var out = '<h1>' + (issue.titleHTML || esc(issue.title)) + ' <span class="muted">#' + issue.number + '</span></h1>' +
        '<p><span class="state ' + (open ? 'open' : 'closed') + '">' + (open ? 'Open' : 'Closed') + '</span>' +
        '<span class="muted"><b>' + esc(dig(issue, 'author', 'login') || 'ghost') + '</b> opened this issue on ' + fmtDate(issue.createdAt) +
        ' · ' + (issue.totalCommentsCount || 0) + ' comments</span>' +
        (dig(issue, 'labels', 'edges') || []).map(function (e) { return '<span class="label">' + esc(dig(e, 'node', 'name')) + '</span>'; }).join('') + '</p>';
    out += commentHtml(dig(issue, 'author', 'login'), issue.createdAt, issue.bodyHTML, dig(issue, 'author', 'avatarUrl'));

    var seen = {};
    function timeline(list) {
        (dig(list, 'edges') || []).forEach(function (e) {
            var n = e.node || {};
            if (!n.id || seen[n.id]) return;
            seen[n.id] = true;
            var who = dig(n, 'author', 'login') || dig(n, 'actor', 'login') || '';
            switch (n.__typename) {
            case 'IssueComment':
                out += commentHtml(who, n.createdAt, n.bodyHTML, dig(n, 'author', 'avatarUrl'));
                break;
            case 'ClosedEvent':
                out += '<div class="event"><b>' + esc(who) + '</b> closed this on ' + fmtDate(n.createdAt) + '</div>';
                break;
            case 'ReopenedEvent':
                out += '<div class="event"><b>' + esc(who) + '</b> reopened this on ' + fmtDate(n.createdAt) + '</div>';
                break;
            case 'LabeledEvent':
                out += '<div class="event"><b>' + esc(who) + '</b> added the label <span class="label">' +
                    esc(dig(n, 'label', 'name') || '') + '</span> on ' + fmtDate(n.createdAt) + '</div>';
                break;
            case 'CrossReferencedEvent':
            case 'ReferencedEvent':
                out += '<div class="event"><b>' + esc(who) + '</b> mentioned this on ' + fmtDate(n.createdAt) + '</div>';
                break;
            default:
                break;
            }
        });
    }
    timeline(issue.timelineItems);
    var front = dig(issue, 'timelineItems', 'edges', 'length') || 0;
    var back = dig(issue, 'backTimelineItems', 'edges', 'length') || 0;
    var total = dig(issue, 'timelineItems', 'totalCount') || 0;
    if (total > front + back)
        out += '<div class="event">… ' + (total - front - back) + ' more events in between …</div>';
    timeline(issue.backTimelineItems);
    return shell({ title: issue.title + ' · Issue #' + issue.number, repo: repo, tab: 'issues', body: out });
}

// Pull request conversation: server-rendered HTML
function pullPage(req) {
    var parts = req.loc.path.split('/');
    var repo = { owner: parts[1], name: parts[2] };
    var num = parts[4];
    var base = '/' + repo.owner + '/' + repo.name + '/pull/' + num;
    if (parts[5] === 'files' || parts[5] === 'changes' || parts[5] === 'commits')
        return diffPage(req, repo, 'https://github.com' + base + '.diff', 'Pull request #' + num + ': changes', 'pulls', base);

    var doc = parseHTML(req.html);
    var title = (cleanText(query(doc, 'title')) || 'Pull request #' + num).replace(/ by .*? · Pull Request.*$/, '').replace(/ · Pull Request.*$/, '');
    var state = cleanText(query(doc, '.State')) || '';
    var out = '<h1>' + esc(title) + ' <span class="muted">#' + esc(num) + '</span></h1><p>' +
        (state ? '<span class="state ' + stateClass(state + ' PULL') + '">' + esc(state) + '</span>' : '') +
        '<a class="btn" href="' + esc(base + '/files') + '">Files changed</a></p>';
    var comments = queryAll(doc, '.timeline-comment');
    if (!comments.length) comments = queryAll(doc, '.js-comment-container');
    comments.forEach(function (c) {
        var body = query(c, '.comment-body');
        if (!body) return;
        var when = query(c, 'relative-time');
        out += commentHtml(cleanText(query(c, '.author')), when && when.attrs.datetime, innerHTML(body, dropNoise),
            (query(c, 'img.avatar') || { attrs: {} }).attrs.src);
    });
    if (!comments.length) out += '<div class="note">The conversation could not be read from this page.</div>';
    return shell({ title: title + ' · Pull Request #' + num, repo: repo, tab: 'pulls', body: out });
}

// ---- commits and diffs -----------------------------------------------------------

function commitsPage(req, payload) {
    var route = payload.commitsRefRoute;
    var repo = repoOf(payload, req.loc);
    if (route.repo) { repo.owner = route.repo.ownerLogin || repo.owner; repo.name = route.repo.name || repo.name; }
    var out = '<h1>Commits <span class="badge">' + esc(dig(route, 'refInfo', 'name') || '') + '</span></h1>';
    (route.commitGroups || []).forEach(function (g) {
        out += '<h2>' + esc(g.title) + '</h2><div class="box">' + (g.commits || []).map(function (c, i) {
            var who = (c.authors || []).map(function (a) { return a.login || a.displayName; }).join(', ');
            return '<div class="item' + (i === 0 ? ' first' : '') + '"><a class="t" href="' + esc(c.url) + '">' + esc(c.shortMessage) +
                '</a><div class="muted small">' + esc(who) + ' committed on ' + fmtDate(c.committedDate) +
                ' · <a href="' + esc(c.url) + '">' + esc(String(c.oid).slice(0, 7)) + '</a></div></div>';
        }).join('') + '</div>';
    });
    var pg = dig(route, 'filters', 'pagination') || {};
    var nav = '';
    if (pg.hasNextPage && pg.endCursor)
        nav = '<div class="pager"><a href="' + esc(req.loc.path + '?after=' + encodeURIComponent(pg.endCursor)) + '">Older &raquo;</a></div>';
    return shell({ title: 'Commits · ' + repo.owner + '/' + repo.name, repo: repo, tab: 'commits', body: out + nav });
}

function diffHtml(text, maxLines) {
    var lines = text.split('\n'), out = '', inHunk = false;
    var n = Math.min(lines.length, maxLines);
    for (var i = 0; i < n; i++) {
        var l = lines[i], cls = '';
        if (l.indexOf('diff --git') === 0) { cls = 'f'; inHunk = false; l = l.replace(/^diff --git a\/(.*) b\/.*$/, '$1'); }
        else if (l.indexOf('@@') === 0) { cls = 'h'; inHunk = true; }
        else if (!inHunk && /^(index |--- |\+\+\+ |new file|deleted file|similarity|rename |old mode|new mode|Binary )/.test(l)) continue;
        else if (l[0] === '+') cls = 'a';
        else if (l[0] === '-') cls = 'd';
        out += '<span' + (cls ? ' class="' + cls + '"' : '') + '>' + (esc(l) || ' ') + '</span>';
    }
    return '<div class="diff">' + out + '</div>' +
        (lines.length > maxLines ? '<div class="note">The diff is longer than ' + maxLines + ' lines; the rest is not shown.</div>' : '');
}

function diffPage(req, repo, url, title, tab, back) {
    var r = http(url, { timeout: 40000 });
    var body;
    if (r.error || r.status >= 400) {
        body = '<div class="note">The diff could not be loaded (' + esc(r.error || 'HTTP ' + r.status) + ').</div>';
    } else {
        var text = r.body, header = '';
        // a .patch starts with mail headers: show them as the commit message
        var m = /^From [0-9a-f]+ .*\nFrom: (.*)\nDate: (.*)\nSubject: (?:\[PATCH[^\]]*\] )?([\s\S]*?)\n---\n/.exec(text);
        if (m) {
            header = '<div class="comment"><div class="who"><b>' + esc(m[1].replace(/\s*<.*$/, '')) + '</b> committed on ' +
                esc(m[2].replace(/ [+-]\d{4}$/, '')) + '</div><div class="body"><pre style="margin:0;white-space:pre-wrap">' +
                esc(m[3]) + '</pre></div></div>';
            text = text.slice(m[0].length);
            var d = text.indexOf('diff --git');
            if (d > 0) text = text.slice(d);
        }
        body = header + '<div class="box">' + diffHtml(text, 4000) + '</div>';
    }
    return shell({ title: title, repo: repo, tab: tab,
        body: '<h1>' + esc(title) + '</h1>' + (back ? '<p><a class="btn" href="' + esc(back) + '">&laquo; Back</a></p>' : '') + body });
}

// ---- releases and tags (Atom feeds) -----------------------------------------------

function feedPage(req, repo, kind) {
    var r = http('https://github.com/' + repo.owner + '/' + repo.name + '/' + kind + '.atom');
    var entries = [];
    String(r.body || '').replace(/<entry>([\s\S]*?)<\/entry>/g, function (_, e) {
        function tag(name) {
            var m = new RegExp('<' + name + '[^>]*>([\\s\\S]*?)</' + name + '>').exec(e);
            return m ? unesc(m[1]) : '';
        }
        var link = /<link[^>]*href="([^"]+)"/.exec(e);
        entries.push({ title: tag('title'), updated: tag('updated'), html: tag('content'), href: link ? unesc(link[1]) : '', author: tag('name') });
        return '';
    });
    var out = '<h1>' + (kind === 'tags' ? 'Tags' : 'Releases') + '</h1>';
    if (!entries.length) out += '<div class="note">Nothing has been published here.</div>';
    entries.forEach(function (e) {
        out += '<div class="comment"><div class="who"><b><a href="' + esc(e.href.replace(/^https?:\/\/github\.com/, '')) + '">' + esc(e.title) +
            '</a></b> · ' + fmtDate(e.updated) + (e.author ? ' · ' + esc(e.author) : '') + '</div>' +
            (stripTags(e.html) ? '<div class="body markdown-body">' + cleanRich(e.html) + '</div>' : '') + '</div>';
    });
    return shell({ title: (kind === 'tags' ? 'Tags' : 'Releases') + ' · ' + repo.owner + '/' + repo.name, repo: repo, tab: kind, body: out });
}

// ---- search, trending, profiles -----------------------------------------------------

function repoCard(href, name, desc, lang, stars, updated, first) {
    return '<div class="item' + (first ? ' first' : '') + '"><a class="t" style="color:#0969da" href="' + esc(href) + '">' + name + '</a>' +
        (desc ? '<div>' + desc + '</div>' : '') + '<div class="muted small">' +
        (lang ? '<span class="lang">' + esc(lang) + '</span>' : '') +
        (stars !== '' && stars !== undefined ? '<span class="lang">&#9733; ' + esc(stars) + '</span>' : '') +
        (updated ? '<span class="lang">Updated ' + fmtDate(updated) + '</span>' : '') + '</div></div>';
}

function searchPage(req, payload) {
    var route = payload.blackbirdSearchRoute || {};
    var q = req.loc.query.q || '';
    if ((route.type || 'repositories') !== 'repositories') return null;
    var rows = (route.results || []).map(function (r, i) {
        var rp = dig(r, 'repo', 'repository') || {};
        var full = rp.owner_login + '/' + rp.name;
        // GitHub marks the matches with <em>: keep those, drop any other markup
        function hl(s) {
            return esc(String(s).replace(/<(\/?)em>/g, '\u0001$1\u0002').replace(/<[^>]*>/g, ''))
                .replace(/\u0001(\/?)\u0002/g, '<$1b>');
        }
        return repoCard('/' + full, hl(r.hl_name || full), hl(r.hl_trunc_description || ''), r.language, fmtCount(r.followers), rp.updated_at, i === 0);
    }).join('');
    return shell({ title: q, query: q,
        body: '<h1>' + fmtCount(route.result_count || 0) + ' repositories for “' + esc(q) + '”</h1><div class="box">' +
            (rows || '<div class="item first muted">No repositories match.</div>') + '</div>' + pager(req.loc, route.page || 1, route.page_count) });
}

function trendingRows(doc) {
    return queryAll(doc, 'article.Box-row').map(function (a, i) {
        var link = query(a, 'h2 a');
        if (!link) return '';
        return repoCard(link.attrs.href, esc(cleanText(link).replace(/\s*\/\s*/, ' / ')), esc(cleanText(query(a, 'p'))),
            cleanText(query(a, '[itemprop=programmingLanguage]')), cleanText(query(a, 'a[href$="/stargazers"]')), '', i === 0);
    }).join('');
}

function trendingPage(req) {
    var rows = trendingRows(parseHTML(mainPart(req.html)));
    if (!rows) return null;
    return shell({ title: 'Trending', body: '<h1>Trending repositories</h1><div class="box">' + rows + '</div>' });
}

function homePage(req) {
    var rows = '';
    try {
        var r = http('https://github.com/trending');
        if (!r.error && r.status === 200) rows = trendingRows(parseHTML(mainPart(r.body)));
    } catch (e) { /* the search box still works */ }
    return shell({ title: '', rawUrl: '/?ns_raw=1',
        body: '<div class="hero"><h1>GitHub</h1><form action="/search" method="get"><input class="q" type="text" name="q">' +
            '<input type="hidden" name="type" value="repositories"> <input class="go" type="submit" value="Search"></form>' +
            '<p class="muted">Open a repository as github.com/owner/name, or search above.</p></div>' +
            (rows ? '<h2>Trending today</h2><div class="box">' + rows + '</div>' : '') });
}

function profilePage(req) {
    var doc = parseHTML(mainPart(req.html));
    var login = cleanText(query(doc, '.p-nickname')) || req.loc.path.split('/')[1];
    var name = cleanText(query(doc, '.p-name'));
    var rows = '';
    queryAll(doc, 'li[itemprop=owns]').forEach(function (li, i) {
        var a = query(li, 'a[itemprop~=codeRepository]');
        if (!a) return;
        var t = query(li, 'relative-time');
        rows += repoCard(a.attrs.href, esc(cleanText(a)), esc(cleanText(query(li, '[itemprop=description]'))),
            cleanText(query(li, '[itemprop=programmingLanguage]')), cleanText(query(li, 'a[href$="/stargazers"]')), t && t.attrs.datetime, i === 0);
    });
    var pinned = !rows;
    if (pinned) {
        queryAll(doc, '.pinned-item-list-item').forEach(function (li, i) {
            var a = query(li, 'a[href]');
            if (!a) return;
            rows += repoCard(a.attrs.href, esc(cleanText(query(li, '.repo')) || cleanText(a)), esc(cleanText(query(li, '.pinned-item-desc'))),
                cleanText(query(li, '[itemprop=programmingLanguage]')), cleanText(query(li, 'a[href$="/stargazers"]')), '', i === 0);
        });
    }
    if (!rows && !name && !query(doc, '.p-nickname')) return null;
    var avatar = (query(doc, 'img.avatar-user') || query(doc, 'img.avatar') || { attrs: {} }).attrs.src || '';
    var bio = cleanText(query(doc, '.user-profile-bio'));
    return shell({ title: login + (name ? ' (' + name + ')' : ''),
        body: '<div class="profile">' + (avatar ? '<img src="' + esc(avatar.replace(/([?&])s=\d+/, '$1s=192')) + '" alt="">' : '') +
            '<span class="n">' + esc(name || login) + '</span><span class="l">' + esc(login) + '</span>' +
            (bio ? '<p>' + esc(bio) + '</p>' : '') + '</div>' +
            '<p><a class="btn" href="/' + esc(login) + '">Overview</a> <a class="btn" href="/' + esc(login) + '?tab=repositories">Repositories</a></p>' +
            '<h2>' + (pinned ? 'Pinned' : 'Repositories') + '</h2><div class="box">' +
            (rows || '<div class="item first muted">No public repositories to show.</div>') + '</div>' +
            pagerFromLinks(doc) });
}

function pagerFromLinks(doc) {
    var prev = query(doc, 'a.previous_page') || query(doc, 'a[rel=prev]');
    var next = query(doc, 'a.next_page') || query(doc, 'a[rel=next]');
    if (!prev && !next) return '';
    return '<div class="pager">' + (prev ? '<a href="' + esc(prev.attrs.href) + '">&laquo; Previous</a>' : '') +
        (next ? '<a href="' + esc(next.attrs.href) + '">Next &raquo;</a>' : '') + '</div>';
}

// ---- any other page: the server's markup without what NetSurf cannot use -------------

function mainPart(html) {
    var a = html.indexOf('<main'), b = html.lastIndexOf('</main>');
    return a >= 0 && b > a ? html.slice(a, b + 7) : html;
}

var NOISE = { script: 1, style: 1, svg: 1, template: 1, dialog: 1, link: 1, meta: 1, noscript: 1,
              'include-fragment': 1, 'tool-tip': 1, 'clipboard-copy': 1, 'details-menu': 1, 'modal-dialog': 1,
              'action-menu': 1, 'anchored-position': 1, 'qbsearch-input': 1, 'react-partial': 1 };

function dropNoise(node) {
    if (NOISE[node.tag]) return true;
    if ('hidden' in node.attrs) return true;
    var cls = node.attrs['class'] || '';
    if (/(^| )(sr-only|d-none|show-on-focus|js-flash-container|Popover|Overlay)( |$)/.test(cls)) return true;
    if (node.tag === 'details' && !/markdown|toc/.test(cls)) return true;
    delete node.attrs.style;
    return false;
}

function genericPage(req) {
    var doc = parseHTML(mainPart(req.html));
    var body = innerHTML(doc, dropNoise);
    if (stripTags(body).length < 40) return null;
    var title = /<title>([^<]*)<\/title>/.exec(req.html);
    return shell({ title: title ? unesc(title[1]).replace(/ · GitHub$/, '') : '',
        body: '<div class="generic markdown-body">' + body + '</div>' });
}

var RESERVED = { about: 1, account: 1, apps: 1, codespaces: 1, collections: 1, contact: 1, copilot: 1, customer: 1,
    dashboard: 1, enterprise: 1, events: 1, explore: 1, features: 1, issues: 1, join: 1, login: 1, logout: 1,
    marketplace: 1, mobile: 1, new: 1, notifications: 1, orgs: 1, organizations: 1, password_reset: 1, pricing: 1,
    pulls: 1, readme: 1, search: 1, security: 1, session: 1, sessions: 1, settings: 1, signup: 1, site: 1, solutions: 1,
    sponsors: 1, stars: 1, team: 1, topics: 1, trending: 1, users: 1, resources: 1, premium: 1, why: 1 };

registerSite({
    name: 'github',
    hosts: ['github.com'],
    transform: function (req) {
        var loc = req.loc;
        if (loc.host !== 'github.com' && loc.host !== 'www.github.com') return null;
        if (!/<html/i.test(req.html.slice(0, 2000))) return null; // raw files, patches, feeds

        var parts = loc.path.replace(/\/+$/, '').split('/'); // ['', owner, repo, kind, ...]
        var payload = embedded(req.html);
        var out = null;

        if (parts.length <= 1) {
            out = homePage(req);
        } else if (parts[1] === 'trending') {
            out = trendingPage(req);
        } else if (parts[1] === 'search') {
            out = payload ? searchPage(req, payload) : null;
        } else if (parts.length === 2 && !RESERVED[parts[1]]) {
            out = profilePage(req);
        } else if (parts.length >= 3 && !RESERVED[parts[1]]) {
            var repo = { owner: parts[1], name: parts[2] };
            var kind = parts[3] || '';
            if (payload && payload.codeViewRepoRoute) out = repoPage(req, payload);
            else if (payload && payload.codeViewTreeRoute) out = treePage(req, payload);
            else if (payload && (payload.codeViewBlobLayoutRoute || payload.codeViewBlobRoute)) out = blobPage(req, payload);
            else if (kind === 'issues' && parts[4] && payload) out = issuePage(req, payload);
            else if (kind === 'issues') out = issuesPage(req, payload, 'issues');
            else if (kind === 'pulls') out = issuesPage(req, null, 'pulls');
            else if (kind === 'pull' && parts[4]) out = pullPage(req);
            else if (kind === 'commits' && payload && payload.commitsRefRoute) out = commitsPage(req, payload);
            else if (kind === 'commit' && parts[4])
                out = diffPage(req, repo, 'https://github.com/' + repo.owner + '/' + repo.name + '/commit/' + parts[4] + '.patch',
                    'Commit ' + parts[4].slice(0, 7), 'commits', '/' + repo.owner + '/' + repo.name + '/commits');
            else if ((kind === 'releases' && !parts[4]) || kind === 'tags') out = feedPage(req, repo, kind);
        }
        return out || genericPage(req);
    }
});

})();
