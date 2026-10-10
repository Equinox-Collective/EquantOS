// YouTube for NetSurf.
//
// youtube.com serves an application shell plus the data of the page as JSON
// ("ytInitialData").  This script lays that data out as plain HTML and asks
// the InnerTube API (the one the official apps use) for what is missing:
// feeds, further result pages, comments and the media stream of a video.
'use strict';

(function () {

var ORIGIN = 'https://www.youtube.com';
var WEB_CLIENT = { clientName: 'WEB', clientVersion: '2.20261009.01.00', hl: 'en', gl: 'US' };

// Clients tried in turn for a stream that plays without the web player's
// JS challenges.  Each needs its own User-Agent and client id header.
var PLAYER_CLIENTS = [
    {
        id: '3',
        ua: 'com.google.android.youtube/20.10.38 (Linux; U; Android 11) gzip',
        client: { clientName: 'ANDROID', clientVersion: '20.10.38', androidSdkVersion: 30,
                  osName: 'Android', osVersion: '11', hl: 'en', gl: 'US' }
    },
    {
        id: '5',
        ua: 'com.google.ios.youtube/20.10.4 (iPhone16,2; U; CPU iOS 18_3_2 like Mac OS X;)',
        client: { clientName: 'IOS', clientVersion: '20.10.4', deviceMake: 'Apple', deviceModel: 'iPhone16,2',
                  osName: 'iPhone', osVersion: '18.3.2.22D82', hl: 'en', gl: 'US' }
    },
    {
        id: '28',
        ua: 'com.google.android.apps.youtube.vr.oculus/1.60.19 (Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip',
        client: { clientName: 'ANDROID_VR', clientVersion: '1.60.19', deviceMake: 'Oculus', deviceModel: 'Quest 3',
                  androidSdkVersion: 32, osName: 'Android', osVersion: '12L', hl: 'en', gl: 'US' }
    }
];

// Feeds offered on the start page (YouTube shows nothing there when signed out)
var FEEDS = [
    { key: 'news', title: 'News', browseId: 'UCYfdidRxbB8Qhf0Nx7ioOYw' },
    { key: 'music', title: 'Music', browseId: 'UC-9-kyTW8ZkZNDHQJ6FgpwQ' },
    { key: 'gaming', title: 'Gaming', browseId: 'UCOpNcN46UbXVtpKMrmU4Abg' },
    { key: 'sports', title: 'Sports', browseId: 'UCEgdi0XIXXZ-qJOFPf4JSKw' },
    { key: 'learning', title: 'Learning', browseId: 'UCtFRv9O2AHqOZjjynzrv-xg' }
];

// Interface language: the one YouTube chose for the page (it goes by address
// and Accept-Language), so the labels below match the texts it sends.
var locale = loadState('youtube-locale', null) || { hl: 'en', gl: 'US' };

var LABELS = {
    ru: {
        'Home': 'Главная',
        'Search': 'Найти',
        'News': 'Новости',
        'Music': 'Музыка',
        'Gaming': 'Игры',
        'Sports': 'Спорт',
        'Learning': 'Обучение',
        'Recommended': 'Рекомендации',
        'Because you watched': 'По мотивам просмотренного:',
        'Show more': 'Показать ещё',
        'More results': 'Ещё результаты',
        'Show comments': 'Показать комментарии',
        'More comments': 'Ещё комментарии',
        'Comments': 'Комментарии',
        'Playlist': 'Плейлист',
        'Results for': 'Результаты по запросу',
        'No results for': 'Ничего не найдено по запросу',
        'Nothing more to show.': 'Больше ничего нет.',
        'Nothing to show on this page.': 'На этой странице нечего показать.',
        'No comments to show.': 'Комментариев нет.',
        'YouTube returned an empty feed. Use the search box above.': 'YouTube вернул пустую ленту. Воспользуйтесь поиском выше.',
        'Live streams are not supported yet.': 'Прямые трансляции пока не поддерживаются.',
        'This video has no stream NetSurf can play.': 'У этого видео нет потока, который NetSurf может воспроизвести.',
        'likes': 'отметок «Нравится»',
        'replies': 'ответов',
        'original page': 'исходная страница',
        'YouTube through NetSurf site scripts': 'YouTube через сайт-скрипты NetSurf'
    }
};

function T(s) {
    var t = LABELS[String(locale.hl || 'en').slice(0, 2)];
    return (t && t[s]) || s;
}

var CSS = [
    'body{margin:0;font-family:sans-serif;font-size:14px;color:#0f0f0f;background:#fff}',
    'a{color:#0f0f0f;text-decoration:none}',
    'a:hover{text-decoration:underline}',
    '.top{background:#fff;border-bottom:1px solid #e5e5e5;padding:8px 16px;height:40px}',
    '.logo{float:left;font-size:22px;font-weight:bold;line-height:40px;margin-right:32px}',
    '.logo .play{background:#f00;color:#fff;padding:1px 9px 3px 10px;margin-right:4px;font-size:16px}',
    '.search{float:left;margin-top:5px}',
    '.search input.q{width:420px;height:26px;font-size:15px;border:1px solid #c6c6c6;padding:1px 8px;vertical-align:middle}',
    '.search input.go{height:30px;font-size:14px;background:#f8f8f8;border:1px solid #c6c6c6;padding:0 16px;vertical-align:middle}',
    '.tabs{padding:10px 16px 0 16px}',
    '.tabs a{display:inline-block;background:#f2f2f2;padding:6px 12px;margin:0 8px 8px 0;font-weight:bold}',
    '.tabs a.on{background:#0f0f0f;color:#fff}',
    '.main{padding:8px 16px 32px 16px}',
    'h1{font-size:20px;margin:12px 0 8px 0}',
    'h2{font-size:17px;margin:20px 0 8px 0}',
    '.grid{margin-right:-12px}',
    '.card{display:inline-block;vertical-align:top;width:320px;margin:0 12px 22px 0}',
    '.thumb{display:block;position:relative;width:320px;height:180px;background:#000;overflow:hidden}',
    '.thumb img{display:block;width:320px;height:180px}',
    '.dur{position:absolute;right:4px;bottom:4px;background:#000;color:#fff;font-size:12px;font-weight:bold;padding:1px 4px}',
    '.card .t{display:block;font-weight:bold;font-size:15px;line-height:20px;margin-top:8px;max-height:40px;overflow:hidden}',
    '.by{display:block;color:#606060;font-size:13px;margin-top:4px}',
    '.by a{color:#606060}',
    '.row{clear:both;margin-bottom:16px;min-height:140px}',
    '.row .thumb{float:left;width:246px;height:138px;margin-right:16px}',
    '.row .thumb img{width:246px;height:138px}',
    '.row .t{display:block;font-size:17px;line-height:22px}',
    '.row .d{display:block;color:#606060;font-size:13px;margin-top:8px}',
    '.chan{clear:both;margin-bottom:16px;min-height:100px}',
    '.chan img{float:left;width:88px;height:88px;margin:0 16px 0 79px}',
    '.chan .t{display:block;font-size:17px;padding-top:16px}',
    '.watch{border-collapse:collapse}',
    '.watch td{vertical-align:top;padding:0}',
    '.left{width:800px}',
    '.right{width:400px;padding-left:24px !important}',
    '.player{background:#000;width:800px;height:450px;text-align:center}',
    '.player video,.player img{display:block;width:800px;height:450px}',
    '.vt{font-size:20px;font-weight:bold;line-height:26px;margin:12px 0 6px 0}',
    '.owner{margin:10px 0;min-height:44px}',
    '.owner img{float:left;width:40px;height:40px;margin-right:12px}',
    '.owner .n{display:block;font-weight:bold;font-size:15px}',
    '.owner .s{display:block;color:#606060;font-size:12px}',
    '.desc{background:#f2f2f2;padding:12px;line-height:20px;margin-top:12px}',
    '.desc .m{font-weight:bold;display:block;margin-bottom:6px}',
    '.side{height:94px;overflow:hidden;margin-bottom:10px}',
    '.side .thumb{float:left;width:168px;height:94px;margin-right:8px}',
    '.side .thumb img{width:168px;height:94px}',
    '.side .t{display:block;font-weight:bold;font-size:14px;line-height:18px;max-height:36px;overflow:hidden}',
    '.side .by{font-size:12px;margin-top:3px}',
    '.more{clear:both;text-align:center;padding:12px 0 24px 0}',
    '.more a,.btn{display:inline-block;background:#f2f2f2;padding:8px 18px;font-weight:bold}',
    '.note{background:#fff8e1;border:1px solid #ffe082;padding:12px;margin:12px 0}',
    '.err{background:#000;color:#fff;width:800px;height:450px}',
    '.err div{padding:190px 40px 0 40px;font-size:16px;text-align:center}',
    '.hdr{min-height:90px;margin:12px 0 4px 0}',
    '.hdr img{float:left;width:80px;height:80px;margin-right:16px}',
    '.hdr .n{display:block;font-size:24px;font-weight:bold;padding-top:8px}',
    '.hdr .s{display:block;color:#606060;margin-top:6px}',
    '.com{clear:both;margin:0 0 16px 0;min-height:44px}',
    '.com img{float:left;width:40px;height:40px;margin-right:12px}',
    '.com .b{margin-left:52px}',
    '.com .a{font-weight:bold;font-size:13px}',
    '.com .w{color:#606060;font-size:12px;margin-left:6px}',
    '.com .x{display:block;margin-top:3px;line-height:19px}',
    '.com .l{display:block;color:#606060;font-size:12px;margin-top:4px}',
    '.foot{clear:both;color:#909090;font-size:12px;padding:16px;border-top:1px solid #e5e5e5}',
    '.foot a{color:#606060}'
].join('\n');

// ---- helpers ---------------------------------------------------------------

function text(o) {
    if (o === undefined || o === null) return '';
    if (typeof o === 'string') return o;
    if (typeof o.simpleText === 'string') return o.simpleText;
    if (typeof o.content === 'string') return o.content;
    if (Array.isArray(o.runs)) return o.runs.map(function (r) { return r.text || ''; }).join('');
    return '';
}

function bestThumb(list, want) {
    if (!Array.isArray(list) || !list.length) return '';
    var best = list[0];
    for (var i = 0; i < list.length; i++) {
        if ((list[i].width || 0) >= want) { best = list[i]; break; }
        best = list[i];
    }
    var u = best.url || '';
    return u.indexOf('//') === 0 ? 'https:' + u : u;
}

// The thumbnail servers accept the plain per-video names, which stay small
function videoThumb(id) {
    return 'https://i.ytimg.com/vi/' + id + '/mqdefault.jpg';
}

function endpointUrl(ep) {
    var u = dig(ep, 'commandMetadata', 'webCommandMetadata', 'url');
    if (u) return u;
    var b = dig(ep, 'browseEndpoint');
    if (b) return b.canonicalBaseUrl || ('/channel/' + b.browseId);
    return '';
}

function bylineLink(o) {
    var run = dig(o, 'runs', 0);
    return { name: text(o), url: endpointUrl(dig(run, 'navigationEndpoint')) };
}

// ---- items: every renderer flavour is reduced to one shape ------------------

var ITEM_KEYS = {
    videoRenderer: true, compactVideoRenderer: true, gridVideoRenderer: true,
    playlistVideoRenderer: true, endScreenVideoRenderer: true, lockupViewModel: true,
    channelRenderer: true, gridChannelRenderer: true, playlistRenderer: true,
    gridPlaylistRenderer: true, shortsLockupViewModel: true, reelItemRenderer: true,
    continuationItemRenderer: true, videoCardRenderer: true
};

function fromVideoRenderer(v) {
    if (!v.videoId) return null;
    var by = bylineLink(v.ownerText || v.longBylineText || v.shortBylineText);
    var meta = [text(v.shortViewCountText) || text(v.viewCountText), text(v.publishedTimeText)]
        .filter(Boolean).join(' • ');
    if (!meta) meta = text(v.videoInfo);
    var dur = text(v.lengthText);
    if (!dur) {
        (v.thumbnailOverlays || []).forEach(function (o) {
            var t = text(dig(o, 'thumbnailOverlayTimeStatusRenderer', 'text'));
            if (t) dur = t;
        });
    }
    var snippet = text(dig(v, 'detailedMetadataSnippets', 0, 'snippetText')) || text(v.descriptionSnippet);
    return { type: 'video', id: v.videoId, title: text(v.title) || text(v.headline), by: by.name, byUrl: by.url,
             meta: meta, dur: dur, snippet: snippet,
             list: dig(v, 'navigationEndpoint', 'watchEndpoint', 'playlistId') };
}

function fromLockup(l) {
    var type = l.contentType || '';
    var md = dig(l, 'metadata', 'lockupMetadataViewModel') || {};
    var rows = dig(md, 'metadata', 'contentMetadataViewModel', 'metadataRows') || [];
    var lines = rows.map(function (r) {
        return (r.metadataParts || []).map(function (p) { return text(p.text); }).filter(Boolean);
    }).filter(function (r) { return r.length; });
    var dur = '';
    walkKeys(l.contentImage, { thumbnailBadgeViewModel: true }, function (k, b) {
        if (!dur && b.text) dur = b.text;
        return true;
    });
    var byUrl = endpointUrl(dig(md, 'image', 'decoratedAvatarViewModel', 'rendererContext',
        'commandContext', 'onTap', 'innertubeCommand'));
    var item = { title: text(md.title), by: (lines[0] || []).join(' '), byUrl: byUrl,
                 meta: (lines[1] || []).join(' • '), dur: dur };
    if (type === 'LOCKUP_CONTENT_TYPE_VIDEO') {
        item.type = 'video';
        item.id = l.contentId;
    } else if (/PLAYLIST|PODCAST|ALBUM/.test(type)) {
        item.type = 'playlist';
        item.id = l.contentId;
        item.first = dig(l, 'rendererContext', 'commandContext', 'onTap', 'innertubeCommand',
            'watchEndpoint', 'videoId');
        item.thumb = bestThumb(findKey(l.contentImage, 'sources'), 320);
        item.meta = lines.slice(1).map(function (r) { return r.join(' • '); }).join(' • ');
    } else {
        return null;
    }
    return item.id ? item : null;
}

function fromChannel(c) {
    if (!c.channelId) return null;
    return { type: 'channel', id: c.channelId, title: text(c.title),
             url: endpointUrl(c.navigationEndpoint) || ('/channel/' + c.channelId),
             thumb: bestThumb(dig(c, 'thumbnail', 'thumbnails'), 88),
             meta: [text(c.subscriberCountText), text(c.videoCountText)].filter(Boolean).join(' • '),
             snippet: text(c.descriptionSnippet) };
}

function fromPlaylist(p) {
    if (!p.playlistId) return null;
    var first = dig(p, 'navigationEndpoint', 'watchEndpoint', 'videoId');
    var by = bylineLink(p.longBylineText || p.shortBylineText);
    return { type: 'playlist', id: p.playlistId, first: first, title: text(p.title), by: by.name, byUrl: by.url,
             thumb: bestThumb(dig(p, 'thumbnails', 0, 'thumbnails') || dig(p, 'thumbnail', 'thumbnails'), 320),
             meta: text(p.videoCountText) || (p.videoCount ? p.videoCount + ' videos' : '') };
}

function fromShort(s) {
    var id = dig(s, 'onTap', 'innertubeCommand', 'reelWatchEndpoint', 'videoId') || s.videoId;
    if (!id && typeof s.entityId === 'string') id = s.entityId.replace(/^shorts-shelf-item-/, '');
    if (!id) return null;
    return { type: 'video', id: id, short: true,
             title: text(dig(s, 'overlayMetadata', 'primaryText')) || text(s.headline),
             meta: text(dig(s, 'overlayMetadata', 'secondaryText')) || text(s.viewCountText), by: '', dur: 'Shorts' };
}

// Collects items (and the first continuation token) below `root`, in order.
function collect(root, limit) {
    var out = { items: [], token: null };
    var seen = {};
    walkKeys(root, ITEM_KEYS, function (k, v) {
        if (limit && out.items.length >= limit) return true;
        var item = null;
        if (k === 'continuationItemRenderer') {
            var t = dig(v, 'continuationEndpoint', 'continuationCommand', 'token') ||
                dig(v, 'button', 'buttonRenderer', 'command', 'continuationCommand', 'token');
            if (t && !out.token) out.token = t;
            return true;
        } else if (k === 'lockupViewModel') {
            item = fromLockup(v);
        } else if (k === 'channelRenderer' || k === 'gridChannelRenderer') {
            item = fromChannel(v);
        } else if (k === 'playlistRenderer' || k === 'gridPlaylistRenderer') {
            item = fromPlaylist(v);
        } else if (k === 'shortsLockupViewModel' || k === 'reelItemRenderer') {
            item = fromShort(v);
        } else {
            item = fromVideoRenderer(v);
        }
        if (item && !seen[item.type + item.id]) {
            seen[item.type + item.id] = true;
            out.items.push(item);
        }
        return true;
    });
    return out;
}

// ---- InnerTube -------------------------------------------------------------

function api(endpoint, body, client) {
    var c = client || { id: '1', client: WEB_CLIENT };
    var ctx = { client: Object.assign({}, c.client, { hl: locale.hl, gl: locale.gl }) };
    var visitor = loadState('youtube-visitor', null);
    if (visitor) ctx.client.visitorData = visitor;
    var headers = {
        'Content-Type': 'application/json',
        'X-YouTube-Client-Name': c.id,
        'X-YouTube-Client-Version': c.client.clientVersion,
        'Origin': ORIGIN
    };
    if (c.ua) headers['User-Agent'] = c.ua;
    if (visitor) headers['X-Goog-Visitor-Id'] = visitor;
    var r = httpJson(ORIGIN + '/youtubei/v1/' + endpoint + '?prettyPrint=false', {
        headers: headers,
        body: JSON.stringify(Object.assign({ context: ctx }, body))
    });
    var vd = dig(r.json, 'responseContext', 'visitorData');
    if (vd && vd !== visitor) saveState('youtube-visitor', vd);
    return r.json;
}

// The page tells who YouTube thinks we are: visitor id and interface language
function rememberVisitor(html) {
    var m = /"VISITOR_DATA":"([^"]+)"/.exec(html);
    if (m && loadState('youtube-visitor', null) !== m[1]) saveState('youtube-visitor', m[1]);
    var hl = /"HL":"([A-Za-z-]+)"/.exec(html), gl = /"GL":"([A-Z]+)"/.exec(html);
    if (hl && gl && (hl[1] !== locale.hl || gl[1] !== locale.gl)) {
        locale = { hl: hl[1], gl: gl[1] };
        saveState('youtube-locale', locale);
    }
}

