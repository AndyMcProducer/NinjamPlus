const http = require('http');
const url = require('url');
const fs = require('fs');
const path = require('path');
let WebSocket = null;
try {
    WebSocket = require('ws');
} catch (e) {
    console.warn("Optional module 'ws' not found; using HTTP polling mode.");
}

const helperPagePath = path.join(__dirname, 'index.html');
const appHtmlPath = path.join(__dirname, 'app.html');
const intervalsJsonPath = path.join(__dirname, 'intervals.json');

const htmlPage = `
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <link rel="icon" type="image/png" href="/icon.png">
    <title>NINJAM Advanced VDO Client</title>
    <style>
        * { box-sizing: border-box; }
        html, body { width: 100%; height: 100%; margin: 0; }
        body {
            background: #0a0b10;
            color: #e7eaf0;
            font-family: "Segoe UI", Arial, sans-serif;
        }
        .app {
            width: 100%;
            height: 100%;
            display: grid;
            grid-template-rows: auto 1fr;
        }
        .header {
            background: linear-gradient(180deg, #171a22 0%, #10131a 100%);
            border-bottom: 1px solid #2a3040;
            padding: 10px 14px;
            display: grid;
            gap: 4px;
        }
        .title {
            font-size: 14px;
            font-weight: 600;
            letter-spacing: 0.2px;
        }
        .meta {
            font-size: 13px;
            color: #aeb7c9;
            display: flex;
            gap: 14px;
            flex-wrap: wrap;
        }
        .meta a {
            color: #6fb1ff;
            text-decoration: none;
            cursor: pointer;
        }
        .frame-wrap {
            width: 100%;
            height: 100%;
            background: #000;
        }
        .renderer-wrap {
            width: 100%;
            height: 100%;
            display: flex;
            flex-direction: column;
            gap: 8px;
            padding: 8px 12px;
            overflow: auto;
        }
        .renderer-info {
            font-size: 13px;
            color: #c4cad8;
        }
        .renderer-grid {
            display: grid;
            grid-template-columns: repeat(auto-fill, minmax(320px, 1fr));
            gap: 10px;
        }
        .renderer-item {
            font-size: 12px;
            padding: 6px 8px;
            border-radius: 4px;
            background: rgba(20, 24, 34, 0.9);
            border: 1px solid #2b3244;
            color: #e7eaf0;
        }
        .peer-card {
            border-radius: 6px;
            overflow: hidden;
            border: 1px solid #2b3244;
            background: rgba(12, 15, 22, 0.95);
            min-height: 220px;
            display: grid;
            grid-template-rows: auto 1fr;
        }
        .peer-head {
            font-size: 12px;
            color: #d4daea;
            padding: 6px 8px;
            display: flex;
            justify-content: space-between;
            align-items: center;
            flex-wrap: wrap;
            gap: 6px;
            border-bottom: 1px solid #2b3244;
            background: rgba(20, 24, 34, 0.95);
        }
        .peer-foot {
            font-size: 11px;
            color: #b8c2d8;
            padding: 6px 8px;
            display: flex;
            justify-content: space-between;
            align-items: center;
            gap: 8px;
            border-top: 1px solid #2b3244;
            background: rgba(20, 24, 34, 0.95);
        }
        .peer-cam {
            white-space: nowrap;
            overflow: hidden;
            text-overflow: ellipsis;
        }
        .peer-controls {
            display: flex;
            align-items: center;
            gap: 6px;
            flex-shrink: 0;
        }
        .peer-offset {
            min-width: 62px;
            text-align: right;
            color: #9dc0ff;
        }
        .peer-btn {
            border: 1px solid #3a455f;
            background: #1a2233;
            color: #e7eaf0;
            border-radius: 4px;
            font-size: 11px;
            line-height: 1;
            padding: 4px 6px;
            cursor: pointer;
        }
        .peer-btn:hover {
            background: #26324b;
        }
        .peer-frame {
            width: 100%;
            height: 100%;
            min-height: 180px;
            border: 0;
            display: block;
            background: #000;
        }
        #ninjamOverlayLayer {
            position: fixed;
            left: 0;
            top: 0;
            width: 100%;
            height: 100%;
            pointer-events: none;
            z-index: 20;
        }
        .overlay-item {
            position: absolute;
            font-size: 11px;
            padding: 2px 4px;
            border-radius: 3px;
            background: rgba(0, 0, 0, 0.75);
            color: #e7eaf0;
        }
        #vdoFrame {
            width: 100%;
            height: 100%;
            border: 0;
            display: block;
        }
        body.green-mode {
            background: #00ff00;
        }
        body.green-mode .app {
            grid-template-rows: 1fr;
        }
        body.green-mode .header {
            display: none;
        }
        body.green-mode .frame-wrap {
            background: #00ff00;
        }
    </style>
</head>
<body>
    <div class="app">
        <div class="header" id="header">
            <div class="title">Advanced VDO client (hosted)</div>
            <div class="meta">
                <span id="roomValue"></span>
                <span id="labelValue"></span>
                <span id="syncValue"></span>
                <a id="openRendererLink">Buffer view</a>
            </div>
        </div>
        <div class="frame-wrap" id="hostedWrap">
            <iframe id="vdoFrame" allow="camera; microphone; fullscreen"></iframe>
        </div>
        <div class="renderer-wrap" id="rendererWrap">
            <div class="renderer-info" id="rendererInfo"></div>
            <div class="renderer-grid" id="rendererGrid"></div>
        </div>
    </div>
    <div id="ninjamOverlayLayer"></div>
    <script>
        const query = new URLSearchParams(window.location.search);
        const ninjamServer = query.get('ninjamServer') || '';
        const roomParam = query.get('room');
        const roomFromServer = ninjamServer ? ninjamServer.replace(/\./g, '_') : '';
        const resolvedRoom = roomParam || roomFromServer || 'ninjam';
        const view = query.get('view') || '';
        const minimumBufferMs = 200;
        const appState = {
            room: resolvedRoom,
            label: query.get('label') || 'NINJAM',
            store: query.get('store') || '',
            bg: query.get('bg') || '',
            intervalSource: query.get('intervalSource') || '',
            intervalPollMs: parseInt(query.get('intervalPollMs') || '', 10) || 500,
            requestedBufferMs: Math.max(minimumBufferMs, parseInt(query.get('buffer') || '', 10) || minimumBufferMs),
            chunkedMs: parseInt(query.get('chunked') || '', 10) || 120,
            ninjamServer: ninjamServer,
            view: view,
            intervalMessages: 0
        };

        const ninjamSync = {
            intervalInfo: null,
            remoteTimecodes: {},
            remoteBuffers: {},
            userLastSeenAt: {},
            userLastSampleKey: {},
            setIntervalInfo(interval, pos, bpm, bpi, length) {
                this.intervalInfo = {
                    interval,
                    pos,
                    bpm,
                    bpi,
                    length: typeof length === 'number' ? length : 0,
                    updatedAt: Date.now()
                };
                console.log('ninjamSync.setIntervalInfo', this.intervalInfo);
            },
            setRemoteTimecode(userId, interval, timecode, bufferMs, sampleKey) {
                const u = normalizeUserId(userId);
                if (!u) {
                    return;
                }
                if (this.userLastSampleKey[u] === sampleKey) {
                    return;
                }
                this.userLastSampleKey[u] = sampleKey;
                if (!this.remoteTimecodes[u]) {
                    this.remoteTimecodes[u] = {};
                }
                this.remoteTimecodes[u][interval] = {
                    timecode,
                    updatedAt: Date.now()
                };
                if (typeof bufferMs === 'number' && isFinite(bufferMs) && bufferMs >= 0) {
                    this.remoteBuffers[u] = bufferMs;
                }
                this.userLastSeenAt[u] = Date.now();
                console.log('ninjamSync.setRemoteTimecode', {
                    userId: u,
                    interval,
                    timecode,
                    bufferMs
                });
            }
        };
        const remoteUserStaleMs = 12000;
        const vdoRefreshState = {
            lastRefreshIdByUser: {},
            refreshTimersByUser: {},
            refreshPendingUntilByUser: {}
        };

        if (!appState.intervalSource) {
            appState.intervalSource = window.location.origin + '/intervals';
        }

        function updateGuestOverlays() {
            if (appState.view !== 'renderer') {
                return;
            }
            const overlayLayer = document.getElementById('ninjamOverlayLayer');
            overlayLayer.innerHTML = '';
        }

        function updateRendererView() {
            if (appState.view !== 'renderer') {
                return;
            }
            const infoEl = document.getElementById('rendererInfo');
            const gridEl = document.getElementById('rendererGrid');
            if (!infoEl || !gridEl) {
                return;
            }
            let infoText = 'Per-user buffer control active';
            infoText += ' | updates ' + String(appState.intervalMessages);
            infoText += ' | Per-user offset controls are on each tile header';
            infoEl.textContent = infoText;
            const users = Object.keys(ninjamSync.remoteTimecodes);
            if (!users.length) {
                gridEl.innerHTML = '';
                return;
            }
        }

        function handleIntervalMessage(data) {
            if (!data || typeof data !== 'object') {
                return false;
            }
            appState.intervalMessages += 1;
            if (data.type === 'intervalInfo') {
                ninjamSync.setIntervalInfo(
                    data.interval,
                    data.pos,
                    data.bpm,
                    data.bpi,
                    typeof data.length === 'number' ? data.length : 0
                );
                updateRendererView();
                updateGuestOverlays();
                updateSyncStatus();
                return false;
            } else if (data.type === 'remoteTimecode') {
                const receiverBufferMs = typeof data.receiverBufferMs === 'number'
                    ? data.receiverBufferMs
                    : ((typeof data.measuredAudioDelayMs === 'number')
                        ? data.measuredAudioDelayMs
                        : (typeof data.bufferTotalMs === 'number' ? data.bufferTotalMs : NaN));
                ninjamSync.setRemoteTimecode(
                    data.userId,
                    data.interval,
                    data.timecode,
                    receiverBufferMs,
                    buildUserSampleKey(data, receiverBufferMs)
                );
                if (data.refreshBuffer && data.bufferRefreshEventId) {
                    scheduleBufferRefresh(data.userId, data.bufferRefreshEventId);
                }
                updateRendererView();
                updateGuestOverlays();
                return false;
            } else if (data.type === 'videoTimecode') {
                const receiverBufferMs = typeof data.receiverBufferMs === 'number'
                    ? data.receiverBufferMs
                    : ((typeof data.measuredAudioDelayMs === 'number')
                        ? data.measuredAudioDelayMs
                        : (typeof data.bufferTotalMs === 'number' ? data.bufferTotalMs : NaN));
                ninjamSync.setRemoteTimecode(
                    data.userId || data.userKey || '',
                    data.interval,
                    data.timecode,
                    receiverBufferMs,
                    buildUserSampleKey(data, receiverBufferMs)
                );
                if (data.refreshBuffer && data.bufferRefreshEventId) {
                    scheduleBufferRefresh(data.userId || data.userKey || '', data.bufferRefreshEventId);
                }
                updateRendererView();
                updateGuestOverlays();
                return false;
            }
            return false;
        }

        function pruneInactiveUsers() {
            const now = Date.now();
            const users = Object.keys(ninjamSync.userLastSeenAt || {});
            for (let i = 0; i < users.length; i++) {
                const userId = users[i];
                const lastSeen = ninjamSync.userLastSeenAt[userId] || 0;
                if ((now - lastSeen) > remoteUserStaleMs) {
                    delete ninjamSync.userLastSeenAt[userId];
                    delete ninjamSync.userLastSampleKey[userId];
                    delete ninjamSync.remoteTimecodes[userId];
                    delete ninjamSync.remoteBuffers[userId];
                    delete vdoRefreshState.lastRefreshIdByUser[userId];
                    if (vdoRefreshState.refreshTimersByUser[userId]) {
                        clearTimeout(vdoRefreshState.refreshTimersByUser[userId]);
                    }
                    delete vdoRefreshState.refreshTimersByUser[userId];
                    delete vdoRefreshState.refreshPendingUntilByUser[userId];
                }
            }
        }

        function buildUserSampleKey(data, bufferMs) {
            return [
                String(data.interval ?? ''),
                String(data.syncTag ?? ''),
                String(data.videoClockMs ?? ''),
                String(data.timecode ?? ''),
                String(data.eventId ?? ''),
                String(data.refreshBuffer ? 1 : 0),
                String(data.bufferRefreshEventId ?? ''),
                (typeof bufferMs === 'number' && isFinite(bufferMs)) ? String(Math.round(bufferMs)) : ''
            ].join('|');
        }

        function processIntervalPayload(payload) {
            const items = Array.isArray(payload) ? payload : [payload];
            for (let i = 0; i < items.length; i++) {
                const msg = items[i] || {};
                handleIntervalMessage(msg);
            }
            pruneInactiveUsers();
            applyRecommendedBuffer(false);
        }

        function startIntervalSource() {
            if (!appState.intervalSource) {
                return;
            }
            if (appState.intervalSource.startsWith('ws:') || appState.intervalSource.startsWith('wss:')) {
                let wsReconnectDelay = 250;
                const wsMaxReconnectDelay = 4000;
                function connectWs() {
                    try {
                        const ws = new WebSocket(appState.intervalSource);
                        ws.onopen = function () {
                            console.log('WebSocket connected to', appState.intervalSource);
                            wsReconnectDelay = 250;
                        };
                        ws.onmessage = function (event) {
                            try {
                                const data = JSON.parse(event.data);
                                processIntervalPayload(data);
                            } catch (e) {
                            }
                        };
                        ws.onclose = function () {
                            console.log('WebSocket closed, reconnecting in', wsReconnectDelay, 'ms');
                            setTimeout(connectWs, wsReconnectDelay);
                            wsReconnectDelay = Math.min(wsReconnectDelay * 2, wsMaxReconnectDelay);
                        };
                        ws.onerror = function () {
                            try { ws.close(); } catch (e) {}
                        };
                    } catch (e) {
                        setTimeout(connectWs, wsReconnectDelay);
                        wsReconnectDelay = Math.min(wsReconnectDelay * 2, wsMaxReconnectDelay);
                    }
                }
                connectWs();
            } else {
                function tick() {
                    fetch(appState.intervalSource, { cache: 'no-store' })
                        .then(function (response) {
                            return response.json();
                        })
                        .then(function (payload) {
                            processIntervalPayload(payload);
                        })
                        .catch(function () {
                        })
                        .finally(function () {
                            setTimeout(tick, appState.intervalPollMs);
                        });
                }
                tick();
            }
        }

        function buildBaseUrl() {
            return window.location.origin + window.location.pathname;
        }

        function buildCameraUrl() {
            const params = new URLSearchParams();
            params.set('room', appState.room);
            params.set('label', appState.label);
            if (appState.bg) params.set('bg', appState.bg);
            if (appState.store) params.set('store', appState.store);
            if (appState.ninjamServer) params.set('ninjamServer', appState.ninjamServer);
            return buildBaseUrl() + '?' + params.toString();
        }

        function buildRendererUrl() {
            const params = new URLSearchParams();
            params.set('room', appState.room);
            params.set('label', appState.label);
            params.set('view', 'renderer');
            if (appState.bg) params.set('bg', appState.bg);
            if (appState.store) params.set('store', appState.store);
            if (appState.ninjamServer) params.set('ninjamServer', appState.ninjamServer);
            return buildBaseUrl() + '?' + params.toString();
        }

        window.ninjamClient = { appState, ninjamSync, buildCameraUrl, buildRendererUrl };

        const hostedWrap = document.getElementById('hostedWrap');
        const rendererWrap = document.getElementById('rendererWrap');
        const titleEl = document.querySelector('.title');
        if (appState.view === 'renderer') {
            if (hostedWrap) {
                hostedWrap.style.display = 'none';
            }
            if (rendererWrap) {
                rendererWrap.style.display = 'block';
            }
            if (titleEl) {
                titleEl.textContent = 'NINJAM renderer view';
            }
        } else {
            if (rendererWrap) {
                rendererWrap.style.display = 'none';
            }
        }

        if (appState.bg.toLowerCase() === 'green') {
            document.body.classList.add('green-mode');
        }

        document.getElementById('roomValue').textContent = 'Room: ' + appState.room;
        document.getElementById('labelValue').textContent = 'Label: ' + appState.label;
        function updateSyncStatus() {
            const syncEl = document.getElementById('syncValue');
            if (!syncEl) {
                return;
            }
            const userCount = Object.keys(ninjamSync.remoteBuffers || {}).length;
            const msgCount = appState.intervalMessages;
            const bufferText = appliedBufferMs >= 0 ? String(appliedBufferMs) + 'ms' : 'pending';
            syncEl.textContent = 'AutoBuffer: ' + bufferText + ' peers: ' + userCount + ' updates: ' + msgCount;
        }

        const rendererLink = document.getElementById('openRendererLink');
        if (rendererLink) {
            rendererLink.addEventListener('click', function (e) {
                e.preventDefault();
                try {
                    const url = buildRendererUrl();
                    const win = window.open(url, '_blank');
                    if (!win) {
                        window.location.href = url;
                    }
                } catch (err) {
                }
            });
        }

        function getRecommendedBufferMs() {
            let maxBuffer = appState.requestedBufferMs;
            const userIds = Object.keys(ninjamSync.remoteBuffers || {});
            for (let i = 0; i < userIds.length; i++) {
                const userId = userIds[i];
                const base = ninjamSync.remoteBuffers[userId];
                if (typeof base !== 'number' || !isFinite(base)) {
                    continue;
                }
                const withOffset = Math.max(0, Math.round(base) + getManualOffsetMs(userId));
                maxBuffer = Math.max(maxBuffer, withOffset);
            }
            return Math.max(minimumBufferMs, maxBuffer);
        }

        function buildIframeUrl(bufferMs) {
            const iframeParams = new URLSearchParams();
            iframeParams.set('room', appState.room);
            iframeParams.set('label', appState.label);
            iframeParams.set('chunked', String(Math.max(60, appState.chunkedMs || 120)));
            iframeParams.set('chunkbufferadaptive', '0');
            iframeParams.set('chunkbufferceil', '180000');
            iframeParams.set('buffer2', '0');
            // Join at the minimum buffer and let the sync layer apply the actual
            // delay after load so the same value is not counted twice.
            iframeParams.set('buffer', String(minimumBufferMs));
            return 'https://vdo.ninja/?' + iframeParams.toString();
        }

        function buildPeerIframeUrl(userId, bufferMs) {
            const iframeParams = new URLSearchParams();
            iframeParams.set('room', appState.room);
            iframeParams.set('view', String(userId));
            iframeParams.set('chunked', String(Math.max(60, appState.chunkedMs || 120)));
            iframeParams.set('chunkbufferadaptive', '0');
            iframeParams.set('chunkbufferceil', '180000');
            iframeParams.set('buffer2', '0');
            // Keep the iframe bootstrap buffer minimal; the desired sync delay is
            // applied after load via postMessage and should not be duplicated here.
            iframeParams.set('buffer', String(minimumBufferMs));
            return 'https://vdo.ninja/?' + iframeParams.toString();
        }

        function sanitizePeerId(userId) {
            return String(userId).replace(/[^a-zA-Z0-9_-]/g, '_');
        }

        function normalizeUserId(userId) {
            let v = String(userId || '').trim().toLowerCase();
            v = v.replace(/^anonymous:/, '');
            v = v.replace(/^guest:/, '');
            v = v.replace(/\s+/g, '');
            v = v.replace(/@.*$/, '');
            v = v.replace(/[^a-z0-9._-]/g, '');
            return v;
        }

        const peerFrameState = {};
        const manualOffsetsMs = {};

        function getOffsetStorageKey(userId) {
            return 'ninjamPeerOffset:' + appState.room + ':' + normalizeUserId(userId);
        }

        function getManualOffsetMs(userId) {
            const k = normalizeUserId(userId);
            if (typeof manualOffsetsMs[k] === 'number' && isFinite(manualOffsetsMs[k])) {
                return Math.round(manualOffsetsMs[k]);
            }
            try {
                if (window.localStorage) {
                    const raw = window.localStorage.getItem(getOffsetStorageKey(k));
                    const parsed = parseInt(raw || '', 10);
                    if (isFinite(parsed)) {
                        manualOffsetsMs[k] = parsed;
                        return parsed;
                    }
                }
            } catch (e) {
            }
            manualOffsetsMs[k] = 0;
            return 0;
        }

        function setManualOffsetMs(userId, value) {
            const k = normalizeUserId(userId);
            const v = Math.max(-6000, Math.min(6000, Math.round(value)));
            manualOffsetsMs[k] = v;
            try {
                if (window.localStorage) {
                    window.localStorage.setItem(getOffsetStorageKey(k), String(v));
                }
            } catch (e) {
            }
            return v;
        }

        window.addEventListener('message', function (event) {
            if (!event || !event.data || typeof event.data !== 'object') {
                return;
            }
            if (event.origin !== window.location.origin) {
                return;
            }
            if (event.data.type !== 'ninjamOffsetUpdate') {
                return;
            }
            const userId = normalizeUserId(event.data.userId || '');
            if (!userId) {
                return;
            }
            const offsetMs = parseInt(event.data.offsetMs, 10);
            if (!isFinite(offsetMs)) {
                return;
            }
            setManualOffsetMs(userId, offsetMs);
            applyRecommendedBuffer(true);
        });

        function formatSignedMs(value) {
            const v = Math.round(value);
            if (v > 0) {
                return '+' + String(v) + 'ms';
            }
            if (v < 0) {
                return String(v) + 'ms';
            }
            return '0ms';
        }

        function getPeerDelayedBufferMs(userId) {
            const remoteBuffer = ninjamSync.remoteBuffers[userId];
            const baseBuffer = (typeof remoteBuffer === 'number' && isFinite(remoteBuffer) && remoteBuffer >= 0)
                ? Math.round(remoteBuffer)
                : appState.requestedBufferMs;
            const manualOffset = getManualOffsetMs(userId);
            return Math.max(minimumBufferMs, baseBuffer + manualOffset);
        }

        function copyTextToClipboard(text) {
            if (navigator.clipboard && navigator.clipboard.writeText) {
                return navigator.clipboard.writeText(text);
            }
            return new Promise(function (resolve, reject) {
                try {
                    const temp = document.createElement('textarea');
                    temp.value = text;
                    temp.style.position = 'fixed';
                    temp.style.left = '-9999px';
                    document.body.appendChild(temp);
                    temp.focus();
                    temp.select();
                    const ok = document.execCommand('copy');
                    document.body.removeChild(temp);
                    if (ok) resolve();
                    else reject(new Error('copy failed'));
                } catch (err) {
                    reject(err);
                }
            });
        }

        function ensurePeerCard(gridEl, userId) {
            const safeId = sanitizePeerId(userId);
            const cardId = 'peer-card-' + safeId;
            let card = document.getElementById(cardId);
            if (card) {
                return card;
            }
            card = document.createElement('div');
            card.className = 'peer-card';
            card.id = cardId;

            const head = document.createElement('div');
            head.className = 'peer-head';

            const nameEl = document.createElement('span');
            nameEl.id = 'peer-name-' + safeId;
            nameEl.textContent = userId;

            const bufferEl = document.createElement('span');
            bufferEl.id = 'peer-buffer-' + safeId;
            bufferEl.textContent = 'Buffer pending';

            const frame = document.createElement('iframe');
            frame.id = 'peer-frame-' + safeId;
            frame.className = 'peer-frame';
            frame.allow = 'camera; microphone; fullscreen';

            const foot = document.createElement('div');
            foot.className = 'peer-foot';

            const camEl = document.createElement('span');
            camEl.id = 'peer-cam-' + safeId;
            camEl.className = 'peer-cam';
            camEl.textContent = 'NINJAM ' + userId + ' | CAM ' + appState.room + '/' + userId;

            const controls = document.createElement('div');
            controls.className = 'peer-controls';

            const minusBtn = document.createElement('button');
            minusBtn.className = 'peer-btn';
            minusBtn.type = 'button';
            minusBtn.textContent = '-20';

            const plusBtn = document.createElement('button');
            plusBtn.className = 'peer-btn';
            plusBtn.type = 'button';
            plusBtn.textContent = '+20';

            const resetBtn = document.createElement('button');
            resetBtn.className = 'peer-btn';
            resetBtn.type = 'button';
            resetBtn.textContent = 'Reset';

            const copyBtn = document.createElement('button');
            copyBtn.className = 'peer-btn';
            copyBtn.type = 'button';
            copyBtn.textContent = 'Copy Cam';

            const offsetEl = document.createElement('span');
            offsetEl.id = 'peer-offset-' + safeId;
            offsetEl.className = 'peer-offset';
            offsetEl.textContent = 'Off ' + formatSignedMs(getManualOffsetMs(userId));

            minusBtn.addEventListener('click', function () {
                const next = setManualOffsetMs(userId, getManualOffsetMs(userId) - 20);
                offsetEl.textContent = 'Off ' + formatSignedMs(next);
                applyRecommendedBuffer(true);
            });
            plusBtn.addEventListener('click', function () {
                const next = setManualOffsetMs(userId, getManualOffsetMs(userId) + 20);
                offsetEl.textContent = 'Off ' + formatSignedMs(next);
                applyRecommendedBuffer(true);
            });
            resetBtn.addEventListener('click', function () {
                const next = setManualOffsetMs(userId, 0);
                offsetEl.textContent = 'Off ' + formatSignedMs(next);
                applyRecommendedBuffer(true);
            });
            copyBtn.addEventListener('click', function () {
                const delayedBufferMs = getPeerDelayedBufferMs(userId);
                const delayedUrl = buildPeerIframeUrl(userId, delayedBufferMs);
                copyTextToClipboard(delayedUrl)
                    .then(function () {
                        copyBtn.textContent = 'Copied';
                        setTimeout(function () {
                            copyBtn.textContent = 'Copy Cam';
                        }, 900);
                    })
                    .catch(function () {
                        copyBtn.textContent = 'Copy Err';
                        setTimeout(function () {
                            copyBtn.textContent = 'Copy Cam';
                        }, 1200);
                    });
            });

            controls.appendChild(minusBtn);
            controls.appendChild(plusBtn);
            controls.appendChild(resetBtn);
            controls.appendChild(copyBtn);
            controls.appendChild(offsetEl);
            foot.appendChild(camEl);

            head.appendChild(nameEl);
            head.appendChild(bufferEl);
            head.appendChild(controls);
            card.appendChild(head);
            card.appendChild(frame);
            card.appendChild(foot);
            gridEl.appendChild(card);
            const stateKey = 'peer-' + safeId;
            if (!peerFrameState[stateKey]) {
                peerFrameState[stateKey] = { appliedBufferMs: -1, lastApplyAtMs: 0, frameLoaded: false, pendingBufferMs: -1 };
            }
            frame.addEventListener('load', function () {
                const state = peerFrameState[stateKey];
                if (!state) {
                    return;
                }
                state.frameLoaded = true;
                const pending = state.pendingBufferMs >= 0 ? state.pendingBufferMs : state.appliedBufferMs;
                if (pending >= 0) {
                    applyPeerFrameBufferDelay(userId, pending);
                }
            });
            return card;
        }

        let appliedBufferMs = -1;
        let lastApplyAtMs = 0;
        let mainFrameLoaded = false;
        let mainFrameDesiredBufferMs = -1;
        function applyPeerFrameBufferDelay(userId, bufferMs) {
            const safeId = sanitizePeerId(userId);
            const frameEl = document.getElementById('peer-frame-' + safeId);
            if (!frameEl || !frameEl.contentWindow) {
                return;
            }
            const next = Math.max(minimumBufferMs, Math.round(bufferMs));
            try {
                frameEl.contentWindow.postMessage({ setBufferDelay: next }, '*');
                frameEl.contentWindow.postMessage({ setBufferDelay: next, UUID: '*' }, '*');
            } catch (e) {
            }
        }
        function applyMainFrameBufferDelay(bufferMs) {
            const iframe = document.getElementById('vdoFrame');
            if (!iframe || !iframe.contentWindow) {
                return;
            }
            const next = Math.max(minimumBufferMs, Math.round(bufferMs));
            try {
                iframe.contentWindow.postMessage({
                    setBufferDelay: next
                }, '*');
                iframe.contentWindow.postMessage({
                    setBufferDelay: next,
                    UUID: '*'
                }, '*');
            } catch (e) {
            }
        }

        function postFrameRefresh(frameEl) {
            if (!frameEl || !frameEl.contentWindow) {
                return false;
            }
            let sent = false;
            try {
                frameEl.contentWindow.postMessage({ refresh: true }, '*');
                sent = true;
            } catch (e) {
            }
            try {
                frameEl.contentWindow.postMessage({ action: 'refresh' }, '*');
                sent = true;
            } catch (e) {
            }
            return sent;
        }

        function isBufferRefreshPending(userKey) {
            const key = normalizeUserId(userKey);
            return !!key && (vdoRefreshState.refreshPendingUntilByUser[key] || 0) > Date.now();
        }

        function hasAnyBufferRefreshPending() {
            const now = Date.now();
            const keys = Object.keys(vdoRefreshState.refreshPendingUntilByUser || {});
            for (let i = 0; i < keys.length; i++) {
                if ((vdoRefreshState.refreshPendingUntilByUser[keys[i]] || 0) > now) {
                    return true;
                }
            }
            return false;
        }

        function scheduleBufferRefresh(userKey, refreshEventId) {
            const key = normalizeUserId(userKey);
            const eventId = String(refreshEventId || '');
            if (!key || !eventId || vdoRefreshState.lastRefreshIdByUser[key] === eventId) {
                return false;
            }

            vdoRefreshState.lastRefreshIdByUser[key] = eventId;
            if (vdoRefreshState.refreshTimersByUser[key]) {
                clearTimeout(vdoRefreshState.refreshTimersByUser[key]);
            }
            vdoRefreshState.refreshPendingUntilByUser[key] = Date.now() + 150;

            if (appState.view === 'renderer') {
                postFrameRefresh(document.getElementById('peer-frame-' + sanitizePeerId(key)));
            } else {
                postFrameRefresh(document.getElementById('vdoFrame'));
            }

            vdoRefreshState.refreshTimersByUser[key] = setTimeout(function () {
                delete vdoRefreshState.refreshTimersByUser[key];
                delete vdoRefreshState.refreshPendingUntilByUser[key];
                if (appState.view === 'renderer') {
                    applyPeerFrameBufferDelay(key, getPeerDelayedBufferMs(key));
                } else {
                    applyMainFrameBufferDelay(getRecommendedBufferMs());
                }
                updateSyncStatus();
            }, 150);
            return true;
        }

        function applyRecommendedBuffer(force) {
            if (appState.view === 'renderer') {
                const gridEl = document.getElementById('rendererGrid');
                if (!gridEl) {
                    return;
                }
                const nowMs = Date.now();
                const userIds = Object.keys(ninjamSync.remoteTimecodes).sort();
                const activeCardIds = {};
                let maxApplied = appState.requestedBufferMs;

                for (let i = 0; i < userIds.length; i++) {
                    const userId = userIds[i];
                    const safeId = sanitizePeerId(userId);
                    const stateKey = 'peer-' + safeId;
                    const manualOffset = getManualOffsetMs(userId);
                    const nextBuffer = getPeerDelayedBufferMs(userId);
                    maxApplied = Math.max(maxApplied, nextBuffer);
                    ensurePeerCard(gridEl, userId);
                    activeCardIds['peer-card-' + safeId] = true;

                    if (!peerFrameState[stateKey]) {
                        peerFrameState[stateKey] = { appliedBufferMs: -1, lastApplyAtMs: 0, frameLoaded: false, pendingBufferMs: -1 };
                    }
                    const state = peerFrameState[stateKey];
                    const frameEl = document.getElementById('peer-frame-' + safeId);
                    const bufferEl = document.getElementById('peer-buffer-' + safeId);
                    const offsetEl = document.getElementById('peer-offset-' + safeId);
                    if (bufferEl) {
                        bufferEl.textContent = 'Buffer ' + nextBuffer + 'ms';
                    }
                    if (offsetEl) {
                        offsetEl.textContent = 'Off ' + formatSignedMs(manualOffset);
                    }
                    state.pendingBufferMs = nextBuffer;
                    if (frameEl && !frameEl.src) {
                        frameEl.src = buildPeerIframeUrl(userId, nextBuffer);
                        state.frameLoaded = false;
                    } else if (frameEl) {
                        if (isBufferRefreshPending(userId)) {
                            continue;
                        }
                        const bufferChanged = state.appliedBufferMs !== nextBuffer;
                        if (bufferChanged || state.frameLoaded) {
                            applyPeerFrameBufferDelay(userId, nextBuffer);
                        }
                    }
                    state.appliedBufferMs = nextBuffer;
                    state.lastApplyAtMs = nowMs;
                }

                const cards = gridEl.querySelectorAll('.peer-card');
                for (let c = 0; c < cards.length; c++) {
                    const card = cards[c];
                    if (!activeCardIds[card.id]) {
                        card.remove();
                    }
                }

                appliedBufferMs = Math.max(0, maxApplied);
                updateSyncStatus();
                return;
            }

            const iframe = document.getElementById('vdoFrame');
            if (!iframe) {
                return;
            }
            const nextBuffer = getRecommendedBufferMs();
            appliedBufferMs = nextBuffer;
            mainFrameDesiredBufferMs = nextBuffer;
            if (!iframe.src) {
                iframe.src = buildIframeUrl(nextBuffer);
                mainFrameLoaded = false;
            } else if (mainFrameLoaded) {
                if (hasAnyBufferRefreshPending()) {
                    updateSyncStatus();
                    return;
                }
                applyMainFrameBufferDelay(nextBuffer);
            }
            updateSyncStatus();
        }

        const vdoFrame = document.getElementById('vdoFrame');
        if (vdoFrame) {
            vdoFrame.addEventListener('load', function () {
                mainFrameLoaded = true;
                const bufferToApply = mainFrameDesiredBufferMs >= 0 ? mainFrameDesiredBufferMs : getRecommendedBufferMs();
                applyMainFrameBufferDelay(bufferToApply);
            });
        }

        applyRecommendedBuffer(true);

        startIntervalSource();

    </script>
</body>
</html>
`;

