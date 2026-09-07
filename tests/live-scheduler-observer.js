// Read-only timing instrumentation, injected before VDO creates its AudioContext.
(() => {
 const state=window.__njSchedulerObserver={ended:[],longTasks:[],wakes:[],messages:[],outOfOrder:[]};
 window.addEventListener('message',e=>{if(e.data && typeof e.data==='object' && 'setBufferDelay' in e.data)state.messages.push({at:Date.now(),data:e.data});});
 const seen=new WeakSet();setInterval(()=>{
   for(const [id,rpc] of Object.entries(window.session?.rpcs||{}))for(const file of rpc.chunkedChannels||[]){
     const controller=file.video?.controller;if(!controller||seen.has(controller))continue;seen.add(controller);
     const originalWake=controller.wake;
     const snap=()=>({origin:file.video.realTime,remote:file.getRemoteNow(),peerBuffer:rpc.buffer,globalBuffer:session.buffer,window:controller.bufferWindow(),stats:{...rpc.stats.chunked_mode_video}});
     controller.wake=function(){const before=snap();const result=originalWake.apply(this,arguments);state.wakes.push({at:Date.now(),id,before,after:snap()});return result;};
     const enqueue=controller.enqueue;
     controller.enqueue=function(frame){const before=snap();if(before.window && frame.timestamp<before.window.newest)state.outOfOrder.push({at:Date.now(),timestamp:frame.timestamp,type:frame.type,before});return enqueue.apply(this,arguments);};
   }
 },100);
 const proto=window.AudioContext?.prototype;if(!proto)return;
 const original=proto.createOscillator;
 proto.createOscillator=function(...args){
   const context=this,osc=original.apply(this,args),stop=osc.stop;let record;
   osc.stop=function(when=0){record={at:Date.now(),perf:performance.now(),audioNow:context.currentTime,when,state:context.state};return stop.apply(this,arguments);};
   osc.addEventListener('ended',()=>{if(record){state.ended.push({...record,endedAt:Date.now(),endedPerf:performance.now(),audioEnd:context.currentTime});if(state.ended.length>12000)state.ended.splice(0,1000);}});
   return osc;
 };
 try{new PerformanceObserver(list=>{for(const e of list.getEntries())state.longTasks.push({start:e.startTime,duration:e.duration});}).observe({entryTypes:['longtask']});}catch{}
})();