// ---- rendering -------------------------------------------------------------

function watchUrl(item) {
    return '/watch?v=' + encodeURIComponent(item.id) + (item.list ? '&list=' + encodeURIComponent(item.list) : '');
}

function byHtml(item) {
    var by = item.by ? (item.byUrl ? '<a href="' + esc(item.byUrl) + '">' + esc(item.by) + '</a>' : esc(item.by)) : '';
    var parts = [by, esc(item.meta)].filter(Boolean);
    return parts.length ? '<span class="by">' + parts.join('<br>') + '</span>' : '';
}

function thumbHtml(item, href) {
    var src = item.type === 'video' ? videoThumb(item.id) : (item.thumb || (item.first ? videoThumb(item.first) : ''));
    return '<a class="thumb" href="' + esc(href) + '">' +
        (src ? '<img src="' + esc(src) + '" alt="">' : '') +
        (item.dur ? '<span class="dur">' + esc(item.dur) + '</span>' : '') + '</a>';
}

function itemHref(item) {
    if (item.type === 'video') return watchUrl(item);
    if (item.type === 'playlist')
        return item.first ? '/watch?v=' + encodeURIComponent(item.first) + '&list=' + encodeURIComponent(item.id)
                          : '/playlist?list=' + encodeURIComponent(item.id);
    return item.url;
}