const server = http.createServer((req, res) => {
    const parsed = url.parse(req.url, true);
    const pathname = parsed.pathname || '/';

    if (pathname === '/intervals') {
        fs.readFile(intervalsJsonPath, 'utf8', (err, body) => {
            const payload = (!err && body && body.trim().length) ? body : '[]';
            res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store, no-cache, must-revalidate' });
            if (req.method === 'HEAD') {
                res.end();
                return;
            }
            res.end(payload);
        });
        return;
    }

    if (pathname === '/' || pathname === '/buffer-room' || pathname === '/sync-buffer-room' || pathname === '/index.html') {
        fs.readFile(helperPagePath, 'utf8', (err, body) => {
            const responseBody = err ? htmlPage : body;
            res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store, no-cache, must-revalidate' });
            if (req.method === 'HEAD') {
                res.end();
                return;
            }
            res.end(responseBody);
        });
        return;
    }

    if (pathname === '/app') {
        fs.readFile(appHtmlPath, 'utf8', (err, body) => {
            const responseBody = (!err && body && body.trim().length) ? body : htmlPage;
            res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store, no-cache, must-revalidate' });
            if (req.method === 'HEAD') {
                res.end();
                return;
            }
            res.end(responseBody);
        });
        return;
    }

    res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
    res.end('Not found');
});

