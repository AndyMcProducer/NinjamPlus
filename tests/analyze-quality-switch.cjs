const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const root=process.argv[2];if(!root)throw Error('Usage: node tests/analyze-quality-switch.cjs RUN_DIR [--expect-recovery]');
const rows=fs.readFileSync(path.join(root,'events.jsonl'),'utf8').trim().split('\n').map(JSON.parse);
const summary={};
for(const phase of [...new Set(rows.map(r=>r.phase))])for(const label of ['alpha','bravo']){
 const samples=rows.filter(r=>r.type==='sample'&&r.phase===phase&&r.label===label).slice(8);
 if(samples.length<3)continue;
 const frames=samples.map(r=>r.data.receivers[0]?.video);if(frames.some(v=>!v))continue;
 const seconds=(samples.at(-1).at-samples[0].at)/1000;
 const total=frames.at(-1).quality.total-frames[0].quality.total,dropped=frames.at(-1).quality.dropped-frames[0].quality.dropped;
 const result=summary[phase+'/'+label]={displayedFPS:Math.round((total-dropped)/seconds*10)/10,droppedFrames:dropped,encoderStates:[...new Set(samples.map(r=>r.data.encoder.state))]};
 if(process.argv.includes('--expect-recovery')&&phase!=='startup'){
  assert(result.encoderStates.every(s=>s==='configured'),phase+'/'+label+' encoder closed');
  assert(result.displayedFPS>=(phase.startsWith('1080')?10:25),phase+'/'+label+' did not sustain playback');
  const width=phase.startsWith('1080')?1920:1280, fps=phase.startsWith('1080')?60:30;
  assert(samples.every(r=>r.data.capture.width===width&&r.data.capture.frameRate===fps),phase+'/'+label+' wrong capture format');
 }
}
if(process.argv.includes('--expect-recovery')){
 assert(rows.some(r=>r.type==='complete'),'Run must complete');
 assert(Object.keys(summary).some(k=>k.startsWith('720-recovery')),'Missing recovery phase');
}
fs.writeFileSync(path.join(root,'summary.json'),JSON.stringify(summary,null,2));console.log(JSON.stringify(summary,null,2));