function cardHtml(item) {
    var href = itemHref(item);
    if (item.type === 'channel') {
        return '<div class="card"><a class="thumb" href="' + esc(href) + '"><img src="' + esc(item.thumb) +
            '" alt="" style="width:180px;height:180px;margin:0 70px"></a><a class="t" href="' + esc(href) + '">' +
            esc(item.title) + '</a><span class="by">' + esc(item.meta) + '</span></div>';
    }
    return '<div class="card">' + thumbHtml(item, href) + '<a class="t" href="' + esc(href) + '">' +
        esc(item.title) + '</a>' + byHtml(item) + '</div>';
}

function rowHtml(item) {
    var href = itemHref(item);
    if (item.type === 'channel') {
        return '<div class="chan"><a href="' + esc(href) + '"><img src="' + esc(item.thumb) + '" alt=""></a>' +
            '<a class="t" href="' + esc(href) + '">' + esc(item.title) + '</a><span class="by">' +
            esc(item.meta) + '</span><span class="d">' + esc(item.snippet) + '</span></div>';
    }
    return '<div class="row">' + thumbHtml(item, href) + '<a class="t" href="' + esc(href) + '">' +
        esc(item.title) + '</a>' + byHtml(item) +
        (item.snippet ? '<span class="d">' + esc(item.snippet) + '</span>' : '') + '</div>';
}