server.listen(8100, '127.0.0.1', () => {
    console.log('Advanced VDO client listening on http://127.0.0.1:8100/');
});

if (WebSocket && WebSocket.Server) {
    // --- WebSocket push server ---
    const wss = new WebSocket.Server({ server, path: '/ws' });
    let lastPushedPayload = '';
    let watchDebounceTimer = null;

    function broadcastIntervals() {
        fs.readFile(intervalsJsonPath, 'utf8', (err, body) => {
            if (err || !body || !body.trim().length) return;
            const payload = body.trim();
            if (payload === lastPushedPayload) return;
            lastPushedPayload = payload;
            wss.clients.forEach((client) => {
                if (client.readyState === WebSocket.OPEN) {
                    client.send(payload);
                }
            });
        });
    }

    // Watch intervals.json for changes and push to all WebSocket clients
    try {
        fs.watch(intervalsJsonPath, { persistent: false }, (eventType) => {
            if (eventType === 'change') {
                if (watchDebounceTimer) clearTimeout(watchDebounceTimer);
                watchDebounceTimer = setTimeout(broadcastIntervals, 15);
            }
        });
        console.log('Watching intervals.json for WebSocket push');
    } catch (e) {
        // File may not exist yet; retry watching periodically
        const retryWatch = setInterval(() => {
            try {
                fs.watch(intervalsJsonPath, { persistent: false }, (eventType) => {
                    if (eventType === 'change') {
                        if (watchDebounceTimer) clearTimeout(watchDebounceTimer);
                        watchDebounceTimer = setTimeout(broadcastIntervals, 15);
                    }
                });
                console.log('Watching intervals.json for WebSocket push (retry succeeded)');
                clearInterval(retryWatch);
            } catch (e2) {
                // still doesn't exist, try again
            }
        }, 2000);
    }

    wss.on('connection', (ws) => {
        console.log('WebSocket client connected');
        // Send current state immediately on connect
        fs.readFile(intervalsJsonPath, 'utf8', (err, body) => {
            if (!err && body && body.trim().length && ws.readyState === WebSocket.OPEN) {
                ws.send(body.trim());
            }
        });
    });
} else {
    console.log("WebSocket push disabled (module 'ws' not available).");
}
