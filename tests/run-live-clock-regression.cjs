// Real installed-Chrome regression; public signaling carries only generated video.
// Run from NinjamPlus: node tests/run-live-clock-regression.cjs [--transport=tcp|udp]
// --webrtc-source=PATH substitutes a saved/fixed script for before/after testing.
const fs = require('node:fs/promises');
const path = require('node:path');
const crypto = require('node:crypto');
const { spawn } = require('node:child_process');
const { chromium } = require('playwright');
const assert = require('node:assert/strict');
const args = Object.fromEntries(process.argv.slice(2).map(a => a.replace(/^--/, '').split(/=(.*)/s).slice(0, 2)));
const transport = args.transport || 'tcp';
assert(['tcp', 'udp'].includes(transport));
const root = path.resolve(__dirname, '..');
const output = path.resolve(args.output || path.join(root, 'test-results', 'clock-browser-' + Date.now()));
const base = args.base || 'https://vdo.ninja/alpha/';
const chromePath = args.chrome || 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const target = Number(args.target || 5000);
assert(target >= 1000 && target <= 30000);
const delay = ms => new Promise(r => setTimeout(r, ms));
const median = a => { a = [...a].sort((x,y) => x-y); return a[Math.floor(a.length/2)]; };
const events = [], pending = [], scriptLoads = [], cases = [];
let chrome, browser, publisher, ctx, watchdog;
async function event(type, details = {}) {
    const row = { at: Date.now(), type, ...details }; events.push(row);
    console.log(JSON.stringify(row));
    await fs.appendFile(path.join(output, 'events.jsonl'), JSON.stringify(row) + '\n');
}
async function until(fn, timeout, message) {
    const end = Date.now() + timeout;
    while (Date.now() < end) { const result = await fn(); if (result) return result; await delay(500); }
    throw new Error(message);
}
const params = {
    noaudio:'1', quality:'1', mfr:'30', bitrate:'2500', chunked:'2500', chunkbitrate:'2500',
    maxvideobitrate:'2500', chunkindex:'1', chunknack:'1', chunknackattempts:'8', chunknackdelay:'250',
    chunkchunksize:'4096', chunkcache:'30000', chunkbufferadaptive:'0', chunkbufferfloor:'0',
    chunkbufferceil:'180000', chunkedbuffer:'500', chunkadapt:'bitrate', chunkadaptfloor:'60',
    chunkadaptceil:'2500', chunkadaptthreshold:'500', chunkadaptmaxdrop:'0', chunkadaptinterval:'1200',
    chunkadaptresolution:'1', relay:'', ...(transport === 'tcp' ? {tcp:''} : {})
};
function url(extra) { const u = new URL(base); u.search = new URLSearchParams({...params,...extra}).toString(); return u.href; }
async function newPage(label, targetUrl) {
    const p = await ctx.newPage(); p.setDefaultTimeout(10000); p.setDefaultNavigationTimeout(30000);
    p.on('response', response => {
        if (/\/(webrtc|lib|main)\.js(?:\?|$)/.test(response.url())) pending.push((async()=>{
            const bytes = await response.body();
            scriptLoads.push({label,url:response.url(),status:response.status(),sha256:crypto.createHash('sha256').update(bytes).digest('hex')});
            if(label==='publisher')await fs.writeFile(path.join(output,new URL(response.url()).pathname.split('/').pop()),bytes);
        })().catch(error=>scriptLoads.push({label,error:String(error)})));
    });
    p.on('pageerror', error => { events.push({at:Date.now(),type:'page-error',label,error:String(error)}); });
    await p.goto(targetUrl, {waitUntil:'domcontentloaded'});
    return p;
}
async function sampleWindow(viewer, name, baseline) {
    const end = Date.now() + 65000;
    while (Date.now() < end) {
        const snapshot = await viewer.evaluate(() => ({ now:Date.now(), samples:__njLiveProbe.samples.slice(-60),
            network:window.__njNetworkObserver?.samples.at(-1),
            peers:Object.values(session.rpcs || {}).map(r=>({state:r.connectionState,stats:r.stats?.chunked_mode_video})) }));
        const source = await publisher.evaluate(() => __njSourceObserver.samples.slice(-30));
        const moving = []; let previous;
        for (const s of snapshot.samples) {
            const capture = s.now - s.delay;
            if (s.now > snapshot.now - 8000 && previous !== undefined && capture - previous > 5) moving.push(s.delay);
            previous = capture;
        }
        const freshSource = source.filter(s=>s.at > snapshot.now - 6000);
        const sourceHealthy = freshSource.length >= 15 && freshSource.filter(s=>s.captureAgeMs>=0 && s.captureAgeMs<500).length / freshSource.length >= .9;
        if (moving.length >= 25 && snapshot.samples.at(-1)?.now > snapshot.now - 1000 && sourceHealthy) {
            const value = median(moving);
            if (Math.abs(value-target)<750 && (baseline===undefined || Math.abs(value-baseline)<350)) {
                const routes=(snapshot.network?.peers||[]).filter(p=>p.direction==='rpcs' && p.connection==='connected');
                assert(routes.length && routes.every(p=>p.candidateType==='relay' &&
                    (transport==='udp' ? p.relayProtocol==='udp' : ['tcp','tls'].includes(p.relayProtocol))),
                    'Expected selected TURN/'+transport+' route');
                const result={name,at:Date.now(),medianAgeMs:value,advancingSamples:moving.length,sourceHealthy,
                    routes:routes.map(p=>({candidateType:p.candidateType,relayProtocol:p.relayProtocol,url:p.url}))};
                cases.push(result); await event('case-pass', result); return value;
            }
            // A stable, fresh source and consistent multi-second offset is a real failure.
            if (moving.length >= 35 && Math.max(...moving)-Math.min(...moving)<700) {
                throw new Error(name+': wrong video age '+value+' ms; expected about '+target+' ms (baseline '+baseline+')');
            }
        }
        await delay(1000);
    }
    throw new Error(name+': no healthy advancing video within 65 seconds (source and receiver traces distinguish stalls)');
}
async function save(label,p) {
    try { const d=await p.evaluate(()=>({at:Date.now(),samples:__njLiveProbe?.samples,source:window.__njSourceObserver?.samples,
        jump:window.__njClockJumpMs||0,clock:typeof session.getChunkedTimestamp==='function'?session.getChunkedTimestamp():null,
        network:window.__njNetworkObserver?.samples, peers:Object.values(session.rpcs||{}).map(r=>({state:r.connectionState,stats:r.stats?.chunked_mode_video}))}));
        await fs.writeFile(path.join(output,label+'.json'),JSON.stringify(d));
    } catch(error) { await event('snapshot-error',{label,error:String(error)}); }
}
(async()=>{
    await fs.mkdir(output,{recursive:true});
    await event('start',{transport,base,output,source:args['webrtc-source']||'deployed'});
    const profile=path.join(output,'chrome-profile');
    chrome=spawn(chromePath,['--remote-debugging-port=0','--user-data-dir='+profile,'--no-first-run','about:blank'],{windowsHide:true,stdio:'ignore'});
    const port=await until(async()=>{try{return (await fs.readFile(path.join(profile,'DevToolsActivePort'),'utf8')).split('\n')[0];}catch{return null;}},20000,'Chrome debugging port unavailable');
    browser=await chromium.connectOverCDP('http://127.0.0.1:'+port); ctx=browser.contexts()[0];
    watchdog=setTimeout(()=>{console.error('Test exceeded eight-minute limit');if(chrome)chrome.kill();process.exit(2);},480000);
    await ctx.addInitScript({content:await fs.readFile(path.join(__dirname,'live-video-probe.js'),'utf8')});
    await ctx.addInitScript({content:await fs.readFile(path.join(__dirname,'live-source-observer.js'),'utf8')});
    const network=await fs.readFile(path.join(__dirname,'live-network-observer.js'),'utf8');
    if(args['webrtc-source']) {
        const body=await fs.readFile(path.resolve(args['webrtc-source']),'utf8');
        await ctx.route('**/webrtc.js?*',r=>r.fulfill({contentType:'application/javascript',body}));
    }
    const stream='njclock'+crypto.randomBytes(7).toString('hex');
    publisher=await newPage('publisher',url({push:stream,label:'alpha',webcam:''}));
    await publisher.waitForFunction(()=>typeof previewWebcam==='function');
    await publisher.evaluate(()=>{previewWebcam();});
    await publisher.locator('#gowebcam').waitFor({state:'visible'});
    await publisher.waitForFunction(()=>!document.getElementById('gowebcam').disabled);
    await publisher.evaluate(()=>document.getElementById('gowebcam').click());
    await publisher.evaluate(network);
    const viewerUrl=url({view:stream,buffer:String(target),buffer2:'0',cleanoutput:'1',autostart:'1'});
    const keepalive=await newPage('keepalive',viewerUrl); await keepalive.evaluate(network);
    const baseline=await sampleWindow(keepalive,'baseline');
    if(args['stall-ms']) {
        const stall=Number(args['stall-ms']);assert(stall>=100 && stall<=2000);
        try {
            for(let attempt=0;attempt<3;attempt++) {
                const started=Date.now();await event('scheduler-stall',{stall,attempt});
                await keepalive.evaluate(ms=>{const end=performance.now()+ms;while(performance.now()<end){};},stall);
                await until(async()=>keepalive.evaluate(({started,target})=>{
                    const samples=__njLiveProbe.samples.filter(s=>s.now>started && Math.abs(s.delay-target)<750);
                    let count=0,prior;for(const s of samples){const capture=s.now-s.delay;if(prior!==undefined && capture-prior>5)count++;prior=capture;}
                    return count>=3;
                },{started,target}),2000,'Intact video failed to catch up within two seconds after scheduler stall');
                await event('stall-recovered',{attempt,elapsedMs:Date.now()-started});await delay(3000);
            }
            await event('pass',{scenario:'scheduler-stall'});
        } finally {await save('scheduler-stall-viewer',keepalive);}
        return;
    }
    await publisher.evaluate(()=>{const original=Date.now;window.__njClockJumpMs=0;Date.now=()=>original()+__njClockJumpMs;});
    for (const jump of [1890,5053,-5053]) {
        await publisher.evaluate(value=>{__njClockJumpMs=value;},jump); await event('clock-jump',{jump});
        const viewer=await newPage('jump-'+jump,viewerUrl);await viewer.evaluate(network);
        try { await sampleWindow(viewer,'late-join-'+jump,baseline);
            if(jump===5053){await viewer.reload({waitUntil:'domcontentloaded'});await viewer.evaluate(network);await sampleWindow(viewer,'reload-'+jump,baseline);}
        } finally { await save('viewer-'+jump,viewer);await viewer.close(); }
    }
    await save('keepalive',keepalive);await event('pass',{cases:cases.length});
})().catch(async error=>{process.exitCode=1;await event('fail',{error:String(error),stack:error.stack});}).finally(async()=>{
    if(publisher)await save('publisher',publisher);
    await Promise.allSettled(pending);await fs.writeFile(path.join(output,'result.json'),JSON.stringify({passed:process.exitCode!==1,cases,events,scriptLoads},null,2));
    if(browser){try{await browser.close();}catch{}}
    if(chrome && chrome.exitCode===null)chrome.kill();
    clearTimeout(watchdog);
});