function sideHtml(item) {
    var href = itemHref(item);
    return '<div class="side">' + thumbHtml(item, href) + '<a class="t" href="' + esc(href) + '">' +
        esc(item.title) + '</a>' + byHtml(item) + '</div>';
}

function moreHtml(loc, token, kind, label) {
    if (!token) return '';
    var q = Object.assign({}, loc.query, { ns_c: token, ns_k: kind });
    return '<div class="more"><a href="' + esc(loc.path + '?' + buildQuery(q)) + '">' + esc(T(label || 'Show more')) + '</a></div>';
}

function shell(title, query, body, activeFeed) {
    var tabs = '<a href="/"' + (activeFeed === 'home' ? ' class="on"' : '') + '>' + esc(T('Home')) + '</a>' +
        FEEDS.map(function (f) {
            return '<a href="/?ns_feed=' + f.key + '"' + (activeFeed === f.key ? ' class="on"' : '') + '>' + esc(T(f.title)) + '</a>';
        }).join('');
    return page({
        title: title ? title + ' - YouTube' : 'YouTube',
        css: CSS,
        body: '<div class="top"><a class="logo" href="/"><span class="play">&#9654;</span>YouTube</a>' +
            '<form class="search" action="/results" method="get">' +
            '<input class="q" type="text" name="search_query" value="' + esc(query || '') + '">' +
            '<input class="go" type="submit" value="' + esc(T('Search')) + '"></form></div>' +
            '<div class="tabs">' + tabs + '</div>' +
            '<div class="main">' + body + '</div>' +
            '<div class="foot">' + esc(T('YouTube through NetSurf site scripts')) + ' &middot; ' +
            '<a href="?ns_raw=1">' + esc(T('original page')) + '</a></div>'
    });
}

