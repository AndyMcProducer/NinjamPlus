// Companion to NINJAMplus_SyncSoakTest --live-vdo; uses its real embedded helpers.
const fs=require('node:fs/promises'), path=require('node:path'), crypto=require('node:crypto');
const {spawn}=require('node:child_process');
const {chromium}=require('playwright');
const args=Object.fromEntries(process.argv.slice(2).map(a=>a.replace(/^--/,'').split(/=(.*)/s).slice(0,2)));
if(!args.room) throw new Error('--room, --alpha-port, --bravo-port required from native harness output');
const duration=Number(args['duration-seconds']||600);
if(!Number.isFinite(duration)||duration<10||duration>1800)throw new Error('--duration-seconds must be 10..1800');
const transport=args.transport||'udp';
if(!['tcp','udp','direct'].includes(transport))throw new Error('--transport must be tcp, udp (TURN), or direct (normal ICE)');
const diagnostics=args.diagnostics||'full';
if(!['minimal','full'].includes(diagnostics))throw new Error('--diagnostics must be minimal or full');
const output=path.resolve(args.output||'test-results/e2e-alpha-udp');
const pause=ms=>new Promise(r=>setTimeout(r,ms));
let chrome,browser;const clients=[],loads=[];
async function event(name,data={}){const row={at:Date.now(),name,...data};console.log(JSON.stringify(row));await fs.appendFile(path.join(output,'browser-events.jsonl'),JSON.stringify(row)+'\n');}
async function frame(page){for(let i=0;i<60;i++){const f=page.frames().find(f=>f.url().startsWith('https://vdo.ninja/alpha/'));if(f)return f;await pause(500);}throw new Error('VDO frame missing');}
async function camera(client){client.frame=await frame(client.page);await client.frame.waitForFunction(()=>typeof previewWebcam==='function');await client.frame.evaluate(()=>previewWebcam());await client.frame.waitForFunction(()=>document.getElementById('gowebcam')?.disabled===false);await client.frame.evaluate(()=>document.getElementById('gowebcam').click());for(const file of ['live-network-observer.js',...(diagnostics==='full'?['live-playout-observer.js']:[])])await client.frame.evaluate(await fs.readFile(path.join(__dirname,file),'utf8'));}
async function snapshot(client){try{const data=await client.frame.evaluate(()=>({at:Date.now(),samples:__njLiveProbe.samples,source:__njSourceObserver.samples,network:__njNetworkObserver.samples,observer:window.__njPlayoutObserver?.export(),scheduler:window.__njSchedulerObserver,discardTrace:window.__njDiscardTrace,url:location.href}));await fs.writeFile(path.join(output,client.label+'-'+client.generation+'.json'),JSON.stringify(data));const last=data.network.at(-1);return {label:client.label,samples:data.samples.length,latest:data.samples.at(-1),network:last};}catch(e){return {label:client.label,error:String(e)};}}
(async()=>{
 await fs.mkdir(output,{recursive:true});
 const profile=path.join(output,'chrome-profile');
 chrome=spawn('C:/Program Files/Google/Chrome/Application/chrome.exe',['--remote-debugging-port=0','--user-data-dir='+profile,'--no-first-run','about:blank'],{windowsHide:true,stdio:'ignore'});
 let port;for(let i=0;i<40;i++){try{port=(await fs.readFile(path.join(profile,'DevToolsActivePort'),'utf8')).split('\n')[0];break;}catch{await pause(500);}}
 browser=await chromium.connectOverCDP('http://127.0.0.1:'+port);const context=browser.contexts()[0];
 if(args['webrtc-source']){const body=await fs.readFile(path.resolve(args['webrtc-source']),'utf8');await context.route('**/webrtc.js?*',r=>r.fulfill({contentType:'application/javascript',body}));}
 for(const file of ['live-video-probe.js','live-source-observer.js',...(diagnostics==='full'?['live-scheduler-observer.js']:[])])await context.addInitScript({content:await fs.readFile(path.join(__dirname,file),'utf8')});
 for(const label of ['alpha','bravo']){
   const page=await context.newPage();page.setDefaultTimeout(20000);
   page.on('response',async r=>{if(/\/(webrtc|main|lib)\.js\?/.test(r.url()))try{loads.push({label,url:r.url(),sha256:crypto.createHash('sha256').update(await r.body()).digest('hex')});}catch{}});
   const u=new URL('http://127.0.0.1:'+args[label+'-port']+'/buffer-room');u.search=new URLSearchParams({room:args.room,label,vdoSyncUserKey:label,cameraQuality:'720p30',bufferMode:'remote',buffer:'0',chunked:'2500',chunkadaptceil:'2500',...(transport==='direct'?{}:{relay:''}),...(transport==='tcp'?{tcp:''}:{})}).toString();
   await page.goto(u.href);const client={label,page,generation:0};clients.push(client);await camera(client);await event('camera-started',{label,url:u.href});
 }
 const deadline=Date.now()+duration*1000;await event('ready',{durationSeconds:duration,diagnostics,transport});
 while(Date.now()<deadline){
   try{const command=JSON.parse((await fs.readFile(path.join(output,'command.json'),'utf8')).replace(/^\uFEFF/,''));await fs.unlink(path.join(output,'command.json'));
     if(command.action==='stop')break;
     if(command.action==='reload'){const c=clients.find(c=>c.label===command.label);if(!c)throw new Error('unknown client');await snapshot(c);await c.page.reload();c.generation++;await camera(c);await event('reloaded',{label:c.label});}
   }catch(e){if(e.code!=='ENOENT')await event('command-error',{error:String(e)});}
   await event('sample',{clients:await Promise.all(clients.map(snapshot))});await fs.writeFile(path.join(output,'loaded-scripts.json'),JSON.stringify(loads,null,2));await pause(5000);
 }
})().catch(e=>{console.error(e);process.exitCode=1;}).finally(async()=>{for(const c of clients)await snapshot(c);await fs.writeFile(path.join(output,'loaded-scripts.json'),JSON.stringify(loads,null,2));if(browser)await browser.close();if(chrome)chrome.kill();});
