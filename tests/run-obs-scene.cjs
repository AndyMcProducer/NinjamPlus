// Optional live check using the real Electron Capture app and an isolated room.
// node tests/run-obs-scene.cjs --electron-root=PATH [--before-ref=COMMIT]
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const { chromium, _electron } = require('playwright');
const args = Object.fromEntries(process.argv.slice(2).map(arg => {
    const index = arg.indexOf('=');
    return [arg.slice(2, index), arg.slice(index + 1)];
}));
const root = path.resolve(__dirname, '..');
const output = path.resolve(args.output || path.join(root, 'test-results', 'obs-scene-' + Date.now()));
const room = 'obsscene' + require('node:crypto').randomBytes(8).toString('hex');
let server, browser, capture, publisher;
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));

async function main() {
    assert.ok(args['electron-root'], '--electron-root must point to an Electron Capture checkout with dependencies installed');
    fs.mkdirSync(output, { recursive: true });
    const helper = fs.readFileSync(path.join(root, 'advanced-vdo-client/app.html'));
    const before = args['before-ref'] ? execFileSync('git', ['show', args['before-ref'] + ':advanced-vdo-client/app.html'], { cwd: root }) : null;
    server = http.createServer((request, response) => {
        const url = new URL(request.url, 'http://localhost');
        response.setHeader('Cache-Control', 'no-store');
        if (url.pathname === '/intervals') {
            response.setHeader('Content-Type', 'application/json');
            response.end(JSON.stringify([{ type: 'videoTimecode', userId: 'scene-camera', interval: 3,
                timecode: 0, receiverBufferMs: 800, receiverBufferFinal: true }]));
        } else if (url.pathname === '/app' || (before && url.pathname === '/before')) {
            response.setHeader('Content-Type', 'text/html');
            response.end(url.pathname === '/before' ? before : helper);
        } else {
            response.writeHead(404);
            response.end();
        }
    });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    const origin = 'http://127.0.0.1:' + server.address().port;
    function helperUrl(route, obs) {
        return origin + route + '?' + new URLSearchParams({ room, label: obs ? 'scene-viewer' : 'scene-camera',
            vdoSyncUserKey: obs ? 'scene-viewer' : 'scene-camera', cameraQuality: '720p30',
            ...(obs ? { obs: '1', hideFooter: '1', obsLayout: 'tiles' } : {}) });
    }
    browser = await chromium.launch({ channel: 'chrome', headless: true,
        args: ['--use-fake-ui-for-media-stream', '--use-fake-device-for-media-stream'] });
    publisher = await browser.newPage();
    publisher.on('console', message => {
        if (message.type() === 'error' || message.type() === 'warning') {
            fs.appendFileSync(path.join(output, 'publisher.log'), message.text() + '\n');
        }
    });
    publisher.on('pageerror', error => fs.appendFileSync(path.join(output, 'publisher.log'), String(error) + '\n'));
    await publisher.goto(helperUrl('/app', false));
    const publisherFrame = publisher.frameLocator('#vdoFrame');
    await publisherFrame.locator('#container-3').waitFor();
    await publisherFrame.locator('#container-3').click();
    const start = publisherFrame.getByText('START', { exact: true });
    await start.waitFor({ state: 'visible' });
    // The live preview resizes while fake camera frames arrive.
    await start.click({ force: true });
    console.log('Guest camera started through the existing join controls.');

    const electronRoot = path.resolve(args['electron-root']);
    const env = { ...process.env, ELECTRON_CAPTURE_USER_DATA_DIR: path.join(output, 'capture-profile') };
    delete env.ELECTRON_RUN_AS_NODE;
    capture = await _electron.launch({ executablePath: require(path.join(electronRoot, 'node_modules/electron')),
        args: [electronRoot, '--multiinstance', '--minimized', '--url=' + helperUrl(before ? '/before' : '/app', true)],
        env, timeout: 30000 });
    const page = await capture.firstWindow();
    async function screenshot(name) {
        const png = await capture.evaluate(async ({ BrowserWindow }) => {
            const image = await BrowserWindow.getAllWindows()[0].webContents.capturePage(undefined,
                { stayHidden: true, stayAwake: true });
            return image.toPNG().toString('base64');
        });
        fs.writeFileSync(path.join(output, name), Buffer.from(png, 'base64'));
    }
    page.on('pageerror', error => fs.appendFileSync(path.join(output, 'page-errors.log'), String(error) + '\n'));
    async function rosterState() {
        const frame = page.frames().find(frame => frame.url().startsWith('https://vdo.ninja/alpha/'));
        if (!frame) return null;
        return frame.evaluate(() => typeof session === 'object' ? {
            scene: session.scene, peers: Object.keys(session.rpcs).length,
            publishing: !!session.videoElement.srcObject
        } : null);
    }
    const result = { room, before: null, after: null };
    if (before) {
        await page.waitForSelector('#vdoFrame', { state: 'attached' });
        await wait(12000);
        result.before = await rosterState();
        assert.ok(result.before, 'baseline VDO frame loaded');
        assert.equal(result.before.scene, false);
        assert.equal(result.before.peers, 0);
        assert.equal(await page.locator('#obsGrid .obs-tile').count(), 0);
        console.log('Before: no room peers and no visible camera tiles.');
        await screenshot('before.png');
        await page.goto(helperUrl('/app', true));
    }
    await capture.evaluate(({ BrowserWindow }) => {
        const window = BrowserWindow.getAllWindows()[0];
        window.restore();
        window.showInactive();
    });
    await page.locator('#obsGrid .obs-tile iframe').waitFor({ timeout: 60000 });
    console.log('Fixed: camera tile created.');
    const tile = page.frameLocator('#obsGrid .obs-tile iframe');
    await tile.locator('video').first().waitFor({ state: 'attached', timeout: 30000 });
    const tileFrame = page.frames().find(frame => new URL(frame.url()).searchParams.has('view'));
    await tileFrame.waitForFunction(() => Array.from(document.querySelectorAll('video')).some(video =>
        video.videoWidth > 0 && video.getVideoPlaybackQuality().totalVideoFrames > 10), null, { timeout: 60000 });
    async function videoFrames() {
        return tileFrame.evaluate(() => Array.from(document.querySelectorAll('video')).map(video => ({
            width: video.videoWidth, height: video.videoHeight, frames: video.getVideoPlaybackQuality().totalVideoFrames
        })).find(video => video.width > 0));
    }
    const first = await videoFrames();
    await wait(2000);
    const last = await videoFrames();
    assert.ok(last.frames > first.frames + 10, 'visible scene video must keep advancing');
    result.after = { roster: await rosterState(), first, last, electron: await capture.evaluate(() => process.versions) };
    assert.equal(String(result.after.roster.scene), '0');
    assert.equal(result.after.roster.publishing, false);
    await screenshot('after.png');
    fs.writeFileSync(path.join(output, 'result.json'), JSON.stringify(result, null, 2));
    console.log(JSON.stringify(result));
    console.log('OBS_SCENE_PASSED ' + output);
}
main().catch(async error => {
    console.error(error);
    if (publisher) {
        await publisher.screenshot({ path: path.join(output, 'publisher-failure.png') }).catch(() => {});
        const frame = publisher.frames().find(frame => frame.url().startsWith('https://vdo.ninja/alpha/'));
        if (frame) console.log((await frame.locator('body').innerText()).slice(-4000));
    }
    console.log('Artifacts: ' + output);
    if (capture) {
        const page = capture.windows()[0];
        for (const frame of page.frames()) {
            console.log('Receiver frame:', frame.url(), await frame.locator('body').innerText().catch(() => 'unavailable'));
            console.log(await frame.evaluate(() => typeof session === 'object' ? {
                scene: session.scene, view: session.view, roomid: session.roomid,
                peers: Object.keys(session.rpcs), waiting: session.waitingWatchList,
                video: Array.from(document.querySelectorAll('video')).map(video => ({
                    width: video.videoWidth, time: video.currentTime, paused: video.paused,
                    frames: video.getVideoPlaybackQuality().totalVideoFrames
                })),
                chunked: Object.values(session.rpcs).map(peer => peer.stats && peer.stats.chunked_mode_video)
            } : null).catch(() => null));
        }
        const png = await capture.evaluate(async ({ BrowserWindow }) =>
            (await BrowserWindow.getAllWindows()[0].webContents.capturePage(undefined,
                { stayHidden: true, stayAwake: true })).toPNG().toString('base64'));
        fs.writeFileSync(path.join(output, 'receiver-failure.png'), Buffer.from(png, 'base64'));
    }
    process.exitCode = 1;
}).finally(async () => {
    if (capture) await capture.close();
    if (browser) await browser.close();
    if (server) { server.closeAllConnections(); server.close(); }
});