function gridHtml(items) {
    return '<div class="grid">' + items.map(cardHtml).join('') + '</div>';
}

// ---- pages -----------------------------------------------------------------

function continuationItems(json) {
    var roots = [];
    ['onResponseReceivedCommands', 'onResponseReceivedActions', 'onResponseReceivedEndpoints'].forEach(function (k) {
        (json[k] || []).forEach(function (a) {
            var c = dig(a, 'appendContinuationItemsAction', 'continuationItems') ||
                dig(a, 'reloadContinuationItemsCommand', 'continuationItems');
            if (c) roots.push(c);
        });
    });
    if (json.continuationContents) roots.push(json.continuationContents);
    return roots;
}

function continuationPage(req, title, query, asRows) {
    var kind = req.loc.query.ns_k || 'browse';
    var json = api(kind === 'search' ? 'search' : (kind === 'next' ? 'next' : 'browse'),
        { continuation: req.loc.query.ns_c });
    var got = collect(continuationItems(json));
    var body = '<h1>' + esc(title) + '</h1>' +
        (got.items.length ? (asRows ? got.items.map(rowHtml).join('') : gridHtml(got.items))
                          : '<p>' + esc(T('Nothing more to show.')) + '</p>') +
        moreHtml(req.loc, got.token, kind);
    return shell(title, query, body);
}

