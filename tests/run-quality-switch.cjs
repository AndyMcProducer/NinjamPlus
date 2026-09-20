const fs=require('node:fs/promises'),path=require('node:path'),{spawn}=require('node:child_process'),crypto=require('node:crypto');
const {chromium}=require('playwright');
const args=Object.fromEntries(process.argv.slice(2).map(x=>x.replace(/^--/,'').split('=')));
const out=path.resolve(args.output||'test-results/quality-switch-'+Date.now());
const wait=ms=>new Promise(r=>setTimeout(r,ms));let chrome,browser;const clients=[];let phase='startup';
async function log(type,data){await fs.appendFile(path.join(out,'events.jsonl'),JSON.stringify({at:Date.now(),phase,type,...data})+'\n');}
async function snap(c){return c.frame.evaluate(()=>{const r=session.chunkedRecorder,t=session.streamSrc?.getVideoTracks?.()[0];return {capture:t?.getSettings(),capabilities:t?.getCapabilities(),constraints:t?.getConstraints(),encoder:{state:r?.videoEncoder?.state,config:r?.videoEncoder?.config,queue:r?.videoEncoder?.encodeQueueSize},config:r?.configVideo,adapt:r?.adaptation,stats:session.stats,receivers:Object.entries(session.rpcs||{}).map(([id,p])=>{const v=document.getElementById('videosource_'+id);return {id,stats:p.stats?.chunked_mode_video,video:v?{time:v.currentTime,width:v.videoWidth,height:v.videoHeight,quality:{total:v.getVideoPlaybackQuality().totalVideoFrames,dropped:v.getVideoPlaybackQuality().droppedVideoFrames}}:null};})};});}
async function sample(seconds){for(let i=0;i<seconds;i++){for(const c of clients)try{await log('sample',{label:c.label,data:await snap(c)});}catch(e){await log('sample-error',{label:c.label,error:String(e)});}await wait(1000);}}
(async()=>{await fs.mkdir(out,{recursive:true});console.log(out);
 chrome=spawn('C:/Program Files/Google/Chrome/Application/chrome.exe',['--remote-debugging-port=0','--user-data-dir='+path.join(out,'chrome-profile'),'--no-first-run','--use-fake-ui-for-media-stream','--use-fake-device-for-media-stream=fps=60',...(args.file?['--use-file-for-fake-video-capture='+path.resolve(args.file)]:[]),'about:blank'],{windowsHide:true,stdio:'ignore'});
 let port;for(let i=0;i<40;i++){try{port=(await fs.readFile(path.join(out,'chrome-profile/DevToolsActivePort'),'utf8')).split('\n')[0];break;}catch{await wait(500);}}
 browser=await chromium.connectOverCDP('http://127.0.0.1:'+port);const context=browser.contexts()[0];
 if(args['webrtc-source']){const body=await fs.readFile(path.resolve(args['webrtc-source']),'utf8');await context.route('**/webrtc.js?*',r=>r.fulfill({contentType:'application/javascript',body}));}
 if(args.isolated)await context.route('**/intervals',r=>r.fulfill({contentType:'application/json',body:JSON.stringify(['alpha','bravo'].map(userId=>({type:'videoTimecode',userId,interval:3,timecode:0,intervalMeasurementSeen:true,bufferCalculated:true,receiverBufferMs:5000,receiverBufferFinal:true,syncRoute:'TEST'})))}));
 for(const [label,port] of [['alpha',args['alpha-port']],['bravo',args['bravo-port']]]){
  const page=await context.newPage();page.on('pageerror',e=>log('error',{label,error:String(e)}));page.on('console',m=>{if(m.type()==='error')log('console-error',{label,text:m.text().slice(0,1500)});});
  page.on('response',async r=>{if(/\/(webrtc|main|lib)\.js\?/.test(r.url()))try{await log('loaded',{label,url:r.url(),sha256:crypto.createHash('sha256').update(await r.body()).digest('hex')});}catch{}});
  const q=new URLSearchParams({room:args.room,label,vdoSyncUserKey:label,cameraQuality:'720p30',bufferMode:'remote',buffer:'0',chunked:'2500',chunkadaptceil:'2500',...(args.codec?{cameraCodec:args.codec}:{})});
  await page.goto('http://127.0.0.1:'+port+'/buffer-room?'+q);let frame;for(let i=0;i<60;i++){frame=page.frames().find(f=>f.url().startsWith('https://vdo.ninja/alpha/'));if(frame)break;await wait(500);}
  await frame.waitForFunction(()=>typeof previewWebcam==='function');await frame.evaluate(()=>previewWebcam());await frame.waitForFunction(()=>document.getElementById('gowebcam')?.disabled===false);await frame.evaluate(()=>document.getElementById('gowebcam').click());clients.push({label,page,frame});
 }
 phase='720-baseline';await sample(35);
 for(let cycle=1;cycle<=Number(args.cycles||2);cycle++){
  phase='1080p60-'+cycle;for(const c of clients)await c.page.selectOption('#vdoCameraQuality','1080p60-10000');await log('quality',{value:'1080p60-10000'});await sample(40);
  phase='720-recovery-'+cycle;for(const c of clients)await c.page.selectOption('#vdoCameraQuality','720p30');await log('quality',{value:'720p30'});await sample(55);
 }
 await log('complete',{});
})().catch(async e=>{console.error(e);await log('failed',{error:String(e)});process.exitCode=1;}).finally(async()=>{if(browser)await browser.close();if(chrome)chrome.kill();});