// Signed out, YouTube's own start page is empty.  Ours shows what the last
// watched video led to (kept by watchPage) and offers a few topic feeds.
function homePage(req) {
    var key = req.loc.query.ns_feed;
    var last = key ? null : loadState('youtube-home', null);
    if (last && last.items && last.items.length) {
        return shell('', '', '<h1>' + esc(T('Recommended')) + '</h1><p class="by">' + esc(T('Because you watched')) +
            ' <a href="/watch?v=' + esc(last.id) + '">' + esc(last.title) + '</a></p>' + gridHtml(last.items), 'home');
    }
    // without history: the first topic feed that has something to show here
    var feed = FEEDS.filter(function (f) { return f.key === key; })[0], got = { items: [] };
    var tries = feed ? [feed] : [FEEDS[1], FEEDS[2], FEEDS[0]];
    for (var i = 0; i < tries.length && got.items.length < 8; i++) {
        feed = tries[i];
        got = collect(api('browse', { browseId: feed.browseId }).contents, 60);
    }
    var body = '<h1>' + esc(T(feed.title)) + '</h1>' + (got.items.length ? gridHtml(got.items)
        : '<p class="note">' + esc(T('YouTube returned an empty feed. Use the search box above.')) + '</p>');
    return shell('', '', body, key ? feed.key : 'home');
}

function searchPage(req, data) {
    var q = req.loc.query.search_query || req.loc.query.q || '';
    if (req.loc.query.ns_c) return continuationPage(req, T('Results for') + ' “' + q + '”', q, true);
    var got = collect(dig(data, 'contents', 'twoColumnSearchResultsRenderer') || data);
    var body = got.items.length ? got.items.map(rowHtml).join('')
        : '<p class="note">' + esc(T('No results for')) + ' “' + esc(q) + '”.</p>';
    return shell(q, q, body + moreHtml(req.loc, got.token, 'search', 'More results'));
}

function channelHeader(data) {
    var meta = dig(data, 'metadata', 'channelMetadataRenderer') || {};
    var vm = dig(data, 'header', 'pageHeaderRenderer', 'content', 'pageHeaderViewModel') || {};
    var rows = dig(vm, 'metadata', 'contentMetadataViewModel', 'metadataRows') || [];
    var facts = rows.map(function (r) {
        return (r.metadataParts || []).map(function (p) { return text(p.text); }).filter(Boolean).join(' • ');
    }).filter(Boolean).join(' • ');
    var avatar = bestThumb(dig(meta, 'avatar', 'thumbnails'), 80);
    var title = meta.title || text(dig(vm, 'title', 'dynamicTextViewModel', 'text')) ||
        text(dig(data, 'header', 'pageHeaderRenderer', 'pageTitle'));
    var base = meta.vanityChannelUrl ? meta.vanityChannelUrl.replace(/^https?:\/\/[^\/]+/, '') :
        (meta.externalId ? '/channel/' + meta.externalId : '');
    return { title: title, base: base, html: '<div class="hdr">' + (avatar ? '<img src="' + esc(avatar) + '" alt="">' : '') +
        '<span class="n">' + esc(title) + '</span><span class="s">' + esc(facts) + '</span></div>' };
}

function browsePage(req, data) {
    var hdr = channelHeader(data);
    if (req.loc.query.ns_c) return continuationPage(req, hdr.title || 'YouTube', '');
    var tabs = dig(data, 'contents', 'twoColumnBrowseResultsRenderer', 'tabs') || [];
    var tabsHtml = '', content = null;
    tabs.forEach(function (t) {
        var r = t.tabRenderer;
        if (!r) return;
        var url = endpointUrl(r.endpoint);
        if (r.title && url) tabsHtml += '<a href="' + esc(url) + '"' + (r.selected ? ' class="on"' : '') + '>' + esc(r.title) + '</a>';
        if (r.selected || (!content && r.content)) content = r.content || content;
    });
    var got = collect(content || data.contents);
    var body = hdr.html + (tabsHtml ? '<div class="tabs" style="padding-left:0">' + tabsHtml + '</div>' : '') +
        (got.items.length ? gridHtml(got.items) : '<p class="note">' + esc(T('Nothing to show on this page.')) + '</p>') +
        moreHtml(req.loc, got.token, 'browse');
    return shell(hdr.title, '', body);
}

function playlistPage(req, data) {
    if (req.loc.query.ns_c) return continuationPage(req, T('Playlist'), '', true);
    var title = text(dig(data, 'metadata', 'playlistMetadataRenderer', 'title')) ||
        text(dig(data, 'header', 'pageHeaderRenderer', 'pageTitle')) || T('Playlist');
    var got = collect(dig(data, 'contents') || data);
    got.items.forEach(function (i) { if (i.type === 'video') i.list = req.loc.query.list; });
    return shell(title, '', '<h1>' + esc(title) + '</h1>' + got.items.map(rowHtml).join('') +
        moreHtml(req.loc, got.token, 'browse'));
}

// Finds a stream NetSurf's player can use: one file with both picture and sound.
function pickStream(videoId) {
    var last = null;
    for (var i = 0; i < PLAYER_CLIENTS.length; i++) {
        var json;
        try {
            json = api('player', { videoId: videoId, contentCheckOk: true, racyCheckOk: true }, PLAYER_CLIENTS[i]);
        } catch (e) {
            last = { reason: e.message };
            continue;
        }
        var status = dig(json, 'playabilityStatus', 'status');
        var details = json.videoDetails || {};
        if (status !== 'OK') {
            last = { reason: dig(json, 'playabilityStatus', 'reason') ||
                text(dig(json, 'playabilityStatus', 'errorScreen', 'playerErrorMessageRenderer', 'subreason')) ||
                status || 'Not playable', details: details };
            continue;
        }
        var formats = (dig(json, 'streamingData', 'formats') || []).filter(function (f) {
            return f.url && /^video\/mp4/.test(f.mimeType || '');
        });
        // prefer 360p: the decoder runs in software
        formats.sort(function (a, b) {
            return Math.abs((a.height || 360) - 360) - Math.abs((b.height || 360) - 360);
        });
        if (formats.length) return { url: formats[0].url, format: formats[0], details: details, client: PLAYER_CLIENTS[i] };
        last = { reason: T(details.isLive ? 'Live streams are not supported yet.'
                 : 'This video has no stream NetSurf can play.'), details: details };
    }
    return last || { reason: 'Not playable' };
}

function watchPage(req, data) {
    var id = req.loc.query.v;
    if (req.loc.query.ns_c) return commentsPage(req, data);

    var results = dig(data, 'contents', 'twoColumnWatchNextResults', 'results', 'results', 'contents') || [];
    var primary = findKey(results, 'videoPrimaryInfoRenderer') || {};
    var secondary = findKey(results, 'videoSecondaryInfoRenderer') || {};
    var stream = pickStream(id);
    var details = stream.details || {};

    var title = text(primary.title) || details.title || 'YouTube';
    var views = text(dig(primary, 'viewCount', 'videoViewCountRenderer', 'viewCount')) ||
        (details.viewCount ? Number(details.viewCount).toLocaleString('en-US') + ' views' : '');
    var date = text(primary.dateText);
    var owner = dig(secondary, 'owner', 'videoOwnerRenderer') || {};
    var ownerName = text(owner.title) || details.author || '';
    var ownerUrl = endpointUrl(owner.navigationEndpoint) || (details.channelId ? '/channel/' + details.channelId : '');
    var desc = dig(secondary, 'attributedDescription', 'content') || details.shortDescription || '';
    var likes = '';
    walkKeys(primary.videoActions, { likeButtonViewModel: true }, function (k, v) {
        var t = findKey(v, 'defaultButtonViewModel');
        if (!likes) likes = dig(t, 'buttonViewModel', 'title') || '';
        return true;
    });

    var player;
    if (stream.url) {
        var w = stream.format.width || 640, h = stream.format.height || 360;
        player = '<div class="player"><video src="' + esc(stream.url) + '" poster="' +
            esc('https://i.ytimg.com/vi/' + id + '/hqdefault.jpg') + '" width="800" height="450" autoplay controls ' +
            'data-duration="' + esc(details.lengthSeconds || '') + '" data-size="' + w + 'x' + h + '"></video></div>';
    } else {
        player = '<div class="err"><div>' + esc(stream.reason) + '</div></div>';
    }

    var commentsToken = null;
    results.forEach(function (r) {
        var s = r.itemSectionRenderer;
        if (s && /comment/.test(s.sectionIdentifier || '') && !commentsToken) commentsToken = collect(s).token;
    });

    var related = collect(dig(data, 'contents', 'twoColumnWatchNextResults', 'secondaryResults'), 30);
    if (related.items.length)
        saveState('youtube-home', { id: id, title: title, items: related.items.slice(0, 24) });
    var left = player + '<div class="vt">' + esc(title) + '</div>' +
        '<div class="owner">' + (owner.thumbnail ? '<img src="' + esc(bestThumb(dig(owner, 'thumbnail', 'thumbnails'), 40)) + '" alt="">' : '') +
        '<a class="n" href="' + esc(ownerUrl) + '">' + esc(ownerName) + '</a><span class="s">' +
        esc(text(owner.subscriberCountText)) + '</span></div>' +
        '<div class="desc"><span class="m">' + esc([views, date, likes ? likes + ' ' + T('likes') : ''].filter(Boolean).join(' • ')) +
        '</span>' + linkify(desc) + '</div>' +
        (commentsToken ? moreHtml(req.loc, commentsToken, 'comments', 'Show comments') : '');
    var right = related.items.map(sideHtml).join('');
    return shell(title, '', '<table class="watch"><tr><td class="left">' + left + '</td><td class="right">' + right + '</td></tr></table>');
}

function commentsPage(req, data) {
    var id = req.loc.query.v;
    var json = api('next', { continuation: req.loc.query.ns_c });
    var comments = [], token = null;
    var mutations = dig(json, 'frameworkUpdates', 'entityBatchUpdate', 'mutations') || [];
    mutations.forEach(function (m) {
        var p = dig(m, 'payload', 'commentEntityPayload');
        if (!p) return;
        comments.push({
            author: dig(p, 'author', 'displayName') || '',
            avatar: dig(p, 'author', 'avatarThumbnailUrl') || '',
            when: dig(p, 'properties', 'publishedTime') || '',
            text: dig(p, 'properties', 'content', 'content') || '',
            likes: dig(p, 'toolbar', 'likeCountNotliked') || '',
            replies: dig(p, 'toolbar', 'replyCount') || ''
        });
    });
    continuationItems(json).forEach(function (items) {
        (Array.isArray(items) ? items : []).forEach(function (it) {
            var t = dig(it, 'continuationItemRenderer', 'continuationEndpoint', 'continuationCommand', 'token');
            if (t) token = t;
        });
    });
    var primary = findKey(data, 'videoPrimaryInfoRenderer') || {};
    var title = text(primary.title) || 'Video';
    var body = '<h1>' + esc(T('Comments')) + ': <a href="/watch?v=' + esc(id) + '">' + esc(title) + '</a></h1>' +
        (comments.length ? comments.map(function (c) {
            return '<div class="com">' + (c.avatar ? '<img src="' + esc(c.avatar) + '" alt="">' : '') +
                '<div class="b"><span class="a">' + esc(c.author) + '</span><span class="w">' + esc(c.when) +
                '</span><span class="x">' + linkify(c.text) + '</span><span class="l">' +
                esc([c.likes ? c.likes + ' ' + T('likes') : '', c.replies ? c.replies + ' ' + T('replies') : ''].filter(Boolean).join(' • ')) +
                '</span></div></div>';
        }).join('') : '<p class="note">' + esc(T('No comments to show.')) + '</p>') +
        moreHtml(req.loc, token, 'comments', 'More comments');
    return shell(T('Comments') + ': ' + title, '', body);
}

function redirect(url) {
    return page({ title: 'YouTube', body: '<meta http-equiv="refresh" content="0;url=' + esc(url) + '"><p><a href="' +
        esc(url) + '">Continue</a></p>' });
}

registerSite({
    name: 'youtube',
    hosts: ['youtube.com', 'youtu.be', 'youtube-nocookie.com'],
    transform: function (req) {
        var loc = req.loc, path = loc.path;
        if (loc.host === 'youtu.be') return null; // the server redirects to /watch
        if (/^(accounts|consent|music|studio|tv|kids)\./.test(loc.host)) return null;
        if (/^\/(youtubei|api|s|img|generate_204|signin|account|t\/|about|howyoutubeworks|premium)/.test(path)) return null;

        var m = /^\/(?:shorts|embed|live|v)\/([\w-]{11})/.exec(path);
        if (m) return redirect('/watch?v=' + m[1]);

        rememberVisitor(req.html);
        if (path === '/' || path === '/index') return homePage(req);

        var data = jsonAfter(req.html, 'var ytInitialData = ') || jsonAfter(req.html, 'window["ytInitialData"] = ');
        if (path === '/watch' && loc.query.v) return watchPage(req, data || {});
        if (!data) return null;
        if (path === '/results') return searchPage(req, data);
        if (path === '/playlist') return playlistPage(req, data);
        if (dig(data, 'contents', 'twoColumnBrowseResultsRenderer') || loc.query.ns_c) return browsePage(req, data);
        return null;
    }
});

})();
