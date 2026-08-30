#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ZapVideoCodec.h"
#include "Chromagram.h"
#include "ChordDetector.h"
#include "SolititoChordModel.h"
#include "EmbeddedVdoHtml.h"
#include <juce_video/juce_video.h>

#ifndef NINJAMPLUS_HAS_LIBROSA
 #define NINJAMPLUS_HAS_LIBROSA 0
#endif

#ifndef NINJAMPLUS_HAS_ONNX_CHORDS
 #define NINJAMPLUS_HAS_ONNX_CHORDS 0
#endif

#if NINJAMPLUS_HAS_LIBROSA
 #include <librosa/beat.hpp>
 #include <librosa/onset.hpp>
 #include <librosa/effects.hpp>
#endif

#ifdef interface
#undef interface
#endif

#include "jnetlib/httpget.h"
#include <ableton/LinkAudio.hpp>
#include <ableton/link/HostTimeFilter.hpp>
#include <ableton/util/FloatIntConversion.hpp>
#include <array>
#include <atomic>
#include <thread>
#include <memory>
#include <cmath>
#include <cstring>
#include <rubberband/RubberBandStretcher.h>
#include <deque>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <vector>

static juce::File getThisModuleFile();

static void writeDiagnosticLogLine(const char* fileName, const juce::String& msg)
{
    static juce::CriticalSection logLock;
    const juce::String line = juce::Time::getCurrentTime().toString(true, true, true, true)
        + "  " + msg + "\n";

    // If we're on the audio thread (not the message thread), defer the file I/O
    // to avoid blocking audio with synchronous disk writes.
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm != nullptr && !mm->isThisTheMessageThread())
    {
        juce::MessageManager::callAsync([fileName, line]()
        {
            const juce::ScopedLock lock(logLock);
            juce::File f = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile(fileName);
            f.appendText(line, false, false);
        });
        return;
    }

    const juce::ScopedLock lock(logLock);
    juce::File f = juce::File::getSpecialLocation(juce::File::tempDirectory)
                       .getChildFile(fileName);
    f.appendText(line, false, false);
}

static void logIntervalPerf(const juce::String& msg)
{
    writeDiagnosticLogLine("ninjam_interval_perf.txt", msg);
}

static constexpr double intervalPerfStepThresholdMs = 3.0;
static constexpr double intervalPerfTotalThresholdMs = 8.0;
static constexpr int ninjamRunDefaultMaxIterationsPerTimer = 50;
static constexpr int ninjamRunMaxIterationsPerTimer = 4;
static constexpr double ninjamRunBudgetMs = 2.0;

static constexpr int advancedVideoHelperBasePort = 8000;
static constexpr int advancedVideoHelperMaxPort = 8199;

class LocalVideoHttpServer final : private juce::Thread
{
public:
    LocalVideoHttpServer(std::function<juce::String()> intervalPayloadProviderIn,
                         std::function<juce::String()> zapFrameListProviderIn,
                         std::function<bool(const juce::String&, int, juce::MemoryBlock&)> zapFrameProviderIn,
                         std::function<juce::String(const juce::String&)> zapBrowserCameraEnableIn,
                         std::function<juce::String()> zapBrowserCameraStateIn,
                         std::function<juce::String()> zapBrowserCameraDisableIn,
                         std::function<bool(const juce::MemoryBlock&, const juce::String&, const juce::String&, bool, double, double, int, int)> zapBrowserFrameConsumerIn)
        : juce::Thread("NINJAMVideoHelperServer"),
          intervalPayloadProvider(std::move(intervalPayloadProviderIn)),
          zapFrameListProvider(std::move(zapFrameListProviderIn)),
          zapFrameProvider(std::move(zapFrameProviderIn)),
          zapBrowserCameraEnable(std::move(zapBrowserCameraEnableIn)),
          zapBrowserCameraState(std::move(zapBrowserCameraStateIn)),
          zapBrowserCameraDisable(std::move(zapBrowserCameraDisableIn)),
          zapBrowserFrameConsumer(std::move(zapBrowserFrameConsumerIn))
    {
        reloadStaticContent();
    }

    ~LocalVideoHttpServer() override
    {
        stop();
    }

    bool start(int preferredPort, int maxPort)
    {
        if (isThreadRunning())
            return listenPort.load() > 0;

        reloadStaticContent();
        if (helperIndexHtml.isEmpty() || helperAppHtml.isEmpty())
            return false;

        const int firstPort = juce::jlimit(1, 65535, preferredPort);
        const int lastPort = juce::jlimit(firstPort, 65535, maxPort);
        for (int port = firstPort; port <= lastPort; ++port)
        {
            auto candidate = std::make_unique<juce::StreamingSocket>();
            if (!candidate->createListener(port, "127.0.0.1"))
                continue;

            listener = std::move(candidate);
            listenPort.store(port);
            startThread();
            return true;
        }

        listener.reset();
        listenPort.store(0);
        return false;
    }

    void stop()
    {
        signalThreadShouldExit();
        if (listener)
            listener->close();
        stopThread(500);
        clientThreadPool.removeAllJobs(true, 1000);
        listener.reset();
        listenPort.store(0);
    }

    int getPort() const
    {
        return listenPort.load();
    }

private:
    struct HttpResponse
    {
        int statusCode = 200;
        juce::String statusText = "OK";
        juce::String contentType = "text/plain; charset=utf-8";
        juce::MemoryBlock body;
        bool noStore = false;
    };

    struct HttpRequest
    {
        juce::String method;
        juce::String target;
        juce::MemoryBlock body;
    };

    std::function<juce::String()> intervalPayloadProvider;
    std::function<juce::String()> zapFrameListProvider;
    std::function<bool(const juce::String&, int, juce::MemoryBlock&)> zapFrameProvider;
    std::function<juce::String(const juce::String&)> zapBrowserCameraEnable;
    std::function<juce::String()> zapBrowserCameraState;
    std::function<juce::String()> zapBrowserCameraDisable;
    std::function<bool(const juce::MemoryBlock&, const juce::String&, const juce::String&, bool, double, double, int, int)> zapBrowserFrameConsumer;
    std::unique_ptr<juce::StreamingSocket> listener;
    std::atomic<int> listenPort { 0 };
    juce::String helperIndexHtml;
    juce::String helperAppHtml;
    juce::MemoryBlock helperIconPng;
    juce::MemoryBlock helperPoweredByPng;
    juce::MemoryBlock helperCloudMaskPng;
    juce::ThreadPool clientThreadPool { 4 };
    const juce::String helperRequestToken { juce::Uuid().toString() };

    class ClientJob final : public juce::ThreadPoolJob
    {
    public:
        ClientJob(LocalVideoHttpServer& ownerIn, std::unique_ptr<juce::StreamingSocket> clientIn)
            : juce::ThreadPoolJob("NINJAMVideoHelperClient"),
              owner(ownerIn),
              client(std::move(clientIn))
        {
        }

        JobStatus runJob() override
        {
            if (client != nullptr)
                owner.handleClient(*client);
            return jobHasFinished;
        }

    private:
        LocalVideoHttpServer& owner;
        std::unique_ptr<juce::StreamingSocket> client;
    };

    void run() override
    {
        while (!threadShouldExit())
        {
            if (!listener)
                break;

            if (listener->waitUntilReady(true, 200) <= 0)
                continue;

            std::unique_ptr<juce::StreamingSocket> client(listener->waitForNextConnection());
            if (client)
                clientThreadPool.addJob(new ClientJob(*this, std::move(client)), true);
        }
    }

    static bool writeAll(juce::StreamingSocket& socket, const void* data, size_t bytesToWrite)
    {
        const char* bytes = static_cast<const char*>(data);
        size_t remaining = bytesToWrite;
        while (remaining > 0)
        {
            const int chunk = (int) juce::jmin(remaining, (size_t) 32768);
            const int written = socket.write(bytes, chunk);
            if (written <= 0)
                return false;
            bytes += written;
            remaining -= (size_t) written;
        }
        return true;
    }

    static juce::MemoryBlock makeUtf8Body(const juce::String& text)
    {
        const auto* utf8 = text.toRawUTF8();
        return juce::MemoryBlock(utf8, std::strlen(utf8));
    }

    static void appendBe32(juce::MemoryBlock& outData, juce::uint32 value)
    {
        const unsigned char bytes[4]
        {
            static_cast<unsigned char>((value >> 24) & 0xff),
            static_cast<unsigned char>((value >> 16) & 0xff),
            static_cast<unsigned char>((value >> 8) & 0xff),
            static_cast<unsigned char>(value & 0xff)
        };
        outData.append(bytes, sizeof(bytes));
    }

    static juce::String getQueryParam(const juce::String& requestTarget, const juce::String& key)
    {
        const int queryStart = requestTarget.indexOfChar('?');
        if (queryStart < 0)
            return {};

        const juce::String query = requestTarget.substring(queryStart + 1);
        juce::StringArray parts;
        parts.addTokens(query, "&", "");
        for (auto part : parts)
        {
            const juce::String name = part.upToFirstOccurrenceOf("=", false, false);
            if (name == key)
            {
                juce::String value = part.fromFirstOccurrenceOf("=", false, false).replace("+", " ");
                return juce::URL::removeEscapeChars(value);
            }
        }

        return {};
    }

    juce::String getZapViewerHtml() const
    {
        juce::String html;
        html << R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>NINJAMZap Video</title>
<style>
html,body{margin:0;min-height:100%;background:#101719;color:#edf5f5;font-family:system-ui,-apple-system,Segoe UI,sans-serif}
body{display:flex;flex-direction:column}
html.obs,html.obs body{background:transparent}
html.obs header,html.obs .camera-panel{display:none}
html.obs #grid{padding:0;background:transparent}
html.obs .tile{background:transparent;border:0;border-radius:0}
html.obs .label{background:rgba(0,0,0,.42);border-bottom:0}
html.obs .stage{background:transparent}
header{display:flex;align-items:center;gap:12px;padding:10px 14px;background:#172326;border-bottom:1px solid #34464a}
h1{font-size:15px;margin:0;font-weight:700}
#status{font-size:12px;color:#a8b8bb}
.camera-panel{display:flex;flex-wrap:wrap;gap:8px;align-items:center;padding:8px 12px;background:#111b1e;border-bottom:1px solid #2f4145}
.camera-panel select,.camera-panel button,.camera-panel input{height:28px;border:1px solid #3b5358;border-radius:4px;background:#19272b;color:#edf5f5}
.camera-panel input[type="range"]{width:96px;padding:0}
.camera-panel input[type="color"]{width:34px;padding:2px}
.camera-panel input[type="checkbox"]{width:16px;height:16px}
.camera-panel input[type="url"]{width:min(220px,32vw);min-width:150px;padding:0 8px}
.camera-panel button{padding:0 10px;cursor:pointer}
.camera-panel button:hover{background:#22343a}
.camera-panel button:disabled{opacity:.45;cursor:default}
.camera-panel label{font-size:12px;color:#aebfc2;display:flex;align-items:center;gap:5px}
#camStatus{font-size:12px;color:#a8b8bb}
#browserPreview{position:absolute;left:-10000px;width:1px;height:1px;opacity:0;pointer-events:none}
#browserFxPreview{width:128px;height:72px;object-fit:contain;background:#050808;border:1px solid #2c3b3e;border-radius:4px}
#grid{display:grid;gap:10px;padding:10px;flex:1}
#grid.layout-tiles{grid-template-columns:repeat(auto-fit,minmax(280px,1fr))}
#grid.layout-rows{grid-template-columns:minmax(280px,1fr)}
#grid.layout-columns{grid-auto-flow:column;grid-auto-columns:minmax(280px,1fr);overflow-x:auto}
#grid.layout-columns .tile{min-width:280px}
.tile{background:#0c1113;border:1px solid #2c3b3e;border-radius:8px;overflow:hidden;min-height:210px;display:flex;flex-direction:column}
.label{font-size:12px;color:#cbd8d9;padding:7px 9px;background:#162124;border-bottom:1px solid #2c3b3e;display:flex;align-items:center;gap:8px}
.label-name{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.copy-link{height:22px;border:1px solid #3b5358;border-radius:4px;background:#19272b;color:#edf5f5;font-size:11px;cursor:pointer}
.copy-link:hover{background:#22343a}
html.obs .copy-link{display:none}
.stage{position:relative;flex:1;display:grid;place-items:center;background:#050808}
img,canvas{display:block;width:100%;height:100%;object-fit:contain}
.stage-note{position:absolute;left:8px;right:8px;bottom:8px;padding:4px 6px;border-radius:4px;background:rgba(0,0,0,.58);color:#d7e4e6;font-size:11px;text-align:center;pointer-events:none}
.stage-note:empty{display:none}
.empty{margin:auto;color:#7f9195;font-size:13px}
.zap-footer{min-height:36px;padding:6px 10px;border-top:1px solid #34464a;background:#0f1218;display:flex;align-items:center;gap:10px}
html.obs .zap-footer{display:none}
.beat-counter{display:flex;align-items:center;gap:9px;width:100%;flex:1 1 auto;min-width:0;color:#fffdd0;font-size:12px;font-variant-numeric:tabular-nums}
.beat-counter-label{min-width:64px;flex:0 0 auto;color:#fffdd0;white-space:nowrap}
.beat-strip{display:flex;flex-wrap:nowrap;column-gap:3px;row-gap:10px;align-items:flex-start;width:100%;flex:1 1 auto;min-width:0;padding-bottom:8px}
.beat-dot{height:13px;min-width:9px;flex:1 1 0;border:1px solid rgba(255,253,208,.28);border-radius:2px;background:rgba(0,0,0,.34);display:inline-flex;align-items:center;justify-content:center;position:relative;overflow:visible;color:transparent;font-variant-numeric:tabular-nums;user-select:none}
.beat-dot.past{background:#fffdd0;border-color:#fffdd0}
.beat-dot.active{background:#fffdd0;border-color:#fffdd0;box-shadow:0 0 9px rgba(255,253,208,var(--beat-glow-alpha,.85))}
.beat-current-number,.beat-bar-label{position:absolute;left:0;right:0;text-align:center;line-height:1;pointer-events:none}
.beat-current-number{top:1px;color:rgba(0,0,0,var(--beat-number-alpha,.95));font-size:7px;font-weight:800}
.beat-current-number.compact{font-size:6px}.beat-current-number.tiny{font-size:5px}
.beat-bar-label{bottom:-8px;height:8px;min-width:100%;display:flex;align-items:center;justify-content:center;background:rgba(18,18,16,.95);border:1px solid rgba(255,253,208,.28);border-top:0;border-radius:0 0 3px 3px;color:rgba(255,253,208,.78);font-size:6px;font-weight:700}
.beat-bar-label.compact{font-size:5px}
.beat-dot.past .beat-bar-label{background:rgba(255,255,255,.36);border-color:rgba(255,255,255,.44);color:rgba(0,0,0,.72)}
.beat-dot.active .beat-bar-label{background:#fffdd0;border-color:#fffdd0;color:rgba(0,0,0,var(--beat-number-alpha,.95));box-shadow:0 2px 7px rgba(255,253,208,var(--beat-glow-alpha,.85))}</style>
</head>
<body>
<header><h1>NINJAMZap Video</h1><div id="status">Waiting for video frames</div></header>
<section class="camera-panel">
  <label>Camera <select id="camSelect"><option value="">Default camera</option></select></label>
  <label>Codec <select id="camCodec"><option value="h264">H.264</option><option value="mjpeg">MJPEG</option><option value="vp8">VP8</option><option value="vp9">VP9</option></select></label>
  <label>Size <select id="camSize"><option value="320x180">320x180</option><option value="640x360" selected>640x360</option><option value="800x450">800x450</option><option value="1280x720">1280x720</option><option value="1920x1080" data-vpx-only="1">1920x1080 VP8/VP9</option></select></label>
  <label>FPS <select id="camFps"><option value="10">10</option><option value="15">15</option><option value="24">24</option><option value="30" selected>30</option><option value="60">60</option></select></label>
  <label>Quality <select id="camQuality"><option value="balanced">Balanced</option><option value="high" selected>High</option><option value="low">Low</option></select></label>
  <label>View <select id="camLayout"><option value="tiles" selected>Tiles</option><option value="rows">Rows</option><option value="columns">Columns</option></select></label>
  <label>Mask <select id="camMask"><option value="none" selected>None</option><option value="cloud">Cloud</option><option value="circle">Circle</option><option value="rounded">Rounded</option><option value="square">Square</option><option value="tree">Xmas tree</option><option value="pumpkin">Pumpkin</option><option value="star">Star</option><option value="heart">Heart</option></select></label>
  <label>FX <select id="camFx"><option value="off" selected>Off</option><option value="blur">Blur</option><option value="bg-blur">Blur background</option><option value="green-bg">Green background</option><option value="black-bg">Black background</option><option value="custom-bg">Custom background</option><option value="video-bg">Video background</option><option value="grayscale">Grayscale</option><option value="sepia">Sepia</option></select></label>
  <label>Strength <input id="camFxStrength" type="range" min="0" max="24" value="10"></label>
  <label>BG <input id="camBgColor" type="color" value="#00ff00"></label>
  <label>Video BG <input id="camVideoBg" type="url" placeholder="https://..."></label>
  <label>Mirror <input id="camMirror" type="checkbox"></label>
  <button id="camRefresh" type="button">Refresh</button>
  <button id="camStart" type="button">Start browser camera</button>
  <button id="camStop" type="button" disabled>Stop</button>
  <button id="camObs" type="button">OBS view</button>
  <video id="browserPreview" autoplay muted playsinline></video>
  <canvas id="browserFxPreview" width="256" height="144"></canvas>
  <span id="camStatus">Choose a camera here to send Zap video.</span>
  <canvas id="browserCanvas" width="640" height="360" style="display:none"></canvas>
</section>
<main id="grid"><div class="empty">No Zap video streams yet</div></main>
<footer class="zap-footer"><div class="beat-counter" id="beatCounter" aria-live="polite"><span class="beat-counter-label" id="beatCounterText">BPI --/--</span><span class="beat-strip" id="beatStrip" aria-hidden="true"></span></div></footer>
<script>
const helperRequestToken='__NINJAM_HELPER_TOKEN__';
function helperMutationUrl(url){
  return url+(url.includes('?')?'&':'?')+'token='+encodeURIComponent(helperRequestToken);
}
const grid=document.getElementById('grid');
const statusEl=document.getElementById('status');
const beatCounterText=document.getElementById('beatCounterText');
const beatStrip=document.getElementById('beatStrip');
const camSelect=document.getElementById('camSelect');
const camCodec=document.getElementById('camCodec');
const camSize=document.getElementById('camSize');
const camFps=document.getElementById('camFps');
const camQuality=document.getElementById('camQuality');
const camLayout=document.getElementById('camLayout');
const camMask=document.getElementById('camMask');
const camFx=document.getElementById('camFx');
const camFxStrength=document.getElementById('camFxStrength');
const camBgColor=document.getElementById('camBgColor');
const camVideoBg=document.getElementById('camVideoBg');
const camMirror=document.getElementById('camMirror');
const camRefresh=document.getElementById('camRefresh');
const camStart=document.getElementById('camStart');
const camStop=document.getElementById('camStop');
const camObs=document.getElementById('camObs');
const camStatus=document.getElementById('camStatus');
const browserPreview=document.getElementById('browserPreview');
const browserFxPreview=document.getElementById('browserFxPreview');
const browserFxPreviewCtx=browserFxPreview.getContext('2d',{alpha:false});
const browserCanvas=document.getElementById('browserCanvas');
const browserCtx=browserCanvas.getContext('2d',{alpha:false});
const browserFxMaskCanvas=document.createElement('canvas');
const browserFxMaskCtx=browserFxMaskCanvas.getContext('2d',{alpha:true});
const browserFxPersonCanvas=document.createElement('canvas');
const browserFxPersonCtx=browserFxPersonCanvas.getContext('2d',{alpha:true});
const tiles=new Map();
let browserStream=null;
let browserTimer=0;
let browserPosting=false;
let browserFrameCounter=0;
let browserSendArmed=false;
let browserEncoder=null;
let browserEncoderCodec='mjpeg';
let browserEncoderH264Format='avc';
let browserEncodeStarts=new Map();
let browserLastCodecConfig='';
let browserLastKeyframeMs=0;
let browserForceNextKeyframe=false;
let browserLastKeyframeRequestId='';
let browserCameraStateTimer=0;
let browserSettingsApplying=false;
let browserPendingSettingsApply=false;
let browserApplySettingsTimer=0;
let browserActiveCodec='mjpeg';
let latestIntervalInfo=null;
let refreshInFlight=false;
let browserFxSegmenter=null;
let browserFxSegmenterLoading=null;
let browserFxSegmenterFailed=false;
let browserFxSegmentationInFlight=false;
let browserFxLastSegmentationMs=0;
let browserFxMaskReady=false;
let browserBgVideo=null;
let browserBgVideoUrl='';
const showStreamDebug=false;
const urlParams=new URLSearchParams(location.search);
const obsMode=location.pathname==='/zap-wall'||urlParams.get('obs')==='1';
const obsStreamFilter=urlParams.get('stream')||'';
const obsSlotFilter=Math.max(0,parseInt(urlParams.get('slot')||'0',10)||0);
if(obsMode) document.documentElement.classList.add('obs');
if(urlParams.get('layout')) camLayout.value=urlParams.get('layout');
if(urlParams.get('mask')) camMask.value=urlParams.get('mask');
function ms(value){
  const n=Number(value||0);
  return Number.isFinite(n)&&n>0?String(Math.round(n)):'0';
}
function intervalInfoFromPayload(payload){
  if(!Array.isArray(payload)) return null;
  return payload.find(entry=>String(entry.type||'')==='intervalInfo')||null;
}
function streamsFromPayload(payload){
  if(!Array.isArray(payload)) return [];
  return payload.filter(entry=>String(entry.type||'')!=='intervalInfo');
}
function getZapBeatState(){
  const info=latestIntervalInfo;
  if(!info) return {bpi:0,beat:0,phase:0};
  const bpi=Math.max(1,Math.min(256,parseInt(info.bpi||'0',10)||0));
  if(!bpi) return {bpi:0,beat:0,phase:0};
  const bpm=Math.max(0,Number(info.bpm||0));
  let totalBeat=Number(info.globalBeat);
  if(!Number.isFinite(totalBeat)){
    const pos=Math.max(0,Number(info.pos||0));
    const length=Math.max(1,Number(info.length||0));
    totalBeat=(pos/length)*bpi;
  }
  const wallClockMs=Number(info.wallClockMs||0);
  if(bpm>0&&wallClockMs>0){
    const elapsedMinutes=(Date.now()-wallClockMs)/60000;
    if(Number.isFinite(elapsedMinutes)&&elapsedMinutes>-0.25&&elapsedMinutes<0.25)
      totalBeat+=elapsedMinutes*bpm;
  }
  if(!Number.isFinite(totalBeat)) return {bpi:0,beat:0,phase:0};
  const wrappedBeat=((totalBeat%bpi)+bpi)%bpi;
  const beatIndex=Math.floor(wrappedBeat);
  return {bpi:bpi,beat:beatIndex+1,phase:wrappedBeat-beatIndex};
}
function renderBeatCounter(){
  if(!beatCounterText||!beatStrip) return;
  const state=getZapBeatState();
  if(state.bpi<=0){
    beatCounterText.textContent='BPI --/--';
    beatStrip.innerHTML='';
    return;
  }
  const phase=Math.max(0,Math.min(1,state.phase||0));
  const pulse=Math.sin(phase*Math.PI);
  const numberAlpha=(0.34+0.66*pulse).toFixed(3);
  const glowAlpha=(0.45+0.45*pulse).toFixed(3);
  beatCounterText.textContent='BPI '+state.beat+'/'+state.bpi;
  beatStrip.innerHTML='';
  for(let i=1;i<=state.bpi;i++){
    const dot=document.createElement('span');
    const isActive=i===state.beat;
    dot.className=isActive?'beat-dot active':(i<state.beat?'beat-dot past':'beat-dot');
    if(isActive){
      dot.style.setProperty('--beat-number-alpha',numberAlpha);
      dot.style.setProperty('--beat-glow-alpha',glowAlpha);
      const currentNumber=document.createElement('span');
      const beatLabel=String(i);
      currentNumber.className='beat-current-number'+(beatLabel.length>2?' tiny':(beatLabel.length>1?' compact':''));
      currentNumber.textContent=beatLabel;
      dot.appendChild(currentNumber);
    }
    if((i-1)%4===0){
      const barLabel=document.createElement('span');
      const label=String(Math.floor((i-1)/4)+1);
      barLabel.className='beat-bar-label'+(label.length>1?' compact':'');
      barLabel.textContent=label;
      dot.appendChild(barLabel);
    }
    beatStrip.appendChild(dot);
  }
}
function setCamStatus(text){ camStatus.textContent=text; }
function normalizedGridLayout(value){
  const layout=String(value||'tiles').toLowerCase();
  return layout==='rows'||layout==='columns'?layout:'tiles';
}
function applyGridLayout(){
  const layout=normalizedGridLayout(camLayout.value);
  camLayout.value=layout;
  grid.classList.remove('layout-tiles','layout-rows','layout-columns');
  grid.classList.add('layout-'+layout);
  if(!obsMode&&history&&history.replaceState){
    const url=new URL(location.href);
    url.searchParams.set('layout',layout);
    history.replaceState(null,'',url.toString());
  }
  return layout;
}
function selectedDisplayMask(){
  const value=String(camMask.value||'none').toLowerCase();
  return ['none','cloud','circle','rounded','square','tree','pumpkin','star','heart'].includes(value)?value:'none';
}
function svgDisplayMaskUrl(shape){
  let inner='';
  if(shape==='tree')
    inner='<path fill="white" d="M50 4 L74 36 H62 L82 63 H66 L91 94 H58 V100 H42 V94 H9 L34 63 H18 L38 36 H26 Z"/>';
  else if(shape==='pumpkin')
    inner='<path fill="white" d="M47 7 H57 V23 H47 Z M50 18 C34 7 12 20 12 53 C12 82 31 98 50 88 C69 98 88 82 88 53 C88 20 66 7 50 18 Z"/>';
  else if(shape==='star')
    inner='<polygon fill="white" points="50,4 61,37 96,37 68,57 79,92 50,71 21,92 32,57 4,37 39,37"/>';
  else if(shape==='heart')
    inner='<path fill="white" d="M50 91 C23 68 8 54 8 33 C8 18 19 8 34 8 C42 8 48 12 50 19 C52 12 58 8 66 8 C81 8 92 18 92 33 C92 54 77 68 50 91 Z"/>';
  if(!inner) return '';
)HTML";
        html << R"HTML(  return 'url("data:image/svg+xml;utf8,'+encodeURIComponent('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" preserveAspectRatio="none">'+inner+'</svg>')+'")';
}
function clearDisplayMask(el){
  if(!el) return;
  el.style.clipPath='';
  el.style.webkitClipPath='';
  el.style.maskImage='none';
  el.style.webkitMaskImage='none';
  el.style.maskSize='';
  el.style.webkitMaskSize='';
  el.style.maskRepeat='';
  el.style.webkitMaskRepeat='';
  el.style.maskPosition='';
  el.style.webkitMaskPosition='';
}
function applyDisplayMaskToElement(el,shape){
  clearDisplayMask(el);
  if(!el||obsMode||shape==='none') return;
  if(shape==='circle'){
    el.style.clipPath='circle(47% at 50% 50%)';
    el.style.webkitClipPath='circle(47% at 50% 50%)';
    return;
  }
  if(shape==='rounded'){
    el.style.clipPath='inset(0 round 10%)';
    el.style.webkitClipPath='inset(0 round 10%)';
    return;
  }
  if(shape==='square'){
    el.style.clipPath='inset(0)';
    el.style.webkitClipPath='inset(0)';
    return;
  }
  const maskUrl=shape==='cloud'?'url("/zap-mask/cloud.png")':svgDisplayMaskUrl(shape);
  if(!maskUrl) return;
  el.style.maskImage=maskUrl;
  el.style.webkitMaskImage=maskUrl;
  el.style.maskSize='100% 100%';
  el.style.webkitMaskSize='100% 100%';
  el.style.maskRepeat='no-repeat';
  el.style.webkitMaskRepeat='no-repeat';
  el.style.maskPosition='center';
  el.style.webkitMaskPosition='center';
}
function applyDisplayMaskToTile(tile){
  const shape=obsMode?'none':selectedDisplayMask();
  if(tile._displayMask===shape) return;
  tile._displayMask=shape;
  applyDisplayMaskToElement(tile._img,shape);
  applyDisplayMaskToElement(tile._canvas,shape);
}
function refreshDisplayMasks(){
  const shape=obsMode?'none':selectedDisplayMask();
  applyDisplayMaskToElement(browserFxPreview,shape);
  for(const [,tile] of tiles){
    tile._displayMask='';
    applyDisplayMaskToTile(tile);
  }
}
function obsUrlForSlot(slot){
  const url=new URL('/zap-wall',location.href);
  url.searchParams.set('layout',normalizedGridLayout(camLayout.value));
  if(slot>0) url.searchParams.set('slot',String(slot));
  return url.toString();
}
async function copyObsLink(slot){
  const url=obsUrlForSlot(slot);
  try{
    await navigator.clipboard.writeText(url);
    statusEl.textContent='OBS link copied';
  }catch(e){
    window.prompt('OBS camera URL',url);
  }
}
function selectedTransportSize(){
  const parts=String(camSize.value||'640x360').split('x');
  const codec=selectedTransportCodec();
  const maxWidth=(codec==='vp8'||codec==='vp9')?1920:1280;
  const maxHeight=(codec==='vp8'||codec==='vp9')?1080:720;
  const width=Math.max(160,Math.min(maxWidth,parseInt(parts[0]||'640',10)||640));
  const height=Math.max(90,Math.min(maxHeight,parseInt(parts[1]||'360',10)||360));
  return {width,height};
}
function selectedTransportCodec(){
  const codec=String(camCodec.value||'h264').toLowerCase();
  return codec==='mjpeg'||codec==='vp8'||codec==='vp9'||codec==='h264'?codec:'h264';
}
function selectedTransportFps(){
  return Math.max(1,Math.min(60,parseInt(camFps.value||'30',10)||30));
}
function selectedTransportQuality(){
  const value=String(camQuality.value||'high').toLowerCase();
  return value==='low'||value==='balanced'||value==='high'?value:'high';
}
function updateTransportControls(){
  const codec=selectedTransportCodec();
  const allowVpx1080=codec==='vp8'||codec==='vp9';
  for(const option of camSize.options){
    if(option.dataset&&option.dataset.vpxOnly==='1'){
      option.disabled=!allowVpx1080;
    }
  }
  if(!allowVpx1080&&camSize.value==='1920x1080'){
    camSize.value='1280x720';
  }
}
function selectedJpegQuality(){
  const quality=selectedTransportQuality();
  return quality==='low'?0.56:(quality==='balanced'?0.68:0.8);
}
function selectedVideoConstraints(){
  const fps=selectedTransportFps();
  const {width,height}=selectedTransportSize();
  const video={width:{ideal:width,max:width},height:{ideal:height,max:height},frameRate:{ideal:fps,max:fps}};
  if(camSelect.value) video.deviceId={exact:camSelect.value};
  return video;
}
function selectedCameraFx(){
  const value=String(camFx.value||'off').toLowerCase();
  return ['off','blur','bg-blur','green-bg','black-bg','custom-bg','video-bg','grayscale','sepia'].includes(value)?value:'off';
}
function selectedFxStrength(){
  const value=parseInt(camFxStrength.value||'10',10);
  return Number.isFinite(value)?Math.max(0,Math.min(24,value)):10;
}
function selectedFxBgColor(){
  const value=String(camBgColor.value||'#00ff00');
  return /^#[0-9a-f]{6}$/i.test(value)?value:'#00ff00';
}
function selectedFxVideoUrl(){
  return String(camVideoBg.value||'').trim();
}
function isSegmentationFx(value){
  const fx=value||selectedCameraFx();
  return fx==='bg-blur'||fx==='green-bg'||fx==='black-bg'||fx==='custom-bg'||fx==='video-bg';
}
function updateFxControls(){
  const fx=selectedCameraFx();
  camFxStrength.disabled=!(fx==='blur'||fx==='bg-blur');
  camBgColor.disabled=fx!=='custom-bg';
  camVideoBg.disabled=fx!=='video-bg';
  if(fx==='video-bg') syncBrowserBgVideo();
  else stopBrowserBgVideo();
  if(!isSegmentationFx(fx)) browserFxMaskReady=false;
}
function ensureCanvasSize(canvas,width,height){
  if(canvas.width!==width) canvas.width=width;
  if(canvas.height!==height) canvas.height=height;
}
function browserFxFilterFor(fx){
  const strength=selectedFxStrength();
  if(fx==='blur') return 'blur('+String(strength)+'px)';
  if(fx==='grayscale') return 'grayscale(1)';
  if(fx==='sepia') return 'sepia(1)';
  return 'none';
}
function drawBrowserVideoTo(ctx,width,height,filter,mirror,scale){
  if(!browserPreview||browserPreview.readyState<2) return false;
  const drawScale=scale||1;
  const drawWidth=width*drawScale;
  const drawHeight=height*drawScale;
  const drawX=(width-drawWidth)/2;
  const drawY=(height-drawHeight)/2;
  ctx.save();
  ctx.filter=filter||'none';
  if(mirror){
    ctx.translate(width,0);
    ctx.scale(-1,1);
  }
  ctx.drawImage(browserPreview,drawX,drawY,drawWidth,drawHeight);
  ctx.restore();
  return true;
}
function drawCoverSourceTo(ctx,source,width,height){
  const sourceWidth=source.videoWidth||source.naturalWidth||source.width||width;
  const sourceHeight=source.videoHeight||source.naturalHeight||source.height||height;
  if(!sourceWidth||!sourceHeight) return false;
  const scale=Math.max(width/sourceWidth,height/sourceHeight);
  const drawWidth=sourceWidth*scale;
  const drawHeight=sourceHeight*scale;
  const drawX=(width-drawWidth)/2;
  const drawY=(height-drawHeight)/2;
  ctx.drawImage(source,drawX,drawY,drawWidth,drawHeight);
  return true;
}
function ensureBrowserBgVideoElement(){
  if(browserBgVideo) return browserBgVideo;
  browserBgVideo=document.createElement('video');
  browserBgVideo.muted=true;
  browserBgVideo.loop=true;
  browserBgVideo.playsInline=true;
  browserBgVideo.crossOrigin='anonymous';
  browserBgVideo.preload='auto';
  browserBgVideo.addEventListener('error',()=>setCamStatus('Video background could not load'));
  return browserBgVideo;
}
function stopBrowserBgVideo(){
  if(!browserBgVideo) return;
  browserBgVideo.pause();
  browserBgVideo.removeAttribute('src');
  browserBgVideo.load();
  browserBgVideoUrl='';
}
function syncBrowserBgVideo(){
  const url=selectedFxVideoUrl();
  if(selectedCameraFx()!=='video-bg'||!url){
    stopBrowserBgVideo();
    return false;
  }
  const video=ensureBrowserBgVideoElement();
  if(browserBgVideoUrl!==url){
    browserBgVideoUrl=url;
    video.src=url;
    video.load();
  }
  if(video.paused){
    const playPromise=video.play();
    if(playPromise&&playPromise.catch) playPromise.catch(()=>{});
  }
  return video.readyState>=2;
}
function drawBrowserVideoBackground(ctx,width,height){
  if(!syncBrowserBgVideo()) return false;
  try{
    return drawCoverSourceTo(ctx,browserBgVideo,width,height);
  }catch(e){
    setCamStatus('Video background cannot be drawn');
    return false;
  }
}
function drawBrowserMaskTo(ctx,width,height,mirror){
  ctx.save();
  if(mirror){
    ctx.translate(width,0);
    ctx.scale(-1,1);
  }
  ctx.drawImage(browserFxMaskCanvas,0,0,width,height);
  ctx.restore();
}
function resetBrowserFxMask(){
  browserFxMaskReady=false;
  browserFxSegmentationInFlight=false;
  browserFxLastSegmentationMs=0;
}
function loadSelfieSegmentationScript(){
  if(window.SelfieSegmentation) return Promise.resolve();
  if(browserFxSegmenterLoading) return browserFxSegmenterLoading;
  browserFxSegmenterLoading=new Promise((resolve,reject)=>{
    const script=document.createElement('script');
    script.src='https://cdn.jsdelivr.net/npm/@mediapipe/selfie_segmentation/selfie_segmentation.js';
    script.crossOrigin='anonymous';
    script.onload=()=>resolve();
    script.onerror=()=>reject(new Error('Selfie Segmentation could not load'));
    document.head.appendChild(script);
  });
  return browserFxSegmenterLoading;
}
async function ensureBrowserFxSegmenter(){
  if(browserFxSegmenter) return browserFxSegmenter;
  if(browserFxSegmenterFailed) return null;
  await loadSelfieSegmentationScript();
  if(!window.SelfieSegmentation) throw new Error('Selfie Segmentation unavailable');
  browserFxSegmenter=new window.SelfieSegmentation({
    locateFile:file=>'https://cdn.jsdelivr.net/npm/@mediapipe/selfie_segmentation/'+file
  });
  browserFxSegmenter.setOptions({modelSelection:1,selfieMode:false});
  browserFxSegmenter.onResults(results=>{
    const mask=results&&results.segmentationMask;
    if(!mask) return;
    const width=browserPreview.videoWidth||selectedTransportSize().width;
    const height=browserPreview.videoHeight||selectedTransportSize().height;
    ensureCanvasSize(browserFxMaskCanvas,width,height);
    browserFxMaskCtx.clearRect(0,0,width,height);
    browserFxMaskCtx.drawImage(mask,0,0,width,height);
    browserFxMaskReady=true;
  });
  return browserFxSegmenter;
}
function updateBrowserFxSegmentation(){
  const fx=selectedCameraFx();
  if(!isSegmentationFx(fx)||!browserStream||browserPreview.readyState<2||browserFxSegmentationInFlight||browserFxSegmenterFailed) return;
  const now=performance.now();
  if(now-browserFxLastSegmentationMs<90) return;
  browserFxLastSegmentationMs=now;
  browserFxSegmentationInFlight=true;
  ensureBrowserFxSegmenter()
    .then(segmenter=>{
      if(segmenter) return segmenter.send({image:browserPreview});
    })
    .catch(err=>{
      browserFxSegmenterFailed=true;
      browserFxMaskReady=false;
      setCamStatus('Background FX unavailable: '+String(err&&err.message?err.message:err));
    })
    .finally(()=>{ browserFxSegmentationInFlight=false; });
}
function drawBrowserFxFrame(ctx,width,height){
  ctx.save();
  ctx.filter='none';
  ctx.globalCompositeOperation='source-over';
  ctx.clearRect(0,0,width,height);
  ctx.fillStyle='#050808';
  ctx.fillRect(0,0,width,height);
  ctx.restore();
  if(!browserStream||browserPreview.readyState<2) return false;
  const fx=selectedCameraFx();
  const mirror=!!camMirror.checked;
)HTML";
        html << R"HTML(  updateBrowserFxSegmentation();
  if(isSegmentationFx(fx)&&browserFxMaskReady){
    if(fx==='bg-blur'){
      drawBrowserVideoTo(ctx,width,height,'blur('+String(selectedFxStrength())+'px)',mirror,1.08);
    }else if(fx==='video-bg'){
      if(!drawBrowserVideoBackground(ctx,width,height)){
        ctx.fillStyle='#050808';
        ctx.fillRect(0,0,width,height);
      }
    }else{
      ctx.fillStyle=fx==='black-bg'?'#000000':(fx==='custom-bg'?selectedFxBgColor():'#00ff00');
      ctx.fillRect(0,0,width,height);
    }
    ensureCanvasSize(browserFxPersonCanvas,width,height);
    browserFxPersonCtx.clearRect(0,0,width,height);
    drawBrowserVideoTo(browserFxPersonCtx,width,height,'none',mirror,1);
    browserFxPersonCtx.save();
    browserFxPersonCtx.globalCompositeOperation='destination-in';
    drawBrowserMaskTo(browserFxPersonCtx,width,height,mirror);
    browserFxPersonCtx.restore();
    browserFxPersonCtx.globalCompositeOperation='source-over';
    ctx.drawImage(browserFxPersonCanvas,0,0,width,height);
    return true;
  }
  return drawBrowserVideoTo(ctx,width,height,browserFxFilterFor(fx),mirror,1);
}
function transportBitrate(codec,width,height,fps){
  const pixels=width*height;
  const fpsScale=Math.max(0.5,fps/30);
  const quality=selectedTransportQuality();
  const qualityScale=quality==='low'?0.65:(quality==='balanced'?1.0:1.35);
  const base=codec==='h264'?1150000:(codec==='vp9'?900000:950000);
  const min=codec==='h264'?180000:(codec==='vp9'?140000:150000);
  return Math.max(min,Math.round(base*(pixels/(640*360))*fpsScale*qualityScale));
}
function bytesToBase64(bytes){
  let binary='';
  for(let i=0;i<bytes.length;i+=8192){
    const slice=bytes.subarray(i,Math.min(i+8192,bytes.length));
    binary+=String.fromCharCode.apply(null,slice);
  }
  return btoa(binary);
}
function avccToZapConfigBase64(description){
  const bytes=description instanceof Uint8Array?description:new Uint8Array(description||[]);
  if(bytes.length<7||bytes[0]!==1) return '';
  let p=5;
  const spsCount=bytes[p++]&31;
  if(spsCount<1||p+2>bytes.length) return '';
  const spsLen=(bytes[p]<<8)|bytes[p+1]; p+=2;
  if(spsLen<=0||p+spsLen>bytes.length) return '';
  const sps=bytes.slice(p,p+spsLen); p+=spsLen;
  if(p>=bytes.length) return '';
  const ppsCount=bytes[p++];
  if(ppsCount<1||p+2>bytes.length) return '';
  const ppsLen=(bytes[p]<<8)|bytes[p+1]; p+=2;
  if(ppsLen<=0||p+ppsLen>bytes.length) return '';
  const pps=bytes.slice(p,p+ppsLen);
  const out=new Uint8Array(2+sps.length+2+pps.length);
  out[0]=(sps.length>>8)&255; out[1]=sps.length&255; out.set(sps,2);
  const ppsOffset=2+sps.length;
  out[ppsOffset]=(pps.length>>8)&255; out[ppsOffset+1]=pps.length&255; out.set(pps,ppsOffset+2);
  return bytesToBase64(out);
}
function splitAnnexBNals(bytes){
  const nals=[];
  const findStart=(from)=>{
    for(let i=from;i+3<bytes.length;i++){
      if(bytes[i]===0&&bytes[i+1]===0&&bytes[i+2]===1) return {pos:i,len:3};
      if(i+4<bytes.length&&bytes[i]===0&&bytes[i+1]===0&&bytes[i+2]===0&&bytes[i+3]===1) return {pos:i,len:4};
    }
    return null;
  };
  let current=findStart(0);
  while(current){
    const nalStart=current.pos+current.len;
    const next=findStart(nalStart);
    const nalEnd=next?next.pos:bytes.length;
    if(nalEnd>nalStart) nals.push(bytes.slice(nalStart,nalEnd));
    current=next;
  }
  return nals;
}
function h264NalsToAvcc(nals){
  const frameNals=nals.filter(nal=>{
    const type=nal.length?nal[0]&31:0;
    return type!==7&&type!==8&&type!==9;
  });
  let size=0;
  frameNals.forEach(nal=>{ size+=4+nal.length; });
  const out=new Uint8Array(size);
  let p=0;
  frameNals.forEach(nal=>{
    out[p++]=(nal.length>>>24)&255;
    out[p++]=(nal.length>>>16)&255;
    out[p++]=(nal.length>>>8)&255;
    out[p++]=nal.length&255;
    out.set(nal,p);
    p+=nal.length;
  });
  return out;
}
function h264ConfigFromNalsBase64(nals){
  const sps=nals.find(nal=>nal.length&&((nal[0]&31)===7));
  const pps=nals.find(nal=>nal.length&&((nal[0]&31)===8));
  if(!sps||!pps||sps.length>65535||pps.length>65535) return '';
  const out=new Uint8Array(2+sps.length+2+pps.length);
  out[0]=(sps.length>>8)&255; out[1]=sps.length&255; out.set(sps,2);
  const ppsOffset=2+sps.length;
  out[ppsOffset]=(pps.length>>8)&255; out[ppsOffset+1]=pps.length&255; out.set(pps,ppsOffset+2);
  return bytesToBase64(out);
}
function h264ConfigFromAvccFrameBase64(bytes){
    let offset=0;
    let sps=null;
    let pps=null;
    while(offset+4<=bytes.length){
        const nalLen=((bytes[offset]<<24)|(bytes[offset+1]<<16)|(bytes[offset+2]<<8)|bytes[offset+3])>>>0;
        offset+=4;
        if(nalLen<=0||offset+nalLen>bytes.length) break;
        const nal=bytes.slice(offset,offset+nalLen);
        const nalType=nal.length?(nal[0]&31):0;
        if(nalType===7&&!sps) sps=nal;
        else if(nalType===8&&!pps) pps=nal;
        offset+=nalLen;
    }
    if(!sps||!pps||sps.length>65535||pps.length>65535) return '';
    const out=new Uint8Array(2+sps.length+2+pps.length);
    out[0]=(sps.length>>8)&255; out[1]=sps.length&255; out.set(sps,2);
    const ppsOffset=2+sps.length;
    out[ppsOffset]=(pps.length>>8)&255; out[ppsOffset+1]=pps.length&255; out.set(pps,ppsOffset+2);
    return bytesToBase64(out);
}
async function refreshCameras(){
  if(!navigator.mediaDevices||!navigator.mediaDevices.enumerateDevices){
    setCamStatus('Browser camera API unavailable');
    return;
  }
  try{
    const devices=await navigator.mediaDevices.enumerateDevices();
    const current=camSelect.value;
    camSelect.innerHTML='<option value="">Default camera</option>';
    devices.filter(d=>d.kind==='videoinput').forEach((device,index)=>{
      const opt=document.createElement('option');
      opt.value=device.deviceId;
      opt.textContent=device.label||('Camera '+String(index+1));
      camSelect.appendChild(opt);
    });
    if(current) camSelect.value=current;
  }catch(e){
    setCamStatus('Could not list cameras');
  }
}
async function armBrowserZapSend(codec){
  const res=await fetch(helperMutationUrl('/zap-browser-camera-enable?codec='+encodeURIComponent(codec||'mjpeg')),{method:'POST',cache:'no-store'});
  const payload=await res.json().catch(()=>({ok:false,error:'helper did not return JSON'}));
  if(!res.ok||!payload.ok) throw new Error(payload.error||'Zap camera send could not start');
  browserSendArmed=true;
  browserActiveCodec=String(payload.codec||codec||'mjpeg').toLowerCase();
  return browserActiveCodec;
}
async function disarmBrowserZapSend(){
  browserSendArmed=false;
  try{ await fetch(helperMutationUrl('/zap-browser-camera-stop'),{method:'POST',cache:'no-store'}); }catch(e){}
}
async function pollBrowserCameraState(){
  if(!browserStream||!browserEncoder) return;
  try{
    const res=await fetch('/zap-browser-camera-state?seq='+encodeURIComponent(String(browserFrameCounter)),{cache:'no-store'});
    if(!res.ok) return;
    const payload=await res.json();
    const requestId=String(payload.keyframeRequestId||'');
    if(requestId&&requestId!==browserLastKeyframeRequestId){
      browserLastKeyframeRequestId=requestId;
      browserForceNextKeyframe=true;
    }
  }catch(e){}
}
function startBrowserCameraStatePolling(){
  if(browserCameraStateTimer) clearInterval(browserCameraStateTimer);
  browserCameraStateTimer=setInterval(pollBrowserCameraState,25);
  pollBrowserCameraState();
}
function stopBrowserCamera(disarm=true){
  if(browserTimer){ clearTimeout(browserTimer); browserTimer=0; }
  if(browserApplySettingsTimer){ clearTimeout(browserApplySettingsTimer); browserApplySettingsTimer=0; }
  if(browserCameraStateTimer){ clearInterval(browserCameraStateTimer); browserCameraStateTimer=0; }
  closeBrowserVideoEncoder();
  browserForceNextKeyframe=false;
  browserLastKeyframeRequestId='';
  browserSettingsApplying=false;
  browserPendingSettingsApply=false;
  stopBrowserBgVideo();
  resetBrowserFxMask();
)HTML";
        html << R"HTML(
  if(browserStream){ browserStream.getTracks().forEach(track=>track.stop()); browserStream=null; }
  browserPreview.srcObject=null;
  browserFxPreviewCtx.fillStyle='#050808';
  browserFxPreviewCtx.fillRect(0,0,browserFxPreview.width,browserFxPreview.height);
  browserPosting=false;
  browserActiveCodec='mjpeg';
  camStart.disabled=false;
  camStop.disabled=true;
  if(disarm) disarmBrowserZapSend();
  setCamStatus('Browser camera stopped');
}
function scheduleBrowserFrame(){
  const fps=selectedTransportFps();
  browserTimer=setTimeout(captureBrowserFrame,1000/fps);
}
async function postBrowserJpeg(blob,captureStartedMs,encodeMs,width,height){
  const ageMs=Math.max(0,performance.now()-captureStartedMs);
  if(ageMs>650){
    setCamStatus('Dropping late browser frame '+Math.round(ageMs)+'ms');
    return;
  }
  const url='/zap-browser-camera-frame?codec=mjpeg&ageMs='+encodeURIComponent(String(Math.round(ageMs)))
    +'&encodeMs='+encodeURIComponent(String(Math.round(encodeMs)))
    +'&width='+encodeURIComponent(String(width||0))
    +'&height='+encodeURIComponent(String(height||0))
    +'&seq='+encodeURIComponent(String(++browserFrameCounter));
  const res=await fetch(helperMutationUrl(url),{method:'POST',headers:{'Content-Type':'image/jpeg'},body:blob,cache:'no-store'});
  if(!res.ok&&res.status!==204) throw new Error('helper rejected frame '+String(res.status));
  setCamStatus('Browser camera sending MJPEG '+String(width)+'x'+String(height)+' @ '+String(selectedTransportFps())+'fps');
}
async function postBrowserEncodedBytes(bytes,codec,captureStartedMs,encodeMs,width,height,keyFrame,configBase64){
  const ageMs=Math.max(0,performance.now()-captureStartedMs);
    const configOnly=codec==='h264'&&configBase64&&(!bytes||!bytes.length);
    if(ageMs>650&&!configOnly){
    setCamStatus('Dropping late browser frame '+Math.round(ageMs)+'ms');
    return;
  }
  const url='/zap-browser-camera-frame?codec='+encodeURIComponent(codec)
    +'&ageMs='+encodeURIComponent(String(Math.round(ageMs)))
    +'&encodeMs='+encodeURIComponent(String(Math.round(encodeMs)))
    +'&width='+encodeURIComponent(String(width||0))
    +'&height='+encodeURIComponent(String(height||0))
    +'&key='+encodeURIComponent(keyFrame?'1':'0')
    +'&config='+encodeURIComponent(configBase64||'')
    +'&seq='+encodeURIComponent(String(++browserFrameCounter));
  const res=await fetch(helperMutationUrl(url),{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:bytes,cache:'no-store'});
  if(!res.ok&&res.status!==204) throw new Error('helper rejected frame '+String(res.status));
    const payloadLabel=bytes&&bytes.length?'frame':'config';
    setCamStatus('Browser camera sending '+codec.toUpperCase()+' '+payloadLabel+' '+String(width)+'x'+String(height)+' @ '+String(selectedTransportFps())+'fps');
}
async function handleEncodedBrowserChunk(chunk,metadata,codecOverride){
  const chunkCodec=codecOverride||browserEncoderCodec;
  const fallbackSize=selectedTransportSize();
  const info=browserEncodeStarts.get(chunk.timestamp)||{captureStartedMs:performance.now(),width:fallbackSize.width,height:fallbackSize.height};
  browserEncodeStarts.delete(chunk.timestamp);
  const bytes=new Uint8Array(chunk.byteLength);
  chunk.copyTo(bytes);
  const encodeMs=Math.max(0,performance.now()-info.captureStartedMs);
  let configBase64='';
  let frameBytes=bytes;
  let keyFrame=chunk.type==='key';
  if(chunkCodec==='h264'&&browserEncoderH264Format==='annexb'){
    const nals=splitAnnexBNals(bytes);
    const annexConfig=h264ConfigFromNalsBase64(nals);
    if(annexConfig) configBase64=annexConfig;
    keyFrame=keyFrame||nals.some(nal=>nal.length&&((nal[0]&31)===5));
    frameBytes=h264NalsToAvcc(nals);
        if(!frameBytes.length&&configBase64)
            return await postBrowserEncodedBytes(new Uint8Array(),chunkCodec,info.captureStartedMs,encodeMs,info.width,info.height,keyFrame,configBase64);
        if(!frameBytes.length) return;
  }
  if(chunkCodec==='h264'&&metadata&&metadata.decoderConfig&&metadata.decoderConfig.description){
    const avcConfig=avccToZapConfigBase64(new Uint8Array(metadata.decoderConfig.description));
    if(avcConfig) configBase64=avcConfig;
  }
    if(chunkCodec==='h264'&&!configBase64){
        const avccConfig=h264ConfigFromAvccFrameBase64(frameBytes);
        if(avccConfig) configBase64=avccConfig;
    }
    const configChanged=chunkCodec==='h264'&&configBase64&&configBase64!==browserLastCodecConfig;
    if(configBase64) browserLastCodecConfig=configBase64;
  else if(chunkCodec==='h264'&&keyFrame&&browserLastCodecConfig) configBase64=browserLastCodecConfig;
    if(configChanged)
        await postBrowserEncodedBytes(new Uint8Array(),chunkCodec,info.captureStartedMs,encodeMs,info.width,info.height,false,configBase64);
  await postBrowserEncodedBytes(frameBytes,chunkCodec,info.captureStartedMs,encodeMs,info.width,info.height,keyFrame,configBase64);
}
async function getSupportedEncoderConfig(codec,width,height,fps){
  if(codec==='mjpeg') return null;
  if(!('VideoEncoder' in window)||!('VideoFrame' in window)) return null;
  browserEncoderH264Format='avc';
  const bitrate=transportBitrate(codec,width,height,fps);
  const bases=codec==='h264'
    ? [
        {codec:'avc1.42E01F',width,height,bitrate,framerate:fps,latencyMode:'realtime',avc:{format:'annexb'}},
        {codec:'avc1.42E01F',width,height,bitrate,framerate:fps,latencyMode:'realtime',avc:{format:'avc'}}
      ]
    : (codec==='vp9'
      ? [
          {codec:'vp09.00.10.08',width,height,bitrate,framerate:fps,latencyMode:'realtime'}
        ]
      : [
          {codec:'vp8',width,height,bitrate,framerate:fps,latencyMode:'realtime'}
        ]);
  for(const base of bases){
    const preferred=Object.assign({},base,{hardwareAcceleration:'prefer-hardware'});
    const preferredSupport=await VideoEncoder.isConfigSupported(preferred).catch(()=>null);
    if(!preferredSupport||preferredSupport.supported!==false){
      if(codec==='h264') browserEncoderH264Format=base.avc&&base.avc.format==='annexb'?'annexb':'avc';
      return preferred;
    }
    const fallbackSupport=await VideoEncoder.isConfigSupported(base).catch(()=>null);
    if(!fallbackSupport||fallbackSupport.supported!==false){
      if(codec==='h264') browserEncoderH264Format=base.avc&&base.avc.format==='annexb'?'annexb':'avc';
      return base;
    }
  }
  return null;
}
function closeBrowserVideoEncoder(){
  if(browserEncoder){
    try{ browserEncoder.close(); }catch(e){}
    browserEncoder=null;
  }
  browserEncodeStarts.clear();
  browserLastCodecConfig='';
  browserLastKeyframeMs=0;
  browserForceNextKeyframe=true;
  browserEncoderH264Format='avc';
}
async function startBrowserVideoEncoder(codec,width,height,fps){
  if(codec==='mjpeg') return;
  if(!('VideoEncoder' in window)||!('VideoFrame' in window)) throw new Error('Browser video encoder unavailable');
  browserEncoderCodec=codec;
  const config=await getSupportedEncoderConfig(codec,width,height,fps);
  if(!config) throw new Error(codec.toUpperCase()+' browser encoder unsupported');
  browserEncoder=new VideoEncoder({
    output(chunk,metadata){ handleEncodedBrowserChunk(chunk,metadata,codec).catch(e=>setCamStatus(String(e.message||e))); },
    error(err){ setCamStatus('Browser encoder error: '+String(err&&err.message?err.message:err)); }
  });
  browserEncoder.configure(config);
}
async function configureBrowserEncoderForSelectedSettings(){
  const desiredCodec=selectedTransportCodec();
  const {width,height}=selectedTransportSize();
  const fps=selectedTransportFps();
  closeBrowserVideoEncoder();
  browserEncoderCodec='mjpeg';
  let activeCodec=desiredCodec;
  if(desiredCodec!=='mjpeg'){
    try{
      await startBrowserVideoEncoder(desiredCodec,width,height,fps);
    }catch(encoderError){
      activeCodec='mjpeg';
      browserEncoderCodec='mjpeg';
      closeBrowserVideoEncoder();
      setCamStatus(desiredCodec.toUpperCase()+' encoder unavailable, using MJPEG');
    }
  }
  activeCodec=await armBrowserZapSend(activeCodec);
  browserActiveCodec=activeCodec;
  browserForceNextKeyframe=true;
  return activeCodec;
}
async function applyLiveVideoConstraints(){
  if(!browserStream) return false;
  const tracks=browserStream.getVideoTracks();
  const track=tracks&&tracks.length?tracks[0]:null;
  if(!track||!track.applyConstraints) return false;
  await track.applyConstraints(selectedVideoConstraints());
  resetBrowserFxMask();
  return true;
}
async function replaceBrowserCameraStream(){
  if(!navigator.mediaDevices||!navigator.mediaDevices.getUserMedia) throw new Error('Browser camera API unavailable');
  const nextStream=await navigator.mediaDevices.getUserMedia({audio:false,video:selectedVideoConstraints()});
  const previousStream=browserStream;
  browserStream=nextStream;
  browserPreview.srcObject=browserStream;
  await browserPreview.play();
  if(previousStream) previousStream.getTracks().forEach(track=>track.stop());
  resetBrowserFxMask();
  browserFxSegmenterFailed=false;
  await refreshCameras();
}
async function applyBrowserCameraSettings(){
  if(!browserStream) return;
  if(browserSettingsApplying){
    browserPendingSettingsApply=true;
    return;
  }
  browserSettingsApplying=true;
  try{
    updateTransportControls();
    let trackUpdated=false;
    try{
      trackUpdated=await applyLiveVideoConstraints();
    }catch(trackError){
      trackUpdated=false;
    }
    if(!trackUpdated) await replaceBrowserCameraStream();
    const activeCodec=await configureBrowserEncoderForSelectedSettings();
    setCamStatus('Browser camera updated '+activeCodec.toUpperCase()+' '+String(selectedTransportSize().width)+'x'+String(selectedTransportSize().height)+' @ '+String(selectedTransportFps())+'fps');
  }catch(e){
    setCamStatus('Could not apply camera settings live: '+String(e&&e.message?e.message:e));
  }finally{
    browserSettingsApplying=false;
    if(browserPendingSettingsApply){
      browserPendingSettingsApply=false;
      scheduleBrowserCameraSettingsApply();
    }
  }
}
function scheduleBrowserCameraSettingsApply(){
  updateTransportControls();
  if(!browserStream) return;
  if(browserApplySettingsTimer) clearTimeout(browserApplySettingsTimer);
  browserApplySettingsTimer=setTimeout(()=>{
    browserApplySettingsTimer=0;
    applyBrowserCameraSettings();
  },120);
}
function captureBrowserFrame(){
  if(!browserStream||browserPosting){
    if(browserStream) scheduleBrowserFrame();
    return;
  }
  const {width,height}=selectedTransportSize();
  const captureStartedMs=performance.now();
  try{
    if(browserEncoder){
      if(browserEncoder.encodeQueueSize>2){
        setCamStatus('Dropping frame, encoder busy');
        scheduleBrowserFrame();
        return;
      }
      const timestamp=Math.round(captureStartedMs*1000);
      browserEncodeStarts.set(timestamp,{captureStartedMs,width,height});
      browserCanvas.width=width;
      browserCanvas.height=height;
      if(!drawBrowserFxFrame(browserCtx,width,height)){
        browserEncodeStarts.delete(timestamp);
        scheduleBrowserFrame();
        return;
      }
      const frame=new VideoFrame(browserCanvas,{timestamp});
      const needsKey=browserLastKeyframeMs<=0
        || browserForceNextKeyframe
        || (captureStartedMs-browserLastKeyframeMs)>2000;
      if(needsKey) browserLastKeyframeMs=captureStartedMs;
      browserForceNextKeyframe=false;
      browserEncoder.encode(frame,{keyFrame:needsKey});
      frame.close();
      scheduleBrowserFrame();
      return;
    }
    browserPosting=true;
    browserCanvas.width=width;
    browserCanvas.height=height;
    if(!drawBrowserFxFrame(browserCtx,width,height)){
      browserPosting=false;
      scheduleBrowserFrame();
      return;
    }
    browserCanvas.toBlob(async blob=>{
      const encodeMs=Math.max(0,performance.now()-captureStartedMs);
      try{
        if(blob&&blob.size>0) await postBrowserJpeg(blob,captureStartedMs,encodeMs,width,height);
      }catch(e){
        setCamStatus(String(e.message||e));
      }finally{
        browserPosting=false;
        if(browserStream) scheduleBrowserFrame();
      }
    },'image/jpeg',selectedJpegQuality());
  }catch(e){
    browserPosting=false;
    setCamStatus('Browser capture failed');
    if(browserStream) scheduleBrowserFrame();
  }
}
async function startBrowserCamera(){
  stopBrowserCamera(false);
  await disarmBrowserZapSend();
  if(!navigator.mediaDevices||!navigator.mediaDevices.getUserMedia){
    setCamStatus('Browser camera API unavailable');
    return;
  }
  const codec=selectedTransportCodec();
  try{
    browserStream=await navigator.mediaDevices.getUserMedia({audio:false,video:selectedVideoConstraints()});
    browserPreview.srcObject=browserStream;
    await browserPreview.play();
    resetBrowserFxMask();
    browserFxSegmenterFailed=false;
    const activeCodec=await configureBrowserEncoderForSelectedSettings();
  camStart.disabled=true;
  camStop.disabled=false;
  await refreshCameras();
  setCamStatus(activeCodec===codec
    ? 'Browser camera started '+activeCodec.toUpperCase()
    : codec.toUpperCase()+' unavailable, sending '+activeCodec.toUpperCase());
  startBrowserCameraStatePolling();
  scheduleBrowserFrame();
)HTML";
        html << R"HTML(
  }catch(e){
    setCamStatus('Could not start browser camera: '+String(e.message||e));
    stopBrowserCamera();
  }
}
camRefresh.addEventListener('click',refreshCameras);
camStart.addEventListener('click',startBrowserCamera);
camStop.addEventListener('click',stopBrowserCamera);
camSelect.addEventListener('change',scheduleBrowserCameraSettingsApply);
camCodec.addEventListener('change',()=>{
  updateTransportControls();
  scheduleBrowserCameraSettingsApply();
});
camSize.addEventListener('change',scheduleBrowserCameraSettingsApply);
camFps.addEventListener('change',scheduleBrowserCameraSettingsApply);
camQuality.addEventListener('change',scheduleBrowserCameraSettingsApply);
camLayout.addEventListener('change',applyGridLayout);
camMask.addEventListener('change',refreshDisplayMasks);
camFx.addEventListener('change',()=>{
  resetBrowserFxMask();
  browserFxSegmenterFailed=false;
  updateFxControls();
});
camVideoBg.addEventListener('change',()=>{
  browserBgVideoUrl='';
  syncBrowserBgVideo();
});
camObs.addEventListener('click',()=>window.open('/zap-wall?layout='+encodeURIComponent(normalizedGridLayout(camLayout.value)),'_blank'));
applyGridLayout();
updateTransportControls();
updateFxControls();
refreshCameras();
function tileFor(stream){
  let tile=tiles.get(stream.streamKey);
  if(tile) return tile;
  tile=document.createElement('section');
  tile.className='tile';
  const label=document.createElement('div');
  label.className='label';
  const labelName=document.createElement('span');
  labelName.className='label-name';
  const copyButton=document.createElement('button');
  copyButton.className='copy-link';
  copyButton.type='button';
  copyButton.textContent='OBS';
  copyButton.title='Copy stable OBS slot link';
  label.appendChild(labelName);
  label.appendChild(copyButton);
  const stage=document.createElement('div');
  stage.className='stage';
  const img=document.createElement('img');
  const canvas=document.createElement('canvas');
  canvas.width=1280;
  canvas.height=720;
  canvas.style.display='none';
  const note=document.createElement('div');
  note.className='stage-note';
  stage.appendChild(img);
  stage.appendChild(canvas);
  stage.appendChild(note);
  tile.appendChild(label);
  tile.appendChild(stage);
  tile._label=label;
  tile._labelName=labelName;
  tile._copyButton=copyButton;
  tile._obsSlot=0;
  tile._img=img;
  tile._canvas=canvas;
  tile._ctx=canvas.getContext('2d',{alpha:false});
  tile._note=note;
  tile._h264Timestamp=0;
  tile._h264SeenKey=false;
  tile._vpxSeenKey=false;
  tile._decodeBufferId='';
  tile._lastDecodedFrameIndex=-1;
  tile._targetFrameIndex=-1;
  tile._latestStream=null;
  tile._localPreview=false;
  tile._displayMask='';
  tile._playbackClockBufferId='';
  tile._playbackClockStartMs=0;
  tile._playbackClockDurationMs=1000;
  copyButton.addEventListener('click',event=>{
    event.stopPropagation();
    copyObsLink(tile._obsSlot);
  });
  tiles.set(stream.streamKey,tile);
  grid.appendChild(tile);
  return tile;
}
function base64ToBytes(text){
  const binary=atob(String(text||''));
  const bytes=new Uint8Array(binary.length);
  for(let i=0;i<binary.length;i++) bytes[i]=binary.charCodeAt(i);
  return bytes;
}
function parseH264Config(configText){
  if(!configText) return null;
  const bytes=base64ToBytes(configText);
  if(bytes.length<8) return null;
  const spsLen=(bytes[0]<<8)|bytes[1];
  const ppsOffset=2+spsLen;
  if(spsLen<=0||ppsOffset+2>bytes.length) return null;
  const ppsLen=(bytes[ppsOffset]<<8)|bytes[ppsOffset+1];
  if(ppsLen<=0||ppsOffset+2+ppsLen!==bytes.length) return null;
  const sps=bytes.slice(2,2+spsLen);
  const pps=bytes.slice(ppsOffset+2,ppsOffset+2+ppsLen);
  if(sps.length<4||pps.length<1) return null;
  const codec='avc1.'+[sps[1],sps[2],sps[3]].map(v=>v.toString(16).padStart(2,'0')).join('').toUpperCase();
  const avcc=new Uint8Array(11+sps.length+pps.length);
  let p=0;
  avcc[p++]=1; avcc[p++]=sps[1]; avcc[p++]=sps[2]; avcc[p++]=sps[3]; avcc[p++]=0xff; avcc[p++]=0xe1;
  avcc[p++]=(sps.length>>8)&255; avcc[p++]=sps.length&255; avcc.set(sps,p); p+=sps.length;
  avcc[p++]=1; avcc[p++]=(pps.length>>8)&255; avcc[p++]=pps.length&255; avcc.set(pps,p);
  return {codec,description:avcc};
}
function h264FrameType(bytes){
  let offset=0;
  while(offset+4<=bytes.length){
    const n=(bytes[offset]<<24)|(bytes[offset+1]<<16)|(bytes[offset+2]<<8)|bytes[offset+3];
    const nalLen=n>>>0;
    offset+=4;
    if(nalLen<=0||offset+nalLen>bytes.length) break;
    const nalType=bytes[offset]&31;
    if(nalType===5) return 'key';
    offset+=nalLen;
  }
  return 'delta';
}
function vp8FrameType(bytes){
  return bytes.length>0&&((bytes[0]&1)===0)?'key':'delta';
}
function vp9FrameType(bytes){
  if(!bytes.length) return 'delta';
  const b=bytes[0];
  const profile=((b>>2)&1)|(((b>>3)&1)<<1);
  let bit=4;
  if(profile===3) bit++;
  const showExisting=((b>>bit)&1)!==0;
  bit++;
  if(showExisting) return 'delta';
  return ((b>>bit)&1)===0?'key':'delta';
}
function frameIndexOf(stream){
  const localStart=Number(stream._localPlaybackStartMs||0);
  const localDuration=Number(stream._localPlaybackDurationMs||0);
  const localFrameCount=Number(stream.frameCount||0);
  if(Number.isFinite(localStart)&&localStart>0
     && Number.isFinite(localDuration)&&localDuration>1
     && Number.isFinite(localFrameCount)&&localFrameCount>1){
    const progress=Math.max(0,Math.min(0.999999,(performance.now()-localStart)/localDuration));
    return Math.max(0,Math.min(localFrameCount-1,Math.floor(progress*localFrameCount)));
  }
  const n=Number(stream.playbackFrameIndex||0);
  return Number.isFinite(n)&&n>=0?Math.floor(n):0;
}
function bufferIdOf(stream){
  return String(stream.playbackBufferId||stream.refreshId||'');
}
function frameUrl(stream,frameIndex){
  return '/zap-frame?stream='+encodeURIComponent(stream.streamKey)
    +'&frame='+encodeURIComponent(String(frameIndexOf({playbackFrameIndex:frameIndex})))
    +'&r='+encodeURIComponent(String(bufferIdOf(stream))+'-'+String(frameIndex));
}
function frameBatchUrl(stream,fromFrame,toFrame){
  return '/zap-frame-batch?stream='+encodeURIComponent(stream.streamKey)
    +'&from='+encodeURIComponent(String(Math.max(0,fromFrame|0)))
    +'&to='+encodeURIComponent(String(Math.max(0,toFrame|0)))
    +'&r='+encodeURIComponent(String(bufferIdOf(stream))+'-'+String(fromFrame)+'-'+String(toFrame));
}
function parseFrameBatch(bytes){
  const frames=[];
  let offset=0;
  while(offset+4<=bytes.length){
    const len=((bytes[offset]<<24)|(bytes[offset+1]<<16)|(bytes[offset+2]<<8)|bytes[offset+3])>>>0;
    offset+=4;
    if(offset+len>bytes.length) break;
    frames.push(len>0?bytes.slice(offset,offset+len):new Uint8Array());
    offset+=len;
  }
  return frames;
}
async function ensureH264Decoder(tile,stream){
  if(!('VideoDecoder' in window)){
    tile._note.textContent='Browser H.264 decode unavailable';
    return false;
  }
  const configKey=String(stream.h264ConfigId||'')+'|'+String(stream.h264Config||'');
  if(tile._decoder&&tile._decoderConfigKey===configKey) return true;
  if(tile._decoder){ try{tile._decoder.close();}catch(e){} }
  tile._decoder=null;
)HTML";
        html << R"HTML(
  tile._decoderConfigKey='';
  tile._h264SeenKey=false;
  const parsed=parseH264Config(stream.h264Config||'');
  if(!parsed){
    tile._note.textContent='Waiting for H.264 config';
    return false;
  }
  let config={codec:parsed.codec,description:parsed.description,hardwareAcceleration:'prefer-hardware',optimizeForLatency:true};
  let support=await VideoDecoder.isConfigSupported(config).catch(()=>null);
  if(support&&support.supported===false){
    config={codec:parsed.codec,description:parsed.description,optimizeForLatency:true};
    support=await VideoDecoder.isConfigSupported(config).catch(()=>null);
  }
  if(support&&support.supported===false){
    tile._note.textContent='Browser H.264 codec unsupported';
    return false;
  }
  tile._decoder=new VideoDecoder({
    output(frame){
      tile._canvas.width=frame.displayWidth||frame.codedWidth||1280;
      tile._canvas.height=frame.displayHeight||frame.codedHeight||720;
      tile._ctx.drawImage(frame,0,0,tile._canvas.width,tile._canvas.height);
      frame.close();
      tile._note.textContent='';
    },
    error(err){
      tile._note.textContent='H.264 decode error: '+String(err&&err.message?err.message:err);
      tile._h264SeenKey=false;
      try{ if(tile._decoder) tile._decoder.close(); }catch(e){}
      tile._decoder=null;
    }
  });
  tile._decoder.configure(config);
  tile._decoderConfigKey=configKey;
  return true;
}
async function ensureVpxDecoder(tile,codec){
  if(!('VideoDecoder' in window)){
    tile._note.textContent='Browser '+codec.toUpperCase()+' decode unavailable';
    return false;
  }
  const decoderCodec=codec==='vp9'?'vp09.00.10.08':'vp8';
  if(tile._decoder&&tile._decoderConfigKey===decoderCodec) return true;
  if(tile._decoder){ try{tile._decoder.close();}catch(e){} }
  tile._decoder=null;
  tile._decoderConfigKey='';
  tile._vpxSeenKey=false;
  let config={codec:decoderCodec,hardwareAcceleration:'prefer-hardware',optimizeForLatency:true};
  let support=await VideoDecoder.isConfigSupported(config).catch(()=>null);
  if(support&&support.supported===false){
    config={codec:decoderCodec,optimizeForLatency:true};
    support=await VideoDecoder.isConfigSupported(config).catch(()=>null);
  }
  if(support&&support.supported===false){
    tile._note.textContent='Browser '+codec.toUpperCase()+' codec unsupported';
    return false;
  }
  tile._decoder=new VideoDecoder({
    output(frame){
      tile._canvas.width=frame.displayWidth||frame.codedWidth||1280;
      tile._canvas.height=frame.displayHeight||frame.codedHeight||720;
      tile._ctx.drawImage(frame,0,0,tile._canvas.width,tile._canvas.height);
      frame.close();
      tile._note.textContent='';
    },
    error(err){
      tile._note.textContent=codec.toUpperCase()+' decode error';
      tile._vpxSeenKey=false;
      try{ if(tile._decoder) tile._decoder.close(); }catch(e){}
      tile._decoder=null;
    }
  });
  tile._decoder.configure(config);
  tile._decoderConfigKey=decoderCodec;
  return true;
}
async function ensureVp8Decoder(tile){ return ensureVpxDecoder(tile,'vp8'); }
async function ensureVp9Decoder(tile){ return ensureVpxDecoder(tile,'vp9'); }
async function renderH264(tile,stream){
  const bufferId=bufferIdOf(stream);
  const targetFrameIndex=frameIndexOf(stream);
  const renderToken=bufferId+'-'+String(targetFrameIndex);
  if(tile._decodeBufferId!==bufferId){
    tile._decodeBufferId=bufferId;
    tile._lastDecodedFrameIndex=-1;
    tile._targetFrameIndex=-1;
  }
  tile._targetFrameIndex=Math.max(tile._targetFrameIndex||0,targetFrameIndex);
  if(tile._h264Pending||tile._lastRenderedRefreshId===renderToken) return;
  tile._h264Pending=true;
  try{
    if(!await ensureH264Decoder(tile,stream)) return;
    const fromFrame=tile._lastDecodedFrameIndex+1;
    const toFrame=Math.min(tile._targetFrameIndex,fromFrame+89);
    const res=await fetch(frameBatchUrl(stream,fromFrame,toFrame),{cache:'no-store'});
    if(!res.ok) return;
    const batch=parseFrameBatch(new Uint8Array(await res.arrayBuffer()));
    for(let i=0;i<batch.length;i++){
      const nextFrame=fromFrame+i;
      const bytes=batch[i];
      if(!bytes.length) break;
      const type=h264FrameType(bytes);
      if(type==='key') tile._h264SeenKey=true;
      if(!tile._h264SeenKey&&type!=='key'){
        tile._note.textContent='Waiting for H.264 keyframe';
        tile._lastDecodedFrameIndex=nextFrame;
        continue;
      }
      tile._decoder.decode(new EncodedVideoChunk({
        type,
        timestamp:++tile._h264Timestamp*33333,
        data:bytes
      }));
      tile._lastDecodedFrameIndex=nextFrame;
    }
    tile._lastRenderedRefreshId=renderToken;
  }catch(e){
    tile._note.textContent='H.264 browser decode failed: '+String(e&&e.message?e.message:e);
    if(tile._decoder){ try{tile._decoder.close();}catch(closeErr){} tile._decoder=null; }
  }finally{
    tile._h264Pending=false;
  }
}
async function renderVpx(tile,stream,codec){
  const bufferId=bufferIdOf(stream);
  const targetFrameIndex=frameIndexOf(stream);
  const renderToken=bufferId+'-'+String(targetFrameIndex);
  if(tile._decodeBufferId!==bufferId){
    tile._decodeBufferId=bufferId;
    tile._lastDecodedFrameIndex=-1;
    tile._targetFrameIndex=-1;
  }
  tile._targetFrameIndex=Math.max(tile._targetFrameIndex||0,targetFrameIndex);
  if(tile._vpxPending||tile._lastRenderedRefreshId===renderToken) return;
  tile._vpxPending=true;
  try{
    if(!await ensureVpxDecoder(tile,codec)) return;
    const fromFrame=tile._lastDecodedFrameIndex+1;
    const toFrame=Math.min(tile._targetFrameIndex,fromFrame+89);
    const res=await fetch(frameBatchUrl(stream,fromFrame,toFrame),{cache:'no-store'});
    if(!res.ok) return;
    const batch=parseFrameBatch(new Uint8Array(await res.arrayBuffer()));
    for(let i=0;i<batch.length;i++){
      const nextFrame=fromFrame+i;
      const bytes=batch[i];
      if(!bytes.length) break;
      const type=codec==='vp9'?vp9FrameType(bytes):vp8FrameType(bytes);
      if(type==='key') tile._vpxSeenKey=true;
      if(!tile._vpxSeenKey&&type!=='key'){
        tile._note.textContent='Waiting for '+codec.toUpperCase()+' keyframe';
        tile._lastDecodedFrameIndex=nextFrame;
        continue;
      }
      tile._decoder.decode(new EncodedVideoChunk({
        type,
        timestamp:++tile._h264Timestamp*33333,
        data:bytes
      }));
      tile._lastDecodedFrameIndex=nextFrame;
    }
    tile._lastRenderedRefreshId=renderToken;
  }catch(e){
    tile._note.textContent=codec.toUpperCase()+' browser decode failed';
    if(tile._decoder){ try{tile._decoder.close();}catch(closeErr){} tile._decoder=null; }
  }finally{
    tile._vpxPending=false;
  }
}
async function renderVp8(tile,stream){ return renderVpx(tile,stream,'vp8'); }
async function renderVp9(tile,stream){ return renderVpx(tile,stream,'vp9'); }
function renderStream(tile,stream){
  const codec=String(stream.codec||'mjpeg').toLowerCase();
  if(codec==='h264'){
    tile._img.style.display='none';
    tile._canvas.style.display='block';
    renderH264(tile,stream);
    return;
  }
  if(codec==='vp8'){
    tile._img.style.display='none';
    tile._canvas.style.display='block';
    renderVp8(tile,stream);
    return;
  }
  if(codec==='vp9'){
    tile._img.style.display='none';
    tile._canvas.style.display='block';
    renderVp9(tile,stream);
    return;
  }
  tile._canvas.style.display='none';
  tile._img.style.display='block';
  tile._note.textContent='';
  const url=frameUrl(stream,frameIndexOf(stream));
  if(tile._lastImgUrl!==url){
    tile._lastImgUrl=url;
    const nextImg=new Image();
    nextImg.onload=()=>{ if(tile._lastImgUrl===url) tile._img.src=url; };
    nextImg.src=url;
  }
  tile._lastRenderedRefreshId=bufferIdOf(stream)+'-'+String(frameIndexOf(stream));
}
function updateTilePlaybackClock(tile,stream){
  const bufferId=bufferIdOf(stream);
  const duration=Math.max(1,Number(stream.playbackDurationMs||0)||1000);
  const age=Math.max(0,Number(stream.playbackAgeMs||0)||0);
  const proposedStart=performance.now()-age;
  if(tile._playbackClockBufferId!==bufferId
     || Math.abs(proposedStart-tile._playbackClockStartMs)>250
     || Math.abs(duration-tile._playbackClockDurationMs)>1){
    tile._playbackClockBufferId=bufferId;
    tile._playbackClockStartMs=proposedStart;
    tile._playbackClockDurationMs=duration;
  }
  const localStream=Object.assign({},stream);
  localStream._localPlaybackStartMs=tile._playbackClockStartMs;
  localStream._localPlaybackDurationMs=tile._playbackClockDurationMs;
  tile._latestStream=localStream;
}
function renderLocalPreviewTile(tile){
  tile._img.style.display='none';
  tile._canvas.style.display='block';
  tile._note.textContent='';
  const width=browserPreview.videoWidth||640;
  const height=browserPreview.videoHeight||360;
  if(tile._canvas.width!==width) tile._canvas.width=width;
  if(tile._canvas.height!==height) tile._canvas.height=height;
  try{ drawBrowserFxFrame(tile._ctx,width,height); }catch(e){}
}
function renderBrowserFxPreview(){
  applyDisplayMaskToElement(browserFxPreview,obsMode?'none':selectedDisplayMask());
  if(!browserStream){
    browserFxPreviewCtx.fillStyle='#050808';
    browserFxPreviewCtx.fillRect(0,0,browserFxPreview.width,browserFxPreview.height);
    return;
  }
  try{ drawBrowserFxFrame(browserFxPreviewCtx,browserFxPreview.width,browserFxPreview.height); }catch(e){}
}
function renderTiles(){
  renderBeatCounter();
  renderBrowserFxPreview();
  for(const [,tile] of tiles){
    applyDisplayMaskToTile(tile);
    if(tile._localPreview) renderLocalPreviewTile(tile);
    else if(tile._latestStream) renderStream(tile,tile._latestStream);
  }
  requestAnimationFrame(renderTiles);
}
async function refresh(){
  if(refreshInFlight) return;
  refreshInFlight=true;
  try{
    const res=await fetch('/zap-frames',{cache:'no-store'});
    const payload=await res.json();
    latestIntervalInfo=intervalInfoFromPayload(payload)||latestIntervalInfo;
    const allStreams=streamsFromPayload(payload);
    const localZapIndex=allStreams.findIndex(stream=>String(stream.streamKey||'')==='local:zap-camera');
    const showDirectLocalPreview=browserStream&&!obsMode&&!obsStreamFilter&&obsSlotFilter===0;
    let streams=obsStreamFilter?allStreams.filter(stream=>String(stream.streamKey||'')===obsStreamFilter):allStreams;
    if(showDirectLocalPreview)
      streams=streams.filter(stream=>String(stream.streamKey||'')!=='local:zap-camera');
    if(!obsStreamFilter&&obsSlotFilter>0)
      streams=allStreams[obsSlotFilter-1]?[allStreams[obsSlotFilter-1]]:[];
    grid.querySelectorAll('.empty').forEach(el=>el.remove());
    const live=new Set();
    streams.forEach(stream=>{
      live.add(stream.streamKey);
      const tile=tileFor(stream);
      tile._localPreview=false;
      const slotIndex=obsSlotFilter>0?obsSlotFilter:(allStreams.findIndex(candidate=>String(candidate.streamKey||'')===String(stream.streamKey||''))+1);
      tile._obsSlot=slotIndex;
      tile._copyButton.style.display=obsMode?'none':'';
      updateTilePlaybackClock(tile,stream);
      const timing=' frames '+String(stream.frameCount||0)+' q '+ms(stream.decodeQueueMs)+'ms dec '+ms(stream.decodeMs)+'ms pub '+ms(stream.receiveToPublishMs)+'ms late '+ms(stream.playbackOffsetMs)+'ms dur '+ms(stream.playbackDurationMs)+'ms age '+ms(stream.playbackAgeMs)+'ms cap '+ms(stream.senderCaptureQueueMs)+'ms enc '+ms(stream.senderEncodeMs)+'ms';
      const title=(stream.sender||'Unknown');
      tile._labelName.textContent=showStreamDebug?title+' '+String(stream.codec||'mjpeg').toUpperCase()+timing:title;
    });
    if(showDirectLocalPreview){
      const localKey='local:browser-camera';
      live.add(localKey);
      const tile=tileFor({streamKey:localKey});
      tile._localPreview=true;
      tile._latestStream=null;
      tile._obsSlot=localZapIndex>=0?localZapIndex+1:0;
      tile._copyButton.style.display=tile._obsSlot>0?'':'none';
      tile._labelName.textContent='Local camera';
    }
    for(const [key,tile] of tiles){
      if(!live.has(key)){
        if(tile._decoder){ try{tile._decoder.close();}catch(e){} }
        tile.remove();
        tiles.delete(key);
      }
    }
    if(streams.length===0 && !showDirectLocalPreview && !grid.querySelector('.empty')){
      const empty=document.createElement('div');
      empty.className='empty';
      empty.textContent='No Zap video streams yet';
      grid.appendChild(empty);
    }
    const streamCount=streams.length+(showDirectLocalPreview?1:0);
    statusEl.textContent=streamCount?String(streamCount)+' stream'+(streamCount===1?'':'s'):'Waiting for video frames';
  }catch(e){
    statusEl.textContent='Waiting for local helper';
  }finally{
    refreshInFlight=false;
  }
}
refresh();
requestAnimationFrame(renderTiles);
setInterval(refresh,33);
</script>
</body>
</html>)HTML";
        return html.replace("__NINJAM_HELPER_TOKEN__", helperRequestToken);
    }

    void handleClient(juce::StreamingSocket& client)
    {
        const HttpRequest request = readRequest(client);
        if (request.method.isEmpty() || request.target.isEmpty())
            return;

        const HttpResponse response = buildResponse(request.method, request.target, request.body);
        sendResponse(client, request.method, response);
    }

    static int findHeaderBodyOffset(const juce::MemoryBlock& data)
    {
        const auto* bytes = static_cast<const unsigned char*>(data.getData());
        const size_t size = data.getSize();
        for (size_t i = 0; i + 3 < size; ++i)
        {
            if (bytes[i] == '\r' && bytes[i + 1] == '\n' && bytes[i + 2] == '\r' && bytes[i + 3] == '\n')
                return (int)i + 4;
        }
        for (size_t i = 0; i + 1 < size; ++i)
        {
            if (bytes[i] == '\n' && bytes[i + 1] == '\n')
                return (int)i + 2;
        }
        return -1;
    }

    static int parseContentLength(const juce::String& headerText)
    {
        juce::StringArray lines;
        lines.addLines(headerText);
        for (const auto& line : lines)
        {
            const juce::String name = line.upToFirstOccurrenceOf(":", false, false).trim().toLowerCase();
            if (name == "content-length")
                return juce::jmax(0, line.fromFirstOccurrenceOf(":", false, false).trim().getIntValue());
        }
        return 0;
    }

    HttpRequest readRequest(juce::StreamingSocket& client) const
    {
        juce::MemoryBlock requestData;
        char buffer[2048] = {};
        int headerBodyOffset = -1;
        int contentLength = 0;

        while (!threadShouldExit() && requestData.getSize() < 20 * 1024 * 1024)
        {
            if (client.waitUntilReady(true, 100) <= 0)
                break;

            const int bytesRead = client.read(buffer, (int) std::size(buffer), false);
            if (bytesRead <= 0)
                break;

            requestData.append(buffer, (size_t) bytesRead);
            if (headerBodyOffset < 0)
            {
                headerBodyOffset = findHeaderBodyOffset(requestData);
                if (headerBodyOffset >= 0)
                {
                    const juce::String headerText = juce::String::fromUTF8(static_cast<const char*>(requestData.getData()),
                                                                            headerBodyOffset);
                    contentLength = parseContentLength(headerText);
                }
            }

            if (headerBodyOffset >= 0 && requestData.getSize() >= (size_t)headerBodyOffset + (size_t)contentLength)
                break;
        }

        HttpRequest request;
        if (requestData.getSize() == 0)
            return request;

        headerBodyOffset = findHeaderBodyOffset(requestData);
        if (headerBodyOffset < 0)
            return request;

        const juce::String headerText = juce::String::fromUTF8(static_cast<const char*>(requestData.getData()),
                                                                headerBodyOffset);
        const juce::String requestLine = headerText.upToFirstOccurrenceOf("\r\n", false, false)
                                                   .upToFirstOccurrenceOf("\n", false, false)
                                                   .trim();
        request.method = requestLine.upToFirstOccurrenceOf(" ", false, false).trim().toUpperCase();
        const juce::String remainder = requestLine.fromFirstOccurrenceOf(" ", false, false).trim();
        request.target = remainder.upToFirstOccurrenceOf(" ", false, false).trim();

        contentLength = parseContentLength(headerText);
        const size_t availableBodyBytes = requestData.getSize() > (size_t)headerBodyOffset
            ? requestData.getSize() - (size_t)headerBodyOffset
            : 0;
        const size_t bodyBytes = juce::jmin((size_t)contentLength, availableBodyBytes);
        if (bodyBytes > 0)
            request.body.append(static_cast<const char*>(requestData.getData()) + headerBodyOffset, bodyBytes);

        return request;
    }

    HttpResponse buildResponse(const juce::String& method, const juce::String& requestTarget, const juce::MemoryBlock& requestBody)
    {
        const juce::String path = requestTarget.upToFirstOccurrenceOf("?", false, false).trim();
        const bool isHead = (method == "HEAD");

        if (path.isEmpty() || path == "/" || path == "/buffer-room" || path == "/sync-buffer-room" || path == "/index.html")
        {
            HttpResponse response;
            response.contentType = "text/html; charset=utf-8";
            response.noStore = true;
            response.body = makeUtf8Body(helperIndexHtml);
            if (isHead)
                response.body.reset();
            return response;
        }

        if (path == "/zap-video" || path == "/zap-wall")
        {
            HttpResponse response;
            response.contentType = "text/html; charset=utf-8";
            response.noStore = true;
            response.body = makeUtf8Body(getZapViewerHtml());
            if (isHead)
                response.body.reset();
            return response;
        }

        if (path == "/zap-browser-camera-enable" || path == "/zap-browser-camera-stop")
        {
            HttpResponse response;
            response.contentType = "application/json; charset=utf-8";
            response.noStore = true;
            if (method != "POST")
            {
                response.statusCode = 405;
                response.statusText = "Method Not Allowed";
                response.body = makeUtf8Body("{\"ok\":false,\"error\":\"POST required\"}");
                return response;
            }
            if (getQueryParam(requestTarget, "token") != helperRequestToken)
            {
                response.statusCode = 403;
                response.statusText = "Forbidden";
                response.body = makeUtf8Body("{\"ok\":false,\"error\":\"invalid helper token\"}");
                return response;
            }

            const bool isEnable = path == "/zap-browser-camera-enable";
            juce::String payload = isEnable
                ? (zapBrowserCameraEnable ? zapBrowserCameraEnable(getQueryParam(requestTarget, "codec")).trim() : juce::String())
                : (zapBrowserCameraDisable ? zapBrowserCameraDisable().trim() : juce::String());
            if (payload.isEmpty())
                payload = "{\"ok\":false,\"error\":\"camera control unavailable\"}";

            if (!payload.startsWith("{\"ok\":true"))
            {
                response.statusCode = 409;
                response.statusText = "Conflict";
            }
            response.body = makeUtf8Body(payload);
            return response;
        }

        if (path == "/zap-browser-camera-state")
        {
            HttpResponse response;
            response.contentType = "application/json; charset=utf-8";
            response.noStore = true;
            juce::String payload = zapBrowserCameraState ? zapBrowserCameraState().trim() : juce::String();
            if (payload.isEmpty())
                payload = "{\"ok\":false}";
            response.body = makeUtf8Body(payload);
            if (isHead)
                response.body.reset();
            return response;
        }

        if (path == "/zap-browser-camera-frame")
        {
            HttpResponse response;
            response.contentType = "application/json; charset=utf-8";
            response.noStore = true;
            if (method != "POST")
            {
                response.statusCode = 405;
                response.statusText = "Method Not Allowed";
                response.body = makeUtf8Body("{\"ok\":false,\"error\":\"POST required\"}");
                return response;
            }
            if (getQueryParam(requestTarget, "token") != helperRequestToken)
            {
                response.statusCode = 403;
                response.statusText = "Forbidden";
                response.body = makeUtf8Body("{\"ok\":false,\"error\":\"invalid helper token\"}");
                return response;
            }

            const double ageMs = getQueryParam(requestTarget, "ageMs").getDoubleValue();
            const double encodeMs = getQueryParam(requestTarget, "encodeMs").getDoubleValue();
            const int width = getQueryParam(requestTarget, "width").getIntValue();
            const int height = getQueryParam(requestTarget, "height").getIntValue();
            const juce::String codec = getQueryParam(requestTarget, "codec");
            const juce::String config = getQueryParam(requestTarget, "config");
            const bool keyFrame = getQueryParam(requestTarget, "key").getIntValue() != 0;
            const bool allowConfigOnly = config.trim().isNotEmpty();
            const bool accepted = (requestBody.getSize() > 0 || allowConfigOnly)
                && zapBrowserFrameConsumer
                && zapBrowserFrameConsumer(requestBody, codec, config, keyFrame, ageMs, encodeMs, width, height);
            if (accepted)
            {
                response.statusCode = 204;
                response.statusText = "No Content";
                response.body.reset();
            }
            else
            {
                response.statusCode = 409;
                response.statusText = "Conflict";
                response.body = makeUtf8Body("{\"ok\":false,\"error\":\"browser camera inactive or frame rejected\"}");
            }
            return response;
        }

        if (path == "/zap-frames")
        {
            HttpResponse response;
            response.contentType = "application/json; charset=utf-8";
            response.noStore = true;
            juce::String payload = zapFrameListProvider ? zapFrameListProvider().trim() : juce::String();
            if (payload.isEmpty())
                payload = "[]";
            response.body = makeUtf8Body(payload);
            if (isHead)
                response.body.reset();
            return response;
        }

        if (path == "/zap-frame")
        {
            HttpResponse response;
            response.contentType = "image/jpeg";
            response.noStore = true;
            const juce::String streamKey = getQueryParam(requestTarget, "stream");
            const int frameIndex = getQueryParam(requestTarget, "frame").isNotEmpty()
                ? getQueryParam(requestTarget, "frame").getIntValue()
                : -1;
            if (streamKey.isNotEmpty() && zapFrameProvider && zapFrameProvider(streamKey, frameIndex, response.body))
            {
                if (isHead)
                    response.body.reset();
                return response;
            }

            response.statusCode = 404;
            response.statusText = "Not Found";
            response.contentType = "text/plain; charset=utf-8";
            response.body = makeUtf8Body("No Zap frame");
            if (isHead)
                response.body.reset();
            return response;
        }

        if (path == "/zap-frame-batch")
        {
            HttpResponse response;
            response.contentType = "application/octet-stream";
            response.noStore = true;
            const juce::String streamKey = getQueryParam(requestTarget, "stream");
            const int fromFrame = juce::jmax(0, getQueryParam(requestTarget, "from").getIntValue());
            const int toFrame = juce::jmax(fromFrame, getQueryParam(requestTarget, "to").getIntValue());
            const int limitedToFrame = juce::jmin(toFrame, fromFrame + 119);
            if (streamKey.isNotEmpty() && zapFrameProvider)
            {
                for (int frameIndex = fromFrame; frameIndex <= limitedToFrame; ++frameIndex)
                {
                    juce::MemoryBlock frame;
                    if (zapFrameProvider(streamKey, frameIndex, frame) && frame.getSize() > 0)
                    {
                        const size_t frameBytes = frame.getSize();
                        const juce::uint32 clampedFrameBytes = frameBytes > (size_t) std::numeric_limits<juce::uint32>::max()
                            ? std::numeric_limits<juce::uint32>::max()
                            : (juce::uint32) frameBytes;
                        appendBe32(response.body, clampedFrameBytes);
                        response.body.append(frame.getData(), frame.getSize());
                    }
                    else
                    {
                        appendBe32(response.body, 0);
                    }
                }

                if (isHead)
                    response.body.reset();
                return response;
            }

            response.statusCode = 404;
            response.statusText = "Not Found";
            response.contentType = "text/plain; charset=utf-8";
            response.body = makeUtf8Body("No Zap frames");
            if (isHead)
                response.body.reset();
            return response;
        }

        if (path == "/app")
        {
            HttpResponse response;
            response.contentType = "text/html; charset=utf-8";
            response.noStore = true;
            response.body = makeUtf8Body(helperAppHtml);
            if (isHead)
                response.body.reset();
            return response;
        }

        if (path == "/intervals")
        {
            HttpResponse response;
            response.contentType = "application/json; charset=utf-8";
            response.noStore = true;
            juce::String payload = intervalPayloadProvider ? intervalPayloadProvider().trim() : juce::String();
            if (payload.isEmpty())
                payload = "[]";
            response.body = makeUtf8Body(payload);
            if (isHead)
                response.body.reset();
            return response;
        }

        if (path == "/icon.png" && helperIconPng.getSize() > 0)
        {
            HttpResponse response;
            response.contentType = "image/png";
            response.noStore = true;
            response.body = helperIconPng;
            if (isHead)
                response.body.reset();
            return response;
        }

        if (path == "/PoweredByVDONinja.png" && helperPoweredByPng.getSize() > 0)
        {
            HttpResponse response;
            response.contentType = "image/png";
            response.noStore = true;
            response.body = helperPoweredByPng;
            if (isHead)
                response.body.reset();
            return response;
        }

        if ((path == "/zap-mask/cloud.png" || path == "/masks/cloud.png") && helperCloudMaskPng.getSize() > 0)
        {
            HttpResponse response;
            response.contentType = "image/png";
            response.noStore = true;
            response.body = helperCloudMaskPng;
            if (isHead)
                response.body.reset();
            return response;
        }

        HttpResponse response;
        response.statusCode = 404;
        response.statusText = "Not Found";
        response.body = makeUtf8Body("Not found");
        if (isHead)
            response.body.reset();
        return response;
    }

    void sendResponse(juce::StreamingSocket& client, const juce::String& method, const HttpResponse& response)
    {
        juce::String header;
        header << "HTTP/1.1 " << response.statusCode << ' ' << response.statusText << "\r\n";
        header << "Content-Type: " << response.contentType << "\r\n";
        header << "Content-Length: " << (juce::int64) response.body.getSize() << "\r\n";
        header << "Connection: close\r\n";
        header << "X-Content-Type-Options: nosniff\r\n";
        header << "X-Frame-Options: SAMEORIGIN\r\n";
        header << "Cross-Origin-Resource-Policy: same-origin\r\n";
        if (response.noStore)
        {
            header << "Cache-Control: no-store, no-cache, must-revalidate\r\n";
            header << "Pragma: no-cache\r\n";
            header << "Expires: 0\r\n";
        }
        header << "\r\n";

        const juce::MemoryBlock headerBytes = makeUtf8Body(header);
        if (!writeAll(client, headerBytes.getData(), headerBytes.getSize()))
            return;

        if (method != "HEAD" && response.body.getSize() > 0)
            writeAll(client, response.body.getData(), response.body.getSize());
    }

    void reloadStaticContent()
    {
        helperIndexHtml = juce::String::fromUTF8(reinterpret_cast<const char*>(ninjamplus::embedded::vdoIndexHtml),
                                                (int) ninjamplus::embedded::vdoIndexHtmlSize);
        helperAppHtml = juce::String::fromUTF8(reinterpret_cast<const char*>(ninjamplus::embedded::vdoAppHtml),
                                              (int) ninjamplus::embedded::vdoAppHtmlSize);

        auto loadEmbeddedPng = [](juce::MemoryBlock& destination, const unsigned char* data, std::size_t size)
        {
            destination.reset();
            destination.append(data, size);
        };

        loadEmbeddedPng(helperIconPng, ninjamplus::embedded::vdoIconPng, ninjamplus::embedded::vdoIconPngSize);
        loadEmbeddedPng(helperPoweredByPng, ninjamplus::embedded::vdoPoweredByPng, ninjamplus::embedded::vdoPoweredByPngSize);
        loadEmbeddedPng(helperCloudMaskPng, ninjamplus::embedded::vdoCloudMaskPng, ninjamplus::embedded::vdoCloudMaskPngSize);
    }
};

class ZapVideoDecodeWorker final : private juce::Thread
{
public:
    explicit ZapVideoDecodeWorker(NinjamVst3AudioProcessor& ownerIn)
        : juce::Thread("NINJAMZapVideoDecodeWorker"),
          owner(ownerIn)
    {
        startThread(juce::Thread::Priority::background);
    }

    ~ZapVideoDecodeWorker() override
    {
        stop();
    }

    void enqueue(NinjamVst3AudioProcessor::ZapVideoDecodeJob job)
    {
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        if (job.receivedMs <= 0.0)
            job.receivedMs = nowMs;
        if (job.queuedMs <= 0.0)
            job.queuedMs = nowMs;
        {
            const juce::ScopedLock lock(queueLock);
            while (queue.size() >= maxQueuedJobs)
                queue.pop_front();
            queue.push_back(std::move(job));
        }
        wakeEvent.signal();
    }

    void stop()
    {
        signalThreadShouldExit();
        wakeEvent.signal();
        stopThread(1000);
        const juce::ScopedLock lock(queueLock);
        queue.clear();
    }

private:
    static constexpr size_t maxQueuedJobs = 48;
    static constexpr double maxDecodeQueueAgeMs = 450.0;

    struct H264StreamState
    {
#if NINJAMPLUS_HAS_H264_DECODE
        std::unique_ptr<ProVideoDecoder> decoder;
#endif
        juce::MemoryBlock cachedSpsPpsAnnexB;
    };

    NinjamVst3AudioProcessor& owner;
    juce::CriticalSection queueLock;
    juce::WaitableEvent wakeEvent;
    std::deque<NinjamVst3AudioProcessor::ZapVideoDecodeJob> queue;
#if NINJAMPLUS_HAS_H264_DECODE
    std::map<juce::String, H264StreamState> h264Streams;
#endif

    bool popJob(NinjamVst3AudioProcessor::ZapVideoDecodeJob& job)
    {
        const juce::ScopedLock lock(queueLock);
        if (queue.empty())
            return false;

        job = std::move(queue.front());
        queue.pop_front();
        return true;
    }

#if NINJAMPLUS_HAS_H264_DECODE
    static juce::uint16 readBe16(const unsigned char* data)
    {
        return (juce::uint16) (((juce::uint16) data[0] << 8) | (juce::uint16) data[1]);
    }

    static juce::uint32 readBe32(const unsigned char* data)
    {
        return ((juce::uint32) data[0] << 24)
             | ((juce::uint32) data[1] << 16)
             | ((juce::uint32) data[2] << 8)
             |  (juce::uint32) data[3];
    }

    static void appendAnnexBStartCodeAndNal(juce::MemoryBlock& out, const void* nalData, size_t nalSize)
    {
        static constexpr unsigned char startCode[] { 0x00, 0x00, 0x00, 0x01 };
        out.append(startCode, sizeof(startCode));
        out.append(nalData, nalSize);
    }

    static bool parseSpsPpsBlock(const juce::MemoryBlock& payload, juce::MemoryBlock& outAnnexB)
    {
        const auto* bytes = static_cast<const unsigned char*>(payload.getData());
        const size_t size = payload.getSize();
        if (bytes == nullptr || size < 4)
            return false;

        const size_t spsLen = readBe16(bytes);
        if (spsLen == 0 || 2 + spsLen + 2 > size)
            return false;

        const size_t ppsOffset = 2 + spsLen;
        const size_t ppsLen = readBe16(bytes + ppsOffset);
        if (ppsLen == 0 || ppsOffset + 2 + ppsLen != size)
            return false;

        const unsigned char spsType = bytes[2] & 0x1f;
        const unsigned char ppsType = bytes[ppsOffset + 2] & 0x1f;
        if (spsType != 7 || ppsType != 8)
            return false;

        outAnnexB.reset();
        appendAnnexBStartCodeAndNal(outAnnexB, bytes + 2, spsLen);
        appendAnnexBStartCodeAndNal(outAnnexB, bytes + ppsOffset + 2, ppsLen);
        return true;
    }

    static bool convertAvccFrameToAnnexB(const juce::MemoryBlock& payload,
                                         const juce::MemoryBlock& cachedSpsPps,
                                         juce::MemoryBlock& outAnnexB)
    {
        const auto* bytes = static_cast<const unsigned char*>(payload.getData());
        const size_t size = payload.getSize();
        if (bytes == nullptr || size < 5)
            return false;

        outAnnexB.reset();
        bool sawNal = false;
        bool sawIdr = false;
        size_t offset = 0;
        while (offset + 4 <= size)
        {
            const size_t nalLen = (size_t) readBe32(bytes + offset);
            offset += 4;
            if (nalLen == 0 || offset + nalLen > size)
                return false;

            const unsigned char nalType = bytes[offset] & 0x1f;
            if (nalType == 5)
                sawIdr = true;
            appendAnnexBStartCodeAndNal(outAnnexB, bytes + offset, nalLen);
            sawNal = true;
            offset += nalLen;
        }

        if (!sawNal || offset != size)
            return false;

        if (sawIdr && cachedSpsPps.getSize() > 0)
        {
            juce::MemoryBlock withHeaders;
            withHeaders.append(cachedSpsPps.getData(), cachedSpsPps.getSize());
            withHeaders.append(outAnnexB.getData(), outAnnexB.getSize());
            outAnnexB = std::move(withHeaders);
        }

        return outAnnexB.getSize() > 0;
    }
#endif

    void run() override
    {
        while (!threadShouldExit())
        {
            NinjamVst3AudioProcessor::ZapVideoDecodeJob job;
            if (!popJob(job))
            {
                wakeEvent.wait(100);
                wakeEvent.reset();
                continue;
            }

            job.decodeStartedMs = juce::Time::getMillisecondCounterHiRes();
            const double queueMs = job.queuedMs > 0.0 ? job.decodeStartedMs - job.queuedMs : 0.0;
            if (queueMs > maxDecodeQueueAgeMs)
            {
                continue;
            }

            if (job.codec == ninjamplus::zap::VideoCodec::mjpeg)
            {
                juce::Image img;
                if (ninjamplus::zap::decodeMjpegFrame(job.payload.getData(), job.payload.getSize(), img) && img.isValid())
                {
                    job.decodeFinishedMs = juce::Time::getMillisecondCounterHiRes();
                    owner.publishDecodedZapVideoFrame(job, img, job.payload);
                }
            }
#if NINJAMPLUS_HAS_H264_DECODE
            else if (job.codec == ninjamplus::zap::VideoCodec::h264)
            {
                auto& stream = h264Streams[job.streamKey];

                juce::MemoryBlock spsPps;
                if (parseSpsPpsBlock(job.payload, spsPps))
                {
                    const bool changed = stream.cachedSpsPpsAnnexB.getSize() != spsPps.getSize()
                        || (spsPps.getSize() > 0
                            && std::memcmp(stream.cachedSpsPpsAnnexB.getData(), spsPps.getData(), spsPps.getSize()) != 0);

                    if (changed)
                    {
                        stream.cachedSpsPpsAnnexB = std::move(spsPps);
                        if (stream.decoder != nullptr)
                            stream.decoder->reset();
                    }
                    continue;
                }

                if (stream.decoder == nullptr)
                    stream.decoder = std::make_unique<ProVideoDecoder>();

                juce::MemoryBlock annexB;
                if (!convertAvccFrameToAnnexB(job.payload, stream.cachedSpsPpsAnnexB, annexB))
                    continue;
                juce::Image img;
                if (stream.decoder->decode(annexB.getData(), (int) annexB.getSize(), img) && img.isValid())
                {
                    job.decodeFinishedMs = juce::Time::getMillisecondCounterHiRes();
                    owner.publishDecodedZapVideoFrame(job, img, {});
                }
            }
#endif
        }
    }
};

#if JUCE_USE_CAMERA && (JUCE_WINDOWS || JUCE_MAC)
class ZapCameraSender final : private juce::Thread,
                              private juce::CameraDevice::Listener
{
public:
    explicit ZapCameraSender(NinjamVst3AudioProcessor& ownerIn)
        : juce::Thread("NINJAMZapCameraSender"),
          owner(ownerIn)
    {
    }

    ~ZapCameraSender() override
    {
        stop();
    }

    bool start(int deviceIndex, ninjamplus::zap::CameraCodecPreference preference)
    {
        if (camera != nullptr)
            return true;

        const auto devices = juce::CameraDevice::getAvailableDevices();
        if (devices.isEmpty())
            return false;

        const int clampedDeviceIndex = juce::jlimit(0, devices.size() - 1, deviceIndex);
        std::unique_ptr<juce::CameraDevice> opened(juce::CameraDevice::openDevice(clampedDeviceIndex,
                                                                                  320,
                                                                                  240,
                                                                                  ninjamplus::zap::kZapVideoWidth,
                                                                                  ninjamplus::zap::kZapVideoHeight,
                                                                                  false));
        if (opened == nullptr)
            return false;

        activeCodec = ninjamplus::zap::VideoCodec::mjpeg;
        if (preference == ninjamplus::zap::CameraCodecPreference::autoCodec
            || preference == ninjamplus::zap::CameraCodecPreference::h264
            || preference == ninjamplus::zap::CameraCodecPreference::h264Hardware
            || preference == ninjamplus::zap::CameraCodecPreference::h264Software)
        {
            ninjamplus::zap::H264EncoderPreference encoderPreference = ninjamplus::zap::H264EncoderPreference::autoHardware;
            if (preference == ninjamplus::zap::CameraCodecPreference::h264Hardware)
                encoderPreference = ninjamplus::zap::H264EncoderPreference::hardwareOnly;
            else if (preference == ninjamplus::zap::CameraCodecPreference::h264Software)
                encoderPreference = ninjamplus::zap::H264EncoderPreference::softwareOnly;

            if (h264Encoder.open(ninjamplus::zap::kZapVideoWidth,
                                 ninjamplus::zap::kZapVideoHeight,
                                 ninjamplus::zap::kZapVideoFps,
                                 2500000,
                                 encoderPreference))
            {
                activeCodec = ninjamplus::zap::VideoCodec::h264;
            }
        }

        owner.ninjamZapCameraActiveCodec.store((int)activeCodec, std::memory_order_relaxed);

        camera = std::move(opened);
        camera->addListener(this);
        startThread(juce::Thread::Priority::background);
        return true;
    }

    void stop()
    {
        signalThreadShouldExit();
        wakeEvent.signal();
        if (camera != nullptr)
            camera->removeListener(this);
        stopThread(5000);
        camera.reset();
        h264Encoder.close();
        {
            const juce::ScopedLock lock(frameLock);
            latestFrame = {};
        }
    }

private:
    NinjamVst3AudioProcessor& owner;
    std::unique_ptr<juce::CameraDevice> camera;
    juce::CriticalSection frameLock;
    juce::WaitableEvent wakeEvent;
    juce::Image latestFrame;
    double latestFrameReceivedMs = 0.0;
    double lastEncodeMs = 0.0;
    double lastLocalPreviewMs = 0.0;
    ninjamplus::zap::VideoCodec activeCodec = ninjamplus::zap::VideoCodec::mjpeg;
    ninjamplus::zap::H264Encoder h264Encoder;

    void imageReceived(const juce::Image& image) override
    {
        if (!image.isValid())
            return;

        {
            const juce::ScopedLock lock(frameLock);
            latestFrame = image;
            latestFrameReceivedMs = juce::Time::getMillisecondCounterHiRes();
        }
        wakeEvent.signal();
    }

    void run() override
    {
        constexpr double minEncodeIntervalMs = 1000.0 / (double)ninjamplus::zap::kZapVideoFps;

        while (!threadShouldExit())
        {
            wakeEvent.wait(50);
            wakeEvent.reset();

            const double nowMs = juce::Time::getMillisecondCounterHiRes();
            if (nowMs - lastEncodeMs < minEncodeIntervalMs)
                continue;

            juce::Image frame;
            double frameReceivedMs = 0.0;
            {
                const juce::ScopedLock lock(frameLock);
                frame = latestFrame;
                frameReceivedMs = latestFrameReceivedMs;
            }

            if (!frame.isValid())
                continue;

            const double captureQueueMs = frameReceivedMs > 0.0 ? juce::jmax(0.0, nowMs - frameReceivedMs) : 0.0;

            if (activeCodec == ninjamplus::zap::VideoCodec::h264)
            {
                // Force an IDR keyframe at the start of each NINJAM interval,
                // as required by the NinjamZap receiver spec.
                if (owner.ninjamZapForceNextKeyframe.exchange(false, std::memory_order_relaxed))
                    h264Encoder.forceKeyframe();

                ninjamplus::zap::EncodedH264Frame encoded;
                const double encodeStartMs = juce::Time::getMillisecondCounterHiRes();
                if (h264Encoder.encodeFrame(frame, encoded))
                {
                    const double encodeFinishedMs = juce::Time::getMillisecondCounterHiRes();
                    const double encodeMs = juce::jmax(0.0, encodeFinishedMs - encodeStartMs);
                    if (encoded.configChunk.getSize() > 0)
                    {
                        const juce::ScopedLock lock(owner.zapVideoFrameLock);
                        owner.ninjamZapCameraH264ConfigChunk = encoded.configChunk;
                        owner.enqueueNinjamZapCameraFrameChunk(encoded.configChunk);
                    }
                    if (encoded.frameChunk.getSize() > 0)
                        owner.enqueueNinjamZapCameraFrameChunk(std::move(encoded.frameChunk));

                    if (nowMs - lastLocalPreviewMs >= 33.0)
                    {
                        juce::MemoryBlock previewJpeg;
                        if (ninjamplus::zap::encodeMjpegFrame(frame, 55, previewJpeg) && previewJpeg.getSize() > 0)
                            owner.publishLocalNinjamZapCameraFrame(frame, previewJpeg, captureQueueMs, encodeMs);
                        lastLocalPreviewMs = nowMs;
                    }
                    lastEncodeMs = nowMs;
                }
                continue;
            }

            juce::MemoryBlock jpeg;
            const double encodeStartMs = juce::Time::getMillisecondCounterHiRes();
            if (ninjamplus::zap::encodeMjpegFrame(frame, ninjamplus::zap::kZapJpegDefaultQuality, jpeg)
                && jpeg.getSize() > 0)
            {
                const double encodeFinishedMs = juce::Time::getMillisecondCounterHiRes();
                const double encodeMs = juce::jmax(0.0, encodeFinishedMs - encodeStartMs);
                juce::MemoryBlock chunk;
                if (ninjamplus::zap::appendLengthPrefixedChunk(jpeg.getData(), jpeg.getSize(), chunk))
                {
                    owner.publishLocalNinjamZapCameraFrame(frame, jpeg, captureQueueMs, encodeMs);
                    owner.enqueueNinjamZapCameraFrameChunk(std::move(chunk));
                    lastEncodeMs = nowMs;
                }
            }
        }
    }
};
#else
class ZapCameraSender final
{
public:
    explicit ZapCameraSender(NinjamVst3AudioProcessor& ownerIn)
        : owner(ownerIn)
    {
    }

    bool start(int deviceIndex, ninjamplus::zap::CameraCodecPreference preference)
    {
        juce::ignoreUnused(deviceIndex, preference);
        owner.ninjamZapCameraActiveCodec.store((int)ninjamplus::zap::VideoCodec::mjpeg,
                                               std::memory_order_relaxed);
        return false;
    }

    void stop() {}

private:
    NinjamVst3AudioProcessor& owner;
};
#endif

static juce::File findSolititoChordAsset(const char* fileName)
{
    std::vector<juce::File> dirs;

#if defined(NINJAMPLUS_SOLITITO_ASSET_DIR)
    dirs.push_back(juce::File(juce::String(NINJAMPLUS_SOLITITO_ASSET_DIR)));
#endif

    const juce::File moduleFile = getThisModuleFile();
    if (moduleFile.exists())
    {
        const auto moduleDir = moduleFile.getParentDirectory();
        dirs.push_back(moduleDir.getChildFile("solitito-ai"));
        dirs.push_back(moduleDir.getChildFile("Resources").getChildFile("solitito-ai"));
        dirs.push_back(moduleDir.getParentDirectory().getChildFile("Resources").getChildFile("solitito-ai"));
    }

    const auto exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    dirs.push_back(exeDir.getChildFile("solitito-ai"));
    dirs.push_back(exeDir.getChildFile("Resources").getChildFile("solitito-ai"));

    for (const auto& dir : dirs)
    {
        const auto candidate = dir.getChildFile(fileName);
        if (candidate.existsAsFile())
            return candidate;
    }

    return {};
}

static juce::File findSolititoChordRuntime()
{
    constexpr auto runtimeName = "ninjamplus_onnxruntime.dll";
    std::vector<juce::File> dirs;

    const juce::File moduleFile = getThisModuleFile();
    if (moduleFile.exists())
    {
        const auto moduleDir = moduleFile.getParentDirectory();
        dirs.push_back(moduleDir);
        dirs.push_back(moduleDir.getChildFile("Resources"));
        dirs.push_back(moduleDir.getParentDirectory().getChildFile("Resources"));
    }

    const auto exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    dirs.push_back(exeDir);
    dirs.push_back(exeDir.getChildFile("Resources"));

    for (const auto& dir : dirs)
    {
        const auto candidate = dir.getChildFile(runtimeName);
        if (candidate.existsAsFile())
            return candidate;
    }

    return {};
}

class BatchedChordAnalyzer final
    : private juce::Thread
{
public:
    static constexpr int localTrackIndex = 0;
    static constexpr int remoteTrackBase = 1;
    static constexpr int masterTrackIndex = remoteTrackBase + NinjamVst3AudioProcessor::maxRemoteChordUsers;
    static constexpr int trackCount = masterTrackIndex + 1;

    static int remoteTrackIndexForUser(int userIndex)
    {
        return remoteTrackBase + userIndex;
    }

    BatchedChordAnalyzer()
        : juce::Thread("NINJAMBatchedChordAnalyzer")
    {
        solititoModel = std::make_unique<SolititoChordModel>(trackCount);
        solititoModel->load(findSolititoChordRuntime(),
                            findSolititoChordAsset("chord_model_v31_16k.onnx"),
                            findSolititoChordAsset("dsp_weights_v31_16k.bin"));

        for (auto& track : tracks)
            track.memoryKb.store(estimateTrackMemoryKb(), std::memory_order_relaxed);
        memoryKb.store(estimateTotalMemoryKb(), std::memory_order_relaxed);
    }

    ~BatchedChordAnalyzer() override
    {
        stop();
    }

    void prepare(double newSampleRate)
    {
        ready.store(false, std::memory_order_release);
        stopThread(1000);

        sampleRate = newSampleRate > 1.0 ? newSampleRate : 44100.0;
        for (auto& track : tracks)
            prepareTrack(track, sampleRate);

        memoryKb.store(estimateTotalMemoryKb(), std::memory_order_relaxed);
        ready.store(true, std::memory_order_release);
        startThread(juce::Thread::Priority::background);
    }

    void processBlock(int trackIndex, const float* input, int numSamples, int inputSampleRate = 0)
    {
        processFrames(trackIndex, input, numSamples, 1, inputSampleRate);
    }

    void processInterleavedBlock(int trackIndex, const float* input, int numFrames, int numChannels, int inputSampleRate)
    {
        processFrames(trackIndex, input, numFrames, juce::jmax(1, numChannels), inputSampleRate);
    }

    void processFrames(int trackIndex, const float* input, int numFrames, int numChannels, int inputSampleRate)
    {
        if (!ready.load(std::memory_order_acquire))
            return;

        auto* track = getTrack(trackIndex);
        if (track == nullptr)
            return;

        if (input == nullptr || numFrames <= 0 || track->audioFifo == nullptr || track->ringBuffer.empty())
        {
            markNoInput(trackIndex);
            return;
        }

        if (inputSampleRate > 1000)
            track->requestedSampleRate.store(juce::jlimit(8000, 192000, inputSampleRate), std::memory_order_relaxed);

        int writableSamples = juce::jmin(numFrames, track->audioFifo->getFreeSpace());
        if (writableSamples <= 0)
        {
            track->droppedSamples.fetch_add(numFrames, std::memory_order_relaxed);
            return;
        }

        int inputStartFrame = 0;
        if (writableSamples < numFrames)
        {
            inputStartFrame = numFrames - writableSamples;
            track->droppedSamples.fetch_add(numFrames - writableSamples, std::memory_order_relaxed);
        }

        int start1 = 0;
        int size1 = 0;
        int start2 = 0;
        int size2 = 0;
        track->audioFifo->prepareToWrite(writableSamples, start1, size1, start2, size2);

        copyMonoFramesToRing(*track, input, inputStartFrame, numChannels, start1, size1);
        copyMonoFramesToRing(*track, input, inputStartFrame + size1, numChannels, start2, size2);

        track->audioFifo->finishedWrite(size1 + size2);
        samplesAvailable.signal();
    }

    void markNoInput(int trackIndex)
    {
        if (auto* track = getTrack(trackIndex))
        {
            track->chordValid.store(false, std::memory_order_relaxed);
            track->noteValid.store(false, std::memory_order_relaxed);
            track->chordConfidence.store(0.0, std::memory_order_relaxed);
            track->noteConfidence.store(0.0, std::memory_order_relaxed);
        }
    }

    void resetTrack(int trackIndex)
    {
        if (auto* track = getTrack(trackIndex))
        {
            track->chordValid.store(false, std::memory_order_relaxed);
            track->noteValid.store(false, std::memory_order_relaxed);
            track->chordConfidence.store(0.0, std::memory_order_relaxed);
            track->noteConfidence.store(0.0, std::memory_order_relaxed);
            track->resetRequested.store(true, std::memory_order_release);
            samplesAvailable.signal();
        }
    }

    void markAllNoInput()
    {
        for (int i = 0; i < trackCount; ++i)
            markNoInput(i);
    }

    void resetAllTracks()
    {
        for (int i = 0; i < trackCount; ++i)
            resetTrack(i);
    }

    void stop()
    {
        ready.store(false, std::memory_order_release);
        signalThreadShouldExit();
        samplesAvailable.signal();
        stopThread(1000);
    }

    bool isPrepared() const
    {
        return ready.load(std::memory_order_acquire);
    }

    static const char* getPitchClassName(int pitchClass)
    {
        static const char* names[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
        return pitchClass >= 0 && pitchClass < 12 ? names[pitchClass] : "--";
    }

    static juce::String formatChordLabel(int root, int quality, int intervals)
    {
        juce::String suffix;

        switch (quality)
        {
            case ChordDetector::Major:       suffix = intervals == 7 ? "maj7" : ""; break;
            case ChordDetector::Minor:       suffix = intervals == 7 ? "m7" : "m"; break;
            case ChordDetector::Suspended:   suffix = intervals == 2 ? "sus2" : (intervals == 4 ? "sus4" : "sus"); break;
            case ChordDetector::Dominant:    suffix = intervals == 13 ? "13" : (intervals == 9 ? "9" : "7"); break;
            case ChordDetector::Dimished5th: suffix = intervals == 7 ? "dim7" : (intervals == 5 ? "m7b5" : "dim"); break;
            case ChordDetector::Augmented5th:suffix = "aug"; break;
            default:                         suffix = ""; break;
        }

        return juce::String(getPitchClassName(root)) + suffix;
    }

    juce::String getLabel(int trackIndex) const
    {
        const auto* track = getTrack(trackIndex);
        if (track == nullptr)
            return "--";

        if (track->chordValid.load(std::memory_order_relaxed))
        {
            const int root = track->chordRoot.load(std::memory_order_relaxed);
            const int quality = track->chordQuality.load(std::memory_order_relaxed);
            const int intervals = track->chordIntervals.load(std::memory_order_relaxed);
            if (root >= 0 && root < 12)
                return formatChordLabel(root, quality, intervals);
        }

        if (track->noteValid.load(std::memory_order_relaxed))
        {
            const int root = track->noteRoot.load(std::memory_order_relaxed);
            if (root >= 0 && root < 12)
                return getPitchClassName(root);
        }

        return "--";
    }

    double getCpuPercent(int trackIndex) const
    {
        const auto* track = getTrack(trackIndex);
        return track != nullptr ? track->cpuPercent.load(std::memory_order_relaxed) : 0.0;
    }

    int getMemoryKb(int trackIndex) const
    {
        const auto* track = getTrack(trackIndex);
        return track != nullptr ? track->memoryKb.load(std::memory_order_relaxed) : 0;
    }

    double getConfidence(int trackIndex) const
    {
        const auto* track = getTrack(trackIndex);
        if (track == nullptr)
            return 0.0;

        if (track->chordValid.load(std::memory_order_relaxed))
            return track->chordConfidence.load(std::memory_order_relaxed);

        if (track->noteValid.load(std::memory_order_relaxed))
            return track->noteConfidence.load(std::memory_order_relaxed);

        return 0.0;
    }

    bool hasStrongMasterTimelineConfidence() const
    {
        return getConfidence(masterTrackIndex) >= masterTimelineConfidenceThreshold;
    }

    int getTotalMemoryKb() const
    {
        return memoryKb.load(std::memory_order_relaxed);
    }

private:
    static constexpr int frameSize = 512;
    static constexpr int chromaCalculationIntervalSamples = 2048;
    static constexpr int chromaHistoryFrames = 8;
    static constexpr int stableCandidateHitThreshold = 3;
    static constexpr int stableChordChangeHitThreshold = 6;
    static constexpr int stableNoteHitThreshold = 1;
    static constexpr int stableNoteChangeHitThreshold = 4;
    static constexpr int masterStableCandidateHitThreshold = 5;
    static constexpr int masterStableChordChangeHitThreshold = 10;
    static constexpr int masterStableNoteHitThreshold = 3;
    static constexpr int masterStableNoteChangeHitThreshold = 6;
    static constexpr int invalidClearThresholdFrames = 8;
    static constexpr float solititoConfidenceThreshold = 0.05f;
    static constexpr float masterSolititoConfidenceThreshold = 0.18f;
    static constexpr double masterTimelineConfidenceThreshold = 0.22;
    static constexpr double silenceRmsThreshold = 0.001;

    struct PendingChordCandidate
    {
        int trackIndex = -1;
        std::array<double, 12> chroma {};
        std::array<float, SolititoChordModel::contextFloats> solititoFeatures {};
        bool hasSolititoFeatures = false;
    };

    struct TrackState
    {
        std::unique_ptr<Chromagram> chromagram;
        std::unique_ptr<juce::AbstractFifo> audioFifo;
        ChordDetector chordDetector;
        std::vector<double> frame;
        std::array<double, 12> averagedChroma {};
        std::array<std::array<double, 12>, chromaHistoryFrames> chromaHistory {};
        std::vector<float> ringBuffer;
        std::vector<float> readBuffer;
        double sampleRate = 44100.0;
        double rmsSmoothed = 0.0;
        double analysisMsSinceCpuUpdate = 0.0;
        int samplesSinceCpuUpdate = 0;
        int analyzerSampleRate = 0;
        int frameFill = 0;
        int chromaHistoryWrite = 0;
        int chromaHistorySize = 0;
        int pendingChordKey = -1;
        int pendingChordHits = 0;
        int pendingNoteRoot = -1;
        int pendingNoteHits = 0;
        int displayedChordKey = -1;
        int invalidChordFrames = 0;
        std::atomic<bool> resetRequested { false };
        std::atomic<bool> chordValid { false };
        std::atomic<bool> noteValid { false };
        std::atomic<int> chordRoot { -1 };
        std::atomic<int> noteRoot { -1 };
        std::atomic<int> chordQuality { ChordDetector::Major };
        std::atomic<int> chordIntervals { 0 };
        std::atomic<double> chordConfidence { 0.0 };
        std::atomic<double> noteConfidence { 0.0 };
        std::atomic<double> cpuPercent { 0.0 };
        std::atomic<int> memoryKb { 0 };
        std::atomic<int> requestedSampleRate { 44100 };
        std::atomic<long long> droppedSamples { 0 };
    };

    static int estimateTrackMemoryKb(int fifoSamples = 48000)
    {
        constexpr int bufferSize = 8192;
        constexpr int downsampledFrameSize = 512 / 4;
        const size_t doubleVectors = (size_t)(bufferSize + bufferSize + (bufferSize / 2 + 1)
                                      + 12 + downsampledFrameSize + 512) * sizeof(double);
        const size_t kissFftBuffers = (size_t)bufferSize * 2 * sizeof(float) * 2;
        const size_t fifoBytes = (size_t)juce::jmax(0, fifoSamples) * sizeof(float);
        const size_t bytes = doubleVectors + kissFftBuffers + fifoBytes + sizeof(ChordDetector) + 2048;
        return (int)((bytes + 1023) / 1024);
    }

    static int estimateMemoryKb(int fifoSamples = 48000)
    {
        return estimateTrackMemoryKb(fifoSamples) * trackCount + 64;
    }

    int estimateTotalMemoryKb() const
    {
        const int fifoSamples = juce::jmax(1, (int)std::round(sampleRate));
        int total = estimateMemoryKb(fifoSamples);
        if (solititoModel != nullptr)
            total += solititoModel->getMemoryKb();
        return total;
    }

    TrackState* getTrack(int trackIndex)
    {
        return trackIndex >= 0 && trackIndex < trackCount ? &tracks[(size_t)trackIndex] : nullptr;
    }

    const TrackState* getTrack(int trackIndex) const
    {
        return trackIndex >= 0 && trackIndex < trackCount ? &tracks[(size_t)trackIndex] : nullptr;
    }

    void prepareTrack(TrackState& track, double newSampleRate)
    {
        track.sampleRate = newSampleRate > 1.0 ? newSampleRate : 44100.0;
        track.frame.assign((size_t)frameSize, 0.0);
        track.averagedChroma.fill(0.0);
        track.readBuffer.assign((size_t)frameSize * 4, 0.0f);
        const int newRingSize = juce::jmax(frameSize * 16, (int)std::round(track.sampleRate));
        track.ringBuffer.assign((size_t)newRingSize, 0.0f);
        track.audioFifo = std::make_unique<juce::AbstractFifo>(newRingSize);
        track.frameFill = 0;
        track.rmsSmoothed = 0.0;
        track.samplesSinceCpuUpdate = 0;
        track.analysisMsSinceCpuUpdate = 0.0;
        track.chromaHistory = {};
        track.chromaHistoryWrite = 0;
        track.chromaHistorySize = 0;
        resetTrackDecisionState(track);
        track.resetRequested.store(false, std::memory_order_relaxed);
        track.droppedSamples.store(0, std::memory_order_relaxed);
        track.cpuPercent.store(0.0, std::memory_order_relaxed);

        const int roundedRate = juce::jlimit(8000, 192000, (int)std::round(track.sampleRate));
        track.requestedSampleRate.store(roundedRate, std::memory_order_relaxed);
        configureChromagram(track, roundedRate);
        track.memoryKb.store(estimateTrackMemoryKb(newRingSize), std::memory_order_relaxed);
    }

    void copyMonoFramesToRing(TrackState& track, const float* input, int inputStartFrame, int numChannels, int ringStart, int count)
    {
        if (count <= 0)
            return;

        if (numChannels <= 1)
        {
            std::memcpy(track.ringBuffer.data() + ringStart,
                        input + inputStartFrame,
                        (size_t)count * sizeof(float));
            return;
        }

        for (int i = 0; i < count; ++i)
        {
            const int source = (inputStartFrame + i) * numChannels;
            track.ringBuffer[(size_t)(ringStart + i)] = 0.5f * (input[source] + input[source + 1]);
        }
    }

    void configureChromagram(TrackState& track, int newSampleRate)
    {
        track.analyzerSampleRate = juce::jlimit(8000, 192000, newSampleRate);
        track.sampleRate = (double)track.analyzerSampleRate;
        track.chromagram = std::make_unique<Chromagram>(frameSize, track.analyzerSampleRate);
        track.chromagram->setChromaCalculationInterval(chromaCalculationIntervalSamples);
        track.frameFill = 0;
        track.rmsSmoothed = 0.0;
        track.chromaHistory = {};
        track.chromaHistoryWrite = 0;
        track.chromaHistorySize = 0;
        resetTrackDecisionState(track);
    }

    void run() override
    {
        std::vector<PendingChordCandidate> chordBatch;
        chordBatch.reserve((size_t)trackCount);

        while (!threadShouldExit())
        {
            chordBatch.clear();
            bool didWork = false;

            for (int i = 0; i < trackCount; ++i)
            {
                auto& track = tracks[(size_t)i];
                if (track.resetRequested.exchange(false, std::memory_order_acquire))
                {
                    resetTrackAnalysis(track, i);
                    didWork = true;
                }

                if (readAvailableSamples(track, i, chordBatch))
                    didWork = true;
            }

            if (!chordBatch.empty())
                classifyChordBatch(chordBatch);

            if (!didWork)
                samplesAvailable.wait(20);
        }
    }

    bool readAvailableSamples(TrackState& track, int trackIndex, std::vector<PendingChordCandidate>& chordBatch)
    {
        if (!ready.load(std::memory_order_acquire))
            return false;

        if (track.audioFifo == nullptr || track.ringBuffer.empty() || track.readBuffer.empty())
            return false;

        const int requestedRate = track.requestedSampleRate.load(std::memory_order_relaxed);
        if (track.chromagram == nullptr || requestedRate != track.analyzerSampleRate)
            configureChromagram(track, requestedRate);

        const int available = track.audioFifo->getNumReady();
        if (available <= 0)
            return false;

        const int toRead = juce::jmin(available, (int)track.readBuffer.size());
        int start1 = 0;
        int size1 = 0;
        int start2 = 0;
        int size2 = 0;
        track.audioFifo->prepareToRead(toRead, start1, size1, start2, size2);

        if (size1 > 0)
            std::memcpy(track.readBuffer.data(), track.ringBuffer.data() + start1, (size_t)size1 * sizeof(float));
        if (size2 > 0)
            std::memcpy(track.readBuffer.data() + size1, track.ringBuffer.data() + start2, (size_t)size2 * sizeof(float));

        track.audioFifo->finishedRead(size1 + size2);

        const double startMs = juce::Time::getMillisecondCounterHiRes();
        const int samplesRead = size1 + size2;
        if (solititoModel != nullptr && solititoModel->isAvailable())
        {
            std::vector<SolititoChordModel::Candidate> solititoCandidates;
            solititoModel->appendSamples(trackIndex,
                                         track.readBuffer.data(),
                                         samplesRead,
                                         requestedRate,
                                         solititoCandidates);

            for (const auto& solititoCandidate : solititoCandidates)
            {
                PendingChordCandidate candidate;
                candidate.trackIndex = solititoCandidate.trackIndex;
                candidate.solititoFeatures = solititoCandidate.features;
                candidate.hasSolititoFeatures = true;
                chordBatch.push_back(candidate);
            }

            double energy = 0.0;
            for (int i = 0; i < samplesRead; ++i)
                energy += (double)track.readBuffer[(size_t)i] * (double)track.readBuffer[(size_t)i];
            const double blockRms = samplesRead > 0 ? std::sqrt(energy / (double)samplesRead) : 0.0;
            if (blockRms < silenceRmsThreshold)
                noteInvalidChordFrame(track);
        }
        else
        {
            for (int i = 0; i < samplesRead; ++i)
            {
                track.frame[(size_t)track.frameFill++] = (double)track.readBuffer[(size_t)i];
                if (track.frameFill >= frameSize)
                {
                    processFrame(track, trackIndex, chordBatch);
                    track.frameFill = 0;
                }
            }
        }

        const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - startMs;
        track.analysisMsSinceCpuUpdate += juce::jmax(0.0, elapsedMs);
        updateCpuUsage(track, samplesRead);

        return true;
    }

    void updateCpuUsage(TrackState& track, int samplesRead)
    {
        track.samplesSinceCpuUpdate += samplesRead;

        const int updateSamples = juce::jmax(1, (int)std::round(track.sampleRate));
        if (track.samplesSinceCpuUpdate >= updateSamples)
        {
            const double realTimeMs = ((double)track.samplesSinceCpuUpdate / track.sampleRate) * 1000.0;
            const double rawCpu = realTimeMs > 0.0 ? (track.analysisMsSinceCpuUpdate / realTimeMs) * 100.0 : 0.0;
            const double previous = track.cpuPercent.load(std::memory_order_relaxed);
            const double smoothed = previous <= 0.0 ? rawCpu : previous * 0.75 + rawCpu * 0.25;
            track.cpuPercent.store(juce::jlimit(0.0, 200.0, smoothed), std::memory_order_relaxed);
            track.analysisMsSinceCpuUpdate = 0.0;
            track.samplesSinceCpuUpdate = 0;
        }
    }

    void resetTrackAnalysis(TrackState& track, int trackIndex)
    {
        if (solititoModel != nullptr)
            solititoModel->resetTrack(trackIndex);

        discardQueuedAudio(track);
        track.frameFill = 0;
        track.rmsSmoothed = 0.0;
        track.samplesSinceCpuUpdate = 0;
        track.analysisMsSinceCpuUpdate = 0.0;
        track.averagedChroma.fill(0.0);
        track.chromaHistory = {};
        track.chromaHistoryWrite = 0;
        track.chromaHistorySize = 0;
        resetTrackDecisionState(track);
    }

    void discardQueuedAudio(TrackState& track)
    {
        if (track.audioFifo == nullptr || track.readBuffer.empty())
            return;

        while (track.audioFifo->getNumReady() > 0)
        {
            const int toRead = juce::jmin(track.audioFifo->getNumReady(), (int)track.readBuffer.size());
            if (toRead <= 0)
                break;

            int start1 = 0;
            int size1 = 0;
            int start2 = 0;
            int size2 = 0;
            track.audioFifo->prepareToRead(toRead, start1, size1, start2, size2);
            track.audioFifo->finishedRead(size1 + size2);
        }
    }

    void processFrame(TrackState& track, int trackIndex, std::vector<PendingChordCandidate>& chordBatch)
    {
        double energy = 0.0;
        for (double sample : track.frame)
            energy += sample * sample;

        const double frameRms = std::sqrt(energy / (double)track.frame.size());
        track.rmsSmoothed = frameRms > track.rmsSmoothed ? frameRms : track.rmsSmoothed * 0.92 + frameRms * 0.08;

        track.chromagram->processAudioFrame(track.frame.data());
        if (!track.chromagram->isReady())
            return;

        if (track.rmsSmoothed < silenceRmsThreshold)
        {
            noteInvalidChordFrame(track);
            return;
        }

        auto chroma = track.chromagram->getChromagram();
        double chromaTotal = 0.0;
        for (double value : chroma)
            chromaTotal += std::abs(value);

        if (chromaTotal <= 1.0e-9)
        {
            noteInvalidChordFrame(track);
            return;
        }

        track.chromaHistory[(size_t)track.chromaHistoryWrite].fill(0.0);
        for (int i = 0; i < 12 && i < (int)chroma.size(); ++i)
            track.chromaHistory[(size_t)track.chromaHistoryWrite][(size_t)i] = chroma[(size_t)i];
        track.chromaHistoryWrite = (track.chromaHistoryWrite + 1) % chromaHistoryFrames;
        track.chromaHistorySize = juce::jmin(track.chromaHistorySize + 1, chromaHistoryFrames);

        track.averagedChroma.fill(0.0);
        for (int h = 0; h < track.chromaHistorySize; ++h)
            for (int i = 0; i < 12; ++i)
                track.averagedChroma[(size_t)i] += track.chromaHistory[(size_t)h][(size_t)i];

        const double scale = 1.0 / (double)juce::jmax(1, track.chromaHistorySize);
        for (double& value : track.averagedChroma)
            value *= scale;

        int dominantPitchClass = -1;
        if (hasEnoughHarmonicContent(track.averagedChroma))
        {
            PendingChordCandidate candidate;
            candidate.trackIndex = trackIndex;
            candidate.chroma = track.averagedChroma;
            chordBatch.push_back(candidate);
            return;
        }

        if (!hasDominantPitchClass(track.averagedChroma, dominantPitchClass))
        {
            noteInvalidChordFrame(track);
            return;
        }

        publishNoteCandidate(track, trackIndex, dominantPitchClass, 1.0f);
    }

    void classifyChordBatch(std::vector<PendingChordCandidate>& chordBatch)
    {
        const double startMs = juce::Time::getMillisecondCounterHiRes();

        if (solititoModel != nullptr && solititoModel->isAvailable())
        {
            std::array<int, trackCount> latestSolititoCandidateByTrack;
            latestSolititoCandidateByTrack.fill(-1);
            for (int i = 0; i < (int)chordBatch.size(); ++i)
            {
                const auto& candidate = chordBatch[(size_t)i];
                if (candidate.hasSolititoFeatures && candidate.trackIndex >= 0 && candidate.trackIndex < trackCount)
                    latestSolititoCandidateByTrack[(size_t)candidate.trackIndex] = i;
            }

            std::vector<SolititoChordModel::Candidate> solititoCandidates;
            solititoCandidates.reserve((size_t)trackCount);
            for (int latestIndex : latestSolititoCandidateByTrack)
            {
                if (latestIndex < 0)
                    continue;

                const auto& candidate = chordBatch[(size_t)latestIndex];
                SolititoChordModel::Candidate solititoCandidate;
                solititoCandidate.trackIndex = candidate.trackIndex;
                solititoCandidate.features = candidate.solititoFeatures;
                solititoCandidates.push_back(solititoCandidate);
            }

            std::vector<SolititoChordModel::Prediction> predictions;
            if (!solititoCandidates.empty() && solititoModel->runBatch(solititoCandidates, predictions))
            {
                for (const auto& prediction : predictions)
                {
                    auto* track = getTrack(prediction.trackIndex);
                    if (track == nullptr || track->resetRequested.load(std::memory_order_acquire))
                        continue;

                    const float minConfidence = prediction.trackIndex == masterTrackIndex
                                                ? masterSolititoConfidenceThreshold
                                                : solititoConfidenceThreshold;
                    if (prediction.isNoise || prediction.root < 0 || prediction.root >= 12 || prediction.confidence < minConfidence)
                    {
                        noteInvalidChordFrame(*track);
                        continue;
                    }

                    if (prediction.isNote)
                        publishNoteCandidate(*track, prediction.trackIndex, prediction.root, prediction.confidence);
                    else
                        publishChordCandidate(*track, prediction.trackIndex, prediction.root, prediction.quality, prediction.intervals, prediction.confidence);
                }
            }
        }

        for (auto& candidate : chordBatch)
        {
            if (candidate.hasSolititoFeatures)
                continue;

            auto* track = getTrack(candidate.trackIndex);
            if (track == nullptr || track->resetRequested.load(std::memory_order_acquire))
                continue;

            track->chordDetector.detectChord(candidate.chroma.data());
            if (track->chordDetector.rootNote < 0 || track->chordDetector.rootNote >= 12)
            {
                noteInvalidChordFrame(*track);
                continue;
            }

            publishChordCandidate(*track,
                                  candidate.trackIndex,
                                  track->chordDetector.rootNote,
                                  track->chordDetector.quality,
                                  track->chordDetector.intervals,
                                  1.0f);
        }

        const double elapsedMs = juce::jmax(0.0, juce::Time::getMillisecondCounterHiRes() - startMs);
        if (elapsedMs > 0.0 && !chordBatch.empty())
        {
            const double perCandidateMs = elapsedMs / (double)chordBatch.size();
            for (const auto& candidate : chordBatch)
                if (auto* track = getTrack(candidate.trackIndex))
                    track->analysisMsSinceCpuUpdate += perCandidateMs;
        }
    }

    static int makeChordKey(int root, int quality, int intervals)
    {
        return root * 100 + quality * 10 + intervals;
    }

    static void resetTrackDecisionState(TrackState& track)
    {
        track.pendingChordKey = -1;
        track.pendingChordHits = 0;
        track.pendingNoteRoot = -1;
        track.pendingNoteHits = 0;
        track.displayedChordKey = -1;
        track.invalidChordFrames = 0;
        track.chordRoot.store(-1, std::memory_order_relaxed);
        track.chordQuality.store(ChordDetector::Major, std::memory_order_relaxed);
        track.chordIntervals.store(0, std::memory_order_relaxed);
        track.chordConfidence.store(0.0, std::memory_order_relaxed);
        track.chordValid.store(false, std::memory_order_relaxed);
        track.noteRoot.store(-1, std::memory_order_relaxed);
        track.noteConfidence.store(0.0, std::memory_order_relaxed);
        track.noteValid.store(false, std::memory_order_relaxed);
    }

    static void noteInvalidChordFrame(TrackState& track)
    {
        track.pendingChordKey = -1;
        track.pendingChordHits = 0;
        track.pendingNoteRoot = -1;
        track.pendingNoteHits = 0;

        if (++track.invalidChordFrames >= invalidClearThresholdFrames)
            resetTrackDecisionState(track);
    }

    static bool hasEnoughHarmonicContent(const std::array<double, 12>& chroma)
    {
        double sum = 0.0;
        double max1 = 0.0;
        double max2 = 0.0;
        double max3 = 0.0;

        for (double value : chroma)
        {
            const double v = std::abs(value);
            sum += v;
            if (v > max1)
            {
                max3 = max2;
                max2 = max1;
                max1 = v;
            }
            else if (v > max2)
            {
                max3 = max2;
                max2 = v;
            }
            else if (v > max3)
            {
                max3 = v;
            }
        }

        if (sum <= 1.0e-9 || max1 <= 1.0e-9)
            return false;

        const double mean = sum / 12.0;
        if (max1 < mean * 1.7)
            return false;

        if (max2 < max1 * 0.24)
            return false;

        const double top3Share = (max1 + max2 + max3) / sum;
        if (top3Share < 0.40)
            return false;

        const double dominantShare = max1 / sum;

        int strongPitchClasses = 0;
        for (double value : chroma)
            if (std::abs(value) >= max1 * 0.25)
                ++strongPitchClasses;

        if (dominantShare > 0.34 && strongPitchClasses <= 2)
            return false;

        return strongPitchClasses >= 2;
    }

    static bool hasDominantPitchClass(const std::array<double, 12>& chroma, int& dominantPitchClass)
    {
        dominantPitchClass = -1;

        double sum = 0.0;
        double max1 = 0.0;
        int strongPitchClasses = 0;

        for (int i = 0; i < 12; ++i)
        {
            const double v = std::abs(chroma[(size_t)i]);
            sum += v;

            if (v > max1)
            {
                max1 = v;
                dominantPitchClass = i;
            }
        }

        if (sum <= 1.0e-9 || max1 <= 1.0e-9 || dominantPitchClass < 0)
            return false;

        const double mean = sum / 12.0;
        if (max1 < mean * 2.0)
            return false;

        if ((max1 / sum) < 0.22)
            return false;

        for (double value : chroma)
            if (std::abs(value) >= max1 * 0.40)
                ++strongPitchClasses;

        return strongPitchClasses <= 4;
    }

    static void publishNoteCandidate(TrackState& track, int trackIndex, int root, float confidence)
    {
        track.invalidChordFrames = 0;
        track.pendingChordKey = -1;
        track.pendingChordHits = 0;

        if (root == track.pendingNoteRoot)
            track.pendingNoteHits = juce::jmin(track.pendingNoteHits + 1, 1000);
        else
        {
            track.pendingNoteRoot = root;
            track.pendingNoteHits = 1;
        }

        const int displayedNote = track.noteRoot.load(std::memory_order_relaxed);
        if (track.noteValid.load(std::memory_order_relaxed) && root == displayedNote)
        {
            track.noteConfidence.store(confidence, std::memory_order_relaxed);
            return;
        }

        const bool isMasterTrack = trackIndex == masterTrackIndex;
        const int requiredHits = displayedNote >= 0
                               ? (isMasterTrack ? masterStableNoteChangeHitThreshold : stableNoteChangeHitThreshold)
                               : (isMasterTrack ? masterStableNoteHitThreshold : stableNoteHitThreshold);
        if (track.pendingNoteHits < requiredHits)
            return;

        track.noteRoot.store(root, std::memory_order_relaxed);
        track.noteConfidence.store(confidence, std::memory_order_relaxed);
        track.noteValid.store(true, std::memory_order_relaxed);
        track.chordConfidence.store(0.0, std::memory_order_relaxed);
        track.chordValid.store(false, std::memory_order_relaxed);
    }

    static void publishChordCandidate(TrackState& track, int trackIndex, int root, int quality, int intervals, float confidence)
    {
        const int candidateKey = makeChordKey(root, quality, intervals);
        track.invalidChordFrames = 0;
        track.pendingNoteRoot = -1;
        track.pendingNoteHits = 0;

        if (candidateKey == track.pendingChordKey)
            track.pendingChordHits = juce::jmin(track.pendingChordHits + 1, 1000);
        else
        {
            track.pendingChordKey = candidateKey;
            track.pendingChordHits = 1;
        }

        if (candidateKey == track.displayedChordKey)
        {
            track.chordConfidence.store(confidence, std::memory_order_relaxed);
            track.noteRoot.store(-1, std::memory_order_relaxed);
            track.noteConfidence.store(0.0, std::memory_order_relaxed);
            track.noteValid.store(false, std::memory_order_relaxed);
            track.chordValid.store(true, std::memory_order_relaxed);
            return;
        }

        const bool isMasterTrack = trackIndex == masterTrackIndex;
        const int requiredHits = track.displayedChordKey >= 0
                               ? (isMasterTrack ? masterStableChordChangeHitThreshold : stableChordChangeHitThreshold)
                               : (isMasterTrack ? masterStableCandidateHitThreshold : stableCandidateHitThreshold);
        if (track.pendingChordHits < requiredHits)
            return;

        track.displayedChordKey = candidateKey;
        track.chordRoot.store(root, std::memory_order_relaxed);
        track.chordQuality.store(quality, std::memory_order_relaxed);
        track.chordIntervals.store(intervals, std::memory_order_relaxed);
        track.chordConfidence.store(confidence, std::memory_order_relaxed);
        track.noteRoot.store(-1, std::memory_order_relaxed);
        track.noteConfidence.store(0.0, std::memory_order_relaxed);
        track.noteValid.store(false, std::memory_order_relaxed);
        track.chordValid.store(true, std::memory_order_relaxed);
    }

    std::array<TrackState, trackCount> tracks;
    std::unique_ptr<SolititoChordModel> solititoModel;
    juce::WaitableEvent samplesAvailable;
    double sampleRate = 44100.0;
    std::atomic<bool> ready { false };
    std::atomic<int> memoryKb { 0 };
};
class AsyncChatTranslationWorker final : private juce::Thread
{
public:
    struct Request
    {
        juce::String originalLine;
        juce::String lineSender;
        juce::String linePrefix;
        juce::String lineBody;
        juce::String targetCode;
        juce::uint64 configRevision = 0;
    };

    explicit AsyncChatTranslationWorker(NinjamVst3AudioProcessor& ownerProcessor)
        : juce::Thread("NINJAMChatTranslation"), owner(ownerProcessor)
    {
        startThread();
    }

    ~AsyncChatTranslationWorker() override
    {
        stop();
    }

    void enqueue(Request request)
    {
        {
            const juce::ScopedLock lock(queueLock);
            queue.push_back(std::move(request));
        }
        workAvailable.signal();
    }

    void stop(int timeoutMs = 1500)
    {
        signalThreadShouldExit();
        workAvailable.signal();
        stopThread(juce::jlimit(100, 6000, timeoutMs));

        const juce::ScopedLock lock(queueLock);
        queue.clear();
    }

private:
    void run() override
    {
        while (!threadShouldExit())
        {
            Request request;
            bool haveRequest = false;

            {
                const juce::ScopedLock lock(queueLock);
                if (!queue.empty())
                {
                    request = std::move(queue.front());
                    queue.pop_front();
                    haveRequest = true;
                }
            }

            if (!haveRequest)
            {
                workAvailable.wait(200);
                continue;
            }

            const juce::String translatedBody = owner.translateTextForTarget(request.lineBody, request.targetCode);
            if (threadShouldExit())
                break;

            owner.applyAsyncTranslatedChatLine(request.originalLine,
                                               request.lineSender,
                                               request.linePrefix + translatedBody,
                                               request.configRevision);
        }
    }

    NinjamVst3AudioProcessor& owner;
    juce::CriticalSection queueLock;
    std::deque<Request> queue;
    juce::WaitableEvent workAvailable;
};

static juce::String getSystemTranslationLanguageCode()
{
    juce::String language = juce::SystemStats::getDisplayLanguage();
    if (language.isEmpty())
        language = juce::SystemStats::getUserLanguage();

    language = language.trim().replaceCharacter('_', '-').toLowerCase();

    if (language.startsWith("zh-hant") || language.startsWith("zh-tw") || language.startsWith("zh-hk"))
        return "zh-Hant";

    if (language.startsWith("zh"))
        return "zh-Hans";

    if (language.startsWith("pt-br"))
        return "pt-BR";

    if (language.startsWith("no") || language.startsWith("nb"))
        return "nb";

    if (language.startsWith("iw"))
        return "he";

    if (language.startsWith("in"))
        return "id";

    const int dash = language.indexOfChar('-');
    if (dash > 0)
        language = language.substring(0, dash);

    if (language.isEmpty())
        language = "en";

    return language;
}

static juce::String resolveTranslateTargetLanguageCode(const juce::String& preferredCode)
{
    juce::String normalised = preferredCode.trim().replaceCharacter('_', '-').toLowerCase();
    if (normalised.isEmpty() || normalised == "system")
        return getSystemTranslationLanguageCode();

    if (normalised == "zh-cn" || normalised == "zh-hans")
        return "zh-Hans";

    if (normalised == "zh-tw" || normalised == "zh-hk" || normalised == "zh-hant")
        return "zh-Hant";

    if (normalised == "pt-br")
        return "pt-BR";

    if (normalised == "no" || normalised == "nb")
        return "nb";

    const int dash = normalised.indexOfChar('-');
    if (dash > 0)
        normalised = normalised.substring(0, dash);

    return normalised.isNotEmpty() ? normalised : "en";
}

static bool detectedLanguageMatchesTarget(const juce::var& detected, const juce::String& targetCode)
{
    auto sameAsTarget = [&targetCode](const juce::String& languageCode)
    {
        return resolveTranslateTargetLanguageCode(languageCode) == targetCode;
    };

    if (auto* detectedObject = detected.getDynamicObject())
        return sameAsTarget(detectedObject->getProperty("language").toString());

    if (auto* detectedArray = detected.getArray(); detectedArray != nullptr && !detectedArray->isEmpty())
    {
        if (auto* firstObject = detectedArray->getReference(0).getDynamicObject())
            return sameAsTarget(firstObject->getProperty("language").toString());
    }

    return false;
}

static bool tryTranslateWithFedilab(const juce::String& text,
                                    const juce::String& targetCode,
                                    juce::String& translatedText,
                                    juce::String& error)
{
    juce::URL requestUrl("https://translate.fedilab.app/translate");
    requestUrl = requestUrl.withParameter("q", text)
                           .withParameter("source", "auto")
                           .withParameter("target", targetCode)
                           .withParameter("format", "text");

    int httpStatusCode = 0;
    auto responseStream = requestUrl.createInputStream(
        juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
            .withConnectionTimeoutMs(2000)
            .withNumRedirectsToFollow(2)
            .withStatusCode(&httpStatusCode)
            .withExtraHeaders("User-Agent: NINJAMVST3/1.0\r\nAccept: application/json\r\nContent-Type: application/x-www-form-urlencoded\r\n")
            .withHttpRequestCmd("POST"));

    if (responseStream == nullptr)
    {
        error = "primary translator could not be reached";
        return false;
    }

    if (httpStatusCode != 0 && httpStatusCode != 200)
    {
        error = "primary translator returned HTTP " + juce::String(httpStatusCode);
        return false;
    }

    const juce::String responseText = responseStream->readEntireStreamAsString();
    if (responseText.isEmpty())
    {
        error = "primary translator returned an empty response";
        return false;
    }

    const juce::var parsed = juce::JSON::parse(responseText);
    auto* root = parsed.getDynamicObject();
    if (root == nullptr)
    {
        error = "primary translator returned invalid JSON";
        return false;
    }

    if (root->hasProperty("error"))
    {
        error = root->getProperty("error").toString().isNotEmpty()
                    ? root->getProperty("error").toString()
                    : juce::String("primary translator reported an error");
        return false;
    }

    if (auto detected = root->getProperty("detectedLanguage"); !detected.isVoid() && detectedLanguageMatchesTarget(detected, targetCode))
    {
        translatedText = text;
        return true;
    }

    translatedText = root->getProperty("translatedText").toString();
    if (translatedText.isEmpty())
    {
        error = "primary translator did not return translated text";
        return false;
    }

    return true;
}

static bool tryTranslateWithGoogleFallback(const juce::String& text,
                                           const juce::String& targetCode,
                                           juce::String& translatedText,
                                           juce::String& error)
{
    juce::URL requestUrl("https://translate.googleapis.com/translate_a/single");
    requestUrl = requestUrl.withParameter("client", "gtx")
                           .withParameter("sl", "auto")
                           .withParameter("tl", targetCode)
                           .withParameter("dt", "t")
                           .withParameter("q", text);

    int httpStatusCode = 0;
    auto responseStream = requestUrl.createInputStream(
        juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs(2000)
            .withNumRedirectsToFollow(2)
            .withStatusCode(&httpStatusCode)
            .withExtraHeaders("User-Agent: NINJAMVST3/1.0\r\nAccept: application/json\r\n")
            .withHttpRequestCmd("GET"));

    if (responseStream == nullptr)
    {
        error = "fallback translator could not be reached";
        return false;
    }

    if (httpStatusCode != 0 && httpStatusCode != 200)
    {
        error = "fallback translator returned HTTP " + juce::String(httpStatusCode);
        return false;
    }

    const juce::String responseText = responseStream->readEntireStreamAsString();
    if (responseText.isEmpty())
    {
        error = "fallback translator returned an empty response";
        return false;
    }

    const juce::var parsed = juce::JSON::parse(responseText);
    auto* rootArray = parsed.getArray();
    if (rootArray == nullptr || rootArray->isEmpty())
    {
        error = "fallback translator returned invalid JSON";
        return false;
    }

    if (rootArray->size() > 2 && resolveTranslateTargetLanguageCode(rootArray->getReference(2).toString()) == targetCode)
    {
        translatedText = text;
        return true;
    }

    auto* segments = rootArray->getReference(0).getArray();
    if (segments == nullptr || segments->isEmpty())
    {
        error = "fallback translator did not return translated segments";
        return false;
    }

    juce::String combined;
    for (const auto& segmentVar : *segments)
    {
        if (auto* segment = segmentVar.getArray(); segment != nullptr && !segment->isEmpty())
            combined << segment->getReference(0).toString();
    }

    if (combined.isEmpty())
    {
        error = "fallback translator returned empty translated text";
        return false;
    }

    translatedText = combined;
    return true;
}

static bool copyPlayHeadPositionToCurrentInfo(juce::AudioPlayHead& playHead,
                                              juce::AudioPlayHead::CurrentPositionInfo& info)
{
    const auto position = playHead.getPosition();
    if (!position)
        return false;

    info.resetToDefault();

    if (const auto sig = position->getTimeSignature())
    {
        info.timeSigNumerator = sig->numerator;
        info.timeSigDenominator = sig->denominator;
    }

    if (const auto loop = position->getLoopPoints())
    {
        info.ppqLoopStart = loop->ppqStart;
        info.ppqLoopEnd = loop->ppqEnd;
    }

    if (const auto frame = position->getFrameRate())
        info.frameRate = *frame;
    if (const auto timeInSeconds = position->getTimeInSeconds())
        info.timeInSeconds = *timeInSeconds;
    if (const auto lastBarStartPpq = position->getPpqPositionOfLastBarStart())
        info.ppqPositionOfLastBarStart = *lastBarStartPpq;
    if (const auto ppqPosition = position->getPpqPosition())
        info.ppqPosition = *ppqPosition;
    if (const auto originTime = position->getEditOriginTime())
        info.editOriginTime = *originTime;
    if (const auto bpm = position->getBpm())
        info.bpm = *bpm;
    if (const auto timeInSamples = position->getTimeInSamples())
        info.timeInSamples = *timeInSamples;

    info.isPlaying = position->getIsPlaying();
    info.isRecording = position->getIsRecording();
    info.isLooping = position->getIsLooping();

    return true;
}

static int computeJamTabaHostSyncStartPositionSamples(const juce::AudioPlayHead::CurrentPositionInfo& hostInfo,
                                                      double sampleRate)
{
    if (sampleRate <= 1.0
        || !std::isfinite(hostInfo.bpm)
        || hostInfo.bpm <= 0.0
        || !std::isfinite(hostInfo.ppqPosition))
    {
        return 0;
    }

    const double samplesPerBeat = (60.0 * sampleRate) / hostInfo.bpm;
    if (!std::isfinite(samplesPerBeat) || samplesPerBeat <= 0.0)
        return 0;

    if (hostInfo.ppqPosition > 0.0)
    {
        const int denominator = hostInfo.timeSigDenominator > 0 ? hostInfo.timeSigDenominator : 4;
        const int numerator = hostInfo.timeSigNumerator > 0 ? hostInfo.timeSigNumerator : 4;
        const double barLengthInQuarterNotes = (4.0 * (double) numerator) / (double) denominator;
        if (barLengthInQuarterNotes > 0.0)
        {
            double cursorPosInMeasure = hostInfo.ppqPosition - hostInfo.ppqPositionOfLastBarStart;
            if (!std::isfinite(hostInfo.ppqPositionOfLastBarStart)
                || cursorPosInMeasure < -1.0e-8
                || cursorPosInMeasure > barLengthInQuarterNotes + 1.0e-8)
            {
                cursorPosInMeasure = std::fmod(hostInfo.ppqPosition, barLengthInQuarterNotes);
                if (cursorPosInMeasure < 0.0)
                    cursorPosInMeasure += barLengthInQuarterNotes;
            }

            constexpr double hostSyncStartGraceMs = 80.0;
            const double startGraceBeats = (hostSyncStartGraceMs / 1000.0) * hostInfo.bpm / 60.0;
            if (cursorPosInMeasure <= juce::jmax(1.0e-8, startGraceBeats))
                return 0;

            if (cursorPosInMeasure > 1.0e-8)
            {
                const double samplesUntilNextMeasure = (barLengthInQuarterNotes - cursorPosInMeasure) * samplesPerBeat;
                return -(int) std::llround(samplesUntilNextMeasure);
            }
        }

        return 0;
    }

    return (int) std::llround(hostInfo.ppqPosition * samplesPerBeat);
}

static int computeLinkSyncStartPositionSamples(double phaseBeats,
                                               double quantum,
                                               double tempoBpm,
                                               double sampleRate)
{
    if (sampleRate <= 1.0
        || !std::isfinite(phaseBeats)
        || !std::isfinite(quantum)
        || quantum <= 0.0
        || !std::isfinite(tempoBpm)
        || tempoBpm <= 0.0)
    {
        return 0;
    }

    double wrappedPhase = std::fmod(phaseBeats, quantum);
    if (wrappedPhase < 0.0)
        wrappedPhase += quantum;

    constexpr double phaseEpsilon = 1.0e-6;
    if (wrappedPhase <= phaseEpsilon || (quantum - wrappedPhase) <= phaseEpsilon)
        return 0;

    const double samplesPerBeat = (60.0 * sampleRate) / tempoBpm;
    if (!std::isfinite(samplesPerBeat) || samplesPerBeat <= 0.0)
        return 0;

    const double samplesUntilNextQuantum = (quantum - wrappedPhase) * samplesPerBeat;
    return -(int) std::llround(samplesUntilNextQuantum);
}

static int normaliseSignedIntervalPosition(int positionSamples, int intervalLength)
{
    if (intervalLength <= 0)
        return 0;

    int normalised = positionSamples % intervalLength;
    if (normalised < 0)
        normalised += intervalLength;
    return normalised;
}

static juce::StringArray extractPublicServerUserNames(const juce::var& usersVar)
{
    juce::StringArray names;
    auto* usersArray = usersVar.getArray();
    if (usersArray == nullptr)
        return names;

    for (const auto& userVar : *usersArray)
    {
        juce::String name;
        if (auto* userObj = userVar.getDynamicObject())
        {
            name = userObj->getProperty("name").toString();
            if (name.isEmpty())
                name = userObj->getProperty("username").toString();
            if (name.isEmpty())
                name = userObj->getProperty("user").toString();
        }
        else
        {
            name = userVar.toString();
        }

        name = name.trim();
        if (name.isNotEmpty())
            names.addIfNotAlreadyThere(name);
    }

    names.sortNatural();
    return names;
}

static juce::String stripAnonymousPrefix(juce::String user)
{
    user = user.trim();
    if (user.startsWithIgnoreCase("anonymous:"))
        return user.fromFirstOccurrenceOf(":", false, false).trim();
    return user;
}

static juce::String buildNumberedUserName(const juce::String& originalUser, int attempt)
{
    const juce::String trimmed = originalUser.trim();
    const bool isAnonymous = trimmed.startsWithIgnoreCase("anonymous:");
    juce::String base = stripAnonymousPrefix(trimmed);
    if (base.isEmpty())
        base = "jammer";

    while (base.isNotEmpty() && juce::CharacterFunctions::isDigit(base.getLastCharacter()))
        base = base.dropLastCharacters(1);
    base = base.trim();
    if (base.isEmpty())
        base = "jammer";

    const juce::String numbered = base + juce::String(juce::jmax(2, attempt + 1));
    return isAnonymous ? "anonymous:" + numbered : numbered;
}

static juce::String chooseAvailableUserNameForServer(const juce::String& requestedUser,
                                                     const std::vector<NinjamVst3AudioProcessor::PublicServerInfo>& servers,
                                                     const juce::String& host)
{
    const juce::String requestedTrimmed = requestedUser.trim();
    juce::String base = stripAnonymousPrefix(requestedTrimmed);
    if (base.isEmpty())
        return requestedTrimmed;

    juce::String hostOnly = host.upToFirstOccurrenceOf(":", false, false).trim();
    const int requestedPort = host.fromFirstOccurrenceOf(":", false, false).getIntValue();
    juce::StringArray existing;
    for (const auto& server : servers)
    {
        const bool hostMatches = server.host.equalsIgnoreCase(hostOnly) || server.name.equalsIgnoreCase(host);
        const bool portMatches = requestedPort <= 0 || server.port == requestedPort;
        if (hostMatches && portMatches)
        {
            existing = server.userNames;
            break;
        }
    }

    if (existing.isEmpty())
        return requestedTrimmed;

    auto nameExists = [&existing](const juce::String& candidate)
    {
        const juce::String shortCandidate = stripAnonymousPrefix(candidate);
        for (const auto& existingName : existing)
            if (stripAnonymousPrefix(existingName).equalsIgnoreCase(shortCandidate))
                return true;
        return false;
    };

    if (!nameExists(requestedTrimmed))
        return requestedTrimmed;

    for (int attempt = 1; attempt <= 3; ++attempt)
    {
        const juce::String candidate = buildNumberedUserName(requestedTrimmed, attempt);
        if (!nameExists(candidate))
            return candidate;
    }

    return buildNumberedUserName(requestedTrimmed, 1);
}

static bool looksLikeDuplicateNameError(const juce::String& errorText)
{
    const juce::String err = errorText.trim().toLowerCase();
    return err.contains("name")
        && (err.contains("use")
            || err.contains("taken")
            || err.contains("duplicate")
            || err.contains("already"));
}

static int computeHostIntervalPhasePositionSamples(const juce::AudioPlayHead::CurrentPositionInfo& hostInfo,
                                                   double sampleRate,
                                                   int bpi,
                                                   int intervalLength)
{
    if (sampleRate <= 1.0
        || bpi <= 0
        || intervalLength <= 0
        || !std::isfinite(hostInfo.bpm)
        || hostInfo.bpm <= 0.0
        || !std::isfinite(hostInfo.ppqPosition))
    {
        return -1;
    }

    const double samplesPerBeat = (60.0 * sampleRate) / hostInfo.bpm;
    if (!std::isfinite(samplesPerBeat) || samplesPerBeat <= 0.0)
        return -1;

    double beatPhase = std::fmod(hostInfo.ppqPosition, (double)bpi);
    if (beatPhase < 0.0)
        beatPhase += (double)bpi;

    return normaliseSignedIntervalPosition((int)std::llround(beatPhase * samplesPerBeat), intervalLength);
}

static int shortestIntervalPhaseError(int targetPosition, int currentPosition, int intervalLength)
{
    if (intervalLength <= 0)
        return 0;

    int error = targetPosition - currentPosition;
    const int halfLength = intervalLength / 2;
    if (error > halfLength)
        error -= intervalLength;
    else if (error < -halfLength)
        error += intervalLength;
    return error;
}

static std::chrono::microseconds getNextLinkQuantumTime(
    const ableton::LinkAudio::SessionState& sessionState,
    std::chrono::microseconds fromTime,
    double quantum,
    double tempoBpm)
{
    if (!std::isfinite(quantum) || quantum <= 0.0
        || !std::isfinite(tempoBpm) || tempoBpm <= 0.0)
    {
        return fromTime;
    }

    double phase = std::fmod(sessionState.phaseAtTime(fromTime, quantum), quantum);
    if (phase < 0.0)
        phase += quantum;

    constexpr double phaseEpsilon = 1.0e-5;
    if (phase <= phaseEpsilon || (quantum - phase) <= phaseEpsilon)
        return fromTime;

    const double microsPerBeat = 60000000.0 / tempoBpm;
    const double microsUntilQuantum = (quantum - phase) * microsPerBeat;
    if (!std::isfinite(microsUntilQuantum) || microsUntilQuantum <= 0.0)
        return fromTime;

    return fromTime + std::chrono::microseconds((long long)std::llround(microsUntilQuantum));
}

static bool openUrlExternal(const juce::String& urlText)
{
#ifdef _WIN32
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", urlText.toWideCharPointer(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)result > 32)
        return true;
#endif
    return juce::URL(urlText).launchInDefaultBrowser();
}

static bool openUrlExternalOnMessageThread(const juce::String& urlText)
{
#if JUCE_WINDOWS
    return openUrlExternal(urlText);
#else
    if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
    {
        if (mm->isThisTheMessageThread())
            return openUrlExternal(urlText);

        struct UrlPayload
        {
            juce::String url;
            bool opened = false;
        } payload { urlText, false };

        mm->callFunctionOnMessageThread(
            [](void* userData) -> void*
            {
                auto* p = static_cast<UrlPayload*>(userData);
                p->opened = openUrlExternal(p->url);
                return nullptr;
            },
            &payload);

        return payload.opened;
    }

    return openUrlExternal(urlText);
#endif
}

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <dlfcn.h>
#endif


namespace
{
    constexpr unsigned int makeNjFourcc(const char a, const char b, const char c, const char d)
    {
        return ((unsigned int)(unsigned char)a) |
               ((unsigned int)(unsigned char)b << 8) |
               ((unsigned int)(unsigned char)c << 16) |
               ((unsigned int)(unsigned char)d << 24);
    }
    constexpr const char* opusSyncAppFamily = "ninjam-vst3";
    constexpr int opusSyncHandshakeVersion = 1;
    constexpr const char* opusSyncChatPrefix = "__NINJAM_VST3_OPUSSYNC__ ";
    // Custom FOURCC for opusSyncSupport broadcast via NINJAM interval channel
    // Any server routes it transparently; other clients ignore unknown FOURCCs
    constexpr unsigned int kOpusSyncFourcc    = makeNjFourcc('N','J','S','3');
    // Custom FOURCC for interval sync signals (intervalSyncTag, transportProbe, latencyReport)
    constexpr unsigned int kSyncSignalFourcc  = makeNjFourcc('N','J','S','4');
    constexpr unsigned int kNinjamZapVideoH264Fourcc = makeNjFourcc('H','2','6','4');
    constexpr unsigned int kNinjamZapVideoVp8Fourcc  = makeNjFourcc('V','P','8',' ');
    constexpr unsigned int kNinjamZapVideoVp9Fourcc  = makeNjFourcc('V','P','9',' ');
    constexpr unsigned int kNinjamZapVideoMjpgFourcc = makeNjFourcc('M','J','P','G');
    constexpr int kNinjamZapVideoOnlyChannelFlag = NJClient::NJCLIENT_CHANNEL_FLAG_VIDEO_ONLY;
    constexpr const char* sideSignalChatPrefix = "__NINJAM_VST3_SIDESIGNAL__ ";
    constexpr int remoteLatencyUpdateCadenceIntervals = 1;
    // Fixed indices: 0=legacy audio, 1=hidden NJ+ control, 2=Opus multichannel lane, 3=voice.
    constexpr int kNinjamPlusControlChannelIndex = 1;
    constexpr int kVoiceChatChannelIndex = 3;
    constexpr int kOpusMultichannelBaseIndex = 2;
    constexpr int kSyncSignalChannelIndex = kNinjamPlusControlChannelIndex;
    constexpr double intervalHelperPayloadMinWriteMs = 500.0;
    constexpr long long intervalSyncMarkerKeyBeatStride = 1024;
    constexpr int kLocalInputLinkAudioSentinel = -2000000000;
    constexpr double linkAudioQuantumBeats = 4.0;

    bool isNinjamZapVideoFourcc(unsigned int fourcc)
    {
        return fourcc == kNinjamZapVideoH264Fourcc
            || fourcc == kNinjamZapVideoVp8Fourcc
            || fourcc == kNinjamZapVideoVp9Fourcc
            || fourcc == kNinjamZapVideoMjpgFourcc;
    }

    ninjamplus::zap::VideoCodec getNinjamZapVideoCodec(unsigned int fourcc)
    {
        if (fourcc == kNinjamZapVideoMjpgFourcc) return ninjamplus::zap::VideoCodec::mjpeg;
        if (fourcc == kNinjamZapVideoH264Fourcc) return ninjamplus::zap::VideoCodec::h264;
        if (fourcc == kNinjamZapVideoVp8Fourcc)  return ninjamplus::zap::VideoCodec::vp8;
        if (fourcc == kNinjamZapVideoVp9Fourcc)  return ninjamplus::zap::VideoCodec::vp9;
        return ninjamplus::zap::VideoCodec::unknown;
    }

    ninjamplus::zap::VideoCodec parseZapBrowserCodec(const juce::String& codecName)
    {
        const juce::String codec = codecName.trim().toLowerCase();
        if (codec == "h264" || codec == "264" || codec == "avc")
            return ninjamplus::zap::VideoCodec::h264;
        if (codec == "vp8")
            return ninjamplus::zap::VideoCodec::vp8;
        if (codec == "vp9")
            return ninjamplus::zap::VideoCodec::vp9;
        return ninjamplus::zap::VideoCodec::mjpeg;
    }

    juce::String zapBrowserCodecName(ninjamplus::zap::VideoCodec codec)
    {
        if (codec == ninjamplus::zap::VideoCodec::h264)
            return "h264";
        if (codec == ninjamplus::zap::VideoCodec::vp8)
            return "vp8";
        if (codec == ninjamplus::zap::VideoCodec::vp9)
            return "vp9";
        return "mjpeg";
    }

    bool isNinjamZapH264ConfigChunk(const juce::MemoryBlock& payload)
    {
        const auto* bytes = static_cast<const unsigned char*>(payload.getData());
        const size_t size = payload.getSize();
        if (bytes == nullptr || size < 8)
            return false;

        const size_t spsLen = ((size_t)bytes[0] << 8) | (size_t)bytes[1];
        if (spsLen == 0 || 2 + spsLen + 2 > size)
            return false;

        const size_t ppsOffset = 2 + spsLen;
        const size_t ppsLen = ((size_t)bytes[ppsOffset] << 8) | (size_t)bytes[ppsOffset + 1];
        if (ppsLen == 0 || ppsOffset + 2 + ppsLen != size)
            return false;

        return ((bytes[2] & 0x1f) == 7) && ((bytes[ppsOffset + 2] & 0x1f) == 8);
    }

    bool makeNinjamZapH264ConfigChunk(const unsigned char* sps,
                                      size_t spsSize,
                                      const unsigned char* pps,
                                      size_t ppsSize,
                                      juce::MemoryBlock& configInner)
    {
        if (sps == nullptr || pps == nullptr || spsSize == 0 || ppsSize == 0
            || spsSize > 0xffff || ppsSize > 0xffff)
            return false;

        configInner.reset();
        const unsigned char spsLen[2]
        {
            static_cast<unsigned char>((spsSize >> 8) & 0xff),
            static_cast<unsigned char>(spsSize & 0xff)
        };
        const unsigned char ppsLen[2]
        {
            static_cast<unsigned char>((ppsSize >> 8) & 0xff),
            static_cast<unsigned char>(ppsSize & 0xff)
        };
        configInner.append(spsLen, sizeof(spsLen));
        configInner.append(sps, spsSize);
        configInner.append(ppsLen, sizeof(ppsLen));
        configInner.append(pps, ppsSize);
        return isNinjamZapH264ConfigChunk(configInner);
    }

    bool extractNinjamZapH264ConfigFromAvcDecoderConfig(const juce::MemoryBlock& payload,
                                                        juce::MemoryBlock& configInner)
    {
        const auto* bytes = static_cast<const unsigned char*>(payload.getData());
        const size_t size = payload.getSize();
        if (bytes == nullptr || size < 7 || bytes[0] != 1)
            return false;

        size_t offset = 5;
        const int spsCount = bytes[offset++] & 0x1f;
        const unsigned char* sps = nullptr;
        const unsigned char* pps = nullptr;
        size_t spsSize = 0;
        size_t ppsSize = 0;

        for (int i = 0; i < spsCount; ++i)
        {
            if (offset + 2 > size)
                return false;
            const size_t nalSize = ((size_t)bytes[offset] << 8) | (size_t)bytes[offset + 1];
            offset += 2;
            if (nalSize == 0 || offset + nalSize > size)
                return false;
            if (sps == nullptr && ((bytes[offset] & 0x1f) == 7))
            {
                sps = bytes + offset;
                spsSize = nalSize;
            }
            offset += nalSize;
        }

        if (offset >= size)
            return false;

        const int ppsCount = bytes[offset++];
        for (int i = 0; i < ppsCount; ++i)
        {
            if (offset + 2 > size)
                return false;
            const size_t nalSize = ((size_t)bytes[offset] << 8) | (size_t)bytes[offset + 1];
            offset += 2;
            if (nalSize == 0 || offset + nalSize > size)
                return false;
            if (pps == nullptr && ((bytes[offset] & 0x1f) == 8))
            {
                pps = bytes + offset;
                ppsSize = nalSize;
            }
            offset += nalSize;
        }

        return makeNinjamZapH264ConfigChunk(sps, spsSize, pps, ppsSize, configInner);
    }

    bool extractNinjamZapH264ConfigFromLength16Nals(const juce::MemoryBlock& payload,
                                                    juce::MemoryBlock& configInner)
    {
        const auto* bytes = static_cast<const unsigned char*>(payload.getData());
        const size_t size = payload.getSize();
        if (bytes == nullptr || size < 8)
            return false;

        const unsigned char* sps = nullptr;
        const unsigned char* pps = nullptr;
        size_t spsSize = 0;
        size_t ppsSize = 0;
        size_t offset = 0;

        while (offset + 2 <= size)
        {
            const size_t nalSize = ((size_t)bytes[offset] << 8) | (size_t)bytes[offset + 1];
            offset += 2;
            if (nalSize == 0 || offset + nalSize > size)
                break;

            const unsigned char nalType = bytes[offset] & 0x1f;
            if (nalType == 7 && sps == nullptr)
            {
                sps = bytes + offset;
                spsSize = nalSize;
            }
            else if (nalType == 8 && pps == nullptr)
            {
                pps = bytes + offset;
                ppsSize = nalSize;
            }

            offset += nalSize;
        }

        return makeNinjamZapH264ConfigChunk(sps, spsSize, pps, ppsSize, configInner);
    }

    bool extractNinjamZapH264ConfigFromAvccFrame(const juce::MemoryBlock& payload, juce::MemoryBlock& configInner)
    {
        const auto* bytes = static_cast<const unsigned char*>(payload.getData());
        const size_t size = payload.getSize();
        if (bytes == nullptr || size < 12)
            return false;

        const unsigned char* sps = nullptr;
        const unsigned char* pps = nullptr;
        size_t spsSize = 0;
        size_t ppsSize = 0;
        size_t offset = 0;
        while (offset + 4 <= size)
        {
            const size_t nalSize = ((size_t)bytes[offset] << 24)
                                 | ((size_t)bytes[offset + 1] << 16)
                                 | ((size_t)bytes[offset + 2] << 8)
                                 | (size_t)bytes[offset + 3];
            offset += 4;
            if (nalSize == 0 || offset + nalSize > size)
                break;

            const unsigned char nalType = bytes[offset] & 0x1f;
            if (nalType == 7)
            {
                sps = bytes + offset;
                spsSize = nalSize;
            }
            else if (nalType == 8)
            {
                pps = bytes + offset;
                ppsSize = nalSize;
            }

            offset += nalSize;
        }

        return makeNinjamZapH264ConfigChunk(sps, spsSize, pps, ppsSize, configInner);
    }

    bool normaliseNinjamZapH264ConfigPayload(const juce::MemoryBlock& payload,
                                             juce::MemoryBlock& configInner)
    {
        if (isNinjamZapH264ConfigChunk(payload))
        {
            configInner.reset();
            configInner.append(payload.getData(), payload.getSize());
            return true;
        }

        if (extractNinjamZapH264ConfigFromLength16Nals(payload, configInner))
            return true;

        if (extractNinjamZapH264ConfigFromAvcDecoderConfig(payload, configInner))
            return true;

        if (extractNinjamZapH264ConfigFromAvccFrame(payload, configInner))
            return true;

        configInner.reset();
        return false;
    }

    bool h264AvccFrameContainsIdr(const juce::MemoryBlock& payload)
    {
        const auto* bytes = static_cast<const unsigned char*>(payload.getData());
        const size_t size = payload.getSize();
        if (bytes == nullptr || size < 5)
            return false;

        size_t offset = 0;
        while (offset + 4 <= size)
        {
            const size_t nalSize = ((size_t)bytes[offset] << 24)
                                 | ((size_t)bytes[offset + 1] << 16)
                                 | ((size_t)bytes[offset + 2] << 8)
                                 | (size_t)bytes[offset + 3];
            offset += 4;
            if (nalSize == 0 || offset + nalSize > size)
                break;

            if ((bytes[offset] & 0x1f) == 5)
                return true;

            offset += nalSize;
        }

        return false;
    }

    juce::String guidToHexString(const unsigned char* guid)
    {
        if (guid == nullptr)
            return {};

        static constexpr char hex[] = "0123456789abcdef";
        char text[33] {};
        for (int i = 0; i < 16; ++i)
        {
            text[i * 2] = hex[(guid[i] >> 4) & 0x0f];
            text[i * 2 + 1] = hex[guid[i] & 0x0f];
        }
        return juce::String::fromUTF8(text);
    }

    juce::String makeShortUserName(juce::String fullName)
    {
        fullName = fullName.trim();
        const int atPos = fullName.indexOfChar('@');
        if (atPos > 0)
            fullName = fullName.substring(0, atPos);
        return fullName;
    }

    int getIntervalBeatIndexForPosition(int pos, int length, int bpi)
    {
        const int safeBpi = juce::jmax(1, bpi);
        if (length <= 0)
            return 0;

        const int safePos = juce::jlimit(0, length - 1, pos);
        const double progress = (double)safePos / (double)length;
        return juce::jlimit(0, safeBpi - 1, (int)std::floor(progress * (double)safeBpi));
    }

    int getIntervalSyncMarkerBeatForBeat(int, int)
    {
        return 0;
    }

    bool isIntervalSyncMarkerBeat(int beatIndex, int bpi)
    {
        const int safeBpi = juce::jmax(1, bpi);
        if (beatIndex < 0 || beatIndex >= safeBpi)
            return false;

        return beatIndex == 0;
    }

    long long makeIntervalSyncMarkerKey(int interval, int markerBeat)
    {
        return ((long long)juce::jmax(0, interval) * intervalSyncMarkerKeyBeatStride)
             + (long long)juce::jlimit(0, (int)intervalSyncMarkerKeyBeatStride - 1, markerBeat);
    }

    juce::String formatIntervalSyncMarkerBeat(int markerBeat)
    {
        return "BPI" + juce::String(juce::jmax(0, markerBeat) + 1);
    }

    juce::String normaliseOpusPeerId(juce::String userId)
    {
        userId = userId.trim();
        const int atPos = userId.indexOfChar('@');
        if (atPos > 0)
            userId = userId.substring(0, atPos);
        if (userId.startsWithIgnoreCase("anonymous:"))
            userId = userId.substring(10);
        if (userId.startsWithIgnoreCase("guest:"))
            userId = userId.substring(6);
        return userId.toLowerCase();
    }

    juce::String normaliseChatTargetNick(juce::String userId)
    {
        userId = userId.trim();
        const int atPos = userId.indexOfChar('@');
        if (atPos > 0)
            userId = userId.substring(0, atPos);
        const int colonPos = userId.lastIndexOfChar(':');
        if (colonPos >= 0 && colonPos < userId.length() - 1)
            userId = userId.substring(colonPos + 1);
        return userId.trim();
    }

    bool isKnownNinjamServerBotUser(juce::String userId)
    {
        const juce::String key = normaliseOpusPeerId(userId);
        juce::String compact;
        for (auto ch : key)
        {
            if (juce::CharacterFunctions::isLetterOrDigit(ch))
                compact << juce::String::charToString((juce_wchar) juce::CharacterFunctions::toLowerCase(ch));
        }

        if (compact.isEmpty())
            return false;

        static const char* const knownBotPrefixes[] = {
            "ninbot", "jambot", "ninjambot", "jamulusbot",
            "jamserverbot", "ninjamserverbot", "serverbot"
        };

        for (const auto* prefix : knownBotPrefixes)
        {
            const juce::String botPrefix(prefix);
            if (compact == botPrefix || compact.startsWith(botPrefix))
                return true;
        }

        return false;
    }

    bool isHttpOrHttpsChatUrl(juce::String url)
    {
        url = url.trim().toLowerCase();
        return url.startsWith("http://") || url.startsWith("https://");
    }

    bool isEmojiLikeCodepoint(juce::uint32 codepoint)
    {
        return (codepoint >= 0x1f000 && codepoint <= 0x1faff)
            || (codepoint >= 0x2600 && codepoint <= 0x27bf)
            || (codepoint >= 0x1f3fb && codepoint <= 0x1f3ff)
            || (codepoint >= 0xfe00 && codepoint <= 0xfe0f)
            || codepoint == 0x200d
            || codepoint == 0x20e3;
    }

    bool isLikelyChatMediaUrl(juce::String text)
    {
        text = text.trim().toLowerCase();
        if (!isHttpOrHttpsChatUrl(text))
            return false;

        return text.contains("giphy.com")
            || text.contains(".gif")
            || text.contains(".webp")
            || text.contains(".png")
            || text.contains(".jpg")
            || text.contains(".jpeg");
    }

    bool isRichChatAttachmentLine(const juce::String& text)
    {
        return text.contains(" shared a GIF: ")
            || text.contains(" shared a image: ")
            || text.contains(" shared a GIF")
            || text.contains(" shared a Image");
    }

    bool containsLikelyTranslatableText(const juce::String& text)
    {
        for (auto c : text)
        {
            const auto codepoint = (juce::uint32)c;
            if (isEmojiLikeCodepoint(codepoint) || juce::CharacterFunctions::isWhitespace(c))
                continue;

            if (juce::CharacterFunctions::isLetterOrDigit(c))
                return true;
        }

        return false;
    }

    bool shouldSkipAutoChatTranslation(const juce::String& originalLine, const juce::String& lineBody)
    {
        const auto trimmedBody = lineBody.trim();
        return trimmedBody.isEmpty()
            || isRichChatAttachmentLine(originalLine)
            || isRichChatAttachmentLine(trimmedBody)
            || isLikelyChatMediaUrl(trimmedBody)
            || !containsLikelyTranslatableText(trimmedBody);
    }

    juce::String normaliseRichChatKind(juce::String kind)
    {
        kind = kind.trim().toLowerCase();
        if (kind == "gif" || kind == "image")
            return kind;
        return "link";
    }

    juce::String normaliseChatColourKey(juce::String key)
    {
        key = key.trim().toLowerCase().removeCharacters(" _-");

        static constexpr const char* validKeys[] = {
            "aurora", "ocean", "sunset", "candy", "lime", "fire", "violet", "mono",
            "ruby", "copper", "lemon", "emerald", "cyan", "sapphire", "plum", "pearl"
        };

        for (const auto* validKey : validKeys)
            if (key == validKey)
                return key;

        return "aurora";
    }

    juce::String richChatKindLabel(const juce::String& kind)
    {
        const juce::String normalised = normaliseRichChatKind(kind);
        if (normalised == "gif")
            return "GIF";
        if (normalised == "image")
            return "image";
        return "link";
    }

    juce::String makeRichChatLine(const juce::String& senderLabel, const juce::String& kind, const juce::String& url)
    {
        return senderLabel + " shared a " + richChatKindLabel(kind) + ": " + url;
    }

    void trimChatArrays(juce::StringArray& history, juce::StringArray& senders)
    {
        if (history.size() > 100)
        {
            history.removeRange(0, history.size() - 100);
            senders.removeRange(0, juce::jmax(0, senders.size() - 100));
        }
    }

    bool tryParseServerEndpoint(juce::String serverText, juce::String& hostOut, int& portOut)
    {
        serverText = serverText.trim();
        if (serverText.isEmpty())
            return false;

        const int schemePos = serverText.indexOf("://");
        if (schemePos >= 0)
            serverText = serverText.substring(schemePos + 3);

        const int slashPos = serverText.indexOfChar('/');
        if (slashPos >= 0)
            serverText = serverText.substring(0, slashPos);

        const int atPos = serverText.lastIndexOfChar('@');
        if (atPos >= 0 && atPos + 1 < serverText.length())
            serverText = serverText.substring(atPos + 1);

        serverText = serverText.trim();
        if (serverText.isEmpty())
            return false;

        hostOut = serverText;
        portOut = 2049;

        if (serverText.startsWithChar('['))
        {
            const int closingBracket = serverText.indexOfChar(']');
            if (closingBracket <= 1)
                return false;

            hostOut = serverText.substring(1, closingBracket).trim();
            if (closingBracket + 1 < serverText.length() && serverText[closingBracket + 1] == ':')
            {
                const juce::String portText = serverText.substring(closingBracket + 2).trim();
                if (portText.isNotEmpty())
                    portOut = juce::jlimit(1, 65535, portText.getIntValue());
            }

            return hostOut.isNotEmpty();
        }

        const int lastColonPos = serverText.lastIndexOfChar(':');
        if (lastColonPos > 0 && serverText.indexOfChar(':') == lastColonPos)
        {
            const juce::String candidatePort = serverText.substring(lastColonPos + 1).trim();
            bool allDigits = candidatePort.isNotEmpty();
            for (int i = 0; i < candidatePort.length() && allDigits; ++i)
                allDigits = juce::CharacterFunctions::isDigit(candidatePort[i]);

            if (allDigits)
            {
                hostOut = serverText.substring(0, lastColonPos).trim();
                portOut = juce::jlimit(1, 65535, candidatePort.getIntValue());
            }
        }

        return hostOut.isNotEmpty();
    }

    juce::String canonicalDelayUserKey(juce::String userId)
    {
        userId = normaliseOpusPeerId(userId);
        if (userId.startsWith("anonymous:"))
            userId = userId.substring(10);
        userId = userId.trim().toLowerCase();
        return userId;
    }

    juce::String getWrapperTypeName(juce::AudioProcessor::WrapperType wrapperType)
    {
        using WrapperType = juce::AudioProcessor::WrapperType;
        switch (wrapperType)
        {
            case WrapperType::wrapperType_Standalone: return "standalone";
            case WrapperType::wrapperType_VST: return "vst";
            case WrapperType::wrapperType_VST3: return "vst3";
            case WrapperType::wrapperType_AudioUnit: return "au";
            case WrapperType::wrapperType_AudioUnitv3: return "auv3";
            case WrapperType::wrapperType_AAX: return "aax";
            case WrapperType::wrapperType_LV2: return "lv2";
            default: break;
        }
        return "unknown";
    }

    inline float softClipSample(float x)
    {
        const float k = 2.0f;
        const float d = std::tanh(k);
        const float c = d / k;
        const float target = 0.891251f;

        float y = std::tanh(k * c * x);
        if (d != 0.0f)
            y = (y / d) * target;
        return y;
    }

    inline juce::String buildDefaultLocalChannelName(int channelIndex)
    {
        return "Ch" + juce::String(channelIndex + 1);
    }

    inline bool isDefaultLocalChannelName(const juce::String& name)
    {
        auto trimmed = name.trim();
        if (!trimmed.startsWithIgnoreCase("ch"))
            return false;

        auto numberPart = trimmed.substring(2).trim();
        if (numberPart.isEmpty() || !numberPart.containsOnly("0123456789"))
            return false;

        return numberPart.getIntValue() > 0;
    }

    inline bool isValidSamplePadIndex(int padIndex)
    {
        return padIndex >= 0 && padIndex < NinjamVst3AudioProcessor::numSamplePads;
    }

    inline bool isValidSamplePadFxSlot(int slotIndex)
    {
        return slotIndex >= 0 && slotIndex < NinjamVst3AudioProcessor::numSamplePadFxSlots;
    }

    static NinjamVst3AudioProcessor::SamplePadFxType sanitizeSamplePadFxType(int type)
    {
        using FxType = NinjamVst3AudioProcessor::SamplePadFxType;
        switch ((FxType)type)
        {
            case FxType::reverb:
            case FxType::delay:
            case FxType::djFilter:
            case FxType::djFilterHp:
            case FxType::djFilterLp:
            case FxType::djFilterBp:
            case FxType::phaser:
            case FxType::delayQuarter:
            case FxType::delayQuarterPingPong:
            case FxType::phaserHalf:
                return (FxType)type;
        }
        return FxType::reverb;
    }

    static NinjamVst3AudioProcessor::SamplePadDuckShape sanitizeSamplePadDuckShape(int shape)
    {
        using DuckShape = NinjamVst3AudioProcessor::SamplePadDuckShape;
        switch ((DuckShape)shape)
        {
            case DuckShape::smoothPump:
            case DuckShape::tightPump:
            case DuckShape::slowPump:
            case DuckShape::hardGate:
            case DuckShape::reverseSwell:
            case DuckShape::notchPulse:
                return (DuckShape)shape;
        }
        return DuckShape::smoothPump;
    }

    static NinjamVst3AudioProcessor::SamplePadDuckLength sanitizeSamplePadDuckLength(int length)
    {
        using DuckLength = NinjamVst3AudioProcessor::SamplePadDuckLength;
        switch ((DuckLength)length)
        {
            case DuckLength::eighth:
            case DuckLength::quarter:
            case DuckLength::half:
                return (DuckLength)length;
        }
        return DuckLength::quarter;
    }

    static NinjamVst3AudioProcessor::SamplePadPlaybackSpeed sanitizeSamplePadPlaybackSpeed(int speed)
    {
        using PlaybackSpeed = NinjamVst3AudioProcessor::SamplePadPlaybackSpeed;
        switch ((PlaybackSpeed)speed)
        {
            case PlaybackSpeed::half:
            case PlaybackSpeed::normal:
            case PlaybackSpeed::doubleSpeed:
                return (PlaybackSpeed)speed;
        }
        return PlaybackSpeed::normal;
    }

    static double samplePadPlaybackSpeedMultiplier(NinjamVst3AudioProcessor::SamplePadPlaybackSpeed speed)
    {
        using PlaybackSpeed = NinjamVst3AudioProcessor::SamplePadPlaybackSpeed;
        switch (speed)
        {
            case PlaybackSpeed::half:        return 0.5;
            case PlaybackSpeed::doubleSpeed: return 2.0;
            case PlaybackSpeed::normal:
            default:                         return 1.0;
        }
    }

    static NinjamVst3AudioProcessor::SamplePadFxType getDefaultSamplePadFxType(int slotIndex)
    {
        using FxType = NinjamVst3AudioProcessor::SamplePadFxType;
        static constexpr FxType defaultFxTypes[NinjamVst3AudioProcessor::numSamplePadFxSlots] =
        {
            FxType::reverb,
            FxType::delay,
            FxType::djFilter,
            FxType::djFilterHp,
            FxType::djFilterLp,
            FxType::djFilterBp,
            FxType::phaser,
            FxType::delay
        };

        return defaultFxTypes[(size_t)juce::jlimit(0,
                                                   NinjamVst3AudioProcessor::numSamplePadFxSlots - 1,
                                                   slotIndex)];
    }

    static double mapNormalisedToLogFrequency(double normalised, double minHz, double maxHz)
    {
        normalised = juce::jlimit(0.0, 1.0, normalised);
        minHz = juce::jmax(1.0, minHz);
        maxHz = juce::jmax(minHz + 1.0, maxHz);
        return minHz * std::pow(maxHz / minHz, normalised);
    }

    static double smoothDuckStep(double x)
    {
        x = juce::jlimit(0.0, 1.0, x);
        return x * x * (3.0 - 2.0 * x);
    }

    static double getSamplePadDuckLengthBeats(NinjamVst3AudioProcessor::SamplePadDuckLength length)
    {
        using DuckLength = NinjamVst3AudioProcessor::SamplePadDuckLength;
        switch (length)
        {
            case DuckLength::eighth:  return 0.5;
            case DuckLength::half:    return 2.0;
            case DuckLength::quarter:
            default:                  return 1.0;
        }
    }

    // Compute a target duck gain envelope value for a given beat phase.
    // This returns the *target* gain (0=ducked, 1=full volume) at this phase position.
    // The actual gain is smoothed per-sample in the render loop to avoid artifacts.
    static float getSamplePadDuckTargetForBeat(double beat,
                                               NinjamVst3AudioProcessor::SamplePadDuckShape shape,
                                               NinjamVst3AudioProcessor::SamplePadDuckLength length)
    {
        const double cycleBeats = getSamplePadDuckLengthBeats(length);
        double phase = beat / cycleBeats;
        phase = std::fmod(phase, 1.0);
        if (phase < 0.0)
            phase += 1.0;

        constexpr float duckAmount = 0.88f;
        double shaped = 1.0;

        using DuckShape = NinjamVst3AudioProcessor::SamplePadDuckShape;
        switch (shape)
        {
            case DuckShape::tightPump:
                // Quick dip then recover: dip at start, back to full by 30% of cycle
                shaped = phase < 0.30 ? smoothDuckStep(phase / 0.30) : 1.0;
                break;

            case DuckShape::slowPump:
                shaped = phase < 0.86 ? smoothDuckStep(phase / 0.86) : 1.0;
                break;

            case DuckShape::hardGate:
                // Hard cut then fast recovery
                shaped = phase < 0.38 ? 0.03 : phase < 0.47 ? smoothDuckStep((phase - 0.38) / 0.09) : 1.0;
                break;

            case DuckShape::reverseSwell:
                // Start full, swell down then back
                shaped = phase < 0.16 ? 1.0 : 1.0 - smoothDuckStep((phase - 0.16) / 0.84);
                break;

            case DuckShape::notchPulse:
            {
                constexpr double centre = 0.55;
                constexpr double width = 0.22;
                shaped = smoothDuckStep(juce::jlimit(0.0, 1.0, std::abs(phase - centre) / width));
                break;
            }

            case DuckShape::smoothPump:
            default:
                // Classic sidechain pump: dip at beat start, smooth recovery
                shaped = phase < 0.62 ? smoothDuckStep(phase / 0.62) : 1.0;
                break;
        }

        return 1.0f - duckAmount * (1.0f - (float)shaped);
    }

    inline juce::String getDefaultSamplePadName(int padIndex)
    {
        return "Pad " + juce::String(padIndex + 1);
    }

    static juce::PropertiesFile::Options makeNinjamplusSettingsOptions()
    {
        juce::PropertiesFile::Options options;
        options.applicationName = JucePlugin_Name;
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support";
#if JUCE_WINDOWS
        options.folderName = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                 .getChildFile("NINJAMplus")
                                 .getFullPathName();
#elif JUCE_LINUX || JUCE_BSD
        options.folderName = "~/.config";
#else
        options.folderName = JucePlugin_Name;
#endif
        return options;
    }

    static juce::File getNinjamplusSettingsDirectory()
    {
        return makeNinjamplusSettingsOptions().getDefaultFile().getParentDirectory();
    }

    constexpr int serverLicenseResultDisagree = 0;
    constexpr int serverLicenseResultAgree = 1;
    constexpr int serverLicenseResultAgreeAlways = 2;

    static juce::uint64 getStableServerLicenseHash(const juce::String& text)
    {
        juce::uint64 hash = 14695981039346656037ull;
        const auto* bytes = reinterpret_cast<const unsigned char*>(text.toRawUTF8());

        while (*bytes != 0)
        {
            hash ^= (juce::uint64)*bytes++;
            hash *= 1099511628211ull;
        }

        return hash;
    }

    static juce::String toFixedLowercaseHex(juce::uint64 value)
    {
        static constexpr char hexDigits[] = "0123456789abcdef";
        char chars[17] {};

        for (int i = 15; i >= 0; --i)
        {
            chars[i] = hexDigits[value & 0x0f];
            value >>= 4;
        }

        return juce::String(chars);
    }

    static juce::String normaliseServerLicenseServerKey(juce::String server)
    {
        server = server.trim().toLowerCase();
        return server.isNotEmpty() ? server : juce::String("unknown-server");
    }

    static juce::String makeServerLicenseAlwaysSettingsKey(const juce::String& serverKey,
                                                           const juce::String& licenseText)
    {
        return "serverLicenseAlways."
             + toFixedLowercaseHex(getStableServerLicenseHash(serverKey + "\n" + licenseText));
    }

    static juce::Component* findLicenseDialogAnchorComponent()
    {
        if (auto* focused = juce::Component::getCurrentlyFocusedComponent())
            if (auto* topLevel = focused->getTopLevelComponent())
                return topLevel;

        auto& desktop = juce::Desktop::getInstance();
        for (int i = desktop.getNumComponents(); --i >= 0;)
            if (auto* component = desktop.getComponent(i))
                if (component->isShowing())
                    return component;

        return nullptr;
    }

    class ServerLicenseAgreementComponent final : public juce::Component
    {
    public:
        ServerLicenseAgreementComponent(const juce::String& serverName,
                                        const juce::String& licenseText)
        {
            setSize(560, 430);

            const juce::String target = serverName.trim().isNotEmpty() ? serverName.trim()
                                                                       : juce::String("this server");
            introLabel.setText("The server has requested that you agree to these terms before connecting to " + target + ".",
                               juce::dontSendNotification);
            introLabel.setJustificationType(juce::Justification::centredLeft);
            introLabel.setColour(juce::Label::textColourId, juce::Colours::white);
            addAndMakeVisible(introLabel);

            termsBox.setMultiLine(true);
            termsBox.setReadOnly(true);
            termsBox.setScrollbarsShown(true);
            termsBox.setCaretVisible(false);
            termsBox.setText(licenseText.trim().isNotEmpty() ? licenseText.trim()
                                                             : juce::String("No license text was provided by the server."));
            termsBox.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff11161c));
            termsBox.setColour(juce::TextEditor::textColourId, juce::Colours::white);
            termsBox.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff3f4d5b));
            termsBox.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xff5b7fa0));
            addAndMakeVisible(termsBox);

            configureButton(disagreeButton, serverLicenseResultDisagree, juce::Colour(0xff5c2525));
            configureButton(agreeAlwaysButton, serverLicenseResultAgreeAlways, juce::Colour(0xff394f6d));
            configureButton(agreeButton, serverLicenseResultAgree, juce::Colour(0xff24623b));
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colour(0xff20262e));
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(16);
            introLabel.setBounds(area.removeFromTop(44));
            area.removeFromTop(10);

            auto buttons = area.removeFromBottom(34);
            area.removeFromBottom(12);
            termsBox.setBounds(area);

            disagreeButton.setBounds(buttons.removeFromLeft(96));
            agreeButton.setBounds(buttons.removeFromRight(86));
            buttons.removeFromRight(8);
            agreeAlwaysButton.setBounds(buttons.removeFromRight(124));
        }

    private:
        void configureButton(juce::TextButton& button, int result, juce::Colour colour)
        {
            button.setColour(juce::TextButton::buttonColourId, colour);
            button.setColour(juce::TextButton::buttonOnColourId, colour.brighter(0.15f));
            button.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
            button.onClick = [this, result]
            {
                if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
                    dialog->exitModalState(result);
            };
            addAndMakeVisible(button);
        }

        juce::Label introLabel;
        juce::TextEditor termsBox;
        juce::TextButton disagreeButton { "Disagree" };
        juce::TextButton agreeAlwaysButton { "Agree Always" };
        juce::TextButton agreeButton { "Agree" };
    };

    class ServerLicenseAlwaysConfirmComponent final : public juce::Component
    {
    public:
        ServerLicenseAlwaysConfirmComponent()
        {
            setSize(500, 170);

            messageLabel.setText("By Agreeing Always to this Server/Room's terms of use you agree every time you re-enter you will adhere to the terms of the server.",
                                 juce::dontSendNotification);
            messageLabel.setJustificationType(juce::Justification::centredLeft);
            messageLabel.setColour(juce::Label::textColourId, juce::Colours::white);
            addAndMakeVisible(messageLabel);

            disagreeButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff5c2525));
            disagreeButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
            disagreeButton.onClick = [this] { close(serverLicenseResultDisagree); };
            addAndMakeVisible(disagreeButton);

            agreeButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff24623b));
            agreeButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
            agreeButton.onClick = [this] { close(serverLicenseResultAgree); };
            addAndMakeVisible(agreeButton);
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colour(0xff20262e));
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(16);
            auto buttons = area.removeFromBottom(34);
            area.removeFromBottom(12);
            messageLabel.setBounds(area);

            agreeButton.setBounds(buttons.removeFromRight(86));
            buttons.removeFromRight(8);
            disagreeButton.setBounds(buttons.removeFromRight(96));
        }

    private:
        void close(int result)
        {
            if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
                dialog->exitModalState(result);
        }

        juce::Label messageLabel;
        juce::TextButton disagreeButton { "Disagree" };
        juce::TextButton agreeButton { "Agree" };
    };

    static juce::DialogWindow* launchServerLicenseAgreementDialog(const juce::String& serverName,
                                                                  const juce::String& licenseText,
                                                                  std::function<void(int)> completion)
    {
        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned(new ServerLicenseAgreementComponent(serverName, licenseText));
        options.dialogTitle = "Server License Agreement";
        options.dialogBackgroundColour = juce::Colour(0xff20262e);
        options.escapeKeyTriggersCloseButton = false;
        options.useNativeTitleBar = false;
        options.resizable = false;
        options.componentToCentreAround = findLicenseDialogAnchorComponent();

        auto* dialog = options.create();
        dialog->enterModalState(true,
                                juce::ModalCallbackFunction::create([completion = std::move(completion)](int result) mutable
                                {
                                    if (completion != nullptr)
                                        completion(result);
                                }),
                                true);
        return dialog;
    }

    static juce::DialogWindow* launchServerLicenseAlwaysConfirmDialog(std::function<void(int)> completion)
    {
        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned(new ServerLicenseAlwaysConfirmComponent());
        options.dialogTitle = "Agree Always";
        options.dialogBackgroundColour = juce::Colour(0xff20262e);
        options.escapeKeyTriggersCloseButton = false;
        options.useNativeTitleBar = false;
        options.resizable = false;
        options.componentToCentreAround = findLicenseDialogAnchorComponent();

        auto* dialog = options.create();
        dialog->enterModalState(true,
                                juce::ModalCallbackFunction::create([completion = std::move(completion)](int result) mutable
                                {
                                    if (completion != nullptr)
                                        completion(result);
                                }),
                                true);
        return dialog;
    }

    static juce::String sanitiseSamplePadBankName(const juce::String& name)
    {
        juce::String safe = name.trim();
        safe = safe.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-");
        safe = safe.trim();
        while (safe.contains("  "))
            safe = safe.replace("  ", " ");
        return safe.substring(0, 48);
    }

    static bool writeSamplePadWavFile(const juce::File& file,
                                      const juce::AudioBuffer<float>& buffer,
                                      double sampleRate)
    {
        if (buffer.getNumSamples() <= 0)
            return false;

        file.getParentDirectory().createDirectory();
        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
        if (stream == nullptr || stream->failedToOpen())
            return false;

        std::unique_ptr<juce::AudioFormatWriter> writer(
            wavFormat.createWriterFor(stream.get(),
                                      juce::jmax(1.0, sampleRate),
                                      (unsigned int)juce::jmax(1, buffer.getNumChannels()),
                                      24,
                                      {},
                                      0));
        if (writer == nullptr)
            return false;

        stream.release();
        return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
    }

    inline double positiveModulo(double value, double divisor)
    {
        if (divisor <= 0.0)
            return 0.0;

        double result = std::fmod(value, divisor);
        if (result < 0.0)
            result += divisor;
        return result;
    }

    static constexpr double samplePadRecordBarBeats = 4.0;
    static constexpr double samplePadHoldScheduleMs = 2000.0;

    static int quantiseSamplePadFreeLoopBeats(double capturedBeats)
    {
        constexpr int beatsPerBar = (int)samplePadRecordBarBeats;
        constexpr double nextBarCommitBeat = 1.0;

        if (!std::isfinite(capturedBeats) || capturedBeats <= 0.0)
            return beatsPerBar;

        const int completedBars = juce::jmax(0, (int)std::floor(capturedBeats / (double)beatsPerBar));
        const double beatsIntoNextBar = capturedBeats - (double)completedBars * (double)beatsPerBar;
        int bars = juce::jmax(1, completedBars);

        // Give short overruns after the bar line a one-beat grace window, then commit to the next bar.
        if (completedBars > 0 && beatsIntoNextBar >= nextBarCommitBeat)
            bars = completedBars + 1;

        return juce::jmax(beatsPerBar, bars * beatsPerBar);
    }

    static double nextSamplePadGridBeat(double currentBeat, int bpi)
    {
        const double safeBpi = (double)juce::jmax(1, bpi);
        const double intervalStart = std::floor(currentBeat / safeBpi) * safeBpi;
        const double beatInInterval = juce::jlimit(0.0, safeBpi, currentBeat - intervalStart);
        const double nextGridOffset = std::floor(beatInInterval / samplePadRecordBarBeats) * samplePadRecordBarBeats + samplePadRecordBarBeats;
        return intervalStart + (nextGridOffset < safeBpi ? nextGridOffset : safeBpi);
    }

    static double nextSamplePadIntervalStartBeat(double currentBeat, int bpi)
    {
        const double safeBpi = (double)juce::jmax(1, bpi);
        double candidate = std::floor(currentBeat / safeBpi) * safeBpi + safeBpi;
        if (candidate <= currentBeat + 0.0001)
            candidate += safeBpi;
        return candidate;
    }


    static constexpr double samplePadPressDebounceMs = 120.0;

    static int samplePadIndexForMidiNoteNumber(int noteNumber)
    {
        if (noteNumber >= 36 && noteNumber < 36 + NinjamVst3AudioProcessor::numSamplePads)
            return noteNumber - 36;
        if (noteNumber >= 0 && noteNumber < NinjamVst3AudioProcessor::numSamplePads)
            return noteNumber;
        return -1;
    }

    static double normaliseDetectedTempoBpm(double bpm)
    {
        if (!std::isfinite(bpm) || bpm <= 1.0)
            return 0.0;

        constexpr double minUsefulBpm = 70.0;
        constexpr double maxUsefulBpm = 170.0;

        while (bpm < minUsefulBpm)
            bpm *= 2.0;
        while (bpm > maxUsefulBpm)
            bpm *= 0.5;

        return (bpm >= minUsefulBpm && bpm <= maxUsefulBpm) ? bpm : 0.0;
    }

    // Given a raw detected BPM and the sample duration in seconds, try the BPM
    // at half, normal, and double speed. Pick the one where the resulting loop
    // length in beats is closest to a whole number. This fixes octave errors
    // where e.g. 80 BPM is detected as 161.5.
    // Returns the corrected raw BPM and the corresponding loop length in beats.
    static void correctTempoOctave(double rawBpm, double durationSeconds,
                                   double& outBpm, int& outLoopBeats)
    {
        outBpm = rawBpm;
        outLoopBeats = 0;

        if (rawBpm <= 1.0 || durationSeconds <= 0.0 || !std::isfinite(rawBpm))
            return;

        struct Candidate
        {
            double bpm;
            double beats;
            double fractionalDist;  // distance to nearest integer
            int roundedBeats;
        };

        std::array<Candidate, 3> candidates;
        const double multipliers[3] = { 0.5, 1.0, 2.0 };

        for (int i = 0; i < 3; ++i)
        {
            const double candidateBpm = rawBpm * multipliers[i];
            if (candidateBpm <= 1.0)
            {
                candidates[i] = { candidateBpm, 0.0, 1.0, 0 };
                continue;
            }
            const double beats = durationSeconds * candidateBpm / 60.0;
            const double rounded = std::round(beats);
            const double fracDist = std::abs(beats - rounded);
            candidates[i] = { candidateBpm, beats, fracDist, (int)rounded };
        }

        // Pick the candidate with the smallest fractional distance (closest to whole beats)
        int bestIdx = 1;  // default to normal
        double bestFrac = candidates[1].fractionalDist;
        for (int i = 0; i < 3; ++i)
        {
            if (candidates[i].roundedBeats >= 1 && candidates[i].fractionalDist < bestFrac)
            {
                bestFrac = candidates[i].fractionalDist;
                bestIdx = i;
            }
        }

        outBpm = candidates[bestIdx].bpm;
        outLoopBeats = juce::jmax(1, candidates[bestIdx].roundedBeats);

        // Snap the BPM so that the loop length is exactly whole beats
        // e.g. if duration=6s and we chose 8 beats, bpm = 8*60/6 = 80.0
        if (outLoopBeats > 0 && durationSeconds > 0.0)
            outBpm = (double)outLoopBeats * 60.0 / durationSeconds;
    }

    struct SampleBpmDetection
    {
        double rawBpm = 0.0;
        double normalisedBpm = 0.0;
        int loopLengthBeats = 0;
    };

    static SampleBpmDetection detectSampleBpmWithLibrosa(const juce::AudioBuffer<float>& buffer, double sampleRate);

    static SampleBpmDetection detectSampleBpmDetailed(const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        // Try librosa first (better tempo detection with beat tracking)
#if NINJAMPLUS_HAS_LIBROSA
        {
            SampleBpmDetection librosaResult = detectSampleBpmWithLibrosa(buffer, sampleRate);
            if (librosaResult.normalisedBpm > 1.0 && librosaResult.loopLengthBeats > 0)
                return librosaResult;
        }
#endif

        juce::ignoreUnused(buffer, sampleRate);
        return {};
    }

    static SampleBpmDetection detectSampleBpmWithLibrosa(const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        SampleBpmDetection result;
#if NINJAMPLUS_HAS_LIBROSA
        const int numSamples = buffer.getNumSamples();
        if (numSamples <= 0 || sampleRate <= 1.0)
            return result;

        // Convert to mono float vector for librosa
        const float* left = buffer.getReadPointer(0);
        const float* right = buffer.getNumChannels() > 1 ? buffer.getReadPointer(1) : left;

        // librosa works best at 22050 Hz — downsample if needed
        const int targetSr = 22050;
        std::vector<float> audio;
        if (sampleRate > targetSr)
        {
            const int ratio = (int)(sampleRate / targetSr);
            const int outLen = numSamples / ratio;
            audio.reserve(outLen);
            for (int i = 0; i < outLen; ++i)
            {
                double sum = 0.0;
                for (int j = 0; j < ratio; ++j)
                {
                    const int idx = i * ratio + j;
                    if (idx < numSamples)
                        sum += 0.5 * (left[idx] + right[idx]);
                }
                audio.push_back((float)(sum / ratio));
            }
        }
        else
        {
            audio.reserve(numSamples);
            for (int i = 0; i < numSamples; ++i)
                audio.push_back(0.5f * (left[i] + right[i]));
        }

        const double effectiveSr = (sampleRate > targetSr) ? (double)targetSr : sampleRate;

        try
        {
            librosa::ArrayXr y(audio.size());
            for (Eigen::Index i = 0; i < (Eigen::Index)audio.size(); ++i)
                y(i) = (librosa::Real)audio[(size_t)i];

            // Use librosa beat_track_audio for tempo estimation
            // This uses onset detection + dynamic programming beat tracking
            const int hopLength = 512;
            auto [tempo, beats] = librosa::beat::beat_track_audio(
                y, effectiveSr, hopLength, 120.0, 100.0, true);

            if (tempo > 1.0 && std::isfinite(tempo))
            {
                const double durationSeconds = (double)numSamples / sampleRate;
                double correctedBpm = tempo;
                int correctedLoopBeats = 0;
                correctTempoOctave(tempo, durationSeconds, correctedBpm, correctedLoopBeats);

                result.rawBpm = correctedBpm;
                result.normalisedBpm = normaliseDetectedTempoBpm(correctedBpm);
                result.loopLengthBeats = correctedLoopBeats;
            }
        }
        catch (...)
        {
            // librosa can throw on edge cases (empty signal, etc.)
            return result;
        }

        return result;
#else
        juce::ignoreUnused(buffer, sampleRate);
        return result;
#endif
    }

    static double detectSampleBpmWithAubio(const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        const auto detection = detectSampleBpmDetailed(buffer, sampleRate);
        return detection.normalisedBpm;
    }

    static juce::AudioBuffer<float> stretchLoopWithSignalsmith(const juce::AudioBuffer<float>& source,
                                                               double sampleRate,
                                                               int targetSamples)
    {
        const int inputSamples = source.getNumSamples();
        if (inputSamples <= 0 || targetSamples <= 0)
            return {};

        if (targetSamples == inputSamples)
        {
            juce::AudioBuffer<float> output(2, targetSamples);
            output.clear();
            output.copyFrom(0, 0, source, 0, 0, inputSamples);
            output.copyFrom(1, 0, source, source.getNumChannels() > 1 ? 1 : 0, 0, inputSamples);
            return output;
        }

        // Use Rubber Band Library in offline mode with the R3 (Finer) engine
        // and percussive transient detection — ideal for drum loops.
        // Offline mode does two passes (study + process) and handles delay
        // compensation internally, so the output starts at sample 0.
        const double timeRatio = (double)targetSamples / (double)inputSamples;
        const int channels = 2;

        using RBS = RubberBand::RubberBandStretcher;
        const RBS::Options options = RBS::OptionProcessOffline
                                   | RBS::OptionEngineFiner
                                   | RBS::OptionDetectorPercussive
                                   | RBS::OptionTransientsCrisp
                                   | RBS::OptionThreadingNever;

        RBS stretcher((size_t)(int)std::round(sampleRate),
                      (size_t)channels,
                      options,
                      timeRatio,
                      1.0); // pitch scale = 1.0 (no pitch change)
        stretcher.setDebugLevel(0);

        // Prepare de-interleaved input pointers
        const float* inL = source.getReadPointer(0);
        const float* inR = source.getNumChannels() > 1 ? source.getReadPointer(1) : inL;

        // Pass 1: study (let Rubber Band analyze the stretch profile)
        {
            const float* inPtrs[2] = { inL, inR };
            stretcher.study(inPtrs, (size_t)inputSamples, true);
        }

        // Pass 2: process and retrieve
        juce::AudioBuffer<float> output(2, targetSamples);
        output.clear();

        {
            const float* inPtrs[2] = { inL, inR };
            stretcher.process(inPtrs, (size_t)inputSamples, true);
        }

        // Retrieve all available output
        float* outL = output.getWritePointer(0);
        float* outR = output.getWritePointer(1);
        int retrieved = 0;

        for (;;)
        {
            int avail = stretcher.available();
            if (avail <= 0)
                break;

            float* outPtrs[2] = { outL + retrieved, outR + retrieved };
            const int toRetrieve = juce::jmin(avail, targetSamples - retrieved);
            if (toRetrieve <= 0)
                break;

            const size_t got = stretcher.retrieve(outPtrs, (size_t)toRetrieve);
            retrieved += (int)got;
            if (got == 0)
                break;
        }

        // If Rubber Band produced fewer samples than requested, pad with silence
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* data = output.getWritePointer(ch);
            for (int i = retrieved; i < targetSamples; ++i)
                data[i] = 0.0f;
            // Sanitize
            for (int i = 0; i < targetSamples; ++i)
                if (!std::isfinite(data[i]))
                    data[i] = 0.0f;
        }

        return output;
    }

}

struct NinjamVst3AudioProcessor::LinkTimingState
{
    ableton::link::HostTimeFilter<ableton::LinkAudio::Clock> hostTimeFilter;
    double nextSampleTime = 0.0;

    void reset()
    {
        hostTimeFilter.reset();
        nextSampleTime = 0.0;
    }
};

namespace
{
    struct PreparedSamplePadLoadData
    {
        juce::AudioBuffer<float> sample;
        juce::File file;
        juce::String defaultName;
        double sourceRate = 44100.0;
        double detectedBpm = 0.0;
        double rawDetectedBpm = 0.0;
        int loopLengthBeats = 0;
    };

    bool prepareSamplePadLoadData(const juce::File& file,
                                  PreparedSamplePadLoadData& outData)
    {
        if (!file.existsAsFile())
            return false;

        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels <= 0)
            return false;

        constexpr double maxSamplePadSeconds = 180.0;
        const double sourceRate = reader->sampleRate > 1.0 ? reader->sampleRate : 44100.0;
        const juce::int64 maxSamplesFromLength = (juce::int64) std::ceil(sourceRate * maxSamplePadSeconds);
        const juce::int64 samplesToRead64 = juce::jmin(reader->lengthInSamples, maxSamplesFromLength);
        if (samplesToRead64 <= 0 || samplesToRead64 > (juce::int64) std::numeric_limits<int>::max())
            return false;

        const int samplesToRead = (int) samplesToRead64;
        juce::AudioBuffer<float> loaded(2, samplesToRead);
        loaded.clear();

        const bool readRightChannel = reader->numChannels > 1;
        if (!reader->read(&loaded, 0, samplesToRead, 0, true, readRightChannel))
            return false;

        if (!readRightChannel)
            loaded.copyFrom(1, 0, loaded, 0, 0, samplesToRead);

        outData.sample = std::move(loaded);
        outData.file = file;
        outData.defaultName = file.getFileNameWithoutExtension();
        outData.sourceRate = sourceRate;
        const auto bpmDetection = detectSampleBpmDetailed(outData.sample, sourceRate);
        outData.detectedBpm = bpmDetection.normalisedBpm;
        outData.rawDetectedBpm = bpmDetection.rawBpm;
        outData.loopLengthBeats = bpmDetection.loopLengthBeats;
        return true;
    }

    class SamplePadBackgroundJob final : public juce::ThreadPoolJob
    {
    public:
        using RunFunction = std::function<JobStatus()>;

        SamplePadBackgroundJob(const juce::String& jobName, RunFunction fn)
            : juce::ThreadPoolJob(jobName), runFunction(std::move(fn))
        {
        }

        JobStatus runJob() override
        {
            return runFunction != nullptr ? runFunction() : jobHasFinished;
        }

    private:
        RunFunction runFunction;
    };
}

class ZapChunkProcessingThread : public juce::Thread
{
public:
    explicit ZapChunkProcessingThread(NinjamVst3AudioProcessor& p)
        : juce::Thread("ZapChunkProcessor"), processor(p) {}

    void run() override
    {
        while (!threadShouldExit())
        {
            processor.processPendingZapChunks();
            wait(5); // 5ms poll — fast enough for video, gentle on CPU
        }
        // Drain any remaining chunks on exit
        processor.processPendingZapChunks();
    }

private:
    NinjamVst3AudioProcessor& processor;
};

NinjamVst3AudioProcessor::NinjamVst3AudioProcessor()
     : AudioProcessor (BusesProperties()
                     .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                     .withInput  ("Input 2", juce::AudioChannelSet::stereo(), false)
                     .withInput  ("Input 3", juce::AudioChannelSet::stereo(), false)
                     .withInput  ("Input 4", juce::AudioChannelSet::stereo(), false)
                     .withInput  ("Input 5", juce::AudioChannelSet::stereo(), false)
                     .withInput  ("Input 6", juce::AudioChannelSet::stereo(), false)
                     .withInput  ("Input 7", juce::AudioChannelSet::stereo(), false)
                     .withInput  ("Input 8", juce::AudioChannelSet::stereo(), false)
                     .withInput  ("Input 9", juce::AudioChannelSet::stereo(), false)
                     .withInput  ("Input 10", juce::AudioChannelSet::stereo(), false)
                     .withInput  ("Input 11", juce::AudioChannelSet::stereo(), false)
                     .withInput  ("Input 12", juce::AudioChannelSet::stereo(), false)
                     .withInput  ("Input 13", juce::AudioChannelSet::stereo(), false)
                     .withInput  ("Input 14", juce::AudioChannelSet::stereo(), false)
                     .withInput  ("Input 15", juce::AudioChannelSet::stereo(), false)
                     .withInput  ("Input 16", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output Main", juce::AudioChannelSet::stereo(), true)
                     .withOutput ("Output 2", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 3", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 4", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 5", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 6", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 7", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 8", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 9", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 10", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 11", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 12", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 13", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 14", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 15", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 16", juce::AudioChannelSet::stereo(), false)
                       )
{
    metronomeFormatManager.registerBasicFormats();
    samplePadFormatManager.registerBasicFormats();

    for (int i = 0; i < maxLocalChannels; ++i)
    {
        localChannelGains[(size_t)i].store(1.0f);
        localChannelPeaks[(size_t)i].store(0.0f);
        localChannelPeaksL[(size_t)i].store(0.0f);
        localChannelPeaksR[(size_t)i].store(0.0f);
        localChannelInputs[(size_t)i].store(-1);
        localChannelReverbSends[(size_t)i].store(0.0f);
        localChannelDelaySends[(size_t)i].store(0.0f);
        localChannelNames[(size_t)i] = buildDefaultLocalChannelName(i);
    }

    for (int i = 0; i < maxRemoteChordUsers; ++i)
    {
        remoteChordDetectionEnabled[(size_t)i].store(true, std::memory_order_relaxed);
        remoteChordUserKeys[(size_t)i].clear();
        remoteOpusPeerActive[(size_t)i].store(false, std::memory_order_relaxed);
        remoteOpusCarrierChannel[(size_t)i].store(-1, std::memory_order_relaxed);
        remoteOpusVirtualChannelCount[(size_t)i].store(1, std::memory_order_relaxed);
        remoteOpusPackedChannelCount[(size_t)i].store(0, std::memory_order_relaxed);
        remoteOpusCombinedPeakL[(size_t)i].store(0.0f, std::memory_order_relaxed);
        remoteOpusCombinedPeakR[(size_t)i].store(0.0f, std::memory_order_relaxed);
        remoteOpusSourcePeakL[(size_t)i].store(0.0f, std::memory_order_relaxed);
        remoteOpusSourcePeakR[(size_t)i].store(0.0f, std::memory_order_relaxed);
        remoteOpusUserVolume[(size_t)i].store(1.0f, std::memory_order_relaxed);
        remoteOpusUserPan[(size_t)i].store(0.0f, std::memory_order_relaxed);
        remoteOpusUserOutput[(size_t)i].store(0, std::memory_order_relaxed);
        remoteOpusUserMute[(size_t)i].store(false, std::memory_order_relaxed);
        remoteOpusUserSolo[(size_t)i].store(false, std::memory_order_relaxed);
        for (int ch = 0; ch < maxLocalChannels; ++ch)
        {
            remoteOpusChannelWidths[(size_t)i][(size_t)ch].store(1, std::memory_order_relaxed);
            remoteOpusChannelGains[(size_t)i][(size_t)ch].store(1.0f, std::memory_order_relaxed);
            remoteOpusChannelPeaks[(size_t)i][(size_t)ch].store(0.0f, std::memory_order_relaxed);
        }
    }

    for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
    {
        samplePadFxSlotTypes[(size_t)slot].store((int)getDefaultSamplePadFxType(slot), std::memory_order_relaxed);
        samplePadFxSlotAmounts[(size_t)slot].store(0.0f, std::memory_order_relaxed);
        for (int targetSlot = 0; targetSlot < numSamplePadFxSlots; ++targetSlot)
            samplePadFxSlotChainRoutes[(size_t)slot][(size_t)targetSlot].store(false, std::memory_order_relaxed);
    }
    samplePadDuckOscillator.initialise([](float x)
    {
        return std::sin(x);
    });

    chordAnalyzer = std::make_unique<BatchedChordAnalyzer>();
    linkTimingState = std::make_unique<LinkTimingState>();

    setLatencySamples(0);
    startTimer(20); // Run NINJAM client loop every 20ms

    zapChunkProcessingThread = std::make_unique<ZapChunkProcessingThread>(*this);
    zapChunkProcessingThread->startThread();

    // Set callbacks
    ninjamClient.LicenseAgreementCallback = LicenseAgreementCallback;
    ninjamClient.LicenseAgreement_User = this;

    ninjamClient.ChatMessage_Callback = ChatMessage_Callback;
    ninjamClient.ChatMessage_User = this;
    ninjamClient.IntervalMediaItem_Callback = IntervalMediaItem_Callback;
    ninjamClient.IntervalMediaItem_User = this;
    ninjamClient.IntervalChunkCallback = IntervalChunkCallback_cb;
    ninjamClient.IntervalChunkCallbackUser = this;
    ninjamClient.RemoteChannelAudioTap = RemoteChannelAudioTap_Callback;
    ninjamClient.RemoteChannelAudioTap_User = this;
    ninjamClient.RemoteMultichannelTap = RemoteMultichannelTap_Callback;
    ninjamClient.RemoteMultichannelTap_User = this;
    ninjamClient.NewIntervalCallback = NewIntervalCallback_cb;
    ninjamClient.NewIntervalCallbackUser = this;
    ninjamClient.PostNewIntervalCallback = PostNewIntervalCallback_cb;
    ninjamClient.PostNewIntervalCallbackUser = this;
    opusSyncInstanceId = juce::Uuid().toString();

    // Default Metronome
    ninjamClient.config_metronome = 1.0f; // -12dB or similar? 1.0 is 0dB

    // Ensure disconnected state
    {
        const juce::ScopedLock clientLock(ninjamClientLock);
        ninjamClient.Disconnect();
    }

    // Initialize JNetLib (WSAStartup on Windows)
    JNL::open_socketlib();

    asyncChatTranslationWorker = std::make_unique<AsyncChatTranslationWorker>(*this);
    abletonLink = std::make_unique<ableton::LinkAudio>(120.0, getLinkPeerName().toStdString());
    refreshAbletonLinkActivation();
}

void NinjamVst3AudioProcessor::connectToServer(juce::String host, juce::String user, juce::String pass)
{
    cancelAutoReconnect(false);
    stopNinjamZapVideoTransportForDisconnect();
    stopAdvancedVideoClient();
    ninjamZapServerVideoSupported.store(false, std::memory_order_relaxed);
    ninjamSideSignalServerSupported.store(false, std::memory_order_relaxed);
    lastNinjamVideoCapSendMs = 0.0;
    lastRemoteVideoRoomActivityMs.store(0.0, std::memory_order_relaxed);

    host = host.trim();
    user = user.trim();
    pass = pass.trim();

    if (host.isEmpty())
        host = "127.0.0.1";

    if (user.isEmpty())
    {
        user = "anonymous:jammer";
        pass = "anon";
    }

    // Parse the NINJAM host and port from the host string
    juce::String ninjamHost = host;
    int ninjamPort = 2049;
    {
        juce::String parsedHost;
        int parsedPort = 2049;
        if (tryParseServerEndpoint(host, parsedHost, parsedPort))
        {
            ninjamHost = parsedHost;
            ninjamPort = parsedPort;
        }
    }

    // SSH tunnel: if enabled, start the tunnel on a background thread and defer
    // the actual NINJAM connection until the tunnel is ready (or fails).
    juce::String connectHost = host;
    if (sshTunnelEnabled.load(std::memory_order_relaxed) && sshTunnelHost.trim().isNotEmpty())
    {
        sshTunnel.stop();

        ninjamplus::SshTunnel::Config tunnelConfig;
        tunnelConfig.enabled = true;
        tunnelConfig.sshHost = sshTunnelHost.trim();
        tunnelConfig.sshPort = sshTunnelPort.load(std::memory_order_relaxed);
        tunnelConfig.sshUser = sshTunnelUser.trim();
        tunnelConfig.keyFile = sshTunnelKeyFile.trim();
        tunnelConfig.remoteHost = ninjamHost;
        tunnelConfig.remotePort = ninjamPort;
        tunnelConfig.localPort = 0; // auto-pick

        addSystemChatLine("Starting SSH tunnel to " + tunnelConfig.sshHost + ":" + juce::String(tunnelConfig.sshPort)
                          + " for " + ninjamHost + ":" + juce::String(ninjamPort) + "...");

        // Start tunnel on a background thread to avoid blocking the message thread
        std::shared_ptr<std::atomic<int>> tunnelLocalPort = std::make_shared<std::atomic<int>>(0);
        std::shared_ptr<juce::String> tunnelError = std::make_shared<juce::String>();

        std::thread tunnelThread([this, tunnelConfig, tunnelLocalPort, tunnelError]()
        {
            const int localPort = sshTunnel.start(tunnelConfig);
            tunnelLocalPort->store(localPort);
            if (localPort <= 0)
                *tunnelError = sshTunnel.getLastError();
        });
        tunnelThread.detach();

        // Wait for the tunnel to finish starting (with a timeout) without blocking
        // the message thread — use short sleeps so the UI stays responsive.
        const double tunnelTimeoutMs = 15000.0;
        const double tunnelStartMs = juce::Time::getMillisecondCounterHiRes();
        while (tunnelLocalPort->load() == 0
               && tunnelError->isEmpty()
               && juce::Time::getMillisecondCounterHiRes() - tunnelStartMs < tunnelTimeoutMs)
        {
            juce::Thread::sleep(50);
        }

        const int localPort = tunnelLocalPort->load();
        if (localPort > 0)
        {
            connectHost = "127.0.0.1:" + juce::String(localPort);
            addSystemChatLine("SSH tunnel established on local port " + juce::String(localPort));
        }
        else
        {
            if (tunnelError->isNotEmpty())
                addSystemChatLine("SSH tunnel failed: " + *tunnelError);
            else
                addSystemChatLine("SSH tunnel timed out.");
        }
    }

    pendingConnectHost = host; // Store original host for reconnect logic
    pendingConnectOriginalUser = user;
    pendingConnectPass = pass;
    pendingConnectNameAttempt = 0;
    duplicateNameRetryEnabled = true;

    {
        const juce::ScopedLock lock(serverListLock);
        user = chooseAvailableUserNameForServer(user, publicServers, host);
        if (user != pendingConnectOriginalUser)
            pendingConnectNameAttempt = 1;
    }

    {
        const juce::ScopedLock lock(opusSyncPeerLock);
        opusSyncPeers.clear();
    }
    clearRemoteAudioTapBuffers();
    clearZapVideoFrameState();
    {
        const juce::ScopedLock lock(intervalSyncAnnouncementLock);
        lastAnnouncedRemoteIntervalByUser.clear();
        localIntervalStartMsByInterval.clear();
        pendingRemoteIntervalStartsByUser.clear();
        lastRemoteServerLatencyMsByUser.clear();
        remoteServerRouteLatencyMsByUser.clear();
        lastRemoteIntervalSignalSeenMsByUser.clear();
        lastRemoteRouteProbeSeenMsByUser.clear();
        pendingTransportProbeSentMsById.clear();
        remoteLatencyLastAppliedIntervalByUser.clear();
        remoteLatencyAverageByUser.clear();
        remoteLatencyFirmDelayMsByUser.clear();
        remoteVideoBufferRefreshIdByUser.clear();
        videoBufferRefreshCounter = 0;
    }
    vdoRosterRevision.fetch_add(1, std::memory_order_relaxed);
    intervalHelperPayloadForceWrite.store(true, std::memory_order_release);
    resetIntervalSyncTimingCache();
    opusSyncAvailable.store(false);
    opusSyncHasLegacyClients.store(false);
    lastOpusSupportBroadcastMs = 0.0;
    lastServerLatencyProbeAttemptMs = 0.0;

    {
        const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
        remoteLinkAudioOutputPairs.clear();
        remoteLinkAudioSinks.clear();
    }

    {
        const juce::ScopedLock lifecycleLock(ninjamAudioLifecycleLock);
        const juce::ScopedLock clientLock(ninjamClientLock);
        applyCodecPreference();
        ninjamClient.Connect(connectHost.toRawUTF8(), user.toRawUTF8(), pass.toRawUTF8());
    }
    currentServer = host;
    currentUser = user;
    {
        const juce::ScopedLock lock(vdoRoomLock);
        announcedVdoRoomServerKey.clear();
        announcedVdoRoomName.clear();
        announcedVdoRoomOwnedLocally = false;
        lastVdoRoomAnnouncementMs = 0.0;
    }
    refreshAbletonLinkActivation();

    // Do NOT reset isTransmitting here — the user may have toggled it before
    // connecting. The NJC_STATUS_OK handler calls syncLocalIntervalChannelConfig()
    // which re-applies the current isTransmitting state to NJClient.
}

void NinjamVst3AudioProcessor::disconnectFromServer()
{
    cancelAutoReconnect(true);
    duplicateNameRetryEnabled = false;
    pendingConnectNameAttempt = 0;
    ninjamZapServerVideoSupported.store(false, std::memory_order_relaxed);
    ninjamSideSignalServerSupported.store(false, std::memory_order_relaxed);
    lastNinjamVideoCapSendMs = 0.0;
    lastRemoteVideoRoomActivityMs.store(0.0, std::memory_order_relaxed);
    {
        const juce::ScopedLock lifecycleLock(ninjamAudioLifecycleLock);
        const juce::ScopedLock clientLock(ninjamClientLock);
        stopNinjamZapVideoTransportForDisconnect();
        ninjamClient.Disconnect();
    }
    // Stop SSH tunnel if active
    if (sshTunnel.isActive())
    {
        sshTunnel.stop();
        addSystemChatLine("SSH tunnel closed.");
    }
    stopAdvancedVideoClient();
    currentServer = {};
    currentUser = {};
    {
        const juce::ScopedLock lock(vdoRoomLock);
        announcedVdoRoomServerKey.clear();
        announcedVdoRoomName.clear();
        announcedVdoRoomOwnedLocally = false;
        lastVdoRoomAnnouncementMs = 0.0;
    }
    clearRemoteAudioTapBuffers();
    refreshAbletonLinkActivation();
    {
        const juce::ScopedLock lock(opusSyncPeerLock);
        opusSyncPeers.clear();
    }
    {
        const juce::ScopedLock lock(intervalSyncAnnouncementLock);
        lastAnnouncedRemoteIntervalByUser.clear();
        localIntervalStartMsByInterval.clear();
        pendingRemoteIntervalStartsByUser.clear();
        lastRemoteServerLatencyMsByUser.clear();
        remoteServerRouteLatencyMsByUser.clear();
        lastRemoteIntervalSignalSeenMsByUser.clear();
        lastRemoteRouteProbeSeenMsByUser.clear();
        pendingTransportProbeSentMsById.clear();
        remoteLatencyLastAppliedIntervalByUser.clear();
        remoteLatencyAverageByUser.clear();
        remoteLatencyFirmDelayMsByUser.clear();
        remoteVideoBufferRefreshIdByUser.clear();
        videoBufferRefreshCounter = 0;
    }
    vdoRosterRevision.fetch_add(1, std::memory_order_relaxed);
    intervalHelperPayloadForceWrite.store(true, std::memory_order_release);
    resetIntervalSyncTimingCache();
    {
        const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
        remoteLinkAudioOutputPairs.clear();
        remoteLinkAudioSinks.clear();
    }
    opusSyncAvailable.store(false);
    opusSyncHasLegacyClients.store(false);
    lastServerLatencyProbeAttemptMs = 0.0;
    applyCodecPreference();
}

void NinjamVst3AudioProcessor::setAutoReconnectEnabled(bool shouldEnable)
{
    autoReconnectEnabled.store(shouldEnable, std::memory_order_relaxed);
    if (!shouldEnable)
    {
        cancelAutoReconnect(true);
        return;
    }

    autoReconnectSuppressed.store(false, std::memory_order_relaxed);
    int status = NJClient::NJC_STATUS_DISCONNECTED;
    {
        const juce::ScopedLock clientLock(ninjamClientLock);
        status = ninjamClient.GetStatus();
    }
    if ((status == NJClient::NJC_STATUS_DISCONNECTED || status == NJClient::NJC_STATUS_CANTCONNECT)
        && pendingConnectHost.trim().isNotEmpty())
        scheduleAutoReconnect(juce::Time::getMillisecondCounterHiRes(), "connection unavailable");
}

bool NinjamVst3AudioProcessor::isAutoReconnectEnabled() const
{
    return autoReconnectEnabled.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::cancelAutoReconnect(bool suppressUntilManualConnect)
{
    autoReconnectNextAttemptMs.store(0.0, std::memory_order_relaxed);
    autoReconnectAttemptStartedMs.store(0.0, std::memory_order_relaxed);
    autoReconnectAttemptCount.store(0, std::memory_order_relaxed);
    autoReconnectConnectedSinceMs.store(0.0, std::memory_order_relaxed);
    autoReconnectSuppressed.store(suppressUntilManualConnect, std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::scheduleAutoReconnect(double nowMs, const juce::String& reason)
{
    if (!autoReconnectEnabled.load(std::memory_order_relaxed)
        || autoReconnectSuppressed.load(std::memory_order_relaxed)
        || pendingConnectHost.trim().isEmpty()
        || autoReconnectNextAttemptMs.load(std::memory_order_relaxed) > 0.0)
        return;

    const int attemptCount = autoReconnectAttemptCount.fetch_add(1, std::memory_order_relaxed) + 1;
    const int exponent = juce::jlimit(0, 5, attemptCount - 1);
    const int baseDelayMs = juce::jmin(30000, 1000 * (1 << exponent));
    const int jitterMs = (attemptCount * 137) % 401;
    const int delayMs = baseDelayMs + jitterMs;
    autoReconnectNextAttemptMs.store(nowMs + (double)delayMs, std::memory_order_relaxed);

    const juce::String reasonText = reason.trim().isNotEmpty() ? reason.trim() : "connection lost";
    addSystemChatLine("NINJAM " + reasonText + "; reconnect attempt "
                      + juce::String(attemptCount) + " in "
                      + juce::String((double)delayMs / 1000.0, 1) + "s.");
}

bool NinjamVst3AudioProcessor::attemptAutoReconnect(double nowMs, int status)
{
    if (!autoReconnectEnabled.load(std::memory_order_relaxed)
        || autoReconnectSuppressed.load(std::memory_order_relaxed)
        || autoReconnectNextAttemptMs.load(std::memory_order_relaxed) <= 0.0
        || nowMs < autoReconnectNextAttemptMs.load(std::memory_order_relaxed)
        || (status != NJClient::NJC_STATUS_DISCONNECTED && status != NJClient::NJC_STATUS_CANTCONNECT))
        return false;

    const juce::String reconnectHost = pendingConnectHost.trim();
    const juce::String reconnectUser = pendingConnectOriginalUser.trim();
    const juce::String reconnectPass = pendingConnectPass;
    if (reconnectHost.isEmpty())
    {
        cancelAutoReconnect(true);
        return false;
    }

    autoReconnectNextAttemptMs.store(0.0, std::memory_order_relaxed);
    pendingConnectNameAttempt = 0;
    duplicateNameRetryEnabled = true;
    addSystemChatLine("Reconnecting to " + reconnectHost + "...");

    // Re-establish SSH tunnel if enabled
    juce::String reconnectConnectHost = reconnectHost;
    if (sshTunnelEnabled.load(std::memory_order_relaxed) && sshTunnelHost.trim().isNotEmpty())
    {
        // Parse host/port from the original host
        juce::String ninjamHost = reconnectHost;
        int ninjamPort = 2049;
        {
            juce::String parsedHost;
            int parsedPort = 2049;
            if (tryParseServerEndpoint(reconnectHost, parsedHost, parsedPort))
            {
                ninjamHost = parsedHost;
                ninjamPort = parsedPort;
            }
        }

        sshTunnel.stop();
        ninjamplus::SshTunnel::Config tunnelConfig;
        tunnelConfig.enabled = true;
        tunnelConfig.sshHost = sshTunnelHost.trim();
        tunnelConfig.sshPort = sshTunnelPort.load(std::memory_order_relaxed);
        tunnelConfig.sshUser = sshTunnelUser.trim();
        tunnelConfig.keyFile = sshTunnelKeyFile.trim();
        tunnelConfig.remoteHost = ninjamHost;
        tunnelConfig.remotePort = ninjamPort;
        tunnelConfig.localPort = 0;

        // Start tunnel on a background thread to avoid blocking the message thread
        std::shared_ptr<std::atomic<int>> tunnelLocalPort = std::make_shared<std::atomic<int>>(0);
        std::shared_ptr<juce::String> tunnelError = std::make_shared<juce::String>();

        std::thread tunnelThread([this, tunnelConfig, tunnelLocalPort, tunnelError]()
        {
            const int localPort = sshTunnel.start(tunnelConfig);
            tunnelLocalPort->store(localPort);
            if (localPort <= 0)
                *tunnelError = sshTunnel.getLastError();
        });
        tunnelThread.detach();

        // Wait for the tunnel with a timeout, using short sleeps so UI stays responsive
        const double tunnelTimeoutMs = 15000.0;
        const double tunnelStartMs = juce::Time::getMillisecondCounterHiRes();
        while (tunnelLocalPort->load() == 0
               && tunnelError->isEmpty()
               && juce::Time::getMillisecondCounterHiRes() - tunnelStartMs < tunnelTimeoutMs)
        {
            juce::Thread::sleep(50);
        }

        const int localPort = tunnelLocalPort->load();
        if (localPort > 0)
        {
            reconnectConnectHost = "127.0.0.1:" + juce::String(localPort);
            addSystemChatLine("SSH tunnel re-established on local port " + juce::String(localPort));
        }
        else
        {
            if (tunnelError->isNotEmpty())
                addSystemChatLine("SSH tunnel failed: " + *tunnelError);
            else
                addSystemChatLine("SSH tunnel timed out.");
        }
    }

    {
        const juce::ScopedLock lifecycleLock(ninjamAudioLifecycleLock);
        const juce::ScopedLock clientLock(ninjamClientLock);
        ninjamClient.Disconnect();
        applyCodecPreference();
        ninjamClient.Connect(reconnectConnectHost.toRawUTF8(), reconnectUser.toRawUTF8(), reconnectPass.toRawUTF8());
    }
    currentServer = reconnectHost;
    currentUser = reconnectUser;
    refreshAbletonLinkActivation();
    autoReconnectAttemptStartedMs.store(nowMs, std::memory_order_relaxed);
    lastStatus = NJClient::NJC_STATUS_PRECONNECT;
    return true;
}

void NinjamVst3AudioProcessor::sendChatMessage(juce::String msg)
{
    msg = msg.trim();
    if (msg.isEmpty())
        return;

    {
        juce::ScopedLock lock(chatLock);
        juce::String localLine = "Me: " + msg;
        chatHistory.add(localLine);
        chatSenders.add("me");
        chatRevision.fetch_add(1);
        if (chatHistory.size() > 100)
        {
            chatHistory.removeRange(0, chatHistory.size() - 100);
            chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
        }
    }

    {
        const juce::ScopedLock clientLock(ninjamClientLock);
        if (ninjamClient.GetStatus() == NJClient::NJC_STATUS_OK)
            ninjamClient.ChatMessage_Send("MSG", msg.toRawUTF8());
    }
}

void NinjamVst3AudioProcessor::sendChatAttachment(const juce::String& kindIn, const juce::String& urlIn)
{
    const juce::String url = urlIn.trim();
    if (!isHttpOrHttpsChatUrl(url))
    {
        addSystemChatMessage("Paste an http:// or https:// image/GIF URL before using +.");
        return;
    }

    const juce::String kind = normaliseRichChatKind(kindIn);
    const juce::String userId = normaliseOpusPeerId(currentUser);

    {
        juce::ScopedLock lock(chatLock);
        chatHistory.add(makeRichChatLine("Me", kind, url));
        chatSenders.add("me");
        chatRevision.fetch_add(1);
        trimChatArrays(chatHistory, chatSenders);
    }

    if (ninjamClient.GetStatus() == NJClient::NJC_STATUS_OK)
    {
        juce::DynamicObject::Ptr obj = new juce::DynamicObject();
        obj->setProperty("kind", kind);
        obj->setProperty("url", url);
        obj->setProperty("userId", userId.isNotEmpty() ? userId : currentUser.trim());
        obj->setProperty("appFamily", opusSyncAppFamily);
        obj->setProperty("eventId", "chatAttachment:" + (userId.isNotEmpty() ? userId : currentUser.trim()) + ":" + juce::String(++sideSignalEventCounter));
        sendSideSignal("*", "chatAttachment", juce::JSON::toString(juce::var(obj.get()), false));
    }

}

namespace
{
    constexpr const char* metronomeSoundClassicKey = "classic";
    constexpr const char* metronomeSoundSoftBeepKey = "soft_beep";
    constexpr const char* metronomeSoundSoftTickKey = "soft_tick";
    constexpr const char* metronomeSoundWoodTickKey = "wood_tick";
    constexpr const char* metronomeSoundFilePrefix = "file:";

    constexpr int metronomeModeClassic = 0;
    constexpr int metronomeModeSoftBeep = 1;
    constexpr int metronomeModeSoftTick = 2;
    constexpr int metronomeModeWoodTick = 3;
    constexpr int metronomeModeCustomFile = 100;
    constexpr int metronomeOutputMonoFlag = 1024;

    struct MetronomeOutputRoute
    {
        float* left = nullptr;
        float* right = nullptr;
    };

    int sanitiseMetronomeOutputChannel(int outputChannel)
    {
        if (outputChannel < 0)
            return 0;

        const bool isMono = (outputChannel & metronomeOutputMonoFlag) != 0;
        const int channelIndex = outputChannel & (metronomeOutputMonoFlag - 1);
        return isMono ? (channelIndex | metronomeOutputMonoFlag) : channelIndex;
    }

    MetronomeOutputRoute makeMetronomeOutputRoute(float** outputs, int outnch, int outputChannel)
    {
        MetronomeOutputRoute route;
        if (outputs == nullptr || outnch <= 0)
            return route;

        const int safeOutputChannel = sanitiseMetronomeOutputChannel(outputChannel);
        const bool isMono = (safeOutputChannel & metronomeOutputMonoFlag) != 0;
        const int channelIndex = safeOutputChannel & (metronomeOutputMonoFlag - 1);

        if (channelIndex >= 0 && channelIndex < outnch && outputs[channelIndex] != nullptr)
        {
            route.left = outputs[channelIndex];
            if (!isMono && channelIndex + 1 < outnch && outputs[channelIndex + 1] != nullptr)
                route.right = outputs[channelIndex + 1];
        }

        if (route.left == nullptr && outputs[0] != nullptr)
        {
            route.left = outputs[0];
            if (outnch > 1 && outputs[1] != nullptr)
                route.right = outputs[1];
        }

        return route;
    }

    bool isSupportedMetronomeSoundFile(const juce::File& file)
    {
        const juce::String ext = file.getFileExtension().toLowerCase();
        return ext == ".wav"
            || ext == ".aif"
            || ext == ".aiff"
            || ext == ".flac"
            || ext == ".ogg"
            || ext == ".mp3";
    }

    juce::String normaliseMetronomeSoundKey(const juce::String& key)
    {
        const juce::String trimmed = key.trim();
        if (trimmed.isEmpty() || trimmed.equalsIgnoreCase(metronomeSoundClassicKey))
            return metronomeSoundClassicKey;
        if (trimmed.equalsIgnoreCase(metronomeSoundSoftBeepKey))
            return metronomeSoundSoftBeepKey;
        if (trimmed.equalsIgnoreCase(metronomeSoundSoftTickKey))
            return metronomeSoundSoftTickKey;
        if (trimmed.equalsIgnoreCase(metronomeSoundWoodTickKey))
            return metronomeSoundWoodTickKey;
        if (trimmed.startsWithIgnoreCase(metronomeSoundFilePrefix))
            return metronomeSoundFilePrefix + trimmed.substring((int)std::strlen(metronomeSoundFilePrefix));
        return metronomeSoundClassicKey;
    }

    juce::File fileFromMetronomeSoundKey(const juce::String& key)
    {
        if (!key.startsWithIgnoreCase(metronomeSoundFilePrefix))
            return {};
        return juce::File(key.substring((int)std::strlen(metronomeSoundFilePrefix)));
    }

    int modeForMetronomeSoundKey(const juce::String& key)
    {
        if (key.equalsIgnoreCase(metronomeSoundSoftBeepKey))
            return metronomeModeSoftBeep;
        if (key.equalsIgnoreCase(metronomeSoundSoftTickKey))
            return metronomeModeSoftTick;
        if (key.equalsIgnoreCase(metronomeSoundWoodTickKey))
            return metronomeModeWoodTick;
        if (key.startsWithIgnoreCase(metronomeSoundFilePrefix))
            return metronomeModeCustomFile;
        return metronomeModeClassic;
    }

    void addMetronomeSoundDirectoryIfUnique(juce::Array<juce::File>& dirs, const juce::File& dir)
    {
        if (dir.getFullPathName().isNotEmpty())
            dirs.addIfNotAlreadyThere(dir);
    }

    void addMetronomeSoundDirectoryCandidates(juce::Array<juce::File>& dirs, const juce::File& root)
    {
        if (root.getFullPathName().isEmpty())
            return;

        juce::File probe = root;
        for (int i = 0; i < 8; ++i)
        {
            addMetronomeSoundDirectoryIfUnique(dirs, probe.getChildFile("Metronome Sounds"));
            addMetronomeSoundDirectoryIfUnique(dirs, probe.getChildFile("metronome-sounds"));
            addMetronomeSoundDirectoryIfUnique(dirs, probe.getChildFile("metronome_sounds"));
            addMetronomeSoundDirectoryIfUnique(dirs, probe.getChildFile("Resources").getChildFile("Metronome Sounds"));
            addMetronomeSoundDirectoryIfUnique(dirs, probe.getChildFile("Resources").getChildFile("metronome-sounds"));
            addMetronomeSoundDirectoryIfUnique(dirs, probe.getParentDirectory().getChildFile("Resources").getChildFile("Metronome Sounds"));
            addMetronomeSoundDirectoryIfUnique(dirs, probe.getParentDirectory().getChildFile("Resources").getChildFile("metronome-sounds"));
            probe = probe.getParentDirectory();
        }
    }
}

juce::String NinjamVst3AudioProcessor::getClassicMetronomeSoundKey()
{
    return metronomeSoundClassicKey;
}

juce::String NinjamVst3AudioProcessor::getSoftBeepMetronomeSoundKey()
{
    return metronomeSoundSoftBeepKey;
}

juce::String NinjamVst3AudioProcessor::getSoftTickMetronomeSoundKey()
{
    return metronomeSoundSoftTickKey;
}

juce::String NinjamVst3AudioProcessor::getWoodTickMetronomeSoundKey()
{
    return metronomeSoundWoodTickKey;
}

juce::String NinjamVst3AudioProcessor::makeCustomMetronomeSoundKey(const juce::File& file)
{
    return juce::String(metronomeSoundFilePrefix) + file.getFullPathName();
}

juce::String NinjamVst3AudioProcessor::getMetronomeSoundDisplayNameForKey(const juce::String& key)
{
    const juce::String normalised = normaliseMetronomeSoundKey(key);
    if (normalised.equalsIgnoreCase(metronomeSoundSoftBeepKey))
        return "Soft Beep";
    if (normalised.equalsIgnoreCase(metronomeSoundSoftTickKey))
        return "Soft Tick";
    if (normalised.equalsIgnoreCase(metronomeSoundWoodTickKey))
        return "Wood Tick";
    if (normalised.startsWithIgnoreCase(metronomeSoundFilePrefix))
    {
        const juce::File file = fileFromMetronomeSoundKey(normalised);
        return file.getFileNameWithoutExtension().isNotEmpty() ? file.getFileNameWithoutExtension() : "Custom File";
    }
    return "Classic";
}

juce::String NinjamVst3AudioProcessor::getMetronomeSoundKey() const
{
    const juce::ScopedLock lock(metronomeSoundKeyLock);
    return metronomeSoundKey;
}

bool NinjamVst3AudioProcessor::setMetronomeSoundKey(const juce::String& key)
{
    const juce::String normalised = normaliseMetronomeSoundKey(key);
    const int mode = modeForMetronomeSoundKey(normalised);

    if (mode == (int)MetronomeSoundMode::customFile)
    {
        const juce::File file = fileFromMetronomeSoundKey(normalised);
        if (!isSupportedMetronomeSoundFile(file) || !loadCustomMetronomeSoundFile(file))
        {
            clearCustomMetronomeSoundFile();
            {
                const juce::ScopedLock lock(metronomeSoundKeyLock);
                metronomeSoundKey = metronomeSoundClassicKey;
            }
            metronomeSoundMode.store((int)MetronomeSoundMode::classic, std::memory_order_release);
            updateMetronomeEngineVolume();
            return false;
        }
    }
    else
    {
        clearCustomMetronomeSoundFile();
    }

    {
        const juce::ScopedLock lock(metronomeSoundKeyLock);
        metronomeSoundKey = normalised;
    }
    metronomeSoundMode.store(mode, std::memory_order_release);
    updateMetronomeEngineVolume();
    return true;
}

juce::File NinjamVst3AudioProcessor::getUserMetronomeSoundsDirectory() const
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("NINJAMplus")
        .getChildFile("Metronome Sounds");
}

juce::Array<juce::File> NinjamVst3AudioProcessor::findCustomMetronomeSoundFiles() const
{
    juce::Array<juce::File> dirs;
    addMetronomeSoundDirectoryIfUnique(dirs, getUserMetronomeSoundsDirectory());
    addMetronomeSoundDirectoryCandidates(dirs, juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory());

    const juce::File moduleFile = getThisModuleFile();
    if (moduleFile.existsAsFile())
        addMetronomeSoundDirectoryCandidates(dirs, moduleFile.getParentDirectory());

    juce::Array<juce::File> files;
    const juce::StringArray patterns { "*.wav", "*.aif", "*.aiff", "*.flac", "*.ogg", "*.mp3" };
    for (const auto& dir : dirs)
    {
        if (!dir.isDirectory())
            continue;

        for (const auto& pattern : patterns)
        {
            auto matches = dir.findChildFiles(juce::File::findFiles, false, pattern);
            for (const auto& file : matches)
                files.addIfNotAlreadyThere(file);
        }
    }

    files.sort();
    return files;
}

bool NinjamVst3AudioProcessor::loadCustomMetronomeSoundFile(const juce::File& file)
{
    if (!file.existsAsFile())
        return false;

    std::unique_ptr<juce::AudioFormatReader> reader(metronomeFormatManager.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels <= 0)
        return false;

    const double sourceRate = reader->sampleRate > 1.0 ? reader->sampleRate : 44100.0;
    constexpr double maxMetronomeSoundSeconds = 3.0;
    const juce::int64 maxSamplesFromLength = (juce::int64)std::ceil(sourceRate * maxMetronomeSoundSeconds);
    const juce::int64 samplesToRead64 = juce::jmin(reader->lengthInSamples, maxSamplesFromLength);
    if (samplesToRead64 <= 0 || samplesToRead64 > (juce::int64)std::numeric_limits<int>::max())
        return false;

    const int samplesToRead = (int)samplesToRead64;
    juce::AudioBuffer<float> loaded(2, samplesToRead);
    loaded.clear();
    const bool readRightChannel = reader->numChannels > 1;
    if (!reader->read(&loaded, 0, samplesToRead, 0, true, readRightChannel))
        return false;
    if (!readRightChannel)
        loaded.copyFrom(1, 0, loaded, 0, 0, samplesToRead);

    float peak = 0.0f;
    for (int ch = 0; ch < loaded.getNumChannels(); ++ch)
        peak = juce::jmax(peak, loaded.getMagnitude(ch, 0, loaded.getNumSamples()));
    if (peak <= 0.000001f)
        return false;
    if (peak > 1.0f)
        loaded.applyGain(1.0f / peak);

    {
        const juce::SpinLock::ScopedLockType lock(metronomeCustomSampleLock);
        metronomeCustomSample = std::move(loaded);
        metronomeCustomSampleRate = sourceRate;
        metronomeCustomSampleFile = file;
    }
    return true;
}

void NinjamVst3AudioProcessor::clearCustomMetronomeSoundFile()
{
    const juce::SpinLock::ScopedLockType lock(metronomeCustomSampleLock);
    metronomeCustomSample.setSize(0, 0);
    metronomeCustomSampleRate = 44100.0;
    metronomeCustomSampleFile = juce::File();
}

void NinjamVst3AudioProcessor::updateMetronomeEngineVolume()
{
    const float vol = metronomeMuted.load(std::memory_order_relaxed)
        ? 0.0f
        : juce::jlimit(0.0f, 1.0f, storedMetronomeVolume.load(std::memory_order_relaxed));
    const int mode = metronomeSoundMode.load(std::memory_order_acquire);
    const int outputChannel = sanitiseMetronomeOutputChannel(metronomeOutputChannel.load(std::memory_order_relaxed));
    const bool useEngineMetronome = mode == (int)MetronomeSoundMode::classic && outputChannel == 0;
    ninjamClient.config_metronome = useEngineMetronome ? vol : 0.0f;
}

void NinjamVst3AudioProcessor::setMetronomeVolume(float vol)
{
    vol = juce::jlimit(0.0f, 1.0f, vol);
    storedMetronomeVolume.store(vol, std::memory_order_relaxed);
    if (vol > 0.0f)
        metronomeMuted.store(false, std::memory_order_relaxed);
    updateMetronomeEngineVolume();
}

float NinjamVst3AudioProcessor::getMetronomeVolume() const
{
    return metronomeMuted.load(std::memory_order_relaxed)
        ? 0.0f
        : juce::jlimit(0.0f, 1.0f, storedMetronomeVolume.load(std::memory_order_relaxed));
}

void NinjamVst3AudioProcessor::setMetronomeMuted(bool shouldMute)
{
    const float current = getMetronomeVolume();
    metronomeMuted.store(shouldMute, std::memory_order_relaxed);
    if (shouldMute)
    {
        if (current > 0.0f)
            storedMetronomeVolume.store(current, std::memory_order_relaxed);
    }
    updateMetronomeEngineVolume();
}

bool NinjamVst3AudioProcessor::isMetronomeMuted() const
{
    return metronomeMuted.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setStoredMetronomeVolume(float vol)
{
    storedMetronomeVolume.store(juce::jlimit(0.0f, 1.0f, vol), std::memory_order_relaxed);
    updateMetronomeEngineVolume();
}

float NinjamVst3AudioProcessor::getStoredMetronomeVolume() const
{
    return juce::jlimit(0.0f, 1.0f, storedMetronomeVolume.load(std::memory_order_relaxed));
}

void NinjamVst3AudioProcessor::setMetronomeOutputChannel(int outputChannel)
{
    metronomeOutputChannel.store(sanitiseMetronomeOutputChannel(outputChannel), std::memory_order_relaxed);
    updateMetronomeEngineVolume();
}

int NinjamVst3AudioProcessor::getMetronomeOutputChannel() const
{
    return sanitiseMetronomeOutputChannel(metronomeOutputChannel.load(std::memory_order_relaxed));
}
void NinjamVst3AudioProcessor::resetMetronomeClickVoices()
{
    for (auto& voice : metronomeClickVoices)
        voice = {};
    nextMetronomeClickVoice = 0;
}

void NinjamVst3AudioProcessor::startMetronomeClick(int mode, bool accent, double sampleRate)
{
    if (sampleRate <= 1.0)
        return;

    auto& voice = metronomeClickVoices[(size_t)nextMetronomeClickVoice];
    nextMetronomeClickVoice = (nextMetronomeClickVoice + 1) % maxMetronomeClickVoices;
    voice = {};
    voice.active = true;
    voice.mode = mode;
    voice.accent = accent;
    voice.gain = accent ? 1.0f : 0.76f;

    const double twoPi = juce::MathConstants<double>::twoPi;
    if (mode == (int)MetronomeSoundMode::customFile)
    {
        const double pitchRatio = accent ? 1.122462048309373 : 1.0; // two semitones up on beat 1
        const double sourceRate = juce::jmax(1.0, metronomeCustomSampleRate);
        voice.sampleIncrement = (sourceRate / sampleRate) * pitchRatio;
        voice.durationSamples = metronomeCustomSample.getNumSamples() > 0
            ? (int)std::ceil((double)metronomeCustomSample.getNumSamples() / juce::jmax(0.0001, voice.sampleIncrement))
            : 0;
        voice.gain = accent ? 0.92f : 0.76f;
        if (voice.durationSamples <= 0)
            voice.active = false;
        return;
    }

    if (mode == (int)MetronomeSoundMode::softTick)
    {
        const double freq = accent ? 1760.0 : 1320.0;
        voice.phaseDelta = twoPi * freq / sampleRate;
        voice.phaseDelta2 = twoPi * (freq * 1.52) / sampleRate;
        voice.durationSamples = (int)std::ceil(sampleRate * (accent ? 0.045 : 0.035));
        voice.gain = accent ? 0.26f : 0.18f;
        return;
    }

    if (mode == (int)MetronomeSoundMode::woodTick)
    {
        const double freq = accent ? 930.0 : 690.0;
        voice.phaseDelta = twoPi * freq / sampleRate;
        voice.phaseDelta2 = twoPi * (freq * 1.47) / sampleRate;
        voice.durationSamples = (int)std::ceil(sampleRate * (accent ? 0.070 : 0.055));
        voice.gain = accent ? 0.34f : 0.24f;
        return;
    }

    const double freq = accent ? 1320.0 : 980.0;
    voice.phaseDelta = twoPi * freq / sampleRate;
    voice.durationSamples = (int)std::ceil(sampleRate * (accent ? 0.085 : 0.070));
    voice.gain = accent ? 0.30f : 0.21f;
}

void NinjamVst3AudioProcessor::mixSelectedMetronomeIntoOutputs(float** outputs, int outnch, int numSamples,
                                                               double sampleRate, int transportStartPosition,
                                                               int transportLength, int bpi,
                                                               bool justMonitor)
{
    const int mode = metronomeSoundMode.load(std::memory_order_acquire);
    const int outputChannel = getMetronomeOutputChannel();
    const bool engineMetronomeActive = mode == (int)MetronomeSoundMode::classic && outputChannel == 0;
    if (engineMetronomeActive
        || justMonitor
        || outputs == nullptr
        || outnch <= 0
        || numSamples <= 0
        || sampleRate <= 1.0
        || transportLength <= 0
        || bpi <= 0
        || metronomeMuted.load(std::memory_order_relaxed))
    {
        return;
    }

    const float volume = juce::jlimit(0.0f, 1.0f, storedMetronomeVolume.load(std::memory_order_relaxed));
    if (volume <= 0.0001f)
        return;

    juce::SpinLock::ScopedTryLockType customSampleLock(metronomeCustomSampleLock);
    const bool usingCustomSample = mode == (int)MetronomeSoundMode::customFile;
    if (usingCustomSample
        && (!customSampleLock.isLocked()
            || metronomeCustomSample.getNumSamples() <= 0
            || metronomeCustomSample.getNumChannels() <= 0))
    {
        return;
    }

    const double samplesPerBeat = (double)transportLength / (double)juce::jmax(1, bpi);
    if (samplesPerBeat <= 1.0)
        return;

    auto normaliseTransportPosition = [transportLength](long long value)
    {
        value %= (long long)transportLength;
        if (value < 0)
            value += transportLength;
        return (int)value;
    };

    auto beatIndexForPosition = [samplesPerBeat, bpi](int position)
    {
        return juce::jlimit(0, bpi - 1, (int)std::floor((double)position / samplesPerBeat));
    };

    auto samplesUntilNextBeatBoundary = [transportLength, &beatIndexForPosition](int position)
    {
        const int beat = beatIndexForPosition(position);
        int low = position + 1;
        int high = transportLength;
        while (low < high)
        {
            const int mid = low + (high - low) / 2;
            if (beatIndexForPosition(mid) == beat)
                low = mid + 1;
            else
                high = mid;
        }

        if (low < transportLength)
            return low - position;
        return transportLength - position;
    };

    int transportPosition = normaliseTransportPosition(transportStartPosition);
    const int previousPosition = transportPosition > 0 ? transportPosition - 1 : transportLength - 1;
    const int currentBeat = beatIndexForPosition(transportPosition);
    const int previousBeat = beatIndexForPosition(previousPosition);
    int samplesToNextBeat = (transportPosition < previousPosition || currentBeat != previousBeat)
        ? 0
        : samplesUntilNextBeatBoundary(transportPosition);
    const auto outputRoute = makeMetronomeOutputRoute(outputs, outnch, outputChannel);
    float* left = outputRoute.left;
    float* right = outputRoute.right;
    if (left == nullptr)
        return;

    const int customSampleChannels = usingCustomSample ? metronomeCustomSample.getNumChannels() : 0;
    const int customSampleLength = usingCustomSample ? metronomeCustomSample.getNumSamples() : 0;

    for (int sample = 0; sample < numSamples; ++sample)
    {
        if (samplesToNextBeat <= 0)
        {
            const int beat = beatIndexForPosition(transportPosition);
            startMetronomeClick(mode, beat == 0, sampleRate);
            samplesToNextBeat = samplesUntilNextBeatBoundary(transportPosition);
            if (samplesToNextBeat <= 0)
                samplesToNextBeat = transportLength;
        }

        float mixedLeft = 0.0f;
        float mixedRight = 0.0f;
        for (auto& voice : metronomeClickVoices)
        {
            if (!voice.active)
                continue;

            float voiceLeft = 0.0f;
            float voiceRight = 0.0f;
            if (voice.mode == (int)MetronomeSoundMode::customFile)
            {
                if (!usingCustomSample || customSampleLength <= 0)
                {
                    voice.active = false;
                    continue;
                }

                const int sourceIndex = (int)voice.samplePosition;
                if (sourceIndex >= customSampleLength - 1)
                {
                    voice.active = false;
                    continue;
                }

                const float frac = (float)(voice.samplePosition - (double)sourceIndex);
                const float l0 = metronomeCustomSample.getSample(0, sourceIndex);
                const float l1 = metronomeCustomSample.getSample(0, sourceIndex + 1);
                voiceLeft = l0 + (l1 - l0) * frac;
                if (customSampleChannels > 1)
                {
                    const float r0 = metronomeCustomSample.getSample(1, sourceIndex);
                    const float r1 = metronomeCustomSample.getSample(1, sourceIndex + 1);
                    voiceRight = r0 + (r1 - r0) * frac;
                }
                else
                {
                    voiceRight = voiceLeft;
                }
                voice.samplePosition += voice.sampleIncrement;
            }
            else
            {
                const double t = (double)voice.ageSamples / sampleRate;
                const double attackSamples = juce::jmax(1.0, sampleRate * 0.003);
                const double attack = juce::jlimit(0.0, 1.0, (double)(voice.ageSamples + 1) / attackSamples);
                double env = 0.0;
                double value = 0.0;

                if (voice.mode == (int)MetronomeSoundMode::softTick)
                {
                    env = attack * std::exp(-t * 92.0);
                    value = (std::sin(voice.phase) * 0.78 + std::sin(voice.phase2) * 0.22) * env;
                    voice.phase2 += voice.phaseDelta2;
                }
                else if (voice.mode == (int)MetronomeSoundMode::woodTick)
                {
                    env = attack * std::exp(-t * 58.0);
                    value = (std::sin(voice.phase) * 0.68 + std::sin(voice.phase2) * 0.32) * env;
                    voice.phase2 += voice.phaseDelta2;
                }
                else
                {
                    env = attack * std::exp(-t * 36.0);
                    value = std::sin(voice.phase) * env;
                }

                voice.phase += voice.phaseDelta;
                voiceLeft = (float)value;
                voiceRight = voiceLeft;
            }

            mixedLeft += voiceLeft * voice.gain;
            mixedRight += voiceRight * voice.gain;

            if (++voice.ageSamples >= voice.durationSamples)
                voice.active = false;
        }

        if (mixedLeft != 0.0f || mixedRight != 0.0f)
        {
            left[sample] += mixedLeft * volume;
            if (right != nullptr)
                right[sample] += mixedRight * volume;
            else
                left[sample] += mixedRight * volume;
        }

        if (++transportPosition >= transportLength)
            transportPosition = 0;
        --samplesToNextBeat;
    }
}

bool NinjamVst3AudioProcessor::isOpusSyncAvailable() const
{
    return opusSyncAvailable.load();
}

juce::String NinjamVst3AudioProcessor::getIntervalSyncStatusText() const
{
    const juce::ScopedLock lock(intervalSyncStatusLock);
    return intervalSyncStatusText;
}

void NinjamVst3AudioProcessor::setIntervalSyncStatusText(const juce::String& text)
{
    const juce::ScopedLock lock(intervalSyncStatusLock);
    intervalSyncStatusText = text;
}

void NinjamVst3AudioProcessor::broadcastIntervalSyncTag(const juce::String& target, int markerBeatIndex)
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
        return;

    const int displayInterval = getDisplayIntervalIndex();
    const int bpi = juce::jmax(1, getBPI());
    const float intervalProgress = juce::jlimit(0.0f, 1.0f, getIntervalProgress());
    const int currentBeatIndex = juce::jlimit(0, bpi - 1, (int)std::floor(intervalProgress * (float)bpi));
    const int beatIndex = markerBeatIndex >= 0 ? juce::jlimit(0, bpi - 1, markerBeatIndex) : currentBeatIndex;
    const juce::String userId = normaliseOpusPeerId(currentUser);
    const juce::String tag = buildIntervalSyncTag(displayInterval, bpi);
    const juce::String safeTarget = target.isNotEmpty() ? target : "*";

    // Build the sync tag payload. The "sendOffsetMs" field tells the receiver
    // how many ms after the interval boundary this particular transmission was
    // sent, so the receiver can deduct it from the buffer calculation.
    // The first send has offset 0; redundant retransmissions (mobile hotspot
    // mode) carry their actual offset so the timing math stays correct.
    // Capture by value (copy) so the lambda is safe to call from timer callbacks
    // after this function has returned.
    const juce::String localUserId = userId.isNotEmpty() ? userId : currentUser;
    const long long intervalAbs = intervalIndex.load();
    auto buildPayload = [this, localUserId, tag, displayInterval, intervalAbs, bpi, beatIndex, intervalProgress](double sendOffsetMs) -> juce::String
    {
        juce::DynamicObject::Ptr obj = new juce::DynamicObject();
        obj->setProperty("type", "intervalSyncTag");
        obj->setProperty("userId", localUserId);
        obj->setProperty("tag", tag);
        obj->setProperty("intervalIndex", displayInterval);
        obj->setProperty("intervalAbsolute", intervalAbs);
        obj->setProperty("bpi", bpi);
        obj->setProperty("beatIndex", beatIndex);
        obj->setProperty("intervalProgress", intervalProgress);
        obj->setProperty("sendOffsetMs", sendOffsetMs);
        obj->setProperty("eventId", "intervalTag:" + localUserId + ":" + juce::String(++sideSignalEventCounter));
        return juce::JSON::toString(juce::var(obj.get()));
    };

    // Primary transmission (offset 0)
    const juce::String payload = buildPayload(0.0);
    const bool vdoSyncOn = vdoVideoSyncEnabled.load(std::memory_order_relaxed)
                         && !ninjamZapVideoEnabled.load(std::memory_order_relaxed);
    if (mobileHotspotModeEnabled.load(std::memory_order_relaxed) && vdoSyncOn)
        sendSideSignal(safeTarget, "intervalSyncTag", payload);
    sendIntervalSignal("intervalSyncTag", payload, safeTarget);

    // Mobile hotspot mode: send redundant copies with increasing offsets.
    // The receiver deducts sendOffsetMs from the buffer calculation, so
    // these duplicates are timing-accurate, not stale.
    // We use short offsets (150ms, 300ms) to stay well within the interval
    // even at low BPI/fast BPM.
    // Only send redundant copies when VDO sync is active — otherwise the
    // receiver won't process them and we waste mobile bandwidth.
    if (mobileHotspotModeEnabled.load(std::memory_order_relaxed) && vdoSyncOn)
    {
        const double sendTimeMs = juce::Time::getMillisecondCounterHiRes();

        // Schedule retransmission at +150ms
        juce::Timer::callAfterDelay(150, [this, safeTarget, buildPayload, sendTimeMs]() mutable
        {
            if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK) return;
            const double offsetMs = juce::Time::getMillisecondCounterHiRes() - sendTimeMs;
            const juce::String p1 = buildPayload(offsetMs);
            sendSideSignal(safeTarget, "intervalSyncTag", p1);
            sendIntervalSignal("intervalSyncTag", p1, safeTarget);
        });

        // Schedule retransmission at +300ms
        juce::Timer::callAfterDelay(300, [this, safeTarget, buildPayload, sendTimeMs]() mutable
        {
            if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK) return;
            const double offsetMs = juce::Time::getMillisecondCounterHiRes() - sendTimeMs;
            const juce::String p2 = buildPayload(offsetMs);
            sendSideSignal(safeTarget, "intervalSyncTag", p2);
            sendIntervalSignal("intervalSyncTag", p2, safeTarget);
        });
    }
}

void NinjamVst3AudioProcessor::broadcastTransportProbe(const juce::String& target)
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
        return;

    const juce::String userId = normaliseOpusPeerId(currentUser);
    const juce::String probeId = "probe:" + (userId.isNotEmpty() ? userId : currentUser) + ":" + juce::String(++transportProbeCounter);
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const int displayInterval = getDisplayIntervalIndex();
    const int bpi = juce::jmax(1, getBPI());
    const float intervalProgress = juce::jlimit(0.0f, 1.0f, getIntervalProgress());
    const int beatIndex = juce::jlimit(0, bpi - 1, (int)std::floor(intervalProgress * (float)bpi));
    {
        const juce::ScopedLock lock(intervalSyncAnnouncementLock);
        pendingTransportProbeSentMsById[probeId] = nowMs;
        while ((int)pendingTransportProbeSentMsById.size() > 256)
            pendingTransportProbeSentMsById.erase(pendingTransportProbeSentMsById.begin());
    }

    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("type", "intervalTransportProbe");
    obj->setProperty("userId", userId.isNotEmpty() ? userId : currentUser);
    obj->setProperty("probeId", probeId);
    obj->setProperty("intervalIndex", displayInterval);
    obj->setProperty("intervalAbsolute", intervalIndex.load());
    obj->setProperty("beatIndex", beatIndex);
    obj->setProperty("eventId", "transportProbe:" + probeId);
    const juce::String payload = juce::JSON::toString(juce::var(obj.get()));
    const juce::String safeTarget = target.isNotEmpty() ? target : "*";
    sendIntervalSignal("intervalTransportProbe", payload, safeTarget);
}

void NinjamVst3AudioProcessor::broadcastOpusSyncSupport(const juce::String& target)
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
        return;

    const juce::String userId = normaliseOpusPeerId(currentUser);
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("type", "opusSyncSupport");
    obj->setProperty("supportsOpus", true);
    obj->setProperty("userId", userId.isNotEmpty() ? userId : currentUser);
    obj->setProperty("clientId", opusSyncInstanceId);
    obj->setProperty("appFamily", opusSyncAppFamily);
    obj->setProperty("handshakeVersion", opusSyncHandshakeVersion);
    obj->setProperty("runtimeFormat", getWrapperTypeName(wrapperType));
    obj->setProperty("pluginName", juce::String(JucePlugin_Name));
    obj->setProperty("pluginVersion", getVersionString());
    const int serverMaxLocalChannels = juce::jmax(1, serverMaxLocalChannelsCached.load(std::memory_order_relaxed));
    const bool opusLanesFit = serverMaxLocalChannels > kOpusMultichannelBaseIndex;
    const int maxFittedLocalChannels = opusLanesFit ? maxLocalChannels : 1;
    const int advertisedNumChannels = juce::jlimit(1, maxFittedLocalChannels, getEffectiveLocalChannelCount());
    obj->setProperty("enabled", advertisedNumChannels > 1 && opusLanesFit && isTransmittingLocal());
    obj->setProperty("numChannels", advertisedNumChannels);
    obj->setProperty("packedChannelCount", getConfiguredLocalOpusPackedChannelCount(advertisedNumChannels));
    obj->setProperty("opusBaseChannel", kOpusMultichannelBaseIndex);
    njplus_debug_log("BROADCAST serverMax=%d lanesFit=%d numCh=%d packed=%d enabled=%d base=%d transmit=%d",
                     serverMaxLocalChannels, opusLanesFit, advertisedNumChannels,
                     getConfiguredLocalOpusPackedChannelCount(advertisedNumChannels),
                     advertisedNumChannels > 1 && opusLanesFit && isTransmittingLocal(),
                     kOpusMultichannelBaseIndex, (int)isTransmittingLocal());
    juce::Array<juce::var> channelWidths;
    juce::Array<juce::var> channelNames;
    for (int i = 0; i < advertisedNumChannels; ++i)
    {
        channelWidths.add(getConfiguredLocalOpusWidth(i));
        channelNames.add(getLocalChannelName(i));
    }
    obj->setProperty("channelWidths", juce::var(channelWidths));
    obj->setProperty("channelNames", juce::var(channelNames));
    obj->setProperty("eventId", "opusSupport:" + (userId.isNotEmpty() ? userId : currentUser) + ":" + juce::String(++sideSignalEventCounter));
    const juce::String payload = juce::JSON::toString(juce::var(obj.get()));

    // Prefer the server side-signal path when it is available.
    sendSideSignal(target.isNotEmpty() ? target : "*", "opusSyncSupport", payload);

    // Raw NJ+ discovery and metadata stay exclusively on the hidden control carrier.
    if (kNinjamPlusControlChannelIndex < serverMaxLocalChannels)
        ninjamClient.SendRawIntervalItem(kNinjamPlusControlChannelIndex, kOpusSyncFourcc, payload.toRawUTF8(), (int)payload.getNumBytesAsUTF8());
}

void NinjamVst3AudioProcessor::refreshOpusSyncAvailabilityFromUsers()
{
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    bool available = false;
    int freshPeerCount = 0;
    {
        const juce::ScopedLock lock(opusSyncPeerLock);
        for (auto it = opusSyncPeers.begin(); it != opusSyncPeers.end();)
        {
            const auto& peer = it->second;
            const bool isFresh = (nowMs - peer.lastSeenMs) <= 6500.0;
            if (peer.supportsOpus && isFresh)
                ++it;
            else
                it = opusSyncPeers.erase(it);
        }
        available = !opusSyncPeers.empty();
        freshPeerCount = (int)opusSyncPeers.size();
    }

    // Rebuild the quick username→multiChan snapshot (separate lock, no njclient calls)
    {
        const juce::ScopedLock lock2(opusSyncPeerLock);
        const juce::ScopedLock mcLock(peerMultiChanLock);
        peerMultiChanByName.clear();
        for (auto& [key, peer] : opusSyncPeers)
        {
            if (peer.supportsOpus && !peer.userId.isEmpty())
            {
                const juce::String snapKey = canonicalDelayUserKey(peer.userId);
                PeerMultiChanInfo info;
                info.isMultiChan = peer.multiChanEnabled;
                info.numChannels = peer.numChannels;
                info.opusBaseChannel = peer.opusBaseChannel;
                info.packedChannelCount = peer.packedChannelCount;
                info.channelWidths = peer.channelWidths;
                info.channelNames = peer.channelNames;
                peerMultiChanByName[snapKey] = info;
            }
        }
    }

    const int remoteUserCount = juce::jmax(0, ninjamClient.GetNumUsers());
    const bool hasLegacyClients = remoteUserCount > freshPeerCount;

    const bool previous = opusSyncAvailable.exchange(available);
    const bool previousLegacy = opusSyncHasLegacyClients.exchange(hasLegacyClients);
    if (!previous && available)
    {
        juce::ScopedLock lock(chatLock);
        chatHistory.add("Multi Channel Client Detected.");
        chatSenders.add("");
        chatRevision.fetch_add(1);
        if (chatHistory.size() > 100)
        {
            chatHistory.removeRange(0, chatHistory.size() - 100);
            chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
        }
    }
    if (previous != available || previousLegacy != hasLegacyClients)
    {
        applyCodecPreference();
        syncLocalIntervalChannelConfig();
    }
}

void NinjamVst3AudioProcessor::setTransmitLocal(bool shouldTransmit)
{
    isTransmitting = shouldTransmit;
    applyCodecPreference();
    syncLocalIntervalChannelConfig();
}

void NinjamVst3AudioProcessor::syncLocalIntervalChannelConfig()
{
    const bool shouldTransmit = isTransmitting;
    const int bitrate = shouldTransmit ? localBitrate : 24;
    const int normalFlags = 0;
    const int voiceFlags = 2;
    const bool shouldTransmitVoice = voiceChatMode;
    const int serverMaxLocalChannels = juce::jmax(1, serverMaxLocalChannelsCached.load(std::memory_order_relaxed));
    const bool opusLanesFit = serverMaxLocalChannels > kOpusMultichannelBaseIndex;
    const int maxFittedLocalChannels = opusLanesFit ? maxLocalChannels : 1;
    const int numCh = juce::jlimit(1, maxFittedLocalChannels, getEffectiveLocalChannelCount());
    const bool multiChanAuto = numCh > 1 && opusSyncAvailable.load() && shouldTransmit && opusLanesFit;
    const bool controlChannelAvailable = kNinjamPlusControlChannelIndex < serverMaxLocalChannels;
    const bool singleStereoLocal = numCh == 1 && getLocalChannelInput(0) < 0;
    const int voiceChannel = voiceChatMode ? getVoiceChatNinjamChannelIndex() : -1;
    const bool voiceChannelAvailable = canUseDedicatedVoiceChatChannel()
                                      && voiceChannel >= 0
                                      && voiceChannel < serverMaxLocalChannels;
    const bool voiceFallsBackToMainChannel = voiceChatMode && !voiceChannelAvailable;
    const bool cameraVideoEnabled = ninjamZapCameraSendEnabled.load(std::memory_order_relaxed);
    const int videoChannel = cameraVideoEnabled ? getNinjamZapVideoChannelIndex() : -1;
    const int maxManagedChannel = kOpusMultichannelBaseIndex + maxLocalChannels + 3;

    auto configureControlChannel = [&]
    {
        if (!controlChannelAvailable)
            return;

        ninjamClient.SetLocalChannelInfo(kNinjamPlusControlChannelIndex, "",
            true, 1023, true, 24, true, true, false, 0, true, kNinjamZapVideoOnlyChannelFlag);
        ninjamClient.SetLocalChannelMonitoring(kNinjamPlusControlChannelIndex, false, 0.f, false, 0.f, true, true, false, false);
    };

    auto configureVoiceChannel = [&](int sourceChannel)
    {
        if (!voiceChannelAvailable)
            return;

        ninjamClient.SetLocalChannelInfo(voiceChannel, "Voice",
            true, shouldTransmitVoice ? sourceChannel : 1023,
            true, bitrate, true, true, false, 0, true, voiceFlags);
        ninjamClient.SetLocalChannelMonitoring(voiceChannel, false, 0.f, false, 0.f, true, true, false, false);
    };

    auto shouldKeepManagedChannel = [&](int channelIndex)
    {
        if (channelIndex == 0)
            return true;
        if (controlChannelAvailable && channelIndex == kNinjamPlusControlChannelIndex)
            return true;
        if (voiceChannelAvailable && channelIndex == voiceChannel)
            return true;
        if (videoChannel >= 0 && channelIndex == videoChannel)
            return true;
        if (multiChanAuto && channelIndex == kOpusMultichannelBaseIndex)
            return true;
        return false;
    };

    auto deleteUnusedManagedChannels = [&]
    {
        for (int i = 1; i <= maxManagedChannel; ++i)
            if (!shouldKeepManagedChannel(i))
                ninjamClient.DeleteLocalChannel(i);
    };

    configureControlChannel();

    if (multiChanAuto)
    {
        // ch0: legacy Vorbis mixdown. ch1: hidden NJ+ control. ch2: single Opus multichannel lane.
        juce::String ch0Name = getLocalChannelName(0);
        if (ch0Name.isEmpty()) ch0Name = "Mix";
        const int ch0SourceChannel = 0 | 1024; // stereo mix buffer at inputs[0]/inputs[1] for legacy Vorbis
        ninjamClient.SetLocalChannelInfo(0, ch0Name.toRawUTF8(),
            true, shouldTransmit ? ch0SourceChannel : 1023,
            true, bitrate, true, true, false, 0, true, normalFlags);
        ninjamClient.SetLocalChannelMonitoring(0, false, 0.f, false, 0.f, true, true, false, false);
        ninjamClient.SetLocalChannelInfo(kOpusMultichannelBaseIndex, "Opus",
            true, 0,
            true, bitrate, true, true, false, 0, true, normalFlags);
        ninjamClient.SetLocalChannelOpusSourceChannels(kOpusMultichannelBaseIndex,
                                                       2,
                                                       shouldTransmit ? getConfiguredLocalOpusPackedChannelCount(numCh) : 0);
        ninjamClient.SetLocalChannelMonitoring(kOpusMultichannelBaseIndex, false, 0.f, false, 0.f, true, true, false, false);
        configureVoiceChannel(0);
        configureNinjamZapVideoLocalChannel();
        deleteUnusedManagedChannels();
    }
    else
    {
        // Vorbis only: single channel
        juce::String ch0Name = getLocalChannelName(0);
        if (ch0Name.isEmpty()) ch0Name = "Input";
        const int voiceSourceChannel = singleStereoLocal ? 2 : 1;
        if (voiceFallsBackToMainChannel) ch0Name = "Voice";
        const int sourceChannel = voiceFallsBackToMainChannel
                                ? voiceSourceChannel
                                : (shouldTransmit ? (singleStereoLocal ? 1024 : 0) : 1023);
        const int ch0Flags = voiceFallsBackToMainChannel ? voiceFlags : normalFlags;
        ninjamClient.SetLocalChannelInfo(0, ch0Name.toRawUTF8(),
            true, shouldTransmit || voiceFallsBackToMainChannel ? sourceChannel : 1023,
            true, bitrate, true, true, false, 0, true, ch0Flags);
        ninjamClient.SetLocalChannelMonitoring(0, false, 0.f, false, 0.f, true, true, false, false);
        configureVoiceChannel(voiceSourceChannel);
        configureNinjamZapVideoLocalChannel();
        deleteUnusedManagedChannels();
    }

    if (ninjamClient.GetStatus() == NJClient::NJC_STATUS_OK)
        ninjamClient.NotifyServerOfChannelChange();
}

void NinjamVst3AudioProcessor::setLocalBitrate(int bitrate)
{
    localBitrate = bitrate;
    syncLocalIntervalChannelConfig();
}

int NinjamVst3AudioProcessor::getLocalBitrate() const
{
    return localBitrate;
}

void NinjamVst3AudioProcessor::setVoiceChatMode(bool enabled)
{
    voiceChatMode = enabled;
    applyCodecPreference();
    syncLocalIntervalChannelConfig();

    if (vdoVideoSyncEnabled.load(std::memory_order_relaxed) && videoHelperRunning.load(std::memory_order_relaxed))
    {
        requestVideoBufferRefreshForMeasuredUsers();
        intervalHelperPayloadForceWrite.store(true, std::memory_order_release);
        lastIntervalHelperPayloadWriteMs = 0.0;
    }
}

bool NinjamVst3AudioProcessor::isVoiceChatMode() const
{
    return voiceChatMode;
}

int NinjamVst3AudioProcessor::getVoiceChatNinjamChannelIndex() const
{
    return kVoiceChatChannelIndex;
}

void NinjamVst3AudioProcessor::applyCodecPreference()
{
    const int serverMaxLocalChannels = juce::jmax(1, serverMaxLocalChannelsCached.load(std::memory_order_relaxed));
    const bool opusLanesFit = serverMaxLocalChannels > kOpusMultichannelBaseIndex;
    const int maxFittedLocalChannels = opusLanesFit ? maxLocalChannels : 1;
    const int numCh = juce::jlimit(1, maxFittedLocalChannels, getEffectiveLocalChannelCount());
    const bool multiChanAuto = numCh > 1 && opusSyncAvailable.load() && isTransmittingLocal() && opusLanesFit;
    const int decodeCaps = NJClient::NJCLIENT_CAP_DECODE_VORBIS | NJClient::NJCLIENT_CAP_DECODE_OPUS;
    unsigned int vorbisMask = 0x1u;
    const int voiceChannel = voiceChatMode && canUseDedicatedVoiceChatChannel() ? getVoiceChatNinjamChannelIndex() : -1;
    const unsigned int voiceBit = (voiceChannel > 0 && voiceChannel < 32) ? (1u << voiceChannel) : 0u;
    if (voiceBit != 0u)
        vorbisMask |= voiceBit; // Voice stays Vorbis so stock clients can decode it.

    if (multiChanAuto)
    {
        // ch0: Vorbis mixdown. ch1: hidden control. ch2: single Opus multichannel lane.
        unsigned int opusMask = 0u;
        if (kOpusMultichannelBaseIndex > 0 && kOpusMultichannelBaseIndex < 32)
            opusMask |= (1u << kOpusMultichannelBaseIndex);
        ninjamClient.SetCodecCapabilities(
            NJClient::NJCLIENT_CAP_ENCODE_VORBIS | NJClient::NJCLIENT_CAP_ENCODE_OPUS, decodeCaps);
        ninjamClient.SetCodecConfig(vorbisMask, opusMask);
    }
    else
    {
        // Single channel or no VST3 peers: Vorbis only.
        ninjamClient.SetCodecCapabilities(NJClient::NJCLIENT_CAP_ENCODE_VORBIS, decodeCaps);
        ninjamClient.SetCodecConfig(vorbisMask, 0u);
    }
}

juce::String NinjamVst3AudioProcessor::buildIntervalSyncTag(int interval, int length) const
{
    const juce::String userPart = currentUser.isNotEmpty() ? currentUser : "unknown";
    return userPart + ":" + juce::String(interval) + ":" + juce::String(length);
}

void NinjamVst3AudioProcessor::resetIntervalSyncTimingCache()
{
    lastBroadcastIntervalTag.store(-1);
    lastProcessedIntervalMarkerKey.store(-1);
    lastLatencyTimingBpi = -1;
    lastLatencyTimingLength = -1;
    lastLatencyTimingBpm = -1.0;
    lastIntervalHelperPayloadWriteMs = 0.0;

    const juce::ScopedLock lock(intervalSyncAnnouncementLock);
    lastAnnouncedRemoteIntervalByUser.clear();
    localIntervalStartMsByInterval.clear();
    pendingRemoteIntervalStartsByUser.clear();
    pendingTransportProbeSentMsById.clear();
    remoteLatencyLastAppliedIntervalByUser.clear();
    remoteLatencyAverageByUser.clear();
    remoteLatencyFirmDelayMsByUser.clear();
    remoteVideoBufferRefreshIdByUser.clear();
    videoBufferRefreshCounter = 0;
    lastRemoteServerLatencyMsByUser.clear();
    remoteServerRouteLatencyMsByUser.clear();
    lastRemoteIntervalSignalSeenMsByUser.clear();
    lastRemoteRouteProbeSeenMsByUser.clear();
    recentVideoTimingChangeEventIds.clear();
}

void NinjamVst3AudioProcessor::invalidateIntervalSyncLatencyState(bool keepRemoteServerLatency)
{
    lastBroadcastIntervalTag.store(-1);
    lastProcessedIntervalMarkerKey.store(-1);
    const juce::ScopedLock lock(intervalSyncAnnouncementLock);
    lastAnnouncedRemoteIntervalByUser.clear();
    localIntervalStartMsByInterval.clear();
    pendingRemoteIntervalStartsByUser.clear();
    pendingTransportProbeSentMsById.clear();
    remoteLatencyLastAppliedIntervalByUser.clear();
    remoteLatencyAverageByUser.clear();
    if (!keepRemoteServerLatency)
    {
        remoteLatencyFirmDelayMsByUser.clear();
        remoteVideoBufferRefreshIdByUser.clear();
        videoBufferRefreshCounter = 0;
        lastRemoteServerLatencyMsByUser.clear();
        remoteServerRouteLatencyMsByUser.clear();
        lastRemoteIntervalSignalSeenMsByUser.clear();
        lastRemoteRouteProbeSeenMsByUser.clear();
    }
}

bool NinjamVst3AudioProcessor::consumeVideoTimingChangeEvent(const juce::String& eventId)
{
    if (eventId.isEmpty())
        return true;

    const juce::ScopedLock lock(intervalSyncAnnouncementLock);
    for (const auto& existing : recentVideoTimingChangeEventIds)
    {
        if (existing == eventId)
            return false;
    }

    recentVideoTimingChangeEventIds.push_back(eventId);
    while (recentVideoTimingChangeEventIds.size() > 128)
        recentVideoTimingChangeEventIds.pop_front();
    return true;
}

void NinjamVst3AudioProcessor::broadcastVideoTimingChange(double previousBpm, double newBpm, int bpi, int length, int timingDelayDeltaMs)
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
        return;
    if (!vdoVideoSyncEnabled.load(std::memory_order_relaxed)
        || ninjamZapVideoEnabled.load(std::memory_order_relaxed))
        return;

    const juce::String userId = normaliseOpusPeerId(currentUser);
    const juce::String senderKey = userId.isNotEmpty() ? userId : currentUser.trim();
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("appFamily", opusSyncAppFamily);
    obj->setProperty("userId", senderKey);
    obj->setProperty("previousBpm", previousBpm);
    obj->setProperty("bpm", newBpm);
    obj->setProperty("bpi", bpi);
    obj->setProperty("length", length);
    obj->setProperty("timingDelayDeltaMs", timingDelayDeltaMs);
    obj->setProperty("intervalIndex", getDisplayIntervalIndex());
    obj->setProperty("intervalAbsolute", intervalIndex.load());
    obj->setProperty("eventId", "videoTimingChange:" + senderKey + ":" + juce::String(++sideSignalEventCounter));
    sendIntervalSignal("videoTimingChange", juce::JSON::toString(juce::var(obj.get())));
}

void NinjamVst3AudioProcessor::requestVideoBufferRefreshForMeasuredUsers()
{
    if (!vdoVideoSyncEnabled.load(std::memory_order_relaxed) || !videoHelperRunning.load())
        return;

    bool refreshed = false;
    {
        const juce::ScopedLock lock(intervalSyncAnnouncementLock);
        if (!remoteLatencyFirmDelayMsByUser.empty())
        {
            const auto refreshId = ++videoBufferRefreshCounter;
            for (const auto& userDelay : remoteLatencyFirmDelayMsByUser)
            {
                if (userDelay.first.isEmpty())
                    continue;

                remoteVideoBufferRefreshIdByUser[userDelay.first] = refreshId;
                refreshed = true;
            }
        }
    }

    if (refreshed)
    {
        intervalHelperPayloadForceWrite.store(true, std::memory_order_release);
        lastIntervalHelperPayloadWriteMs = 0.0;
    }
}

void NinjamVst3AudioProcessor::pruneDisconnectedRemoteSyncState()
{
    std::set<juce::String> activeUserKeys;
    const int numUsers = juce::jmax(0, ninjamClient.GetNumUsers());
    for (int userIdx = 0; userIdx < numUsers; ++userIdx)
    {
        const char* userNameChars = ninjamClient.GetUserState(userIdx, nullptr, nullptr, nullptr);
        if (userNameChars == nullptr || userNameChars[0] == '\0')
            continue;

        const juce::String userName = juce::String::fromUTF8(userNameChars);
        if (isKnownNinjamServerBotUser(userName))
            continue;

        const juce::String senderKey = normaliseOpusPeerId(userName);
        const juce::String canonicalUserKey = canonicalDelayUserKey(userName);
        if (senderKey.isNotEmpty())
            activeUserKeys.insert(senderKey);
        if (canonicalUserKey.isNotEmpty())
            activeUserKeys.insert(canonicalUserKey);
    }

    const auto isActiveUserKey = [&activeUserKeys](const juce::String& key) -> bool
    {
        return key.isNotEmpty() && activeUserKeys.find(key) != activeUserKeys.end();
    };

    const juce::ScopedLock lock(intervalSyncAnnouncementLock);

    for (auto it = lastAnnouncedRemoteIntervalByUser.begin(); it != lastAnnouncedRemoteIntervalByUser.end();)
    {
        if (!isActiveUserKey(it->first))
            it = lastAnnouncedRemoteIntervalByUser.erase(it);
        else
            ++it;
    }

    for (auto it = lastRemoteServerLatencyMsByUser.begin(); it != lastRemoteServerLatencyMsByUser.end();)
    {
        if (!isActiveUserKey(it->first))
            it = lastRemoteServerLatencyMsByUser.erase(it);
        else
            ++it;
    }

    for (auto it = remoteServerRouteLatencyMsByUser.begin(); it != remoteServerRouteLatencyMsByUser.end();)
    {
        if (!isActiveUserKey(it->first))
            it = remoteServerRouteLatencyMsByUser.erase(it);
        else
            ++it;
    }

    for (auto it = lastRemoteIntervalSignalSeenMsByUser.begin(); it != lastRemoteIntervalSignalSeenMsByUser.end();)
    {
        if (!isActiveUserKey(it->first))
            it = lastRemoteIntervalSignalSeenMsByUser.erase(it);
        else
            ++it;
    }

    for (auto it = lastRemoteRouteProbeSeenMsByUser.begin(); it != lastRemoteRouteProbeSeenMsByUser.end();)
    {
        if (!isActiveUserKey(it->first))
            it = lastRemoteRouteProbeSeenMsByUser.erase(it);
        else
            ++it;
    }

    for (auto it = remoteLatencyLastAppliedIntervalByUser.begin(); it != remoteLatencyLastAppliedIntervalByUser.end();)
    {
        if (!isActiveUserKey(it->first))
            it = remoteLatencyLastAppliedIntervalByUser.erase(it);
        else
            ++it;
    }

    for (auto it = remoteLatencyAverageByUser.begin(); it != remoteLatencyAverageByUser.end();)
    {
        if (!isActiveUserKey(it->first))
            it = remoteLatencyAverageByUser.erase(it);
        else
            ++it;
    }

    for (auto it = remoteLatencyFirmDelayMsByUser.begin(); it != remoteLatencyFirmDelayMsByUser.end();)
    {
        if (!isActiveUserKey(it->first))
            it = remoteLatencyFirmDelayMsByUser.erase(it);
        else
            ++it;
    }

    for (auto it = remoteVideoBufferRefreshIdByUser.begin(); it != remoteVideoBufferRefreshIdByUser.end();)
    {
        if (!isActiveUserKey(it->first))
            it = remoteVideoBufferRefreshIdByUser.erase(it);
        else
            ++it;
    }

    for (auto it = pendingRemoteIntervalStartsByUser.begin(); it != pendingRemoteIntervalStartsByUser.end();)
    {
        const juce::String pendingSenderKey = it->second.senderKey.isNotEmpty()
            ? it->second.senderKey
            : it->first.upToFirstOccurrenceOf(":", false, false);

        if (!isActiveUserKey(pendingSenderKey))
            it = pendingRemoteIntervalStartsByUser.erase(it);
        else
            ++it;
    }
}

static juce::File getThisModuleFile()
{
#ifdef _WIN32
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&getThisModuleFile,
                            &hm))
        return {};

    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(hm, path, (DWORD)std::size(path)) == 0)
        return {};
    return juce::File(juce::String(path));
#else
    Dl_info info {};
    if (dladdr((void*)&getThisModuleFile, &info) == 0 || info.dli_fname == nullptr)
        return {};
    return juce::File(juce::String::fromUTF8(info.dli_fname));
#endif
}

bool NinjamVst3AudioProcessor::isAdvancedVideoClientAvailable(int port) const
{
    if (port <= 0)
        return false;

    // Use a fast localhost TCP probe; if connect succeeds, helper is up.
    juce::StreamingSocket sock;
    if (!sock.connect("127.0.0.1", port, 500))
        return false;
    return true;
}

bool NinjamVst3AudioProcessor::ensureAdvancedVideoClientStarted()
{
    const int existingPort = advancedVideoHelperPort.load();
    if (advancedVideoServer != nullptr && existingPort > 0 && isAdvancedVideoClientAvailable(existingPort))
    {
        videoHelperRunning.store(true);
        requestVideoBufferRefreshForMeasuredUsers();
        lastIntervalHelperPayloadWriteMs = 0.0;
        return true;
    }

    if (!advancedVideoServer)
    {
        advancedVideoServer = std::make_unique<LocalVideoHttpServer>(
            [this]()
            {
                const juce::ScopedLock lock(intervalHelperPayloadLock);
                return intervalHelperPayload;
            },
            [this]()
            {
                return buildZapVideoFrameListJson();
            },
            [this](const juce::String& streamKey, int frameIndex, juce::MemoryBlock& jpegData)
            {
                return getZapVideoFrameJpeg(streamKey, frameIndex, jpegData);
            },
            [this](const juce::String& codecName)
            {
                return enableNinjamZapBrowserCameraSendForHelper(codecName);
            },
            [this]()
            {
                return buildNinjamZapBrowserCameraStateJson();
            },
            [this]()
            {
                stopNinjamZapCameraSend();
                return juce::String("{\"ok\":true}");
            },
            [this](const juce::MemoryBlock& encodedFrame,
                   const juce::String& codecName,
                   const juce::String& configBase64,
                   bool keyFrame,
                   double browserAgeMs,
                   double encodeMs,
                   int width,
                   int height)
            {
                return handleBrowserNinjamZapCameraFrame(encodedFrame, codecName, configBase64, keyFrame, browserAgeMs, encodeMs, width, height);
            });
    }

    const int preferredPort = existingPort > 0 ? existingPort : advancedVideoHelperBasePort;
    if (!advancedVideoServer->start(preferredPort, advancedVideoHelperMaxPort))
    {
        advancedVideoServer.reset();
        advancedVideoHelperPort.store(0);
        return false;
    }

    const int helperPort = advancedVideoServer->getPort();
    advancedVideoHelperPort.store(helperPort);
    for (int i = 0; i < 10; ++i)
    {
        juce::Thread::sleep(50);
        if (isAdvancedVideoClientAvailable(helperPort))
        {
            videoHelperRunning.store(true);
            requestVideoBufferRefreshForMeasuredUsers();
            lastIntervalHelperPayloadWriteMs = 0.0;
            return true;
        }
    }

    if (advancedVideoServer)
        advancedVideoServer->stop();
    advancedVideoServer.reset();
    advancedVideoHelperPort.store(0);
    videoHelperRunning.store(false);
    lastIntervalHelperPayloadWriteMs = 0.0;
    return false;
}

bool NinjamVst3AudioProcessor::ensureZapVideoClientStarted()
{
    const int existingPort = advancedVideoHelperPort.load();
    if (advancedVideoServer != nullptr && existingPort > 0 && isAdvancedVideoClientAvailable(existingPort))
    {
        videoHelperRunning.store(true);
        return true;
    }

    return ensureAdvancedVideoClientStarted();
}

namespace
{
    struct VdoRoomResolution
    {
        juce::String serverKey;
        juce::String room;
        juce::String suggestedRoom;
        bool needsPrompt = false;
    };

    juce::String makeUrlSafeVdoRoomToken(const juce::String& raw);
    juce::String makeVdoRoomSettingsKey(const juce::String& serverKey);
    VdoRoomResolution resolveVdoRoomForServer(const juce::String& roomSource);
}

void NinjamVst3AudioProcessor::beginVideoLaunchWorker(juce::String room)
{
    const juce::ScopedLock launchLock(videoLaunchWorkerLock);
    if (videoLaunchFuture.valid())
        videoLaunchFuture.wait();

    videoLaunchFuture = std::async(std::launch::async, [this, room = std::move(room)]()
    {
        struct VideoLaunchScope
        {
            explicit VideoLaunchScope(std::atomic<bool>& inProgress) : flag(inProgress) {}
            ~VideoLaunchScope() { flag.store(false); }

            std::atomic<bool>& flag;
        } scope(videoLaunchInProgress);

        try
        {
            launchVideoSession(room);
        }
        catch (...)
        {
            juce::ScopedLock lock(chatLock);
            chatHistory.add("Video launch failed unexpectedly; opening direct VDO link may be unavailable.");
            chatSenders.add("");
            chatRevision.fetch_add(1);
            if (chatHistory.size() > 100)
            {
                chatHistory.removeRange(0, chatHistory.size() - 100);
                chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
            }
        }
    });
}

juce::String NinjamVst3AudioProcessor::getVdoRoomNameForServer(const juce::String& serverKey) const
{
    const juce::String key = serverKey.trim().toLowerCase();
    if (key.isEmpty())
        return {};

    const juce::ScopedLock lock(vdoRoomLock);
    return announcedVdoRoomServerKey == key ? announcedVdoRoomName : juce::String();
}

bool NinjamVst3AudioProcessor::rememberVdoRoomNameForServer(const juce::String& serverKey, const juce::String& room, bool ownedLocally)
{
    const juce::String key = serverKey.trim().toLowerCase();
    const juce::String cleanRoom = makeUrlSafeVdoRoomToken(room);
    if (key.isEmpty() || cleanRoom.isEmpty())
        return false;

    const juce::ScopedLock lock(vdoRoomLock);
    const bool changed = announcedVdoRoomServerKey != key
                      || announcedVdoRoomName != cleanRoom
                      || announcedVdoRoomOwnedLocally != ownedLocally;
    announcedVdoRoomServerKey = key;
    announcedVdoRoomName = cleanRoom;
    announcedVdoRoomOwnedLocally = ownedLocally;
    if (changed)
        lastVdoRoomAnnouncementMs = 0.0;
    return changed;
}

juce::String NinjamVst3AudioProcessor::loadSavedVdoRoomNameForServer(const juce::String& serverKey) const
{
    const juce::String key = makeVdoRoomSettingsKey(serverKey);
    if (key.isEmpty())
        return {};

    juce::PropertiesFile settings(makeNinjamplusSettingsOptions());
    return makeUrlSafeVdoRoomToken(settings.getValue(key, {}));
}

void NinjamVst3AudioProcessor::saveVdoRoomNameForServer(const juce::String& serverKey, const juce::String& room)
{
    const juce::String key = makeVdoRoomSettingsKey(serverKey);
    const juce::String cleanRoom = makeUrlSafeVdoRoomToken(room);
    if (key.isEmpty() || cleanRoom.isEmpty())
        return;

    juce::PropertiesFile settings(makeNinjamplusSettingsOptions());
    settings.setValue(key, cleanRoom);
    settings.setValue(key + ".owner", true);
    settings.saveIfNeeded();
}

bool NinjamVst3AudioProcessor::canChangeVdoRoomName()
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
        return false;

    const VdoRoomResolution roomResolution = resolveVdoRoomForServer(currentServer);
    {
        const juce::ScopedLock lock(vdoRoomLock);
        if (announcedVdoRoomServerKey == roomResolution.serverKey
            && announcedVdoRoomName.isNotEmpty()
            && announcedVdoRoomOwnedLocally)
            return true;
    }

    const juce::String key = makeVdoRoomSettingsKey(roomResolution.serverKey);
    if (key.isEmpty())
        return false;

    juce::PropertiesFile settings(makeNinjamplusSettingsOptions());
    return settings.getBoolValue(key + ".owner", false)
        && makeUrlSafeVdoRoomToken(settings.getValue(key, {})).isNotEmpty();
}

void NinjamVst3AudioProcessor::promptToChangeVdoRoomNameAsync()
{
    if (!canChangeVdoRoomName())
        return;

    const VdoRoomResolution roomResolution = resolveVdoRoomForServer(currentServer);
    juce::String currentRoom = getVdoRoomNameForServer(roomResolution.serverKey);
    if (currentRoom.isEmpty())
        currentRoom = loadSavedVdoRoomNameForServer(roomResolution.serverKey);
    if (currentRoom.isEmpty())
        currentRoom = roomResolution.suggestedRoom;

    auto* editor = dynamic_cast<juce::Component*>(getActiveEditor());
    juce::Component::SafePointer<juce::Component> safeEditor(editor);
    if (safeEditor == nullptr)
        return;

    juce::MessageManager::callAsync([this, safeEditor, roomResolution, currentRoom]() mutable
    {
        if (safeEditor == nullptr)
            return;

        auto* alert = new juce::AlertWindow("Change VDO Room",
                                            "Enter the VDO room name everyone on this NINJAM server should use.",
                                            juce::AlertWindow::QuestionIcon);
        alert->addTextEditor("room", currentRoom, "Room:");
        alert->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
        alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        alert->enterModalState(true,
                               juce::ModalCallbackFunction::create(
                                   [this, safeEditor, alert, roomResolution](int result) mutable
                                   {
                                       std::unique_ptr<juce::AlertWindow> alertOwner(alert);
                                       if (safeEditor == nullptr || result != 1)
                                           return;

                                       const juce::String selectedRoom = makeUrlSafeVdoRoomToken(alert->getTextEditorContents("room"));
                                       if (selectedRoom.isEmpty())
                                       {
                                           addSystemChatLine("VDO room was not changed: room name was empty.");
                                           return;
                                       }

                                       rememberVdoRoomNameForServer(roomResolution.serverKey, selectedRoom, true);
                                       saveVdoRoomNameForServer(roomResolution.serverKey, selectedRoom);
                                       announceVdoRoomName(roomResolution.serverKey, selectedRoom);
                                       addSystemChatLine("VDO room changed to " + selectedRoom + ". Other NINJAMplus users on this server will use it.");

                                       if (vdoVideoSyncEnabled.load(std::memory_order_relaxed)
                                           && !ninjamZapVideoEnabled.load(std::memory_order_relaxed))
                                           launchVideoSessionAsync();
                                   }),
                               false);
    });
}

void NinjamVst3AudioProcessor::announceVdoRoomName(const juce::String& serverKey, const juce::String& room)
{
    const juce::String key = serverKey.trim().toLowerCase();
    const juce::String cleanRoom = makeUrlSafeVdoRoomToken(room);
    if (key.isEmpty() || cleanRoom.isEmpty())
        return;

    juce::DynamicObject::Ptr payloadObj = new juce::DynamicObject();
    payloadObj->setProperty("appFamily", opusSyncAppFamily);
    payloadObj->setProperty("userId", normaliseOpusPeerId(currentUser));
    payloadObj->setProperty("scope", "currentNinjamSession");
    payloadObj->setProperty("room", cleanRoom);
    const juce::String payload = juce::JSON::toString(juce::var(payloadObj.get()), false);

    sendSideSignal("*", "vdoRoom", payload);
    sendIntervalSignal("vdoRoom", payload);
}

void NinjamVst3AudioProcessor::launchVideoSessionAsync()
{
    bool expected = false;
    if (!videoLaunchInProgress.compare_exchange_strong(expected, true))
        return;

    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
    {
        beginVideoLaunchWorker({});
        return;
    }

    const VdoRoomResolution roomResolution = resolveVdoRoomForServer(currentServer);
    juce::String room = getVdoRoomNameForServer(roomResolution.serverKey);
    if (room.isEmpty() && roomResolution.needsPrompt)
    {
        room = loadSavedVdoRoomNameForServer(roomResolution.serverKey);
        if (room.isNotEmpty())
            rememberVdoRoomNameForServer(roomResolution.serverKey, room, true);
    }
    if (room.isEmpty() && !roomResolution.needsPrompt)
        room = roomResolution.room;

    if (room.isNotEmpty())
    {
        beginVideoLaunchWorker(room);
        return;
    }

    auto* editor = dynamic_cast<juce::Component*>(getActiveEditor());
    juce::Component::SafePointer<juce::Component> safeEditor(editor);
    if (safeEditor == nullptr)
    {
        room = roomResolution.suggestedRoom;
        rememberVdoRoomNameForServer(roomResolution.serverKey, room, true);
        saveVdoRoomNameForServer(roomResolution.serverKey, room);
        announceVdoRoomName(roomResolution.serverKey, room);
        beginVideoLaunchWorker(room);
        return;
    }

    juce::MessageManager::callAsync([this, safeEditor, roomResolution]() mutable
    {
        if (safeEditor == nullptr)
        {
            videoLaunchInProgress.store(false);
            return;
        }

        auto* alert = new juce::AlertWindow("VDO Room Name",
                                            "This server does not provide a public room name. Enter the VDO room name everyone on this NINJAM server should use.",
                                            juce::AlertWindow::QuestionIcon);
        alert->addTextEditor("room", roomResolution.suggestedRoom, "Room:");
        alert->addButton("Join", 1, juce::KeyPress(juce::KeyPress::returnKey));
        alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        alert->enterModalState(true,
                               juce::ModalCallbackFunction::create(
                                   [this, safeEditor, alert, roomResolution](int result) mutable
                                   {
                                       std::unique_ptr<juce::AlertWindow> alertOwner(alert);
                                       if (safeEditor == nullptr)
                                       {
                                           videoLaunchInProgress.store(false);
                                           return;
                                       }

                                       if (result != 1)
                                       {
                                           videoLaunchInProgress.store(false);
                                           return;
                                       }

                                       const juce::String selectedRoom = makeUrlSafeVdoRoomToken(alert->getTextEditorContents("room"));
                                       if (selectedRoom.isEmpty())
                                       {
                                           addSystemChatLine("VDO launch cancelled: room name was empty.");
                                           videoLaunchInProgress.store(false);
                                           return;
                                       }

                                       rememberVdoRoomNameForServer(roomResolution.serverKey, selectedRoom, true);
                                       saveVdoRoomNameForServer(roomResolution.serverKey, selectedRoom);
                                       announceVdoRoomName(roomResolution.serverKey, selectedRoom);
                                       addSystemChatLine("VDO room set to " + selectedRoom + ". Other NINJAMplus users on this server will use it.");
                                       beginVideoLaunchWorker(selectedRoom);
                                   }),
                               false);
    });
}

bool NinjamVst3AudioProcessor::isNinjamZapVideoAvailable()
{
    return ninjamClient.GetStatus() == NJClient::NJC_STATUS_OK
        && ninjamZapServerVideoSupported.load(std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::isNinjamZapServerSupported() const
{
    return ninjamZapServerVideoSupported.load(std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::isNinjamZapVideoEnabled() const
{
    return ninjamZapVideoEnabled.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::noteRemoteVideoRoomActivity(double nowMs)
{
    if (nowMs <= 0.0)
        nowMs = juce::Time::getMillisecondCounterHiRes();
    lastRemoteVideoRoomActivityMs.store(nowMs, std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::shouldPulseVideoRoomButton() const
{
    if (vdoVideoSyncEnabled.load(std::memory_order_relaxed)
        || videoLaunchInProgress.load(std::memory_order_relaxed)
        || ninjamZapVideoEnabled.load(std::memory_order_relaxed)
        || ninjamZapCameraSendEnabled.load(std::memory_order_relaxed)
        || ninjamZapBrowserCameraSendEnabled.load(std::memory_order_relaxed))
        return false;

    const double lastActivityMs = lastRemoteVideoRoomActivityMs.load(std::memory_order_relaxed);
    if (lastActivityMs <= 0.0)
        return false;

    const double ageMs = juce::Time::getMillisecondCounterHiRes() - lastActivityMs;
    return ageMs >= 0.0 && ageMs <= 30000.0;
}

void NinjamVst3AudioProcessor::addSystemChatLine(const juce::String& message)
{
    juce::ScopedLock lock(chatLock);
    chatHistory.add(message);
    chatSenders.add("");
    chatRevision.fetch_add(1);
    if (chatHistory.size() > 100)
    {
        chatHistory.removeRange(0, chatHistory.size() - 100);
        chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
    }
}

bool NinjamVst3AudioProcessor::isKnownOpusMultichannelLane(int userIndex, int channelIndex)
{
    if (channelIndex <= 0)
        return false;

    const char* name = ninjamClient.GetUserState(userIndex, nullptr, nullptr, nullptr);
    if (name == nullptr)
        return false;

    const juce::String normName = canonicalDelayUserKey(makeShortUserName(juce::String::fromUTF8(name)));
    const juce::ScopedLock mcLock(peerMultiChanLock);
    auto it = peerMultiChanByName.find(normName);
    if (it == peerMultiChanByName.end() || !it->second.isMultiChan)
        return false;

    const int opusBaseChannel = juce::jlimit(1, 31, it->second.opusBaseChannel);
    return channelIndex == opusBaseChannel;
}

bool NinjamVst3AudioProcessor::isNinjamRemoteChannelVideoOnly(int userIndex, int channelIndex)
{
    int flags = 0;
    const char* chName = ninjamClient.GetUserChannelState(userIndex, channelIndex,
                                                          nullptr, nullptr, nullptr,
                                                          nullptr, nullptr, nullptr,
                                                          &flags);
    if (chName == nullptr || ((flags & kNinjamZapVideoOnlyChannelFlag) == 0))
        return false;

    return !isKnownOpusMultichannelLane(userIndex, channelIndex);
}


bool NinjamVst3AudioProcessor::isRemoteUserVoiceChatMode(int userIndex)
{
    for (int channelIndex = 0; channelIndex < 32; ++channelIndex)
    {
        int flags = 0;
        const char* channelName = ninjamClient.GetUserChannelState(userIndex, channelIndex,
                                                                   nullptr, nullptr, nullptr,
                                                                   nullptr, nullptr, nullptr,
                                                                   &flags);
        if (channelName != nullptr
            && ((flags & kNinjamZapVideoOnlyChannelFlag) == 0)
            && ((flags & 2) != 0))
        {
            return true;
        }
    }
    return false;
}

bool NinjamVst3AudioProcessor::isRemoteOpusMultichannelPeer(int userIndex) const
{
    if (userIndex < 0 || userIndex >= maxRemoteChordUsers)
        return false;
    return remoteOpusPeerActive[(size_t)userIndex].load(std::memory_order_relaxed)
        && remoteOpusCarrierChannel[(size_t)userIndex].load(std::memory_order_relaxed) >= 0;
}

int NinjamVst3AudioProcessor::syncNinjamZapVideoSubscriptions(bool subscribe)
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
        return 0;

    int videoChannelCount = 0;

    const int numUsers = ninjamClient.GetNumUsers();
    for (int userIndex = 0; userIndex < numUsers; ++userIndex)
    {
        for (int channelIndex = 0; channelIndex < 32; ++channelIndex)
        {
            bool isSubscribed = false;
            int flags = 0;
            const char* chName = ninjamClient.GetUserChannelState(userIndex, channelIndex,
                                                                  &isSubscribed,
                                                                  nullptr, nullptr,
                                                                  nullptr, nullptr,
                                                                  nullptr,
                                                                  &flags);
            if (chName == nullptr || ((flags & kNinjamZapVideoOnlyChannelFlag) == 0))
                continue;
            if (channelIndex == kNinjamPlusControlChannelIndex)
                continue;
            if (isKnownOpusMultichannelLane(userIndex, channelIndex))
                continue;

            ++videoChannelCount;
            if (isSubscribed != subscribe)
            {
                ninjamClient.SetUserChannelState(userIndex, channelIndex,
                                                 true, subscribe,
                                                 false, 0.0f,
                                                 false, 0.0f,
                                                 false, false,
                                                 false, false);
            }
        }
    }

    return videoChannelCount;
}

int NinjamVst3AudioProcessor::ensureRawIntervalSyncFallbackSubscriptions()
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
        return 0;

    int changed = 0;
    const int serverMaxLocalChannels = juce::jmax(1, serverMaxLocalChannelsCached.load(std::memory_order_relaxed));
    const int numUsers = ninjamClient.GetNumUsers();
    for (int userIndex = 0; userIndex < numUsers; ++userIndex)
    {
        auto ensureSubscription = [&](int channelIndex, int desiredFlags, const char* desiredName, bool allowExistingVisibleChannel)
        {
            if (channelIndex < 0 || channelIndex >= serverMaxLocalChannels || channelIndex >= 32)
                return;

            bool isSubscribed = false;
            int flags = 0;
            const char* chName = ninjamClient.GetUserChannelState(userIndex, channelIndex,
                                                                  &isSubscribed,
                                                                  nullptr, nullptr,
                                                                  nullptr, nullptr,
                                                                  nullptr,
                                                                  &flags);
            if (chName != nullptr && !allowExistingVisibleChannel && ((flags & kNinjamZapVideoOnlyChannelFlag) == 0))
                return;

            const bool needsPresent = chName == nullptr || flags != desiredFlags;
            if (!isSubscribed || needsPresent)
            {
                ninjamClient.SetUserChannelSubscriptionRaw(userIndex, channelIndex, true, true, desiredFlags, desiredName);
                ++changed;
                njplus_debug_log("SUB user=%d chan=%d name='%s' flags=%d", userIndex, channelIndex, desiredName ? desiredName : "", desiredFlags);
            }
        };

        const char* userNameRaw = ninjamClient.GetUserState(userIndex);
        if (userNameRaw != nullptr)
        {
            int advertisedControlFlags = 0;
            const char* advertisedControlName = nullptr;
            if (kNinjamPlusControlChannelIndex < serverMaxLocalChannels)
            {
                advertisedControlName = ninjamClient.GetUserChannelState(userIndex,
                                                                          kNinjamPlusControlChannelIndex,
                                                                          nullptr, nullptr, nullptr,
                                                                          nullptr, nullptr, nullptr,
                                                                          &advertisedControlFlags);
            }
            const bool hasAdvertisedControlCarrier = advertisedControlName != nullptr
                && ((advertisedControlFlags & kNinjamZapVideoOnlyChannelFlag) != 0);
            const juce::String normName = canonicalDelayUserKey(juce::String::fromUTF8(userNameRaw));
            bool isKnownNjPlusUser = false;
            PeerMultiChanInfo peerInfo;
            bool hasPeerInfo = false;
            {
                const juce::ScopedLock mcLock(peerMultiChanLock);
                auto it = peerMultiChanByName.find(normName);
                isKnownNjPlusUser = (it != peerMultiChanByName.end());
                if (it != peerMultiChanByName.end() && it->second.isMultiChan)
                {
                    peerInfo = it->second;
                    hasPeerInfo = true;
                }
            }

            // Subscribe only when the peer actually advertised the hidden control flag
            // (or was already identified as NJ+). Legacy clients never advertise this
            // carrier, so their channel state is left untouched.
            njplus_debug_log("SUBSCAN user=%d name='%s' hasPeerInfo=%d multiChan=%d controlCarrier=%d isKnownNJPlus=%d", userIndex, userNameRaw, hasPeerInfo, peerInfo.isMultiChan, hasAdvertisedControlCarrier, isKnownNjPlusUser);
            if (hasAdvertisedControlCarrier || isKnownNjPlusUser)
                ensureSubscription(kNinjamPlusControlChannelIndex, kNinjamZapVideoOnlyChannelFlag, "", false);

            if (hasPeerInfo)
            {
                const int opusBaseChannel = juce::jlimit(1, 31, peerInfo.opusBaseChannel);
                ensureSubscription(opusBaseChannel, 0, "Opus", true);

                bool ch0Subscribed = false;
                const char* ch0Name = ninjamClient.GetUserChannelState(userIndex, 0,
                                                                       &ch0Subscribed,
                                                                       nullptr, nullptr,
                                                                       nullptr, nullptr,
                                                                       nullptr,
                                                                       nullptr);
                if (ch0Name != nullptr && ch0Subscribed)
                {
                    ninjamClient.SetUserChannelState(userIndex, 0,
                                                     true, false,
                                                     false, 0.0f,
                                                     false, 0.0f,
                                                     false, false,
                                                     false, false);
                    ++changed;
                    njplus_debug_log("SUB user=%d ch0 UNSUBSCRIBED (multichannel peer)", userIndex);
                }

                const int peerChannels = juce::jlimit(1, maxLocalChannels, peerInfo.numChannels);
                for (int ch = 1; ch < peerChannels; ++ch)
                {
                    const int njChannel = opusBaseChannel + ch;
                    bool subscribed = false;
                    const char* laneName = ninjamClient.GetUserChannelState(userIndex, njChannel,
                                                                            &subscribed,
                                                                            nullptr, nullptr,
                                                                            nullptr, nullptr,
                                                                            nullptr,
                                                                            nullptr);
                    if (laneName != nullptr && subscribed)
                    {
                        ninjamClient.SetUserChannelState(userIndex, njChannel,
                                                         true, false,
                                                         false, 0.0f,
                                                         false, 0.0f,
                                                         false, false,
                                                         false, false);
                        ++changed;
                        njplus_debug_log("SUB user=%d lane chan=%d UNSUBSCRIBED", userIndex, njChannel);
                    }
                }
            }
        }

        // Keep channel 0 subscribed for legacy builds that still emitted raw sync there.
        bool ch0Subscribed = false;
        const char* ch0Name = ninjamClient.GetUserChannelState(userIndex, 0,
                                                               &ch0Subscribed,
                                                               nullptr, nullptr,
                                                               nullptr, nullptr,
                                                               nullptr,
                                                               nullptr);
        if (ch0Name != nullptr && !ch0Subscribed)
        {
            ninjamClient.SetUserChannelState(userIndex, 0,
                                             true, true,
                                             false, 0.0f,
                                             false, 0.0f,
                                             false, false,
                                             false, false);
            ++changed;
        }
    }

    return changed;
}

void NinjamVst3AudioProcessor::launchNinjamZapVideoSession()
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
    {
        addSystemChatLine("Connect to a server first, then click Video Room.");
        return;
    }

    if (!ninjamZapServerVideoSupported.load(std::memory_order_relaxed))
    {
        addSystemChatLine("This server does not advertise NINJAMZap video support.");
        return;
    }

    if (stopVdoVideoSync())
        addSystemChatLine("VDO sync disabled while NINJAMZap video is active.");

    ninjamZapVideoEnabled.store(true, std::memory_order_relaxed);
    ninjamZapVideoReceivedNotice.store(false, std::memory_order_relaxed);
    {
        const juce::ScopedLock lock(ninjamZapVideoChunkLock);
        ninjamZapVideoChunkReassemblers.clear();
        ninjamZapVideoAudioGuidByReassemblyKey.clear();
        ninjamZapVideoMarkerIntervalByReassemblyKey.clear();
        ninjamZapVideoMarkerSeenByReassemblyKey.clear();
    }
    const int videoChannels = syncNinjamZapVideoSubscriptions(true);

    juce::String message = "NINJAMZap video transport enabled";
    if (videoChannels > 0)
        message << " (" << videoChannels << " video channel" << (videoChannels == 1 ? "" : "s") << " subscribed)";
    else
        message << " (waiting for video channels)";
    message << ". Zap codec support: " << ninjamplus::zap::getCodecCapabilitySummary() << ".";
    addSystemChatLine(message);

    if (ensureZapVideoClientStarted())
    {
        const int helperPort = advancedVideoHelperPort.load();
        const juce::String helperUrlText = "http://127.0.0.1:" + juce::String(helperPort) + "/zap-video";
        if (!openUrlExternalOnMessageThread(helperUrlText))
            addSystemChatLine("Failed to open Zap video helper URL: " + helperUrlText);
    }
    else
    {
        addSystemChatLine("Zap video transport is enabled, but the local video helper page could not be started.");
    }
}

juce::StringArray NinjamVst3AudioProcessor::getNinjamZapCameraDevices() const
{
#if JUCE_USE_CAMERA && (JUCE_WINDOWS || JUCE_MAC)
    return juce::CameraDevice::getAvailableDevices();
#else
    return {};
#endif
}

ninjamplus::zap::CameraCodecPreference NinjamVst3AudioProcessor::getNinjamZapCameraCodecPreference() const
{
    const int value = ninjamZapCameraCodecPreference.load(std::memory_order_relaxed);
    if (value == (int)ninjamplus::zap::CameraCodecPreference::h264)
        return ninjamplus::zap::CameraCodecPreference::h264;
    if (value == (int)ninjamplus::zap::CameraCodecPreference::mjpeg)
        return ninjamplus::zap::CameraCodecPreference::mjpeg;
    if (value == (int)ninjamplus::zap::CameraCodecPreference::h264Hardware)
        return ninjamplus::zap::CameraCodecPreference::h264Hardware;
    if (value == (int)ninjamplus::zap::CameraCodecPreference::h264Software)
        return ninjamplus::zap::CameraCodecPreference::h264Software;
    return ninjamplus::zap::CameraCodecPreference::autoCodec;
}

void NinjamVst3AudioProcessor::setNinjamZapCameraCodecPreference(ninjamplus::zap::CameraCodecPreference preference)
{
    ninjamZapCameraCodecPreference.store((int)preference, std::memory_order_relaxed);
}

ninjamplus::zap::VideoCodec NinjamVst3AudioProcessor::getNinjamZapCameraActiveCodec() const
{
    const int value = ninjamZapCameraActiveCodec.load(std::memory_order_relaxed);
    if (value == (int)ninjamplus::zap::VideoCodec::h264)
        return ninjamplus::zap::VideoCodec::h264;
    if (value == (int)ninjamplus::zap::VideoCodec::vp8)
        return ninjamplus::zap::VideoCodec::vp8;
    if (value == (int)ninjamplus::zap::VideoCodec::vp9)
        return ninjamplus::zap::VideoCodec::vp9;
    return ninjamplus::zap::VideoCodec::mjpeg;
}

int NinjamVst3AudioProcessor::getNinjamZapVideoChannelIndex() const
{
    const int serverMaxLocalChannels = juce::jmax(1, serverMaxLocalChannelsCached.load(std::memory_order_relaxed));
    const bool opusLanesFit = serverMaxLocalChannels > kOpusMultichannelBaseIndex;
    const int maxFittedLocalChannels = opusLanesFit ? maxLocalChannels : 1;
    const int numCh = juce::jlimit(1, maxFittedLocalChannels, getEffectiveLocalChannelCount());
    const bool multiChanAuto = numCh > 1 && opusSyncAvailable.load() && isTransmittingLocal() && opusLanesFit;
    return multiChanAuto ? kNinjamPlusControlChannelIndex
                         : kOpusMultichannelBaseIndex;
}

void NinjamVst3AudioProcessor::configureNinjamZapVideoLocalChannel()
{
    const bool cameraVideoEnabled = ninjamZapCameraSendEnabled.load(std::memory_order_relaxed);
    if (!cameraVideoEnabled)
        return;

    const int videoChannel = getNinjamZapVideoChannelIndex();
    if (videoChannel < 0 || videoChannel >= ninjamClient.GetMaxLocalChannels())
    {
        return;
    }

    ninjamClient.SetLocalChannelInfo(videoChannel,
                                     "Video",
                                     true,
                                     1023,
                                     true,
                                     0,
                                     true,
                                     true,
                                     false,
                                     0,
                                     true,
                                     kNinjamZapVideoOnlyChannelFlag);
    ninjamClient.SetLocalChannelMonitoring(videoChannel,
                                           false,
                                           0.0f,
                                           false,
                                           0.0f,
                                           true,
                                           true,
                                           false,
                                           false);
}

void NinjamVst3AudioProcessor::startNinjamZapCameraSend()
{
    startNinjamZapCameraSend(0);
}

void NinjamVst3AudioProcessor::startNinjamZapCameraSend(int deviceIndex)
{
    startNinjamZapCameraSend(deviceIndex, getNinjamZapCameraCodecPreference());
}

void NinjamVst3AudioProcessor::startNinjamZapCameraSend(int deviceIndex, ninjamplus::zap::CameraCodecPreference preference)
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
    {
        addSystemChatLine("Connect to a server first, then start Zap camera.");
        return;
    }

    if (!ninjamZapServerVideoSupported.load(std::memory_order_relaxed))
    {
        addSystemChatLine("This server does not advertise NINJAMZap video support.");
        return;
    }

    if (!isTransmittingLocal())
    {
        addSystemChatLine("Turn on local transmit before starting NINJAMZap camera video.");
        return;
    }

    const int videoChannel = getNinjamZapVideoChannelIndex();
    if (videoChannel < 0 || videoChannel >= ninjamClient.GetMaxLocalChannels())
    {
        addSystemChatLine("This server only allows " + juce::String(ninjamClient.GetMaxLocalChannels())
                          + " local channel" + (ninjamClient.GetMaxLocalChannels() == 1 ? "" : "s")
                          + "; NINJAMZap camera needs one extra video channel.");
        return;
    }

    if (stopVdoVideoSync())
        addSystemChatLine("VDO sync disabled while NINJAMZap camera is active.");

    if (zapCameraSender == nullptr)
        zapCameraSender = std::make_unique<ZapCameraSender>(*this);

    ninjamZapBrowserCameraSendEnabled.store(false, std::memory_order_relaxed);
    setNinjamZapCameraCodecPreference(preference);
    ninjamZapCameraActiveCodec.store((int)ninjamplus::zap::VideoCodec::mjpeg, std::memory_order_relaxed);
    {
        const juce::ScopedLock lock(zapVideoFrameLock);
        ninjamZapCameraH264ConfigChunk.reset();
    }

    if (!zapCameraSender->start(deviceIndex, preference))
    {
        addSystemChatLine("Could not open a camera for NINJAMZap video.");
        zapCameraSender.reset();
        return;
    }

    ninjamZapCameraSendEnabled.store(true, std::memory_order_relaxed);
    ninjamZapVideoEnabled.store(true, std::memory_order_relaxed);
    {
        const juce::ScopedLock clientLock(ninjamClientLock);
        configureNinjamZapVideoLocalChannel();
        if (ninjamClient.GetStatus() == NJClient::NJC_STATUS_OK)
        {
            ninjamClient.NotifyServerOfChannelChange();
        }
    }

    const auto activeCodec = getNinjamZapCameraActiveCodec();
    addSystemChatLine("NINJAMZap camera sending enabled (" + ninjamplus::zap::getCodecName(activeCodec) + ").");
}

void NinjamVst3AudioProcessor::startNinjamZapBrowserCameraSend()
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
    {
        addSystemChatLine("Connect to a server first, then start Zap browser camera.");
        return;
    }

    if (!ninjamZapServerVideoSupported.load(std::memory_order_relaxed))
    {
        addSystemChatLine("This server does not advertise NINJAMZap video support.");
        return;
    }

    if (!isTransmittingLocal())
    {
        addSystemChatLine("Turn on local transmit before starting NINJAMZap browser camera.");
        return;
    }

    const int videoChannel = getNinjamZapVideoChannelIndex();
    if (videoChannel < 0 || videoChannel >= ninjamClient.GetMaxLocalChannels())
    {
        addSystemChatLine("This server only allows " + juce::String(ninjamClient.GetMaxLocalChannels())
                          + " local channel" + (ninjamClient.GetMaxLocalChannels() == 1 ? "" : "s")
                          + "; NINJAMZap browser camera needs one extra video channel.");
        return;
    }

    if (stopVdoVideoSync())
        addSystemChatLine("VDO sync disabled while NINJAMZap browser camera is active.");

    if (zapCameraSender != nullptr)
    {
        zapCameraSender->stop();
        zapCameraSender.reset();
    }

    ninjamZapBrowserCameraSendEnabled.store(true, std::memory_order_relaxed);
    ninjamZapCameraSendEnabled.store(true, std::memory_order_relaxed);
    ninjamZapVideoEnabled.store(true, std::memory_order_relaxed);
    ninjamZapCameraActiveCodec.store((int)ninjamplus::zap::VideoCodec::mjpeg, std::memory_order_relaxed);
    {
        const juce::ScopedLock lock(zapVideoFrameLock);
        ninjamZapCameraH264ConfigChunk.reset();
    }
    {
        const juce::SpinLock::ScopedLockType lock(ninjamZapCameraChunkQueueLock);
        pendingNinjamZapCameraChunks.clear();
    }

    {
        const juce::ScopedLock clientLock(ninjamClientLock);
        configureNinjamZapVideoLocalChannel();
        if (ninjamClient.GetStatus() == NJClient::NJC_STATUS_OK)
            ninjamClient.NotifyServerOfChannelChange();
    }

    if (ensureZapVideoClientStarted())
    {
        const int helperPort = advancedVideoHelperPort.load();
        const juce::String helperUrlText = "http://127.0.0.1:" + juce::String(helperPort) + "/zap-video?browserCamera=1";
        if (!openUrlExternalOnMessageThread(helperUrlText))
            addSystemChatLine("Failed to open Zap browser camera helper URL: " + helperUrlText);
        else
            addSystemChatLine("NINJAMZap browser camera enabled. Select and start your camera in the browser page.");
    }
    else
    {
        addSystemChatLine("NINJAMZap browser camera is enabled, but the local video helper page could not be started.");
    }
}

void NinjamVst3AudioProcessor::stopNinjamZapCameraSend()
{
    const bool wasEnabled = ninjamZapCameraSendEnabled.exchange(false, std::memory_order_relaxed);
    const bool wasBrowserEnabled = ninjamZapBrowserCameraSendEnabled.exchange(false, std::memory_order_relaxed);
    if (!wasEnabled && !wasBrowserEnabled && zapCameraSender == nullptr)
        return;

    closeNinjamZapVideoIntervalStream();
    {
        const juce::SpinLock::ScopedLockType lock(ninjamZapCameraChunkQueueLock);
        pendingNinjamZapCameraChunks.clear();
    }
    if (zapCameraSender != nullptr)
        zapCameraSender->stop();
    zapCameraSender.reset();
    ninjamZapCameraActiveCodec.store((int)ninjamplus::zap::VideoCodec::mjpeg, std::memory_order_relaxed);
    ninjamZapBrowserAwaitingIntervalKeyframe.store(false, std::memory_order_relaxed);
    {
        const juce::ScopedLock lock(zapVideoFrameLock);
        ninjamZapCameraH264ConfigChunk.reset();
    }
    syncLocalIntervalChannelConfig();
    if (wasEnabled)
        addSystemChatLine("NINJAMZap camera sending disabled.");
}

bool NinjamVst3AudioProcessor::isNinjamZapCameraSending() const
{
    return ninjamZapCameraSendEnabled.load(std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::isNinjamZapBrowserCameraSending() const
{
    return ninjamZapBrowserCameraSendEnabled.load(std::memory_order_relaxed);
}

juce::String NinjamVst3AudioProcessor::enableNinjamZapBrowserCameraSendForHelper(const juce::String& codecName)
{
    const auto requestedCodec = parseZapBrowserCodec(codecName);
    auto fail = [](const juce::String& message)
    {
        return juce::String("{\"ok\":false,\"error\":")
            + juce::JSON::toString(juce::var(message), true) + "}";
    };

    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
        return fail("Connect to a server first, then start the Zap camera.");

    if (!ninjamZapServerVideoSupported.load(std::memory_order_relaxed))
        return fail("This server does not advertise NINJAMZap video support.");

    if (!isTransmittingLocal())
        return fail("Turn on local transmit before starting NINJAMZap browser camera.");

    const int videoChannel = getNinjamZapVideoChannelIndex();
    if (videoChannel < 0 || videoChannel >= ninjamClient.GetMaxLocalChannels())
    {
        return fail("This server only allows " + juce::String(ninjamClient.GetMaxLocalChannels())
                    + " local channel" + (ninjamClient.GetMaxLocalChannels() == 1 ? "" : "s")
                    + "; NINJAMZap browser camera needs one extra video channel.");
    }

    stopVdoVideoSync();

    if (zapCameraSender != nullptr)
    {
        zapCameraSender->stop();
        zapCameraSender.reset();
    }

    ninjamZapBrowserCameraSendEnabled.store(true, std::memory_order_relaxed);
    ninjamZapCameraSendEnabled.store(true, std::memory_order_relaxed);
    ninjamZapVideoEnabled.store(true, std::memory_order_relaxed);
    ninjamZapVideoReceivedNotice.store(false, std::memory_order_relaxed);
    ninjamZapCameraActiveCodec.store((int)requestedCodec, std::memory_order_relaxed);
    ninjamZapBrowserAwaitingIntervalKeyframe.store(false, std::memory_order_relaxed);
    {
        const juce::ScopedLock lock(zapVideoFrameLock);
        ninjamZapCameraH264ConfigChunk.reset();
    }
    {
        const juce::SpinLock::ScopedLockType lock(ninjamZapCameraChunkQueueLock);
        pendingNinjamZapCameraChunks.clear();
    }

    syncNinjamZapVideoSubscriptions(true);
    {
        const juce::ScopedLock clientLock(ninjamClientLock);
        configureNinjamZapVideoLocalChannel();
        if (ninjamClient.GetStatus() == NJClient::NJC_STATUS_OK)
            ninjamClient.NotifyServerOfChannelChange();
    }

    return juce::String("{\"ok\":true,\"channelIndex\":") + juce::String(videoChannel)
        + ",\"codec\":\"" + zapBrowserCodecName(requestedCodec) + "\"}";
}

juce::String NinjamVst3AudioProcessor::buildNinjamZapBrowserCameraStateJson() const
{
    const auto codec = getNinjamZapCameraActiveCodec();
    int h264ConfigBytes = 0;
    {
        const juce::ScopedLock lock(zapVideoFrameLock);
        h264ConfigBytes = (int) ninjamZapCameraH264ConfigChunk.getSize();
    }
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("ok", true);
    obj->setProperty("enabled", ninjamZapCameraSendEnabled.load(std::memory_order_relaxed)
                              && ninjamZapBrowserCameraSendEnabled.load(std::memory_order_relaxed));
    obj->setProperty("streamOpen", ninjamZapVideoStreamOpen.load(std::memory_order_relaxed));
    obj->setProperty("codec", zapBrowserCodecName(codec));
    obj->setProperty("keyframeRequestId", juce::String((juce::int64)ninjamZapBrowserKeyframeRequestCounter.load(std::memory_order_relaxed)));
    obj->setProperty("h264ConfigBytes", h264ConfigBytes);
    obj->setProperty("hasH264Config", h264ConfigBytes > 0);
    return juce::JSON::toString(juce::var(obj.get()), false);
}

void NinjamVst3AudioProcessor::beginNinjamZapVideoIntervalStream(const unsigned char audioGuid[16], int intervalCounter)
{
    if (!ninjamZapCameraSendEnabled.load(std::memory_order_relaxed)
        || ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK
        || ninjamZapVideoStreamOpen.load(std::memory_order_relaxed))
        return;

    const int videoChannel = getNinjamZapVideoChannelIndex();
    if (videoChannel < 0 || videoChannel >= ninjamClient.GetMaxLocalChannels())
        return;

    if (audioGuid == nullptr)
        return;

    std::array<unsigned char, 16> guid {};
    const auto activeCodec = getNinjamZapCameraActiveCodec();
    unsigned int videoFourcc = kNinjamZapVideoMjpgFourcc;
    if (activeCodec == ninjamplus::zap::VideoCodec::h264)
        videoFourcc = kNinjamZapVideoH264Fourcc;
    else if (activeCodec == ninjamplus::zap::VideoCodec::vp8)
        videoFourcc = kNinjamZapVideoVp8Fourcc;
    else if (activeCodec == ninjamplus::zap::VideoCodec::vp9)
        videoFourcc = kNinjamZapVideoVp9Fourcc;
    const int beginResult = ninjamClient.BeginRawIntervalStream(videoChannel,
                                                               videoFourcc,
                                                               guid.data());
    if (beginResult != 0)
        return;

    {
        const juce::SpinLock::ScopedLockType lock(ninjamZapCameraChunkQueueLock);
        ninjamZapVideoStreamGuid = guid;
        ninjamZapVideoStreamOpen.store(true, std::memory_order_relaxed);
    }

    if (activeCodec == ninjamplus::zap::VideoCodec::h264
        || activeCodec == ninjamplus::zap::VideoCodec::vp8
        || activeCodec == ninjamplus::zap::VideoCodec::vp9)
    {
        ninjamZapBrowserKeyframeRequestCounter.fetch_add(1, std::memory_order_relaxed);
        ninjamZapBrowserAwaitingIntervalKeyframe.store(true, std::memory_order_relaxed);
        // Force the local H.264 encoder to produce an IDR on the next frame,
        // as required by the NinjamZap receiver (first frame after BEGIN must
        // be a keyframe so decoders can sync at each interval boundary).
        if (activeCodec == ninjamplus::zap::VideoCodec::h264)
            ninjamZapForceNextKeyframe.store(true, std::memory_order_relaxed);
    }
    else
    {
        ninjamZapBrowserAwaitingIntervalKeyframe.store(false, std::memory_order_relaxed);
    }

    juce::MemoryBlock markerChunk;
    if (ninjamplus::zap::makeSyncMarkerChunk((juce::uint32)juce::jmax(0, intervalCounter),
                                             audioGuid,
                                             markerChunk))
    {
        const int writeResult = ninjamClient.WriteRawIntervalChunk(ninjamZapVideoStreamGuid.data(),
                                                                   markerChunk.getData(),
                                                                   (int)markerChunk.getSize());
        juce::ignoreUnused(writeResult);
    }

    if (activeCodec == ninjamplus::zap::VideoCodec::h264)
    {
        juce::MemoryBlock configChunk;
        {
            const juce::ScopedLock lock(zapVideoFrameLock);
            configChunk = ninjamZapCameraH264ConfigChunk;
        }

        if (configChunk.getSize() > 0)
        {
            const int writeResult = ninjamClient.WriteRawIntervalChunk(ninjamZapVideoStreamGuid.data(),
                                                                       configChunk.getData(),
                                                                       (int)configChunk.getSize());
            juce::ignoreUnused(writeResult);
        }
    }
}

void NinjamVst3AudioProcessor::closeNinjamZapVideoIntervalStream()
{
    std::array<unsigned char, 16> closingGuid {};
    {
        const juce::SpinLock::ScopedLockType lock(ninjamZapCameraChunkQueueLock);
        if (!ninjamZapVideoStreamOpen.exchange(false, std::memory_order_relaxed))
            return;
        closingGuid = ninjamZapVideoStreamGuid;
        ninjamZapVideoStreamGuid.fill(0);
    }

    if (closingGuid == std::array<unsigned char, 16> {})
        return;

    if (ninjamClient.GetStatus() == NJClient::NJC_STATUS_OK)
    {
        const int endResult = ninjamClient.EndRawIntervalStream(closingGuid.data());
        juce::ignoreUnused(endResult);
    }
}

void NinjamVst3AudioProcessor::requestNinjamZapVideoIntervalRotateFromAudioThread()
{
    if (!ninjamZapCameraSendEnabled.load(std::memory_order_relaxed))
        return;

    pendingNinjamZapIntervalRotate.store(true, std::memory_order_release);
}

void NinjamVst3AudioProcessor::processPendingNinjamZapVideoIntervalRotate()
{
    if (!pendingNinjamZapIntervalRotate.exchange(false, std::memory_order_acquire))
        return;

    if (!ninjamZapCameraSendEnabled.load(std::memory_order_relaxed))
        return;

    unsigned char audioGuid[16] {};
    if (!ninjamClient.GetLocalChannelCurrentGuid(0, audioGuid))
        return;

    rotateNinjamZapVideoIntervalStream(audioGuid, getDisplayIntervalIndex());
}

void NinjamVst3AudioProcessor::rotateNinjamZapVideoIntervalStream(const unsigned char audioGuid[16], int intervalCounter)
{
    if (!ninjamZapCameraSendEnabled.load(std::memory_order_relaxed))
        return;

    closeNinjamZapVideoIntervalStream();
    {
        const juce::SpinLock::ScopedLockType lock(ninjamZapCameraChunkQueueLock);
        pendingNinjamZapCameraChunks.clear();
    }
    beginNinjamZapVideoIntervalStream(audioGuid, intervalCounter);
}

void NinjamVst3AudioProcessor::enqueueNinjamZapCameraFrameChunk(juce::MemoryBlock chunk)
{
    if (chunk.getSize() == 0)
        return;

    const juce::SpinLock::ScopedLockType lock(ninjamZapCameraChunkQueueLock);
    if (!ninjamZapVideoStreamOpen.load(std::memory_order_relaxed))
        return;

    static constexpr size_t maxPendingZapCameraChunks = 360;
    while (pendingNinjamZapCameraChunks.size() >= maxPendingZapCameraChunks)
        pendingNinjamZapCameraChunks.erase(pendingNinjamZapCameraChunks.begin());

    PendingNinjamZapCameraChunk pending;
    pending.videoGuid = ninjamZapVideoStreamGuid;
    pending.chunk = std::move(chunk);
    pendingNinjamZapCameraChunks.push_back(std::move(pending));
}

void NinjamVst3AudioProcessor::broadcastNinjamZapVideoTiming(double captureQueueMs, double encodeMs)
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK
        || !ninjamZapCameraSendEnabled.load(std::memory_order_relaxed))
        return;

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const double previousMs = lastNinjamZapVideoTimingBroadcastMs.load(std::memory_order_relaxed);
    if (previousMs > 0.0 && (nowMs - previousMs) < 250.0)
        return;
    lastNinjamZapVideoTimingBroadcastMs.store(nowMs, std::memory_order_relaxed);

    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("appFamily", opusSyncAppFamily);
    obj->setProperty("userId", currentUser);
    obj->setProperty("channelIndex", getNinjamZapVideoChannelIndex());
    obj->setProperty("captureQueueMs", juce::jlimit(0.0, 500.0, captureQueueMs));
    obj->setProperty("encodeMs", juce::jlimit(0.0, 500.0, encodeMs));
    obj->setProperty("eventId", "zapVideoTiming:" + currentUser + ":" + juce::String(++sideSignalEventCounter));
    sendSideSignal("*", "zapVideoTiming", juce::JSON::toString(juce::var(obj.get()), false));
}

void NinjamVst3AudioProcessor::flushPendingNinjamZapCameraVideo(int maxChunksToFlush)
{
    if (!ninjamZapCameraSendEnabled.load(std::memory_order_relaxed)
        || ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
        return;

    maxChunksToFlush = juce::jlimit(1, 32, maxChunksToFlush);

    std::vector<PendingNinjamZapCameraChunk> chunks;
    {
        const juce::SpinLock::ScopedLockType lock(ninjamZapCameraChunkQueueLock);
        const int chunksToFlush = juce::jmin(maxChunksToFlush, (int)pendingNinjamZapCameraChunks.size());
        if (chunksToFlush <= 0)
            return;

        chunks.reserve((size_t)chunksToFlush);
        for (int i = 0; i < chunksToFlush; ++i)
            chunks.push_back(std::move(pendingNinjamZapCameraChunks[(size_t)i]));
        pendingNinjamZapCameraChunks.erase(pendingNinjamZapCameraChunks.begin(),
                                           pendingNinjamZapCameraChunks.begin() + chunksToFlush);
    }

    for (const auto& pending : chunks)
    {
        if (pending.videoGuid == std::array<unsigned char, 16> {})
            continue;

        const auto& chunk = pending.chunk;
        const int writeResult = ninjamClient.WriteRawIntervalChunk(pending.videoGuid.data(),
                                                                   chunk.getData(),
                                                                   (int)chunk.getSize());
        juce::ignoreUnused(writeResult);
    }
}

bool NinjamVst3AudioProcessor::handleBrowserNinjamZapCameraFrame(const juce::MemoryBlock& encodedFrame,
                                                                 const juce::String& codecName,
                                                                 const juce::String& configBase64,
                                                                 bool keyFrame,
                                                                 double browserAgeMs,
                                                                 double encodeMs,
                                                                 int width,
                                                                 int height)
{
    juce::ignoreUnused(width, height);
    const auto requestedCodec = parseZapBrowserCodec(codecName);
    const bool hasH264Config = requestedCodec == ninjamplus::zap::VideoCodec::h264
        && configBase64.trim().isNotEmpty();

    if (!ninjamZapCameraSendEnabled.load(std::memory_order_relaxed)
        || !ninjamZapBrowserCameraSendEnabled.load(std::memory_order_relaxed))
        return false;

    if (getNinjamZapCameraActiveCodec() != requestedCodec)
        return false;

    const bool hasConfigOnly = hasH264Config
        && encodedFrame.getSize() == 0;

    if ((!hasConfigOnly && encodedFrame.getSize() == 0)
        || encodedFrame.getSize() > ninjamplus::zap::kZapMaxChunkPayloadBytes)
        return false;

    if (requestedCodec == ninjamplus::zap::VideoCodec::mjpeg && encodedFrame.getSize() < 128)
        return false;

    const double safeBrowserAgeMs = std::isfinite(browserAgeMs) ? juce::jmax(0.0, browserAgeMs) : 0.0;
    const bool lateFrame = safeBrowserAgeMs > 650.0;
    if (lateFrame && !hasH264Config)
        return true;

    juce::MemoryBlock configChunkToSend;
    juce::MemoryBlock localH264ConfigInner;
    if (requestedCodec == ninjamplus::zap::VideoCodec::h264 && configBase64.trim().isNotEmpty())
    {
        juce::MemoryBlock configInner;
        juce::MemoryOutputStream configStream(configInner, false);
        if (juce::Base64::convertFromBase64(configStream, configBase64)
            && configInner.getSize() > 0
            && configInner.getSize() <= ninjamplus::zap::kZapMaxChunkPayloadBytes)
        {
            juce::MemoryBlock normalisedConfig;
            if (!normaliseNinjamZapH264ConfigPayload(configInner, normalisedConfig))
            {
                normalisedConfig.reset();
            }

            juce::MemoryBlock configChunk;
            if (normalisedConfig.getSize() > 0
                && ninjamplus::zap::appendLengthPrefixedChunk(normalisedConfig.getData(),
                                                              normalisedConfig.getSize(),
                                                              configChunk))
            {
                localH264ConfigInner = normalisedConfig;
                {
                    const juce::ScopedLock lock(zapVideoFrameLock);
                    ninjamZapCameraH264ConfigChunk = configChunk;
                }
                configChunkToSend = std::move(configChunk);
            }
        }
    }

    if (requestedCodec == ninjamplus::zap::VideoCodec::h264 && configChunkToSend.getSize() == 0)
    {
        const juce::ScopedLock lock(zapVideoFrameLock);
        if (ninjamZapCameraH264ConfigChunk.getSize() > 0)
            configChunkToSend = ninjamZapCameraH264ConfigChunk;
    }

    auto publishLocalBrowserPayload = [&](const juce::MemoryBlock& payload)
    {
        if (payload.getSize() == 0)
            return;

        ZapVideoDecodeJob job;
        job.streamKey = "local:zap-camera";
        job.sender = "Local camera";
        job.channelIndex = getNinjamZapVideoChannelIndex();
        job.codec = requestedCodec;
        job.payload.append(payload.getData(), payload.getSize());
        job.receivedMs = juce::Time::getMillisecondCounterHiRes();
        publishBrowserDecodedZapVideoFrame(job);
    };

    if (localH264ConfigInner.getSize() > 0)
        publishLocalBrowserPayload(localH264ConfigInner);

    const bool predictiveCodec = requestedCodec == ninjamplus::zap::VideoCodec::h264
                              || requestedCodec == ninjamplus::zap::VideoCodec::vp8
                              || requestedCodec == ninjamplus::zap::VideoCodec::vp9;
    const bool effectiveKeyFrame = keyFrame
        || (requestedCodec == ninjamplus::zap::VideoCodec::h264
            && encodedFrame.getSize() > 0
            && h264AvccFrameContainsIdr(encodedFrame));
    const bool intervalWaitingForKey = predictiveCodec
        && ninjamZapBrowserAwaitingIntervalKeyframe.load(std::memory_order_relaxed);
    const bool streamOpen = ninjamZapVideoStreamOpen.load(std::memory_order_relaxed);

    if (streamOpen
        && configChunkToSend.getSize() > 0
        && (hasConfigOnly || lateFrame || (intervalWaitingForKey && !effectiveKeyFrame)))
    {
        enqueueNinjamZapCameraFrameChunk(configChunkToSend);
    }

    if (hasConfigOnly || lateFrame)
        return true;

    if (intervalWaitingForKey && !effectiveKeyFrame)
    {
        return true;
    }

    if (intervalWaitingForKey && effectiveKeyFrame)
    {
        ninjamZapBrowserAwaitingIntervalKeyframe.store(false, std::memory_order_relaxed);
    }

    publishLocalBrowserPayload(encodedFrame);
    broadcastNinjamZapVideoTiming(safeBrowserAgeMs, std::isfinite(encodeMs) ? juce::jmax(0.0, encodeMs) : 0.0);

    if (streamOpen)
    {
        juce::MemoryBlock chunk;
        if (!ninjamplus::zap::appendLengthPrefixedChunk(encodedFrame.getData(), encodedFrame.getSize(), chunk))
            return false;

        if (configChunkToSend.getSize() > 0)
        {
            juce::MemoryBlock combinedChunk;
            combinedChunk.append(configChunkToSend.getData(), configChunkToSend.getSize());
            combinedChunk.append(chunk.getData(), chunk.getSize());
            enqueueNinjamZapCameraFrameChunk(std::move(combinedChunk));
        }
        else
        {
            enqueueNinjamZapCameraFrameChunk(std::move(chunk));
        }
    }

    return true;
}

void NinjamVst3AudioProcessor::startZapVideoDecodeWorker()
{
    if (zapVideoDecodeWorker == nullptr)
        zapVideoDecodeWorker = std::make_unique<ZapVideoDecodeWorker>(*this);
}

void NinjamVst3AudioProcessor::stopZapVideoDecodeWorker()
{
    if (zapVideoDecodeWorker != nullptr)
        zapVideoDecodeWorker->stop();
    zapVideoDecodeWorker.reset();
}

void NinjamVst3AudioProcessor::enqueueZapVideoDecodeJob(ZapVideoDecodeJob job)
{
    startZapVideoDecodeWorker();
    if (zapVideoDecodeWorker != nullptr)
        zapVideoDecodeWorker->enqueue(std::move(job));
}

void NinjamVst3AudioProcessor::publishBrowserDecodedZapVideoFrame(const ZapVideoDecodeJob& job)
{
    if (job.streamKey.isEmpty() || job.payload.getSize() == 0)
        return;

    juce::MemoryBlock encodedData;
    encodedData.append(job.payload.getData(), job.payload.getSize());

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const double receiveToPublishMs = job.receivedMs > 0.0
        ? juce::jmax(0.0, nowMs - job.receivedMs)
        : 0.0;

    juce::uint64 configId = 0;
    {
        const juce::ScopedLock lock(zapVideoFrameLock);
        if (job.codec == ninjamplus::zap::VideoCodec::h264)
        {
            juce::MemoryBlock configInner;
            bool configOnlyChunk = false;
            if (isNinjamZapH264ConfigChunk(encodedData)
                || extractNinjamZapH264ConfigFromLength16Nals(encodedData, configInner)
                || extractNinjamZapH264ConfigFromAvcDecoderConfig(encodedData, configInner))
            {
                configOnlyChunk = true;
                if (configInner.getSize() == 0)
                    normaliseNinjamZapH264ConfigPayload(encodedData, configInner);
            }
            else
            {
                extractNinjamZapH264ConfigFromAvccFrame(encodedData, configInner);
            }

            if (configInner.getSize() > 0)
            {
                bool changed = true;
                auto existingConfigIt = zapVideoCodecConfigByStream.find(job.streamKey);
                if (existingConfigIt != zapVideoCodecConfigByStream.end()
                    && existingConfigIt->second.getSize() == configInner.getSize())
                {
                    changed = std::memcmp(existingConfigIt->second.getData(),
                                          configInner.getData(),
                                          configInner.getSize()) != 0;
                }

                if (changed)
                {
                    zapVideoCodecConfigByStream[job.streamKey] = configInner;
                    zapVideoCodecConfigIdByStream[job.streamKey] = ++videoBufferRefreshCounter;
                }

                auto configIdIt = zapVideoCodecConfigIdByStream.find(job.streamKey);
                if (configIdIt != zapVideoCodecConfigIdByStream.end())
                    configId = configIdIt->second;

                auto infoIt = remoteVideoFrameInfoByUser.find(job.streamKey);
                if (infoIt != remoteVideoFrameInfoByUser.end())
                {
                    infoIt->second.codec = ninjamplus::zap::VideoCodec::h264;
                    infoIt->second.codecConfigId = configId;
                    infoIt->second.refreshId = juce::jmax(infoIt->second.refreshId, configId);
                    infoIt->second.lastUpdateMs = nowMs;
                }

                lastIntervalHelperPayloadWriteMs = 0.0;
                if (configOnlyChunk)
                    return;
            }
        }

        auto configIt = zapVideoCodecConfigIdByStream.find(job.streamKey);
        if (configIt != zapVideoCodecConfigIdByStream.end())
            configId = configIt->second;
    }

    if (job.audioGuidHex.isNotEmpty())
    {
        const juce::ScopedLock lock(zapVideoFrameLock);
        auto& promotedGuids = zapVideoPromotedGuidsByStream[job.streamKey];
        for (auto it = promotedGuids.begin(); it != promotedGuids.end();)
        {
            if (nowMs - it->second > 30000.0)
                it = promotedGuids.erase(it);
            else
                ++it;
        }

        if (promotedGuids.find(job.audioGuidHex) != promotedGuids.end())
        {
            auto playbackIt = zapVideoPlaybackByStream.find(job.streamKey);
            if (playbackIt != zapVideoPlaybackByStream.end()
                && playbackIt->second.audioGuidHex == job.audioGuidHex)
            {
                auto& playback = playbackIt->second;
                while (playback.frames.size() >= 360)
                    playback.frames.erase(playback.frames.begin());
                playback.frames.push_back(std::move(encodedData));
                playback.info.lastUpdateMs = nowMs;
                playback.info.lastReceiveToPublishMs = receiveToPublishMs;
                playback.info.lastSenderCaptureQueueMs = job.senderCaptureQueueMs;
                playback.info.lastSenderEncodeMs = job.senderEncodeMs;
                playback.info.frameCount = (int)playback.frames.size();
                playback.info.codec = job.codec;
                playback.info.codecConfigId = configId;
                remoteVideoFrameInfoByUser[job.streamKey] = playback.info;
                return;
            }
            return;
        }

        auto deferredIt = zapVideoDeferredPlaybackByStream.find(job.streamKey);
        if (deferredIt != zapVideoDeferredPlaybackByStream.end()
            && deferredIt->second.audioGuidHex == job.audioGuidHex)
        {
            auto& intervalBuffer = deferredIt->second;
            intervalBuffer.lastUpdateMs = nowMs;
            intervalBuffer.lastDecodeQueueMs = 0.0;
            intervalBuffer.lastDecodeMs = 0.0;
            intervalBuffer.lastReceiveToPublishMs = receiveToPublishMs;
            intervalBuffer.lastSenderCaptureQueueMs = job.senderCaptureQueueMs;
            intervalBuffer.lastSenderEncodeMs = job.senderEncodeMs;
            intervalBuffer.decodedFrameCount += 1;
            intervalBuffer.codec = job.codec;
            intervalBuffer.codecConfigId = configId;
            while (intervalBuffer.frames.size() >= 360)
                intervalBuffer.frames.erase(intervalBuffer.frames.begin());
            intervalBuffer.frames.push_back(std::move(encodedData));
            ninjamZapVideoPlaybackWorkPending.store(true, std::memory_order_release);
            return;
        }

        auto& byGuid = zapVideoDecodedIntervalsByStream[job.streamKey];
        auto& intervalBuffer = byGuid[job.audioGuidHex];
        intervalBuffer.streamKey = job.streamKey;
        intervalBuffer.sender = job.sender;
        intervalBuffer.audioGuidHex = job.audioGuidHex;
        intervalBuffer.markerInterval = job.markerInterval;
        intervalBuffer.channelIndex = job.channelIndex;
        intervalBuffer.lastUpdateMs = nowMs;
        intervalBuffer.lastDecodeQueueMs = 0.0;
        intervalBuffer.lastDecodeMs = 0.0;
        intervalBuffer.lastReceiveToPublishMs = receiveToPublishMs;
        intervalBuffer.lastSenderCaptureQueueMs = job.senderCaptureQueueMs;
        intervalBuffer.lastSenderEncodeMs = job.senderEncodeMs;
        intervalBuffer.decodedFrameCount += 1;
        intervalBuffer.codec = job.codec;
        intervalBuffer.codecConfigId = configId;
        while (intervalBuffer.frames.size() >= 360)
            intervalBuffer.frames.erase(intervalBuffer.frames.begin());
        intervalBuffer.frames.push_back(std::move(encodedData));

        while (byGuid.size() > 6)
        {
            auto oldest = byGuid.begin();
            for (auto it = byGuid.begin(); it != byGuid.end(); ++it)
                if (it->second.lastUpdateMs < oldest->second.lastUpdateMs)
                    oldest = it;
            byGuid.erase(oldest);
        }
        ninjamZapVideoPlaybackWorkPending.store(true, std::memory_order_release);
        return;
    }

    juce::uint64 refreshId = 0;
    {
        const juce::ScopedLock lock(zapVideoFrameLock);
        refreshId = ++videoBufferRefreshCounter;
        remoteVideoLatestJpegByUser[job.streamKey] = std::move(encodedData);

        ZapVideoFrameInfo info;
        info.streamKey = job.streamKey;
        info.sender = job.sender;
        info.channelIndex = job.channelIndex;
        info.refreshId = refreshId;
        info.lastUpdateMs = nowMs;
        info.lastDecodeQueueMs = 0.0;
        info.lastDecodeMs = 0.0;
        info.lastReceiveToPublishMs = receiveToPublishMs;
        info.lastSenderCaptureQueueMs = job.senderCaptureQueueMs;
        info.lastSenderEncodeMs = job.senderEncodeMs;
        info.frameCount = 1;
        info.codec = job.codec;
        info.codecConfigId = configId;
        remoteVideoFrameInfoByUser[job.streamKey] = info;
    }

    lastIntervalHelperPayloadWriteMs = 0.0;
}

void NinjamVst3AudioProcessor::publishDecodedZapVideoFrame(const ZapVideoDecodeJob& job,
                                                           const juce::Image& frame,
                                                           const juce::MemoryBlock& encodedJpeg)
{
    juce::MemoryBlock jpegData;
    if (encodedJpeg.getSize() > 0)
    {
        jpegData.append(encodedJpeg.getData(), encodedJpeg.getSize());
    }
    else if (frame.isValid())
    {
        ninjamplus::zap::encodeMjpegFrame(frame, 82, jpegData);
    }

    if (!frame.isValid() || jpegData.getSize() == 0 || job.streamKey.isEmpty())
        return;

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const double decodeQueueMs = (job.decodeStartedMs > 0.0 && job.queuedMs > 0.0)
        ? juce::jmax(0.0, job.decodeStartedMs - job.queuedMs)
        : 0.0;
    const double decodeMs = (job.decodeFinishedMs > 0.0 && job.decodeStartedMs > 0.0)
        ? juce::jmax(0.0, job.decodeFinishedMs - job.decodeStartedMs)
        : 0.0;
    const double receiveToPublishMs = job.receivedMs > 0.0
        ? juce::jmax(0.0, nowMs - job.receivedMs)
        : 0.0;
    if (job.audioGuidHex.isNotEmpty())
    {
        const juce::ScopedLock lock(zapVideoFrameLock);
        auto& promotedGuids = zapVideoPromotedGuidsByStream[job.streamKey];
        for (auto it = promotedGuids.begin(); it != promotedGuids.end();)
        {
            if (nowMs - it->second > 30000.0)
                it = promotedGuids.erase(it);
            else
                ++it;
        }

        if (promotedGuids.find(job.audioGuidHex) != promotedGuids.end())
        {
            auto playbackIt = zapVideoPlaybackByStream.find(job.streamKey);
            if (playbackIt != zapVideoPlaybackByStream.end()
                && playbackIt->second.audioGuidHex == job.audioGuidHex)
            {
                auto& playback = playbackIt->second;
                while (playback.frames.size() >= 360)
                    playback.frames.erase(playback.frames.begin());
                playback.frames.push_back(std::move(jpegData));
                playback.info.lastUpdateMs = nowMs;
                playback.info.lastDecodeQueueMs = decodeQueueMs;
                playback.info.lastDecodeMs = decodeMs;
                playback.info.lastReceiveToPublishMs = receiveToPublishMs;
                playback.info.lastSenderCaptureQueueMs = job.senderCaptureQueueMs;
                playback.info.lastSenderEncodeMs = job.senderEncodeMs;
                playback.info.frameCount = (int)playback.frames.size();
                playback.info.codec = ninjamplus::zap::VideoCodec::mjpeg;
                remoteVideoFrameInfoByUser[job.streamKey] = playback.info;
                return;
            }
            return;
        }

        auto deferredIt = zapVideoDeferredPlaybackByStream.find(job.streamKey);
        if (deferredIt != zapVideoDeferredPlaybackByStream.end()
            && deferredIt->second.audioGuidHex == job.audioGuidHex)
        {
            auto& intervalBuffer = deferredIt->second;
            intervalBuffer.lastUpdateMs = nowMs;
            intervalBuffer.lastDecodeQueueMs = decodeQueueMs;
            intervalBuffer.lastDecodeMs = decodeMs;
            intervalBuffer.lastReceiveToPublishMs = receiveToPublishMs;
            intervalBuffer.lastSenderCaptureQueueMs = job.senderCaptureQueueMs;
            intervalBuffer.lastSenderEncodeMs = job.senderEncodeMs;
            intervalBuffer.decodedFrameCount += 1;
            intervalBuffer.codec = ninjamplus::zap::VideoCodec::mjpeg;
            while (intervalBuffer.frames.size() >= 360)
                intervalBuffer.frames.erase(intervalBuffer.frames.begin());
            intervalBuffer.frames.push_back(std::move(jpegData));
            ninjamZapVideoPlaybackWorkPending.store(true, std::memory_order_release);
            return;
        }

        auto& byGuid = zapVideoDecodedIntervalsByStream[job.streamKey];
        auto& intervalBuffer = byGuid[job.audioGuidHex];
        intervalBuffer.streamKey = job.streamKey;
        intervalBuffer.sender = job.sender;
        intervalBuffer.audioGuidHex = job.audioGuidHex;
        intervalBuffer.markerInterval = job.markerInterval;
        intervalBuffer.channelIndex = job.channelIndex;
        intervalBuffer.lastUpdateMs = nowMs;
        intervalBuffer.lastDecodeQueueMs = decodeQueueMs;
        intervalBuffer.lastDecodeMs = decodeMs;
        intervalBuffer.lastReceiveToPublishMs = receiveToPublishMs;
        intervalBuffer.lastSenderCaptureQueueMs = job.senderCaptureQueueMs;
        intervalBuffer.lastSenderEncodeMs = job.senderEncodeMs;
        intervalBuffer.decodedFrameCount += 1;
        intervalBuffer.codec = ninjamplus::zap::VideoCodec::mjpeg;
        while (intervalBuffer.frames.size() >= 360)
            intervalBuffer.frames.erase(intervalBuffer.frames.begin());
        intervalBuffer.frames.push_back(std::move(jpegData));

        while (byGuid.size() > 6)
        {
            auto oldest = byGuid.begin();
            for (auto it = byGuid.begin(); it != byGuid.end(); ++it)
                if (it->second.lastUpdateMs < oldest->second.lastUpdateMs)
                    oldest = it;
            byGuid.erase(oldest);
        }
        ninjamZapVideoPlaybackWorkPending.store(true, std::memory_order_release);
        return;
    }

    juce::uint64 refreshId = 0;
    {
        const juce::ScopedLock lock(zapVideoFrameLock);
        refreshId = ++videoBufferRefreshCounter;
        remoteVideoLatestFrameByUser[job.streamKey] = frame;
        remoteVideoLatestJpegByUser[job.streamKey] = std::move(jpegData);

        ZapVideoFrameInfo info;
        info.streamKey = job.streamKey;
        info.sender = job.sender;
        info.channelIndex = job.channelIndex;
        info.refreshId = refreshId;
        info.lastUpdateMs = nowMs;
        info.lastDecodeQueueMs = decodeQueueMs;
        info.lastDecodeMs = decodeMs;
        info.lastReceiveToPublishMs = receiveToPublishMs;
        info.lastSenderCaptureQueueMs = job.senderCaptureQueueMs;
        info.lastSenderEncodeMs = job.senderEncodeMs;
        info.frameCount = 1;
        info.codec = ninjamplus::zap::VideoCodec::mjpeg;
        remoteVideoFrameInfoByUser[job.streamKey] = info;
    }

    lastIntervalHelperPayloadWriteMs = 0.0;
}

void NinjamVst3AudioProcessor::processPendingNinjamZapVideoPlaybackSwap()
{
    if (!ninjamZapVideoEnabled.load(std::memory_order_relaxed))
    {
        pendingNinjamZapVideoPlaybackSwap.store(false, std::memory_order_release);
        pendingNinjamZapVideoPlaybackBoundaryMs.store(0.0, std::memory_order_release);
        return;
    }

    if (!ninjamZapVideoPlaybackWorkPending.load(std::memory_order_acquire))
    {
        pendingNinjamZapVideoPlaybackSwap.store(false, std::memory_order_release);
        pendingNinjamZapVideoPlaybackBoundaryMs.store(0.0, std::memory_order_release);
        return;
    }

    if (!pendingNinjamZapVideoPlaybackSwap.exchange(false, std::memory_order_acquire))
        return;

    const double callbackBoundaryMs = pendingNinjamZapVideoPlaybackBoundaryMs.exchange(0.0,
                                                                                      std::memory_order_acq_rel);
    std::map<juce::String, std::set<juce::String>> currentAudioGuidsBySender;
    std::map<juce::String, std::set<juce::String>> previousAudioGuidsBySender;
    double intervalDurationMs = 1000.0;

    {
        const juce::ScopedLock clientLock(ninjamClientLock);
        const double bpm = juce::jmax(1.0, (double)ninjamClient.GetActualBPM());
        const int bpi = juce::jmax(1, ninjamClient.GetBPI());
        int intervalLengthSamples = 0;
        ninjamClient.GetPosition(nullptr, &intervalLengthSamples);
        const double sampleRate = juce::jmax(1.0, getSampleRate());
        if (intervalLengthSamples > 0)
            intervalDurationMs = juce::jlimit(250.0, 60000.0, ((double)intervalLengthSamples * 1000.0) / sampleRate);
        else
            intervalDurationMs = juce::jlimit(250.0, 60000.0, (60000.0 * (double)bpi) / bpm);

        const int numUsers = ninjamClient.GetNumUsers();
        for (int userIndex = 0; userIndex < numUsers; ++userIndex)
        {
            const char* userNameRaw = ninjamClient.GetUserState(userIndex);
            if (userNameRaw == nullptr)
                continue;

            const juce::String userName = juce::String::fromUTF8(userNameRaw);
            for (int channelIndex = 0; channelIndex < 32; ++channelIndex)
            {
                int channelFlags = 0;
                const char* channelName = ninjamClient.GetUserChannelState(userIndex, channelIndex,
                                                                           nullptr, nullptr, nullptr,
                                                                           nullptr, nullptr, nullptr,
                                                                           &channelFlags);
                if (channelName == nullptr || ((channelFlags & kNinjamZapVideoOnlyChannelFlag) != 0))
                    continue;

                unsigned char currentGuid[16] {};
                unsigned char previousGuid[16] {};
                bool hasCurrent = false;
                bool hasPrevious = false;
                if (!ninjamClient.GetUserChannelPlaybackGuids(userIndex, channelIndex,
                                                              currentGuid, &hasCurrent,
                                                              previousGuid, &hasPrevious))
                    continue;

                if (hasCurrent)
                    currentAudioGuidsBySender[userName].insert(guidToHexString(currentGuid));
                if (hasPrevious)
                    previousAudioGuidsBySender[userName].insert(guidToHexString(previousGuid));
            }
        }
    }

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const double rawPlaybackOffsetMs = callbackBoundaryMs > 0.0 ? juce::jmax(0.0, nowMs - callbackBoundaryMs) : 0.0;
    const double playbackOffsetMs = juce::jlimit(0.0, juce::jmax(0.0, intervalDurationMs - 1.0), rawPlaybackOffsetMs);
    const juce::ScopedTryLock lock(zapVideoFrameLock);
    if (!lock.isLocked())
    {
        if (callbackBoundaryMs > 0.0)
            pendingNinjamZapVideoPlaybackBoundaryMs.store(callbackBoundaryMs, std::memory_order_release);
        pendingNinjamZapVideoPlaybackSwap.store(true, std::memory_order_release);
        return;
    }

    auto promote = [this, nowMs, intervalDurationMs, playbackOffsetMs](ZapVideoIntervalFrameBuffer buffer)
    {
        if (buffer.frames.empty() || buffer.streamKey.isEmpty())
            return;

        if (buffer.audioGuidHex.isNotEmpty())
            zapVideoPromotedGuidsByStream[buffer.streamKey][buffer.audioGuidHex] = nowMs;

        const double senderDelayMs = juce::jlimit(0.0, 500.0,
                                                  buffer.lastSenderCaptureQueueMs + buffer.lastSenderEncodeMs);
        const double receiverDelayMs = juce::jlimit(0.0, 500.0,
                                                    juce::jmax(buffer.lastReceiveToPublishMs,
                                                               buffer.lastDecodeQueueMs + buffer.lastDecodeMs));
        const double compensationMs = juce::jlimit(0.0,
                                                   juce::jmin(250.0, intervalDurationMs * 0.25),
                                                   senderDelayMs + receiverDelayMs);
        const double compensatedPlaybackOffsetMs = juce::jlimit(0.0,
                                                                juce::jmax(0.0, intervalDurationMs - 1.0),
                                                                playbackOffsetMs + compensationMs);
        const double compensatedPlaybackStartMs = nowMs - compensatedPlaybackOffsetMs;

        ZapVideoPlaybackBuffer playback;
        playback.audioGuidHex = buffer.audioGuidHex;
        playback.frames = std::move(buffer.frames);
        playback.startedMs = compensatedPlaybackStartMs;
        playback.durationMs = intervalDurationMs;
        playback.playbackOffsetMs = compensatedPlaybackOffsetMs;
        playback.holdCount = 0;
        playback.info.streamKey = buffer.streamKey;
        playback.info.sender = buffer.sender;
        playback.info.channelIndex = buffer.channelIndex;
        playback.info.refreshId = ++videoBufferRefreshCounter;
        playback.info.lastUpdateMs = nowMs;
        playback.info.lastDecodeQueueMs = buffer.lastDecodeQueueMs;
        playback.info.lastDecodeMs = buffer.lastDecodeMs;
        playback.info.lastReceiveToPublishMs = buffer.lastReceiveToPublishMs;
        playback.info.lastPlaybackOffsetMs = compensatedPlaybackOffsetMs;
        playback.info.lastPlaybackCompensationMs = compensationMs;
        playback.info.lastSenderCaptureQueueMs = buffer.lastSenderCaptureQueueMs;
        playback.info.lastSenderEncodeMs = buffer.lastSenderEncodeMs;
        playback.info.frameCount = (int)buffer.frames.size();
        playback.info.codec = buffer.codec;
        playback.info.codecConfigId = buffer.codecConfigId;

        zapVideoPlaybackByStream[buffer.streamKey] = playback;
        remoteVideoFrameInfoByUser[buffer.streamKey] = playback.info;
        remoteVideoLatestJpegByUser[buffer.streamKey] = playback.frames.front();

        const juce::String senderKey = normaliseOpusPeerId(buffer.sender);
        if (senderKey.isNotEmpty())
        {
            remoteVideoBufferRefreshIdByUser[senderKey] = playback.info.refreshId;
            const juce::String canonicalSenderKey = canonicalDelayUserKey(senderKey);
            if (canonicalSenderKey.isNotEmpty())
                remoteVideoBufferRefreshIdByUser[canonicalSenderKey] = playback.info.refreshId;
        }
    };

    for (auto it = zapVideoDeferredPlaybackByStream.begin(); it != zapVideoDeferredPlaybackByStream.end();)
    {
        promote(std::move(it->second));
        it = zapVideoDeferredPlaybackByStream.erase(it);
    }

    for (auto streamIt = zapVideoDecodedIntervalsByStream.begin(); streamIt != zapVideoDecodedIntervalsByStream.end();)
    {
        const juce::String streamKey = streamIt->first;
        auto& intervalsByGuid = streamIt->second;

        auto prevMatchIt = intervalsByGuid.end();
        auto currentMatchIt = intervalsByGuid.end();

        for (auto it = intervalsByGuid.begin(); it != intervalsByGuid.end();)
        {
            if (nowMs - it->second.lastUpdateMs > 30000.0)
            {
                it = intervalsByGuid.erase(it);
                continue;
            }

            const auto prevSetIt = previousAudioGuidsBySender.find(it->second.sender);
            if (prevSetIt != previousAudioGuidsBySender.end()
                && prevSetIt->second.find(it->second.audioGuidHex) != prevSetIt->second.end())
            {
                prevMatchIt = it;
            }

            const auto currentSetIt = currentAudioGuidsBySender.find(it->second.sender);
            if (currentSetIt != currentAudioGuidsBySender.end()
                && currentSetIt->second.find(it->second.audioGuidHex) != currentSetIt->second.end())
            {
                currentMatchIt = it;
            }

            ++it;
        }

        // PREV is the decode stream that has reached the audible interval, so play
        // that video immediately. DS/current will not reach the speaker until the
        // next NINJAM swap, so defer that video by one swap to keep audio/video aligned.
        if (prevMatchIt != intervalsByGuid.end())
        {
            promote(std::move(prevMatchIt->second));
            intervalsByGuid.erase(prevMatchIt);
            zapVideoPlaybackByStream[streamKey].holdCount = 0;
        }
        else if (currentMatchIt != intervalsByGuid.end())
        {
            ZapVideoIntervalFrameBuffer buffer = std::move(currentMatchIt->second);
            intervalsByGuid.erase(currentMatchIt);

            if (!buffer.frames.empty() && buffer.streamKey.isNotEmpty())
                zapVideoDeferredPlaybackByStream[buffer.streamKey] = std::move(buffer);

            auto playbackIt = zapVideoPlaybackByStream.find(streamKey);
            if (playbackIt != zapVideoPlaybackByStream.end())
                playbackIt->second.holdCount = 0;
        }

        if (intervalsByGuid.empty())
            streamIt = zapVideoDecodedIntervalsByStream.erase(streamIt);
        else
            ++streamIt;
    }

    ninjamZapVideoPlaybackWorkPending.store(!zapVideoDecodedIntervalsByStream.empty()
                                            || !zapVideoDeferredPlaybackByStream.empty(),
                                            std::memory_order_release);
    lastIntervalHelperPayloadWriteMs = 0.0;
}

void NinjamVst3AudioProcessor::publishLocalNinjamZapCameraFrame(const juce::Image& frame,
                                                                const juce::MemoryBlock& encodedJpeg,
                                                                double captureQueueMs,
                                                                double encodeMs)
{
    if (!frame.isValid() || encodedJpeg.getSize() == 0)
        return;

    juce::MemoryBlock jpegData;
    jpegData.append(encodedJpeg.getData(), encodedJpeg.getSize());

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const juce::String streamKey = "local:zap-camera";
    const juce::String sender = currentUser.isNotEmpty() ? currentUser.upToFirstOccurrenceOf("@", false, false) + " (local camera)"
                                                         : "Local Camera";
    broadcastNinjamZapVideoTiming(captureQueueMs, encodeMs);

    juce::uint64 refreshId = 0;
    {
        const juce::ScopedLock lock(intervalSyncAnnouncementLock);
        refreshId = ++videoBufferRefreshCounter;
    }

    {
        const juce::ScopedLock lock(zapVideoFrameLock);
        remoteVideoLatestFrameByUser[streamKey] = frame;
        remoteVideoLatestJpegByUser[streamKey] = std::move(jpegData);

        ZapVideoFrameInfo info;
        info.streamKey = streamKey;
        info.sender = sender;
        info.channelIndex = getNinjamZapVideoChannelIndex();
        info.refreshId = refreshId;
        info.lastUpdateMs = nowMs;
        info.lastSenderCaptureQueueMs = juce::jmax(0.0, captureQueueMs);
        info.lastSenderEncodeMs = juce::jmax(0.0, encodeMs);
        info.frameCount = 1;
        info.codec = ninjamplus::zap::VideoCodec::mjpeg;
        remoteVideoFrameInfoByUser[streamKey] = info;
    }
}

juce::String NinjamVst3AudioProcessor::buildZapVideoFrameListJson() const
{
    juce::Array<juce::var> entries;
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const double wallClockMs = (double)juce::Time::currentTimeMillis();
    const int bpi = juce::jmax(1, cachedNinjamBpi.load(std::memory_order_relaxed));
    const double bpm = juce::jmax(1.0, (double)cachedNinjamBpm.load(std::memory_order_relaxed));
    const int transportLength = juce::jmax(1, cachedNinjamTransportLen.load(std::memory_order_relaxed));
    int transportPos = juce::jlimit(0, transportLength, cachedNinjamTransportPos.load(std::memory_order_relaxed));
    if (isTransportSyncEnabled() && (!hostWasPlaying.load() || syncWaitForInterval.load()))
    {
        transportPos = 0;
    }
    else if (isTransportSyncEnabled() && hostWasPlaying.load())
    {
        const int basePos = syncDisplayPositionOffset.load();
        int relativePos = transportPos - basePos;
        if (relativePos < 0)
            relativePos += transportLength;
        transportPos = juce::jlimit(0, transportLength, relativePos);
    }
    const double intervalProgress = juce::jlimit(0.0, 1.0, (double)transportPos / (double)transportLength);
    const double globalBeat = (double)getDisplayIntervalIndex() * (double)bpi + intervalProgress * (double)bpi;
    {
        juce::DynamicObject::Ptr infoObj = new juce::DynamicObject();
        infoObj->setProperty("type", "intervalInfo");
        infoObj->setProperty("interval", getDisplayIntervalIndex());
        infoObj->setProperty("pos", transportPos);
        infoObj->setProperty("length", transportLength);
        infoObj->setProperty("bpm", bpm);
        infoObj->setProperty("bpi", bpi);
        infoObj->setProperty("globalBeat", globalBeat);
        infoObj->setProperty("videoClockMs", nowMs);
        infoObj->setProperty("wallClockMs", wallClockMs);
        entries.add(juce::var(infoObj.get()));
    }

    const juce::ScopedLock lock(zapVideoFrameLock);
    for (const auto& entry : remoteVideoFrameInfoByUser)
    {
        const auto& info = entry.second;
        if (nowMs - info.lastUpdateMs > 30000.0)
            continue;

        juce::DynamicObject::Ptr obj = new juce::DynamicObject();
        obj->setProperty("streamKey", info.streamKey);
        obj->setProperty("sender", info.sender);
        obj->setProperty("channelIndex", info.channelIndex);
        juce::String codecName = "unknown";
        if (info.codec == ninjamplus::zap::VideoCodec::mjpeg)
            codecName = "mjpeg";
        else if (info.codec == ninjamplus::zap::VideoCodec::h264)
            codecName = "h264";
        else if (info.codec == ninjamplus::zap::VideoCodec::vp8)
            codecName = "vp8";
        else if (info.codec == ninjamplus::zap::VideoCodec::vp9)
            codecName = "vp9";
        obj->setProperty("codec", codecName);
        obj->setProperty("browserDecode", true);
        if (info.codec == ninjamplus::zap::VideoCodec::h264)
        {
            auto configIt = zapVideoCodecConfigByStream.find(info.streamKey);
            const int h264ConfigBytes = (configIt != zapVideoCodecConfigByStream.end())
                ? (int) configIt->second.getSize()
                : 0;
            obj->setProperty("debugH264ConfigBytes", h264ConfigBytes);
            if (configIt != zapVideoCodecConfigByStream.end() && configIt->second.getSize() > 0)
            {
                obj->setProperty("h264Config", juce::Base64::toBase64(configIt->second.getData(),
                                                                       configIt->second.getSize()));
                auto configIdIt = zapVideoCodecConfigIdByStream.find(info.streamKey);
                if (configIdIt != zapVideoCodecConfigIdByStream.end())
                    obj->setProperty("h264ConfigId", juce::String((juce::int64)configIdIt->second));
            }
            else
            {
                auto configIdIt = zapVideoCodecConfigIdByStream.find(info.streamKey);
                if (configIdIt != zapVideoCodecConfigIdByStream.end())
                    obj->setProperty("debugH264ConfigIdOnly", juce::String((juce::int64)configIdIt->second));
            }
        }
        juce::uint64 refreshId = info.refreshId;
        obj->setProperty("playbackBufferId", juce::String((juce::int64) info.refreshId));
        obj->setProperty("playbackFrameIndex", 0);
        obj->setProperty("playbackDurationMs", 0.0);
        obj->setProperty("playbackAgeMs", 0.0);
        auto playbackIt = zapVideoPlaybackByStream.find(info.streamKey);
        if (playbackIt != zapVideoPlaybackByStream.end() && playbackIt->second.frames.size() > 1)
        {
            const auto& playback = playbackIt->second;
            const double progress = juce::jlimit(0.0, 0.999999, (nowMs - playback.startedMs) / juce::jmax(1.0, playback.durationMs));
            const size_t frameIndex = std::min(playback.frames.size() - 1,
                                               (size_t)std::floor(progress * (double)playback.frames.size()));
            refreshId += (juce::uint64)frameIndex;
            obj->setProperty("playbackFrameIndex", (int)frameIndex);
            obj->setProperty("playbackDurationMs", playback.durationMs);
            obj->setProperty("playbackAgeMs", juce::jmax(0.0, nowMs - playback.startedMs));
        }
        obj->setProperty("refreshId", juce::String((juce::int64) refreshId));
        obj->setProperty("frameCount", info.frameCount);
        obj->setProperty("decodeQueueMs", info.lastDecodeQueueMs);
        obj->setProperty("decodeMs", info.lastDecodeMs);
        obj->setProperty("receiveToPublishMs", info.lastReceiveToPublishMs);
        obj->setProperty("playbackOffsetMs", info.lastPlaybackOffsetMs);
        obj->setProperty("playbackCompensationMs", info.lastPlaybackCompensationMs);
        obj->setProperty("senderCaptureQueueMs", info.lastSenderCaptureQueueMs);
        obj->setProperty("senderEncodeMs", info.lastSenderEncodeMs);
        entries.add(juce::var(obj.get()));
    }

    return juce::JSON::toString(juce::var(entries), false);
}

bool NinjamVst3AudioProcessor::getZapVideoFrameJpeg(const juce::String& streamKey,
                                                    int requestedFrameIndex,
                                                    juce::MemoryBlock& jpegData) const
{
    const juce::ScopedLock lock(zapVideoFrameLock);
    auto playbackIt = zapVideoPlaybackByStream.find(streamKey);
    if (playbackIt != zapVideoPlaybackByStream.end() && !playbackIt->second.frames.empty())
    {
        const auto& playback = playbackIt->second;
        size_t frameIndex = 0;
        if (requestedFrameIndex >= 0)
        {
            frameIndex = std::min(playback.frames.size() - 1, (size_t)requestedFrameIndex);
        }
        else
        {
            const double nowMs = juce::Time::getMillisecondCounterHiRes();
            const double progress = juce::jlimit(0.0, 0.999999, (nowMs - playback.startedMs) / juce::jmax(1.0, playback.durationMs));
            frameIndex = std::min(playback.frames.size() - 1,
                                  (size_t)std::floor(progress * (double)playback.frames.size()));
        }
        jpegData.reset();
        jpegData.append(playback.frames[frameIndex].getData(), playback.frames[frameIndex].getSize());
        return jpegData.getSize() > 0;
    }

    auto it = remoteVideoLatestJpegByUser.find(streamKey);
    if (it == remoteVideoLatestJpegByUser.end() || it->second.getSize() == 0)
        return false;

    jpegData.reset();
    jpegData.append(it->second.getData(), it->second.getSize());
    return true;
}

void NinjamVst3AudioProcessor::clearZapVideoFrameState()
{
    ninjamZapVideoPlaybackWorkPending.store(false, std::memory_order_release);
    {
        const juce::ScopedLock lock(zapVideoFrameLock);
        remoteVideoFrameInfoByUser.clear();
        zapVideoSenderTimingByStream.clear();
        remoteVideoLatestJpegByUser.clear();
        remoteVideoLatestFrameByUser.clear();
        zapVideoDecodedIntervalsByStream.clear();
        zapVideoDeferredPlaybackByStream.clear();
        zapVideoPlaybackByStream.clear();
        zapVideoPromotedGuidsByStream.clear();
        zapVideoCodecConfigByStream.clear();
        zapVideoCodecConfigIdByStream.clear();
    }
    {
        const juce::ScopedLock lock(ninjamZapVideoChunkLock);
        ninjamZapVideoChunkReassemblers.clear();
        ninjamZapVideoAudioGuidByReassemblyKey.clear();
        ninjamZapVideoMarkerIntervalByReassemblyKey.clear();
        ninjamZapVideoMarkerSeenByReassemblyKey.clear();
        remoteVideoChunkReassemblersByUser.clear();
    }
}

void NinjamVst3AudioProcessor::stopNinjamZapVideoTransportForDisconnect()
{
    stopNinjamZapCameraSend();
    if (ninjamClient.GetStatus() == NJClient::NJC_STATUS_OK)
        syncNinjamZapVideoSubscriptions(false);
    ninjamZapVideoEnabled.store(false, std::memory_order_relaxed);
    ninjamZapVideoReceivedNotice.store(false, std::memory_order_relaxed);
    pendingNinjamZapIntervalRotate.store(false, std::memory_order_release);
    pendingNinjamZapVideoPlaybackSwap.store(false, std::memory_order_release);
    pendingNinjamZapVideoPlaybackBoundaryMs.store(0.0, std::memory_order_release);
    lastNinjamZapVideoTimingBroadcastMs.store(0.0, std::memory_order_relaxed);
    lastNinjamZapVideoSubscriptionSyncMs = 0.0;
    clearZapVideoFrameState();
    stopZapVideoDecodeWorker();
}

bool NinjamVst3AudioProcessor::stopVdoVideoSync()
{
    const bool wasEnabled = vdoVideoSyncEnabled.exchange(false, std::memory_order_relaxed);
    vdoCarrierChannelConfigured.store(false, std::memory_order_release);
    intervalHelperPayloadForceWrite.store(false, std::memory_order_release);
    lastIntervalHelperPayloadWriteMs = 0.0;
    {
        const juce::ScopedLock lock(intervalHelperPayloadLock);
        intervalHelperPayload = "[]";
    }
    {
        const juce::ScopedLock lock(intervalSyncAnnouncementLock);
        remoteVideoBufferRefreshIdByUser.clear();
        recentVideoTimingChangeEventIds.clear();
    }
    if (wasEnabled)
        syncLocalIntervalChannelConfig();
    return wasEnabled;
}

void NinjamVst3AudioProcessor::stopAdvancedVideoClient()
{
    stopVdoVideoSync();
    videoHelperRunning.store(false);
    lastIntervalHelperPayloadWriteMs = 0.0;
    stopZapVideoDecodeWorker();
    {
        const juce::ScopedLock lock(intervalSyncAnnouncementLock);
        remoteVideoBufferRefreshIdByUser.clear();
        videoBufferRefreshCounter = 0;
    }
    clearZapVideoFrameState();
    if (advancedVideoServer)
        advancedVideoServer->stop();
    advancedVideoServer.reset();
    advancedVideoHelperPort.store(0);
}



namespace
{
    juce::String makeUrlSafeVdoRoomToken(const juce::String& raw)
    {
        juce::String room;
        bool lastWasUnderscore = false;

        for (int i = 0; i < raw.length(); ++i)
        {
            const juce_wchar ch = raw[i];
            if (juce::CharacterFunctions::isLetterOrDigit(ch))
            {
                room << juce::String::charToString((juce_wchar) juce::CharacterFunctions::toLowerCase(ch));
                lastWasUnderscore = false;
            }
            else if (!lastWasUnderscore)
            {
                room << "_";
                lastWasUnderscore = true;
            }
        }

        return room.trimCharactersAtStart("_").trimCharactersAtEnd("_");
    }

    bool isDigitsOnly(const juce::String& text)
    {
        if (text.isEmpty())
            return false;

        for (int i = 0; i < text.length(); ++i)
            if (!juce::CharacterFunctions::isDigit(text[i]))
                return false;

        return true;
    }

    bool isIpAddressText(const juce::String& host)
    {
        bool sawDigit = false;
        bool sawSeparator = false;

        for (int i = 0; i < host.length(); ++i)
        {
            const juce_wchar ch = host[i];
            if (juce::CharacterFunctions::isDigit(ch))
            {
                sawDigit = true;
            }
            else if (ch == '.' || ch == ':')
            {
                sawSeparator = true;
            }
            else
            {
                return false;
            }
        }

        return sawDigit && sawSeparator;
    }

    juce::String makeStableVdoRoomHash(const juce::String& endpoint)
    {
        juce::uint64 hash = static_cast<juce::uint64>(1469598103934665603ull);
        const char* const text = endpoint.toRawUTF8();

        for (const char* p = text; *p != 0; ++p)
        {
            hash ^= static_cast<unsigned char>(*p);
            hash *= static_cast<juce::uint64>(1099511628211ull);
        }

        static constexpr char hex[] = "0123456789abcdef";
        juce::String suffix;
        for (int shift = 60; shift >= 0; shift -= 4)
            suffix << juce::String::charToString((juce_wchar) hex[static_cast<int>((hash >> shift) & 0xf)]);

        return "ninjam_" + suffix;
    }

    juce::String makeVdoRoomSettingsKey(const juce::String& serverKey)
    {
        return "vdoRoom." + makeStableVdoRoomHash(serverKey.trim().toLowerCase());
    }

    struct VdoRoomEndpointParts
    {
        juce::String serverKey;
        juce::String hostPart;
        juce::String portPart;
    };

    VdoRoomEndpointParts parseVdoRoomEndpoint(juce::String roomSource)
    {
        roomSource = roomSource.trim();
        const int schemePos = roomSource.indexOf("://");
        if (schemePos >= 0)
            roomSource = roomSource.substring(schemePos + 3);

        const int slashPos = roomSource.indexOfChar('/');
        if (slashPos >= 0)
            roomSource = roomSource.substring(0, slashPos);

        const int atPos = roomSource.lastIndexOfChar('@');
        if (atPos >= 0 && atPos + 1 < roomSource.length())
            roomSource = roomSource.substring(atPos + 1);

        juce::String hostPart = roomSource.trim();
        juce::String portPart;

        if (hostPart.startsWithChar('['))
        {
            const int closeBracket = hostPart.indexOfChar(']');
            if (closeBracket > 0)
            {
                const juce::String bracketHost = hostPart.substring(1, closeBracket).trim();
                const juce::String afterBracket = hostPart.substring(closeBracket + 1).trim();
                if (afterBracket.startsWithChar(':') && isDigitsOnly(afterBracket.substring(1).trim()))
                    portPart = afterBracket.substring(1).trim();

                hostPart = bracketHost;
            }
        }
        else
        {
            const int firstColonPos = hostPart.indexOfChar(':');
            const int lastColonPos = hostPart.lastIndexOfChar(':');
            if (lastColonPos > 0 && firstColonPos == lastColonPos && lastColonPos + 1 < hostPart.length())
            {
                const juce::String candidatePort = hostPart.substring(lastColonPos + 1).trim();
                if (isDigitsOnly(candidatePort))
                {
                    hostPart = hostPart.substring(0, lastColonPos);
                    portPart = candidatePort;
                }
            }
        }

        VdoRoomEndpointParts parts;
        parts.hostPart = hostPart.trim().trimCharactersAtStart("[").trimCharactersAtEnd("]").toLowerCase();
        parts.portPart = portPart.trim();
        parts.serverKey = parts.hostPart;
        if (parts.portPart.isNotEmpty())
            parts.serverKey << ":" << parts.portPart;
        if (parts.serverKey.isEmpty())
            parts.serverKey = "ninjam";
        return parts;
    }

    VdoRoomResolution resolveVdoRoomForServer(const juce::String& roomSource)
    {
        const VdoRoomEndpointParts endpoint = parseVdoRoomEndpoint(roomSource);
        VdoRoomResolution resolution;
        resolution.serverKey = endpoint.serverKey;
        resolution.suggestedRoom = makeStableVdoRoomHash(endpoint.serverKey);

        const bool hostHasFriendlyName = endpoint.hostPart.isNotEmpty()
                                      && !endpoint.hostPart.equalsIgnoreCase("localhost")
                                      && !isIpAddressText(endpoint.hostPart);

        if (hostHasFriendlyName)
        {
            juce::String friendlyHost = endpoint.hostPart;
            const int firstDotPos = friendlyHost.indexOfChar('.');
            if (firstDotPos > 0)
                friendlyHost = friendlyHost.substring(0, firstDotPos);

            juce::String roomRaw = friendlyHost;
            if (endpoint.portPart.isNotEmpty())
                roomRaw << "_" << endpoint.portPart;

            resolution.room = makeUrlSafeVdoRoomToken(roomRaw);
        }

        resolution.needsPrompt = resolution.room.isEmpty();
        return resolution;
    }
}

void NinjamVst3AudioProcessor::launchVideoSession(const juce::String& requestedRoom)
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
    {
        juce::ScopedLock lock(chatLock);
        chatHistory.add("Connect to a server first, then click VDO.");
        chatSenders.add("");
        chatRevision.fetch_add(1);
        if (chatHistory.size() > 100)
        {
            chatHistory.removeRange(0, chatHistory.size() - 100);
            chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
        }
        return;
    }

    const bool hadZapVideo = ninjamZapVideoEnabled.load(std::memory_order_relaxed)
                          || ninjamZapCameraSendEnabled.load(std::memory_order_relaxed)
                          || ninjamZapBrowserCameraSendEnabled.load(std::memory_order_relaxed);
    if (hadZapVideo)
    {
        stopNinjamZapVideoTransportForDisconnect();
        addSystemChatLine("NINJAMZap video transport disabled for VDO.");
    }

    const bool wasVdoSyncEnabled = vdoVideoSyncEnabled.exchange(true, std::memory_order_relaxed);
    intervalHelperPayloadForceWrite.store(true, std::memory_order_release);
    lastIntervalHelperPayloadWriteMs = 0.0;
    if (!wasVdoSyncEnabled)
        resetIntervalSyncTimingCache();
    else
        requestVideoBufferRefreshForMeasuredUsers();
    {
        const juce::ScopedLock clientLock(ninjamClientLock);
        if (ninjamClient.GetStatus() == NJClient::NJC_STATUS_OK
            && ninjamClient.GetServerVideoSupported())
        {
            ninjamZapServerVideoSupported.store(true, std::memory_order_relaxed);
            ninjamSideSignalServerSupported.store(true, std::memory_order_relaxed);
            ninjamClient.ChatMessage_Send("VIDEO_CAP", "1", nullptr, nullptr, nullptr);
            lastNinjamVideoCapSendMs = juce::Time::getMillisecondCounterHiRes();
        }
    }
    const bool carrierConfigured = ninjamSideSignalServerSupported.load(std::memory_order_acquire);
    syncLocalIntervalChannelConfig();
    vdoCarrierChannelConfigured.store(carrierConfigured, std::memory_order_release);


    juce::String room = makeUrlSafeVdoRoomToken(requestedRoom);
    if (room.isEmpty())
    {
        const VdoRoomResolution roomResolution = resolveVdoRoomForServer(currentServer);
        room = roomResolution.room.isNotEmpty() ? roomResolution.room : roomResolution.suggestedRoom;
    }
    const juce::String cleanUserLabel = normaliseChatTargetNick(currentUser);
    const juce::String label = cleanUserLabel.isNotEmpty() ? cleanUserLabel : "NINJAM";
    const juce::String canonicalCurrentUserKey = canonicalDelayUserKey(currentUser);
    const juce::String syncUserKey = canonicalCurrentUserKey.isNotEmpty()
        ? canonicalCurrentUserKey
        : canonicalDelayUserKey(label);
    static constexpr int minimumVdoBufferMs = 0;
    int viewDelayMs = 0;
    {
        const juce::ScopedLock lock(intervalSyncAnnouncementLock);
        for (const auto& entry : remoteLatencyFirmDelayMsByUser)
            viewDelayMs = juce::jmax(viewDelayMs, juce::jmax(0, entry.second));
    }
    const int launchBufferMs = juce::jmax(minimumVdoBufferMs, viewDelayMs);
    const int videoBitrateKbps = 2500;
    const int videoBitrateCeilingKbps = 2500;

    if (ensureAdvancedVideoClientStarted())
    {
        const int helperPort = advancedVideoHelperPort.load();
        juce::URL helperUrl("http://127.0.0.1:" + juce::String(helperPort) + "/buffer-room");
        // VDO sync mode is video-only. NINJam remains the audio clock, while
        // the helper sends live per-peer VDO buffer updates. Keep the URL
        // bitrate knobs (chunked/chunkbitrate/bitrate/maxvideobitrate) in kbps;
        // do not derive them from launchBufferMs or BPI/BPM delay. That earlier
        // unit mix-up made chunked video encode at 60-120 kbps. Tweak video quality
        // with videoBitrateKbps, fps, quality, or the helper's camera presets.
        // Tweak loss recovery with chunknack/chunkchunksize/chunkcache, and
        // cellular/backpressure behavior with chunkedbuffer/chunkadapt*, without
        // changing the NINJam-owned receiver delay contract.
        helperUrl = helperUrl.withParameter("room", room)
                             .withParameter("label", label)
                             .withParameter("vdoSyncUserKey", syncUserKey)
                             .withParameter("cameraQuality", "720p30")
                             .withParameter("bufferMode", "remote")
                             .withParameter("buffer", juce::String(launchBufferMs))
                             .withParameter("chunked", juce::String(videoBitrateKbps))
                             .withParameter("chunkbitrate", juce::String(videoBitrateKbps))
                             .withParameter("bitrate", juce::String(videoBitrateKbps))
                             .withParameter("maxvideobitrate", juce::String(videoBitrateCeilingKbps))
                             .withParameter("quality", "1")
                             .withParameter("mfr", "30")
                             .withParameter("chunkindex", "1")
                             .withParameter("chunknack", "1")
                             .withParameter("chunknackattempts", "8")
                             .withParameter("chunknackdelay", "250")
                             .withParameter("chunkchunksize", "4096")
                             .withParameter("chunkcache", "30000")
                             .withParameter("chunkbufferadaptive", "0")
                             .withParameter("chunkbufferfloor", "0")
                             .withParameter("chunkbufferceil", "180000")
                             .withParameter("chunkedbuffer", "500")
                             .withParameter("chunkadapt", "bitrate")
                             .withParameter("chunkadaptfloor", "60")
                             .withParameter("chunkadaptceil", juce::String(videoBitrateCeilingKbps))
                             .withParameter("chunkadaptthreshold", "500")
                             .withParameter("chunkadaptmaxdrop", "0")
                             .withParameter("chunkadaptinterval", "1200")
                             .withParameter("chunkadaptresolution", "1")
                             .withParameter("helperVersion", getVersionString())
                             .withParameter("cacheBust", juce::String((juce::int64)juce::Time::getMillisecondCounter()));

        // Mobile hotspot mode: force TURN relay + TCP to bypass UDP blocking
        if (mobileHotspotModeEnabled.load(std::memory_order_relaxed))
        {
            helperUrl = helperUrl.withParameter("relay", "")
                                 .withParameter("tcp", "");
        }
        {
            juce::ScopedLock lock(chatLock);
            chatHistory.add("Tip: If your cam isn't showing, refresh the video page and select your camera before entering the room.");
            chatSenders.add("");
            chatRevision.fetch_add(1);
            if (chatHistory.size() > 100)
            {
                chatHistory.removeRange(0, chatHistory.size() - 100);
                chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
            }
        }
        const auto helperUrlText = helperUrl.toString(true);
        const bool opened = openUrlExternalOnMessageThread(helperUrlText);
        if (!opened)
        {
            juce::ScopedLock lock(chatLock);
            chatHistory.add("Failed to open video helper URL: " + helperUrlText);
            chatSenders.add("");
            chatRevision.fetch_add(1);
            if (chatHistory.size() > 100)
            {
                chatHistory.removeRange(0, chatHistory.size() - 100);
                chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
            }
        }
        return;
    }

    // NINJam sync currently depends on VDO.Ninja alpha chunked-mode fixes.
    // Switch this fallback to https://vdo.ninja/ after those fixes are promoted.
    juce::URL url("https://vdo.ninja/alpha/");
    url = url.withParameter("room", room)
             .withParameter("label", label)
             .withParameter("chunked", juce::String(videoBitrateKbps))
             .withParameter("chunkbitrate", juce::String(videoBitrateKbps))
             .withParameter("bitrate", juce::String(videoBitrateKbps))
             .withParameter("maxvideobitrate", juce::String(videoBitrateCeilingKbps))
             .withParameter("quality", "1")
             .withParameter("mfr", "30")
             .withParameter("chunkindex", "1")
             .withParameter("chunknack", "1")
             .withParameter("chunknackattempts", "8")
             .withParameter("chunknackdelay", "250")
             .withParameter("chunkchunksize", "4096")
             .withParameter("chunkcache", "30000")
             .withParameter("chunkbufferadaptive", "0")
             .withParameter("chunkbufferfloor", "0")
             .withParameter("chunkbufferceil", "180000")
             .withParameter("chunkedbuffer", "500")
             .withParameter("chunkadapt", "bitrate")
             .withParameter("chunkadaptfloor", "60")
             .withParameter("chunkadaptceil", juce::String(videoBitrateCeilingKbps))
             .withParameter("chunkadaptthreshold", "500")
             .withParameter("chunkadaptmaxdrop", "0")
             .withParameter("chunkadaptinterval", "1200")
             .withParameter("chunkadaptresolution", "1")
             .withParameter("noaudio", "1")
             .withParameter("buffer2", "0")
             .withParameter("buffer", juce::String(launchBufferMs));

    // Mobile hotspot mode: force TURN relay + TCP to bypass UDP blocking
    if (mobileHotspotModeEnabled.load(std::memory_order_relaxed))
    {
        url = url.withParameter("relay", "")
                 .withParameter("tcp", "");
    }
    {
        juce::ScopedLock lock(chatLock);
        chatHistory.add("Advanced sync helper unavailable on this machine; opening direct VDO view without live auto-buffer updates.");
        chatSenders.add("");
        chatHistory.add("Tip: If your cam isn't showing, refresh the video page and select your camera before entering the room.");
        chatSenders.add("");
        chatRevision.fetch_add(2);
        if (chatHistory.size() > 100)
        {
            chatHistory.removeRange(0, chatHistory.size() - 100);
            chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
        }
    }
    const auto directUrlText = url.toString(true);
    const bool opened = openUrlExternalOnMessageThread(directUrlText);
    if (!opened)
    {
        juce::ScopedLock lock(chatLock);
        chatHistory.add("Failed to open VDO URL: " + directUrlText);
        chatSenders.add("");
        chatRevision.fetch_add(1);
        if (chatHistory.size() > 100)
        {
            chatHistory.removeRange(0, chatHistory.size() - 100);
            chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
        }
    }
}

void NinjamVst3AudioProcessor::writeIntervalHelperJson(int pos, int length)
{
    if (!videoHelperRunning.load())
        return;

    const int safeLength = juce::jmax(1, length);
    const int displayInterval = getDisplayIntervalIndex();
    const int bpi = juce::jmax(1, getBPI());
    const double bpm = juce::jmax(1.0, (double)getBPM());
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const double wallClockMs = (double)juce::Time::currentTimeMillis();
    const double globalUnit = (double)displayInterval * (double)safeLength + (double)juce::jlimit(0, safeLength, pos);
    const double beatLength = (double)safeLength / (double)bpi;
    const double globalBeat = beatLength > 0.0 ? std::floor(globalUnit / beatLength) : 0.0;
    const juce::String syncTag = buildIntervalSyncTag(displayInterval, safeLength);

    juce::Array<juce::var> entries;
    juce::Array<juce::var> activeRoster;
    {
        juce::DynamicObject::Ptr infoObj = new juce::DynamicObject();
        infoObj->setProperty("type", "intervalInfo");
        infoObj->setProperty("interval", displayInterval);
        infoObj->setProperty("pos", pos);
        infoObj->setProperty("length", safeLength);
        infoObj->setProperty("bpm", bpm);
        infoObj->setProperty("bpi", bpi);
        infoObj->setProperty("globalUnit", globalUnit);
        infoObj->setProperty("globalBeat", globalBeat);
        infoObj->setProperty("videoClockMs", nowMs);
        infoObj->setProperty("wallClockMs", wallClockMs);
        infoObj->setProperty("syncTag", syncTag);
        infoObj->setProperty("bufferMode", "remote");
        infoObj->setProperty("voiceChatMode", false);
        entries.add(juce::var(infoObj.get()));
    }

    const int numUsers = ninjamClient.GetNumUsers();
    for (int userIdx = 0; userIdx < numUsers; ++userIdx)
    {
        const char* userNameChars = ninjamClient.GetUserState(userIdx, nullptr, nullptr, nullptr);
        if (!userNameChars || !userNameChars[0])
            continue;

        const juce::String userName = juce::String::fromUTF8(userNameChars);
        if (isKnownNinjamServerBotUser(userName))
            continue;

        const juce::String senderKey = normaliseOpusPeerId(userName);
        const juce::String canonicalUserKey = canonicalDelayUserKey(userName);
        const juce::String rosterKey = canonicalUserKey.isNotEmpty() ? canonicalUserKey : senderKey;
        if (rosterKey.isNotEmpty())
            activeRoster.add(rosterKey);
        const bool remoteVoiceChatMode = isRemoteUserVoiceChatMode(userIdx);
        time_t lastUpdate = 0;
        double maxLen = 0.0;
        const double userPos = ninjamClient.GetUserSessionPos(userIdx, &lastUpdate, &maxLen);

        int bufferMs = -1;
        int remoteServerLatencyMs = -1;
        int serverRouteLatencyMs = -1;
        int intervalSampleCount = 0;
        int lastIntervalMeasurementMs = -1;
        int averageIntervalMeasurementMs = -1;
        int firmIntervalMeasurementMs = -1;
        bool intervalMeasurementSeen = false;
        double lastIntervalSignalAgeMs = -1.0;
        double serverRouteAgeMs = -1.0;
        juce::uint64 bufferRefreshId = 0;
        {
            const juce::ScopedLock lock(intervalSyncAnnouncementLock);
            auto firmIt = remoteLatencyFirmDelayMsByUser.find(senderKey);
            if (firmIt != remoteLatencyFirmDelayMsByUser.end())
                bufferMs = juce::jmax(0, firmIt->second);
            if (bufferMs < 0 && canonicalUserKey.isNotEmpty())
            {
                auto canonicalFirmIt = remoteLatencyFirmDelayMsByUser.find(canonicalUserKey);
                if (canonicalFirmIt != remoteLatencyFirmDelayMsByUser.end())
                    bufferMs = juce::jmax(0, canonicalFirmIt->second);
            }
            auto serverIt = lastRemoteServerLatencyMsByUser.find(senderKey);
            if (serverIt != lastRemoteServerLatencyMsByUser.end())
                remoteServerLatencyMs = juce::jmax(0, serverIt->second);
            if (remoteServerLatencyMs < 0 && canonicalUserKey.isNotEmpty())
            {
                auto canonicalServerIt = lastRemoteServerLatencyMsByUser.find(canonicalUserKey);
                if (canonicalServerIt != lastRemoteServerLatencyMsByUser.end())
                    remoteServerLatencyMs = juce::jmax(0, canonicalServerIt->second);
            }
            auto routeIt = remoteServerRouteLatencyMsByUser.find(senderKey);
            if (routeIt != remoteServerRouteLatencyMsByUser.end())
                serverRouteLatencyMs = juce::jmax(0, routeIt->second);
            if (serverRouteLatencyMs < 0 && canonicalUserKey.isNotEmpty())
            {
                auto canonicalRouteIt = remoteServerRouteLatencyMsByUser.find(canonicalUserKey);
                if (canonicalRouteIt != remoteServerRouteLatencyMsByUser.end())
                    serverRouteLatencyMs = juce::jmax(0, canonicalRouteIt->second);
            }
            auto intervalSeenIt = lastRemoteIntervalSignalSeenMsByUser.find(senderKey);
            if (intervalSeenIt != lastRemoteIntervalSignalSeenMsByUser.end())
                lastIntervalSignalAgeMs = juce::jmax(0.0, nowMs - intervalSeenIt->second);
            if (lastIntervalSignalAgeMs < 0.0 && canonicalUserKey.isNotEmpty())
            {
                auto canonicalIntervalSeenIt = lastRemoteIntervalSignalSeenMsByUser.find(canonicalUserKey);
                if (canonicalIntervalSeenIt != lastRemoteIntervalSignalSeenMsByUser.end())
                    lastIntervalSignalAgeMs = juce::jmax(0.0, nowMs - canonicalIntervalSeenIt->second);
            }
            auto routeSeenIt = lastRemoteRouteProbeSeenMsByUser.find(senderKey);
            if (routeSeenIt != lastRemoteRouteProbeSeenMsByUser.end())
                serverRouteAgeMs = juce::jmax(0.0, nowMs - routeSeenIt->second);
            if (serverRouteAgeMs < 0.0 && canonicalUserKey.isNotEmpty())
            {
                auto canonicalRouteSeenIt = lastRemoteRouteProbeSeenMsByUser.find(canonicalUserKey);
                if (canonicalRouteSeenIt != lastRemoteRouteProbeSeenMsByUser.end())
                    serverRouteAgeMs = juce::jmax(0.0, nowMs - canonicalRouteSeenIt->second);
            }
            const int routeLatencyForBufferMs = serverRouteLatencyMs >= 0 ? serverRouteLatencyMs : 0;
            auto refreshIt = remoteVideoBufferRefreshIdByUser.find(senderKey);
            if (refreshIt != remoteVideoBufferRefreshIdByUser.end())
                bufferRefreshId = refreshIt->second;
            if (bufferRefreshId == 0 && canonicalUserKey.isNotEmpty())
            {
                auto canonicalRefreshIt = remoteVideoBufferRefreshIdByUser.find(canonicalUserKey);
                if (canonicalRefreshIt != remoteVideoBufferRefreshIdByUser.end())
                    bufferRefreshId = canonicalRefreshIt->second;
            }
            if (bufferMs < 0)
            {
                auto avgIt = remoteLatencyAverageByUser.find(senderKey);
                if (avgIt != remoteLatencyAverageByUser.end())
                {
                    const auto& state = avgIt->second;
                    intervalSampleCount = juce::jmax(intervalSampleCount, state.sampleCount);
                    if (state.lastMeasurementMs >= 0.0)
                    {
                        intervalMeasurementSeen = true;
                        lastIntervalMeasurementMs = juce::jmax(lastIntervalMeasurementMs, (int)std::llround(state.lastMeasurementMs));
                    }
                    if (state.averageMs > 0.0)
                        averageIntervalMeasurementMs = juce::jmax(averageIntervalMeasurementMs, (int)std::llround(state.averageMs));
                    if (state.firmAverageMs > 0.0)
                        firmIntervalMeasurementMs = juce::jmax(firmIntervalMeasurementMs, (int)std::llround(state.firmAverageMs));
                    double fallback = state.firmAverageMs;
                    if (!(fallback > 0.0))
                        fallback = state.averageMs;
                    if (!(fallback > 0.0))
                        fallback = state.lastMeasurementMs;
                    if (fallback > 0.0)
                        // The measured delay already includes network route latency,
                        // so we do NOT add routeLatencyForBufferMs on top.
                        bufferMs = juce::jmax(0, (int)std::llround(fallback));
                }
            }
            if (bufferMs < 0 && canonicalUserKey.isNotEmpty())
            {
                auto canonicalAvgIt = remoteLatencyAverageByUser.find(canonicalUserKey);
                if (canonicalAvgIt != remoteLatencyAverageByUser.end())
                {
                    const auto& state = canonicalAvgIt->second;
                    intervalSampleCount = juce::jmax(intervalSampleCount, state.sampleCount);
                    if (state.lastMeasurementMs >= 0.0)
                    {
                        intervalMeasurementSeen = true;
                        lastIntervalMeasurementMs = juce::jmax(lastIntervalMeasurementMs, (int)std::llround(state.lastMeasurementMs));
                    }
                    if (state.averageMs > 0.0)
                        averageIntervalMeasurementMs = juce::jmax(averageIntervalMeasurementMs, (int)std::llround(state.averageMs));
                    if (state.firmAverageMs > 0.0)
                        firmIntervalMeasurementMs = juce::jmax(firmIntervalMeasurementMs, (int)std::llround(state.firmAverageMs));
                    double fallback = state.firmAverageMs;
                    if (!(fallback > 0.0))
                        fallback = state.averageMs;
                    if (!(fallback > 0.0))
                        fallback = state.lastMeasurementMs;
                    if (fallback > 0.0)
                        // The measured delay already includes network route latency,
                        // so we do NOT add routeLatencyForBufferMs on top.
                        bufferMs = juce::jmax(0, (int)std::llround(fallback));
                }
            }
            if (!intervalMeasurementSeen)
            {
                auto avgIt = remoteLatencyAverageByUser.find(senderKey);
                if (avgIt != remoteLatencyAverageByUser.end())
                {
                    const auto& state = avgIt->second;
                    intervalSampleCount = juce::jmax(intervalSampleCount, state.sampleCount);
                    if (state.lastMeasurementMs >= 0.0)
                    {
                        intervalMeasurementSeen = true;
                        lastIntervalMeasurementMs = juce::jmax(lastIntervalMeasurementMs, (int)std::llround(state.lastMeasurementMs));
                    }
                    if (state.averageMs > 0.0)
                        averageIntervalMeasurementMs = juce::jmax(averageIntervalMeasurementMs, (int)std::llround(state.averageMs));
                    if (state.firmAverageMs > 0.0)
                        firmIntervalMeasurementMs = juce::jmax(firmIntervalMeasurementMs, (int)std::llround(state.firmAverageMs));
                }
            }
            if (canonicalUserKey.isNotEmpty())
            {
                auto canonicalAvgIt = remoteLatencyAverageByUser.find(canonicalUserKey);
                if (canonicalAvgIt != remoteLatencyAverageByUser.end())
                {
                    const auto& state = canonicalAvgIt->second;
                    intervalSampleCount = juce::jmax(intervalSampleCount, state.sampleCount);
                    if (state.lastMeasurementMs >= 0.0)
                    {
                        intervalMeasurementSeen = true;
                        lastIntervalMeasurementMs = juce::jmax(lastIntervalMeasurementMs, (int)std::llround(state.lastMeasurementMs));
                    }
                    if (state.averageMs > 0.0)
                        averageIntervalMeasurementMs = juce::jmax(averageIntervalMeasurementMs, (int)std::llround(state.averageMs));
                    if (state.firmAverageMs > 0.0)
                        firmIntervalMeasurementMs = juce::jmax(firmIntervalMeasurementMs, (int)std::llround(state.firmAverageMs));
                }
            }
            // Diagnostic log per-user buffer decision
        }

        juce::DynamicObject::Ptr userObj = new juce::DynamicObject();
        userObj->setProperty("type", "videoTimecode");
        userObj->setProperty("userId", userName);
        userObj->setProperty("userKey", canonicalUserKey);
        userObj->setProperty("interval", displayInterval);
        userObj->setProperty("timecode", userPos);
        userObj->setProperty("globalUnit", (double)displayInterval * (double)safeLength + userPos);
        userObj->setProperty("globalBeat", globalBeat);
        userObj->setProperty("videoClockMs", nowMs);
        userObj->setProperty("syncTag", syncTag);
        userObj->setProperty("bufferMode", remoteVoiceChatMode ? "realtime" : "remote");
        userObj->setProperty("voiceChatMode", remoteVoiceChatMode);
        userObj->setProperty("intervalMeasurementSeen", intervalMeasurementSeen);
        userObj->setProperty("intervalSampleCount", intervalSampleCount);
        if (lastIntervalMeasurementMs >= 0)
            userObj->setProperty("lastIntervalMeasurementMs", lastIntervalMeasurementMs);
        if (averageIntervalMeasurementMs >= 0)
            userObj->setProperty("averageIntervalMeasurementMs", averageIntervalMeasurementMs);
        if (firmIntervalMeasurementMs >= 0)
            userObj->setProperty("firmIntervalMeasurementMs", firmIntervalMeasurementMs);
        userObj->setProperty("bufferCalculated", bufferMs >= 0);
        userObj->setProperty("sideSignalMode", ninjamSideSignalServerSupported.load(std::memory_order_relaxed));
        userObj->setProperty("serverRouteLatencyReady", serverRouteLatencyMs >= 0);
        if (lastIntervalSignalAgeMs >= 0.0)
            userObj->setProperty("intervalSignalAgeMs", lastIntervalSignalAgeMs);
        if (serverRouteAgeMs >= 0.0)
            userObj->setProperty("serverRouteAgeMs", serverRouteAgeMs);
        if (bufferMs >= 0)
        {
            userObj->setProperty("bufferTotalMs", (double)bufferMs);
            userObj->setProperty("senderBufferMs", 0.0);
            userObj->setProperty("receiverBufferMs", (double)bufferMs);
            userObj->setProperty("receiverBufferFinal", true);
            userObj->setProperty("measuredAudioDelayMs", (double)bufferMs);
            if (remoteServerLatencyMs >= 0)
                userObj->setProperty("senderServerLatencyMs", (double)remoteServerLatencyMs);
            if (serverRouteLatencyMs >= 0)
                userObj->setProperty("serverRouteLatencyMs", (double)serverRouteLatencyMs);
            if (bufferRefreshId != 0)
            {
                userObj->setProperty("refreshBuffer", true);
                userObj->setProperty("bufferRefreshEventId", "videoBufferRefresh:" + canonicalUserKey + ":" + juce::String((juce::int64)bufferRefreshId));
            }
        }
        entries.add(juce::var(userObj.get()));
    }

    {
        juce::DynamicObject::Ptr rosterObj = new juce::DynamicObject();
        rosterObj->setProperty("type", "activeRoster");
        rosterObj->setProperty("revision", juce::String((juce::int64)vdoRosterRevision.load(std::memory_order_relaxed)));
        rosterObj->setProperty("videoClockMs", nowMs);
        rosterObj->setProperty("users", juce::var(activeRoster));
        entries.add(juce::var(rosterObj.get()));
    }

    const juce::String payload = juce::JSON::toString(juce::var(entries), false);
    const juce::ScopedLock lock(intervalHelperPayloadLock);
    intervalHelperPayload = payload;
}

bool NinjamVst3AudioProcessor::isTransmittingLocal() const
{
    return isTransmitting;
}

juce::StringArray NinjamVst3AudioProcessor::getChatMessages()
{
    juce::ScopedLock lock(chatLock);
    return chatHistory;
}

void NinjamVst3AudioProcessor::setLocalChatColourKey(const juce::String& colourKey)
{
    const juce::String normalised = normaliseChatColourKey(colourKey);
    bool changed = false;

    {
        const juce::ScopedLock lock(chatStyleLock);
        changed = localChatColourKey != normalised;
        localChatColourKey = normalised;

        const juce::String localKey = normaliseOpusPeerId(currentUser);
        if (localKey.isNotEmpty())
            chatColourKeyByUser[localKey] = normalised;
    }

    if (changed)
    {
        chatRevision.fetch_add(1);
        broadcastChatStyle();
    }
}

juce::String NinjamVst3AudioProcessor::getLocalChatColourKey() const
{
    const juce::ScopedLock lock(chatStyleLock);
    return localChatColourKey;
}

juce::String NinjamVst3AudioProcessor::getChatColourKeyForSender(const juce::String& sender) const
{
    if (sender == "me")
        return getLocalChatColourKey();

    const juce::String senderKey = normaliseOpusPeerId(sender);
    if (senderKey.isEmpty())
        return {};

    const juce::ScopedLock lock(chatStyleLock);
    auto it = chatColourKeyByUser.find(senderKey);
    return it != chatColourKeyByUser.end() ? it->second : juce::String();
}

void NinjamVst3AudioProcessor::broadcastChatStyle()
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
        return;

    const juce::String userId = normaliseOpusPeerId(currentUser);
    const juce::String colourKey = getLocalChatColourKey();
    if (colourKey.isEmpty())
        return;

    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("userId", userId.isNotEmpty() ? userId : currentUser.trim());
    obj->setProperty("appFamily", opusSyncAppFamily);
    obj->setProperty("colourKey", colourKey);
    obj->setProperty("eventId", "chatStyle:" + (userId.isNotEmpty() ? userId : currentUser.trim()) + ":" + juce::String(++sideSignalEventCounter));
    sendSideSignal("*", "chatStyle", juce::JSON::toString(juce::var(obj.get()), false));
}

void NinjamVst3AudioProcessor::addSystemChatMessage(const juce::String& message)
{
    const juce::ScopedLock lock(chatLock);
    chatHistory.add(message);
    chatSenders.add("");
    chatRevision.fetch_add(1);
    if (chatHistory.size() > 100)
    {
        chatHistory.removeRange(0, chatHistory.size() - 100);
        chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
    }
}

void NinjamVst3AudioProcessor::noteTranslationFailure(const juce::String& reason)
{

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    bool shouldPostNotice = false;
    {
        const juce::ScopedLock lock(chatLock);
        const bool reasonChanged = lastTranslationFailureReason != reason;
        const bool cooldownExpired = (nowMs - lastTranslationFailureNoticeMs) >= 30000.0;

        if (!translationFailureActive || reasonChanged || cooldownExpired)
        {
            translationFailureActive = true;
            lastTranslationFailureNoticeMs = nowMs;
            lastTranslationFailureReason = reason;
            shouldPostNotice = true;
        }
    }

    if (shouldPostNotice)
    {
        addSystemChatMessage("Auto Translate failed; incoming chat will stay in the original language until the translator responds again.");
    }
}

void NinjamVst3AudioProcessor::clearTranslationFailureState()
{
    const juce::ScopedLock lock(chatLock);
    translationFailureActive = false;
    lastTranslationFailureReason.clear();
}

void NinjamVst3AudioProcessor::setAutoTranslateEnabled(bool shouldEnable)
{
    bool changed = false;
    {
        juce::ScopedLock lock(chatLock);
        changed = autoTranslate != shouldEnable;
        autoTranslate = shouldEnable;
    }

    if (changed)
        translationConfigRevision.fetch_add(1, std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::isAutoTranslateEnabled() const
{
    return autoTranslate;
}

void NinjamVst3AudioProcessor::setTranslateSourceLang(const juce::String& langCode)
{
    juce::ScopedLock lock(chatLock);
    juce::ignoreUnused(langCode);
    translateSourceLang = "auto";
}

juce::String NinjamVst3AudioProcessor::getTranslateSourceLang() const
{
    return translateSourceLang;
}

void NinjamVst3AudioProcessor::setTranslateTargetLang(const juce::String& langCode)
{
    juce::String normalised = langCode.trim().toLowerCase();
    if (normalised.isEmpty())
        normalised = "system";

    bool changed = false;
    {
        juce::ScopedLock lock(chatLock);
        changed = translateTargetLang != normalised;
        translateTargetLang = normalised;
    }

    if (changed)
        translationConfigRevision.fetch_add(1, std::memory_order_relaxed);
}

juce::String NinjamVst3AudioProcessor::getTranslateTargetLang() const
{
    return translateTargetLang;
}

std::vector<NinjamVst3AudioProcessor::UserInfo> NinjamVst3AudioProcessor::getConnectedUsers()
{
    std::vector<UserInfo> users;
    int numUsers = ninjamClient.GetNumUsers();
    bool spread = spreadOutputsEnabled.load();

    const int totalOutputChannels = juce::jmax(2, getTotalNumOutputChannels());
    const int maxOutputPairs = juce::jmin(16, totalOutputChannels / 2);
    std::set<int> reservedPairs;
    if (spread)
    {
        for (auto& kv : userOutputAssignment)
        {
            int pair = kv.second;
            if (pair >= 0 && pair < maxOutputPairs)
                reservedPairs.insert(pair);
        }

        const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
        for (const auto& pair : remoteLinkAudioOutputPairs)
        {
            if (pair.second >= 0 && pair.second < maxOutputPairs)
                reservedPairs.insert(pair.second);
        }
    }

    std::set<int> usedPairsThisCall;
    std::set<int> activeUserIndexes;

    for (int i=0; i<numUsers; ++i)
    {
        const char* name = ninjamClient.GetUserState(i, nullptr, nullptr, nullptr);
        if (name)
        {
            if (isKnownNinjamServerBotUser(juce::String::fromUTF8(name)))
                continue;

            bool hasAudioChannel = false;
            for (int ch = 0; ch < 32; ++ch)
            {
                int channelFlags = 0;
                const char* channelName = ninjamClient.GetUserChannelState(i, ch,
                                                                           nullptr, nullptr, nullptr,
                                                                           nullptr, nullptr, nullptr,
                                                                           &channelFlags);
                if (channelName != nullptr && ((channelFlags & kNinjamZapVideoOnlyChannelFlag) == 0))
                {
                    hasAudioChannel = true;
                    break;
                }
            }
            if (!hasAudioChannel)
                continue;

            activeUserIndexes.insert(i);
            UserInfo u;
            u.index = i;
            juce::String fullName = juce::String::fromUTF8(name);
            int atPos = fullName.indexOfChar('@');
            if (atPos > 0)
                u.name = fullName.substring(0, atPos);
            else
                u.name = fullName;

            auto identityIt = remoteUserNameByIndex.find(i);
            if (identityIt == remoteUserNameByIndex.end() || identityIt->second != fullName)
            {
                resetRemoteUserIndexState(i, u.name);
                remoteUserNameByIndex[i] = fullName;
            }

            bool sub = false;
            float chVol = 1.0f, chPan = 0.0f;
            bool chMute = false, chSolo = false;
            int outCh = 0, flags = 0;
            const char* chName = ninjamClient.GetUserChannelState(i, 0, &sub, &chVol, &chPan, &chMute, &chSolo, &outCh, &flags);
            if (chName && ((flags & kNinjamZapVideoOnlyChannelFlag) == 0))
            {
                float baseVol = chVol;
                bool hasStored = false;
                auto byNameIt = userVolumeByName.find(u.name);
                if (byNameIt != userVolumeByName.end())
                {
                    baseVol = byNameIt->second;
                    hasStored = true;
                }

                auto volIt = userBaseVolume.find(i);
                if (volIt != userBaseVolume.end())
                {
                    baseVol = volIt->second;
                    hasStored = true;
                }

                if (!hasStored)
                    baseVol = 1.0f;

                u.volume = baseVol;

                auto panIt = userPanOverrides.find(i);
                if (panIt != userPanOverrides.end())
                    u.pan = panIt->second;
                else
                    u.pan = chPan;

                u.isMuted = chMute;
                u.isSolo = chSolo;
                u.outputChannel = outCh;

                if (!hasStored || std::abs(baseVol - chVol) > 1.0e-4f)
                    setUserVolume(i, baseVol);
            }
            else
            {
                float baseVol = 1.0f;
                bool hasStored = false;
                auto byNameIt = userVolumeByName.find(u.name);
                if (byNameIt != userVolumeByName.end())
                {
                    baseVol = byNameIt->second;
                    hasStored = true;
                }

                auto volIt = userBaseVolume.find(i);
                if (volIt != userBaseVolume.end())
                {
                    baseVol = volIt->second;
                    hasStored = true;
                }

                u.volume = baseVol;

                u.pan = 0.0f;
                u.isMuted = false;
                u.isSolo = false;
                u.outputChannel = ninjamClient.GetUserChannelOutput(i, 0);

                if (!hasStored)
                    setUserVolume(i, baseVol);
            }

            int linkOutputPair = -1;
            {
                const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
                auto linkIt = remoteLinkAudioOutputPairs.find(u.name);
                if (linkIt != remoteLinkAudioOutputPairs.end())
                    linkOutputPair = linkIt->second;
            }

            if (linkOutputPair >= 0)
            {
                const int desiredChannel = linkOutputPair * 2;
                if (u.outputChannel != desiredChannel)
                {
                    for (int ch = 0; ch < 32; ++ch)
                    {
                        if (!isNinjamRemoteChannelVideoOnly(i, ch))
                            ninjamClient.SetUserChannelState(i, ch, false, false, false, 0, false, 0, false, false, false, false, true, desiredChannel);
                    }
                }
                u.outputChannel = desiredChannel;
                u.outputUsesLinkAudio = true;
                usedPairsThisCall.insert(linkOutputPair);
            }
            else if (spread)
            {
                juce::String shortName = u.name;
                auto itAssign = userOutputAssignment.find(shortName);
                int desiredPair = -1;
                const int startPair = spreadOutputStartPair.load(std::memory_order_relaxed);

                if (itAssign != userOutputAssignment.end())
                {
                    desiredPair = itAssign->second;
                }
                else
                {
                    if ((int)reservedPairs.size() < maxOutputPairs)
                    {
                        for (int i = 0; i < maxOutputPairs; ++i)
                        {
                            int cand = (startPair + i) % maxOutputPairs;
                            if (cand >= startPair && !reservedPairs.count(cand))
                            {
                                desiredPair = cand;
                                reservedPairs.insert(cand);
                                break;
                            }
                        }
                    }
                    else
                    {
                        std::set<int> connectedNow = usedPairsThisCall;
                        int fallback = -1;
                        for (int i = 0; i < maxOutputPairs; ++i)
                        {
                            int cand = (startPair + i) % maxOutputPairs;
                            if (cand >= startPair && !connectedNow.count(cand))
                            {
                                fallback = cand;
                                break;
                            }
                        }
                        if (fallback < 0)
                            fallback = startPair;
                        desiredPair = fallback;
                    }

                    userOutputAssignment[shortName] = desiredPair;
                }

                if (desiredPair >= 0)
                {
                    int desiredChannel = desiredPair * 2;
                    if (u.outputChannel != desiredChannel)
                        setUserOutput(i, desiredChannel);
                    u.outputChannel = desiredChannel;
                    usedPairsThisCall.insert(desiredPair);
                }
            }

            userBaseVolume[i] = u.volume;
            userVolumeByName[u.name] = u.volume;
            if (i >= 0 && i < maxRemoteChordUsers)
            {
                remoteOpusUserVolume[(size_t)i].store(u.volume, std::memory_order_relaxed);
                remoteOpusUserPan[(size_t)i].store(u.pan, std::memory_order_relaxed);
                remoteOpusUserMute[(size_t)i].store(u.isMuted, std::memory_order_relaxed);
                remoteOpusUserSolo[(size_t)i].store(u.isSolo, std::memory_order_relaxed);
                remoteOpusUserOutput[(size_t)i].store(u.outputChannel, std::memory_order_relaxed);
            }

            // Look up multichannel state from the snapshot updated by refreshOpusSyncAvailabilityFromUsers().
            // This map is keyed by normalised username and never holds njclient locks.
            int peerOpusBaseChannel = 1;
            PeerMultiChanInfo peerInfo;
            bool hasPeerInfo = false;
            {
                const juce::String normName = canonicalDelayUserKey(u.name);
                const juce::ScopedLock mcLock(peerMultiChanLock);
                auto it = peerMultiChanByName.find(normName);
                if (it != peerMultiChanByName.end())
                {
                    peerInfo = it->second;
                    hasPeerInfo = true;
                    u.isMultiChanPeer = it->second.isMultiChan;
                    peerOpusBaseChannel = juce::jlimit(1, 31, it->second.opusBaseChannel);
                    if (u.isMultiChanPeer)
                        u.numChannels = juce::jmax(2, it->second.numChannels);
                }
            }

            // Populate channel names from NINJAM state (safe: no locks held here)
            if (u.isMultiChanPeer)
            {
                u.channelNames.clear();
                for (int ch = 0; ch < u.numChannels; ++ch)
                {
                    const juce::String fallbackName = "Ch " + juce::String(ch + 1);
                    if (hasPeerInfo && ch < peerInfo.channelNames.size() && peerInfo.channelNames[ch].isNotEmpty())
                        u.channelNames.add(peerInfo.channelNames[ch]);
                    else
                        u.channelNames.add(fallbackName);

                    if (i >= 0 && i < maxRemoteChordUsers)
                        remoteOpusChannelWidths[(size_t)i][(size_t)ch].store(hasPeerInfo ? juce::jlimit(1, 2, peerInfo.channelWidths[(size_t)ch]) : 1,
                                                                            std::memory_order_relaxed);
                }

                if (i >= 0 && i < maxRemoteChordUsers)
                {
                    remoteOpusPeerActive[(size_t)i].store(true, std::memory_order_relaxed);
                    remoteOpusCarrierChannel[(size_t)i].store(peerOpusBaseChannel, std::memory_order_relaxed);
                    remoteOpusVirtualChannelCount[(size_t)i].store(u.numChannels, std::memory_order_relaxed);
                    remoteOpusPackedChannelCount[(size_t)i].store(hasPeerInfo ? peerInfo.packedChannelCount : u.numChannels, std::memory_order_relaxed);
                    njplus_debug_log("PEERACTIVE user=%d (%s) base=%d vcount=%d packed=%d numCh=%d", i, u.name.toRawUTF8(), peerOpusBaseChannel, u.numChannels, hasPeerInfo ? peerInfo.packedChannelCount : u.numChannels, u.numChannels);
                }
            }
            else
            {
                // Count basic NINJAM channel names for non-VST3 peers (display only, no expand button)
                u.channelNames.clear();
                for (int ch = 0; ch < 32; ++ch)
                {
                    int channelFlags = 0;
                    const char* chName = ninjamClient.GetUserChannelState(i, ch,
                                                                          nullptr, nullptr, nullptr,
                                                                          nullptr, nullptr, nullptr,
                                                                          &channelFlags);
                    if (chName != nullptr && ((channelFlags & kNinjamZapVideoOnlyChannelFlag) == 0))
                    {
                        ++u.numChannels;
                        u.channelNames.add(juce::String::fromUTF8(chName));
                    }
                }
                if (u.numChannels < 1) { u.numChannels = 1; u.channelNames.add(""); }
                if (i >= 0 && i < maxRemoteChordUsers)
                    remoteOpusPeerActive[(size_t)i].store(false, std::memory_order_relaxed);
            }

            if (i >= 0 && i < maxRemoteChordUsers)
            {
                if (remoteChordUserKeys[(size_t)i] != u.name)
                {
                    remoteChordUserKeys[(size_t)i] = u.name;
                    remoteChordDetectionEnabled[(size_t)i].store(true, std::memory_order_relaxed);
                    if (chordAnalyzer && chordAnalyzer->isPrepared())
                        chordAnalyzer->resetTrack(BatchedChordAnalyzer::remoteTrackIndexForUser(i));
                }

                if (chordAnalyzer && !chordAnalyzer->isPrepared())
                    chordAnalyzer->prepare(processingSampleRate);
            }

            users.push_back(u);
        }
    }

    for (int i = numUsers; i < maxRemoteChordUsers; ++i)
    {
        remoteChordUserKeys[(size_t)i].clear();
        remoteChordDetectionEnabled[(size_t)i].store(true, std::memory_order_relaxed);
        remoteOpusPeerActive[(size_t)i].store(false, std::memory_order_relaxed);
        remoteOpusCarrierChannel[(size_t)i].store(-1, std::memory_order_relaxed);
        remoteOpusVirtualChannelCount[(size_t)i].store(1, std::memory_order_relaxed);
        remoteOpusPackedChannelCount[(size_t)i].store(0, std::memory_order_relaxed);
        remoteOpusCombinedPeakL[(size_t)i].store(0.0f, std::memory_order_relaxed);
        remoteOpusCombinedPeakR[(size_t)i].store(0.0f, std::memory_order_relaxed);
        remoteOpusSourcePeakL[(size_t)i].store(0.0f, std::memory_order_relaxed);
        remoteOpusSourcePeakR[(size_t)i].store(0.0f, std::memory_order_relaxed);
        if (chordAnalyzer && chordAnalyzer->isPrepared())
            chordAnalyzer->resetTrack(BatchedChordAnalyzer::remoteTrackIndexForUser(i));
    }

    for (auto it = remoteUserNameByIndex.begin(); it != remoteUserNameByIndex.end();)
    {
        const int userIndex = it->first;
        if (activeUserIndexes.count(userIndex) == 0)
        {
            const juce::String removedUserKey = it->second;
            userBaseVolume.erase(userIndex);
            userPanOverrides.erase(userIndex);
            userClipEnabled.erase(userIndex);
            it = remoteUserNameByIndex.erase(it);
            if (removedUserKey.isNotEmpty())
            {
                vdoRosterRevision.fetch_add(1, std::memory_order_relaxed);
                intervalHelperPayloadForceWrite.store(true, std::memory_order_release);
                lastIntervalHelperPayloadWriteMs = 0.0;
            }
        }
        else
        {
            ++it;
        }
    }

    return users;
}

void NinjamVst3AudioProcessor::rememberUserVolume(int userIndex, float volume, const juce::String& name)
{
    userBaseVolume[userIndex] = volume;
    juce::String shortName = name;
    int atPos = shortName.indexOfChar('@');
    if (atPos > 0)
        shortName = shortName.substring(0, atPos);
    userVolumeByName[shortName] = volume;
}

void NinjamVst3AudioProcessor::resetRemoteUserIndexState(int userIndex, const juce::String& userName)
{
    const int numUsers = ninjamClient.GetNumUsers();
    if (userIndex < 0 || userIndex >= numUsers)
        return;

    userBaseVolume.erase(userIndex);
    userPanOverrides.erase(userIndex);
    userClipEnabled.erase(userIndex);

    if (userIndex >= 0 && userIndex < maxRemoteChordUsers)
    {
        remoteChordDetectionEnabled[(size_t)userIndex].store(true, std::memory_order_relaxed);
        remoteChordUserKeys[(size_t)userIndex].clear();
        remoteOpusPeerActive[(size_t)userIndex].store(false, std::memory_order_relaxed);
        remoteOpusCarrierChannel[(size_t)userIndex].store(-1, std::memory_order_relaxed);
        remoteOpusVirtualChannelCount[(size_t)userIndex].store(1, std::memory_order_relaxed);
        remoteOpusPackedChannelCount[(size_t)userIndex].store(0, std::memory_order_relaxed);
        remoteOpusCombinedPeakL[(size_t)userIndex].store(0.0f, std::memory_order_relaxed);
        remoteOpusCombinedPeakR[(size_t)userIndex].store(0.0f, std::memory_order_relaxed);
        remoteOpusSourcePeakL[(size_t)userIndex].store(0.0f, std::memory_order_relaxed);
        remoteOpusSourcePeakR[(size_t)userIndex].store(0.0f, std::memory_order_relaxed);
        if (chordAnalyzer && chordAnalyzer->isPrepared())
            chordAnalyzer->resetTrack(BatchedChordAnalyzer::remoteTrackIndexForUser(userIndex));
    }

    float baseVol = 1.0f;
    const juce::String shortName = makeShortUserName(userName);
    auto byNameIt = userVolumeByName.find(shortName);
    if (byNameIt != userVolumeByName.end())
        baseVol = byNameIt->second;

    for (int ch = 0; ch < 32; ++ch)
    {
        ninjamClient.SetUserChannelState(userIndex, ch,
                                         false, false,
                                         true, baseVol,
                                         true, 0.0f,
                                         true, false,
                                         true, false);
    }

    const juce::String senderKey = normaliseOpusPeerId(userName);
    const juce::String canonicalSenderKey = canonicalDelayUserKey(senderKey);
    const juce::String senderPrefix = senderKey + ":";
    const juce::String canonicalSenderPrefix = canonicalSenderKey + ":";
    {
        const juce::ScopedLock lock(intervalSyncAnnouncementLock);
        auto eraseForUser = [&senderKey, &canonicalSenderKey](auto& state)
        {
            if (senderKey.isNotEmpty())
                state.erase(senderKey);
            if (canonicalSenderKey.isNotEmpty())
                state.erase(canonicalSenderKey);
        };

        eraseForUser(lastAnnouncedRemoteIntervalByUser);
        eraseForUser(lastRemoteServerLatencyMsByUser);
        eraseForUser(remoteServerRouteLatencyMsByUser);
        eraseForUser(lastRemoteIntervalSignalSeenMsByUser);
        eraseForUser(lastRemoteRouteProbeSeenMsByUser);
        eraseForUser(remoteLatencyLastAppliedIntervalByUser);
        eraseForUser(remoteLatencyAverageByUser);
        eraseForUser(remoteLatencyFirmDelayMsByUser);
        eraseForUser(remoteVideoBufferRefreshIdByUser);

        for (auto it = pendingRemoteIntervalStartsByUser.begin(); it != pendingRemoteIntervalStartsByUser.end();)
        {
            const auto& pendingSenderKey = it->second.senderKey;
            if (pendingSenderKey == senderKey || pendingSenderKey == canonicalSenderKey)
                it = pendingRemoteIntervalStartsByUser.erase(it);
            else
                ++it;
        }
    }

    {
        const juce::ScopedLock lock(ninjamZapVideoChunkLock);
        auto eraseBySenderPrefix = [&senderPrefix, &canonicalSenderPrefix](auto& state)
        {
            for (auto it = state.begin(); it != state.end();)
            {
                const auto& key = it->first;
                if (key.startsWith(senderPrefix) || key.startsWith(canonicalSenderPrefix))
                    it = state.erase(it);
                else
                    ++it;
            }
        };
        eraseBySenderPrefix(ninjamZapVideoChunkReassemblers);
        eraseBySenderPrefix(ninjamZapVideoAudioGuidByReassemblyKey);
        eraseBySenderPrefix(ninjamZapVideoMarkerIntervalByReassemblyKey);
        eraseBySenderPrefix(ninjamZapVideoMarkerSeenByReassemblyKey);
        if (senderKey.isNotEmpty())
            remoteVideoChunkReassemblersByUser.erase(senderKey);
        if (canonicalSenderKey.isNotEmpty())
            remoteVideoChunkReassemblersByUser.erase(canonicalSenderKey);
    }
}

void NinjamVst3AudioProcessor::setUserOutput(int userIndex, int outputChannelIndex)
{
    const int numUsers = ninjamClient.GetNumUsers();
    if (userIndex < 0 || userIndex >= numUsers)
        return;

    if (userIndex >= 0 && userIndex < maxRemoteChordUsers)
        remoteOpusUserOutput[(size_t)userIndex].store(outputChannelIndex, std::memory_order_relaxed);

    // Update all channels for this user to the new output
    // Iterate through all potential channels (MAX_USER_CHANNELS is 32)
    for (int i = 0; i < 32; ++i)
    {
        // SetUserChannelState arguments: useridx, channelidx, setsub, sub, setvol, vol, setpan, pan, setmute, mute, setsolo, solo, setoutch, outchannel
        if (!isNinjamRemoteChannelVideoOnly(userIndex, i))
            ninjamClient.SetUserChannelState(userIndex, i, false, false, false, 0, false, 0, false, false, false, false, true, outputChannelIndex);
    }

    const char* name = ninjamClient.GetUserState(userIndex, nullptr, nullptr, nullptr);
    if (name)
    {
        const juce::String shortName = makeShortUserName(juce::String::fromUTF8(name));
        {
            const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
            remoteLinkAudioOutputPairs.erase(shortName);
            remoteLinkAudioSinks.erase(shortName);
        }
        int pairIndex = (outputChannelIndex & 1023) / 2;
        userOutputAssignment[shortName] = pairIndex;
    }

    lastLinkAudioEndpointRefreshMs = 0.0;
    rebuildLinkAudioEndpoints();
}

bool NinjamVst3AudioProcessor::setUserOutputToLinkAudio(int userIndex)
{
    if (!isLinkAudioEnabled() || !isLinkAudioSendEnabled())
        return false;

    const int numUsers = ninjamClient.GetNumUsers();
    if (userIndex < 0 || userIndex >= numUsers)
        return false;

    const char* nameChars = ninjamClient.GetUserState(userIndex, nullptr, nullptr, nullptr);
    if (nameChars == nullptr || nameChars[0] == 0)
        return false;

    const juce::String shortName = makeShortUserName(juce::String::fromUTF8(nameChars));
    const int maxPairs = getTotalNumOutputChannels() / 2;
    if (maxPairs <= 0)
        return false;

    std::set<int> usedPairs;
    for (int otherUser = 0; otherUser < numUsers; ++otherUser)
    {
        if (otherUser == userIndex)
            continue;

        const char* otherNameChars = ninjamClient.GetUserState(otherUser, nullptr, nullptr, nullptr);
        if (otherNameChars == nullptr || otherNameChars[0] == 0)
            continue;

        const juce::String otherShortName = makeShortUserName(juce::String::fromUTF8(otherNameChars));
        int pairIndex = -1;
        {
            const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
            auto linkIt = remoteLinkAudioOutputPairs.find(otherShortName);
            if (linkIt != remoteLinkAudioOutputPairs.end())
                pairIndex = linkIt->second;
        }

        if (pairIndex < 0)
        {
            const int outChannel = ninjamClient.GetUserChannelOutput(otherUser, 0);
            pairIndex = (outChannel & 1023) / 2;
        }

        if (pairIndex >= 0 && pairIndex < maxPairs)
            usedPairs.insert(pairIndex);
    }

    int stagingPair = -1;
    {
        const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
        auto existing = remoteLinkAudioOutputPairs.find(shortName);
        if (existing != remoteLinkAudioOutputPairs.end())
            stagingPair = existing->second;
    }

    if (stagingPair < 0)
    {
        for (int candidate = maxPairs - 1; candidate >= 0; --candidate)
        {
            if (!usedPairs.count(candidate))
            {
                stagingPair = candidate;
                break;
            }
        }
    }

    if (stagingPair < 0)
        return false;

    if (userIndex >= 0 && userIndex < maxRemoteChordUsers)
        remoteOpusUserOutput[(size_t)userIndex].store(stagingPair * 2, std::memory_order_relaxed);

    for (int channel = 0; channel < 32; ++channel)
    {
        if (!isNinjamRemoteChannelVideoOnly(userIndex, channel))
            ninjamClient.SetUserChannelState(userIndex, channel, false, false, false, 0, false, 0, false, false, false, false, true, stagingPair * 2);
    }

    {
        const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
        remoteLinkAudioOutputPairs[shortName] = stagingPair;
    }

    lastLinkAudioEndpointRefreshMs = 0.0;
    rebuildLinkAudioEndpoints();
    return true;
}

void NinjamVst3AudioProcessor::setUserLevel(int userIndex, float volume, float pan, bool isMuted, bool isSolo)
{
    const int numUsers = ninjamClient.GetNumUsers();
    if (userIndex < 0 || userIndex >= numUsers)
        return;

    if (userIndex >= 0 && userIndex < maxRemoteChordUsers)
    {
        remoteOpusUserVolume[(size_t)userIndex].store(volume, std::memory_order_relaxed);
        remoteOpusUserPan[(size_t)userIndex].store(pan, std::memory_order_relaxed);
        remoteOpusUserMute[(size_t)userIndex].store(isMuted, std::memory_order_relaxed);
        remoteOpusUserSolo[(size_t)userIndex].store(isSolo, std::memory_order_relaxed);
    }

    userBaseVolume[userIndex] = volume;
    if (userIndex >= 0 && userIndex < numUsers)
    {
        const char* name = ninjamClient.GetUserState(userIndex, nullptr, nullptr, nullptr);
        if (name)
        {
            juce::String fullName = juce::String::fromUTF8(name);
            int atPos = fullName.indexOfChar('@');
            juce::String shortName;
            if (atPos > 0)
                shortName = fullName.substring(0, atPos);
            else
                shortName = fullName;
            userVolumeByName[shortName] = volume;
        }
    }
    userPanOverrides[userIndex] = pan;
    for (int i = 0; i < 32; ++i)
    {
        if (!isNinjamRemoteChannelVideoOnly(userIndex, i))
            ninjamClient.SetUserChannelState(userIndex, i, false, false, true, volume, true, pan, true, isMuted, true, isSolo);
    }
}

void NinjamVst3AudioProcessor::setUserVolume(int userIndex, float volume)
{
    const int numUsers = ninjamClient.GetNumUsers();
    if (userIndex < 0 || userIndex >= numUsers)
        return;

    if (userIndex >= 0 && userIndex < maxRemoteChordUsers)
        remoteOpusUserVolume[(size_t)userIndex].store(volume, std::memory_order_relaxed);

    for (int i = 0; i < 32; ++i)
    {
        if (!isNinjamRemoteChannelVideoOnly(userIndex, i))
            ninjamClient.SetUserChannelState(userIndex, i, false, false, true, volume, false, 0, false, false, false, false, false, 0);
    }
}

float NinjamVst3AudioProcessor::getUserPeak(int userIndex, int channelIndex)
{
    const int numUsers = ninjamClient.GetNumUsers();
    if (userIndex < 0 || userIndex >= numUsers)
        return 0.0f;

    if (isTransportSyncEnabled() && (!hostWasPlaying.load() || syncWaitForInterval.load()))
        return 0.0f;

    if (userIndex >= 0 && userIndex < maxRemoteChordUsers
        && remoteOpusPeerActive[(size_t)userIndex].load(std::memory_order_relaxed))
    {
        return channelIndex == 0
            ? remoteOpusCombinedPeakL[(size_t)userIndex].load(std::memory_order_relaxed)
            : remoteOpusCombinedPeakR[(size_t)userIndex].load(std::memory_order_relaxed);
    }

    float maxPeak = 0.0f;
    for (int i = 0; i < 32; ++i)
    {
        if (isNinjamRemoteChannelVideoOnly(userIndex, i))
            continue;
        float p = ninjamClient.GetUserChannelPeak(userIndex, i, channelIndex);
        if (p > maxPeak) maxPeak = p;
    }
    return maxPeak;
}

float NinjamVst3AudioProcessor::getUserSourcePeak(int userIndex, int channelIndex)
{
    const int numUsers = ninjamClient.GetNumUsers();
    if (userIndex < 0 || userIndex >= numUsers)
        return 0.0f;

    if (isTransportSyncEnabled() && (!hostWasPlaying.load() || syncWaitForInterval.load()))
        return 0.0f;

    if (userIndex >= 0 && userIndex < maxRemoteChordUsers
        && remoteOpusPeerActive[(size_t)userIndex].load(std::memory_order_relaxed))
    {
        return channelIndex == 0
            ? remoteOpusSourcePeakL[(size_t)userIndex].load(std::memory_order_relaxed)
            : remoteOpusSourcePeakR[(size_t)userIndex].load(std::memory_order_relaxed);
    }

    // For non-Opus (legacy NINJAM) users, the NJClient channel peak is already pre-volume.
    float maxPeak = 0.0f;
    for (int i = 0; i < 32; ++i)
    {
        if (isNinjamRemoteChannelVideoOnly(userIndex, i))
            continue;
        float p = ninjamClient.GetUserChannelPeak(userIndex, i, channelIndex);
        if (p > maxPeak) maxPeak = p;
    }
    return maxPeak;
}

float NinjamVst3AudioProcessor::getUserChannelPeak(int userIndex, int njChanIdx, int lrSide)
{
    const int numUsers = ninjamClient.GetNumUsers();
    if (userIndex < 0 || userIndex >= numUsers || njChanIdx < 0)
        return 0.0f;

    if (userIndex >= 0 && userIndex < maxRemoteChordUsers
        && remoteOpusPeerActive[(size_t)userIndex].load(std::memory_order_relaxed))
    {
        const int virtualIndex = njChanIdx;
        if (virtualIndex >= 0 && virtualIndex < maxLocalChannels)
            return remoteOpusChannelPeaks[(size_t)userIndex][(size_t)virtualIndex].load(std::memory_order_relaxed);
        return 0.0f;
    }

    if (isNinjamRemoteChannelVideoOnly(userIndex, njChanIdx))
        return 0.0f;

    return ninjamClient.GetUserChannelPeak(userIndex, njChanIdx, lrSide);
}

void NinjamVst3AudioProcessor::setUserNjChannelVolume(int userIndex, int njChanIdx, float volume)
{
    const int numUsers = ninjamClient.GetNumUsers();
    if (userIndex < 0 || userIndex >= numUsers || njChanIdx < 0)
        return;

    if (userIndex >= 0 && userIndex < maxRemoteChordUsers
        && remoteOpusPeerActive[(size_t)userIndex].load(std::memory_order_relaxed))
    {
        const int virtualIndex = njChanIdx;
        if (virtualIndex >= 0 && virtualIndex < maxLocalChannels)
            remoteOpusChannelGains[(size_t)userIndex][(size_t)virtualIndex].store(volume, std::memory_order_relaxed);
        return;
    }

    if (isNinjamRemoteChannelVideoOnly(userIndex, njChanIdx))
        return;

    ninjamClient.SetUserChannelState(userIndex, njChanIdx, false, false, true, volume, false, 0, false, false, false, false);
}

void NinjamVst3AudioProcessor::setMasterOutputGain(float gain)
{
    masterOutputGain.store(gain);
}

float NinjamVst3AudioProcessor::getMasterOutputGain() const
{
    return masterOutputGain.load();
}

float NinjamVst3AudioProcessor::getMasterPeak() const
{
    return masterPeak.load();
}

float NinjamVst3AudioProcessor::getMasterPeakLeft() const
{
    return masterPeakL.load();
}

float NinjamVst3AudioProcessor::getMasterPeakRight() const
{
    return masterPeakR.load();
}

float NinjamVst3AudioProcessor::getMasterLufsAvg() const
{
    return masterLufsAvg.load(std::memory_order_relaxed);
}

float NinjamVst3AudioProcessor::getMasterLufsPeak() const
{
    return masterLufsPeak.load(std::memory_order_relaxed);
}

float NinjamVst3AudioProcessor::getUserLufs(int userIndex) const
{
    if (userIndex < 0 || userIndex >= maxRemoteChordUsers) return -70.0f;
    return userLufsAvg[userIndex].load(std::memory_order_relaxed);
}

void LufsMeter::prepare(double sampleRate)
{
    sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    alpha = 1.0 - std::exp(-1.0 / (0.3 * sr));

    preFilter.b0 = 1.53512485958697;
    preFilter.b1 = -2.69169618940638;
    preFilter.b2 = 1.19839281085285;
    preFilter.a1 = -1.69065929318241;
    preFilter.a2 = 0.73248077421585;
    preFilter.reset();

    rlpFilter.b0 = 1.0;
    rlpFilter.b1 = -2.0;
    rlpFilter.b2 = 1.0;
    rlpFilter.a1 = -1.99004745483398;
    rlpFilter.a2 = 0.99007225036621;
    rlpFilter.reset();

    meanSquare = 0.0;
    samplesProcessed = 0;
}

void LufsMeter::reset()
{
    preFilter.reset();
    rlpFilter.reset();
    meanSquare = 0.0;
    samplesProcessed = 0;
}

void LufsMeter::processSample(float sample)
{
    double x = preFilter.process(static_cast<double>(sample));
    x = rlpFilter.process(x);
    meanSquare += (x * x - meanSquare) * alpha;
    ++samplesProcessed;
}

void LufsMeter::processBlock(const float* data, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
        processSample(data[i]);
}

float LufsMeter::getCurrentLufs() const
{
    if (samplesProcessed < 1000 || meanSquare < 1.0e-20)
        return -70.0f;
    return static_cast<float>(-0.691 + 10.0 * std::log10(meanSquare + 1.0e-20));
}

double LufsMeter::Biquad::process(double input)
{
    double y = b0 * input + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = input;
    y2 = y1; y1 = y;
    return y;
}

void LufsMeter::Biquad::reset()
{
    x1 = x2 = y1 = y2 = 0.0;
}

juce::String NinjamVst3AudioProcessor::getVersionString() const
{
    return NINJAM_DISPLAY_VERSION;
}

void NinjamVst3AudioProcessor::setSoftLimiterEnabled(bool shouldEnable)
{
    softLimiterEnabled.store(shouldEnable);
}

bool NinjamVst3AudioProcessor::startSessionRecording(const juce::File& outputFile)
{
    if (sessionRecorder.isRecording())
        return false;

    double sr = getSampleRate();
    if (sr <= 0.0)
        sr = 48000.0;

    int numLocal = juce::jlimit(1, maxLocalChannels, numLocalChannels.load());

    // Snapshot currently connected remote users and their channel counts
    std::vector<int> userIds;
    std::vector<int> channelCounts;
    auto users = getConnectedUsers();
    for (const auto& u : users)
    {
        if (u.index < 0 || u.index >= SessionRecorder::maxRemoteUsers)
            continue;
        int nch = 2; // default stereo for non-multichannel peers
        if (isRemoteOpusMultichannelPeer(u.index))
        {
            int vcount = remoteOpusVirtualChannelCount[(size_t)u.index].load(std::memory_order_relaxed);
            int packed = remoteOpusPackedChannelCount[(size_t)u.index].load(std::memory_order_relaxed);
            nch = juce::jmax(1, packed > 0 ? packed : vcount);
        }
        userIds.push_back(u.index);
        channelCounts.push_back(nch);
    }

    return sessionRecorder.startRecording(outputFile, sr, numLocal, userIds, channelCounts);
}

bool NinjamVst3AudioProcessor::stopSessionRecording()
{
    return sessionRecorder.stopRecording();
}

bool NinjamVst3AudioProcessor::isSessionRecording() const
{
    return sessionRecorder.isRecording() || sessionRecorder.isStarting();
}

bool NinjamVst3AudioProcessor::isSessionRecordingFinishing() const
{
    return sessionRecorder.isFinishing();
}

juce::String NinjamVst3AudioProcessor::getSessionRecordingStatus() const
{
    return sessionRecorder.getStatusMessage();
}

juce::File NinjamVst3AudioProcessor::getSessionRecordingFile() const
{
    return sessionRecorder.getOutputFile();
}

bool NinjamVst3AudioProcessor::isSoftLimiterEnabled() const
{
    return softLimiterEnabled.load();
}

void NinjamVst3AudioProcessor::setUserClipEnabled(int userIndex, bool enabled)
{
    userClipEnabled[userIndex] = enabled;
}

bool NinjamVst3AudioProcessor::isUserClipEnabled(int userIndex) const
{
    auto it = userClipEnabled.find(userIndex);
    if (it != userClipEnabled.end())
        return it->second;

    return softLimiterEnabled.load();
}

void NinjamVst3AudioProcessor::setMasterLimiterEnabled(bool shouldEnable)
{
    dspLimiterEnabled.store(shouldEnable);
}

bool NinjamVst3AudioProcessor::isMasterLimiterEnabled() const
{
    return dspLimiterEnabled.load();
}

void NinjamVst3AudioProcessor::setLimiterThreshold(float db)
{
    limiterThresholdDb.store(db);
    masterLimiter.setThreshold(db);
}

void NinjamVst3AudioProcessor::setLimiterRelease(float ms)
{
    limiterReleaseMs.store(ms);
    masterLimiter.setRelease(ms);
}

void NinjamVst3AudioProcessor::setLocalInputGain(float gain)
{
    localInputGain.store(gain);
    setLocalChannelGain(0, gain);
}

float NinjamVst3AudioProcessor::getLocalInputGain() const
{
    return localChannelGains[0].load();
}

void NinjamVst3AudioProcessor::setVoiceChannelGain(float gain)
{
    voiceChannelGain.store(gain);
}

float NinjamVst3AudioProcessor::getVoiceChannelGain() const
{
    return voiceChannelGain.load();
}

void NinjamVst3AudioProcessor::setVoiceChannelInput(int inputIndex)
{
    voiceChannelInput.store(inputIndex);
}

int NinjamVst3AudioProcessor::getVoiceChannelInput() const
{
    return voiceChannelInput.load();
}

float NinjamVst3AudioProcessor::getVoiceChannelPeak() const
{
    return voiceChannelPeak.load();
}

float NinjamVst3AudioProcessor::getVoiceChannelPeakLeft() const
{
    return voiceChannelPeakL.load();
}

float NinjamVst3AudioProcessor::getVoiceChannelPeakRight() const
{
    return voiceChannelPeakR.load();
}

int NinjamVst3AudioProcessor::getServerMaxLocalChannels() const
{
    return juce::jmax(1, serverMaxLocalChannelsCached.load(std::memory_order_relaxed));
}

bool NinjamVst3AudioProcessor::canUseDedicatedVoiceChatChannel() const
{
    const int serverMaxLocalChannels = getServerMaxLocalChannels();
    const int voiceChannelIndex = getVoiceChatNinjamChannelIndex();
    return voiceChannelIndex >= 0 && voiceChannelIndex < serverMaxLocalChannels;
}

void NinjamVst3AudioProcessor::setNumLocalChannels(int num)
{
    const int previous = numLocalChannels.load();
    int clamped = juce::jlimit(1, maxLocalChannels, num);

    {
        juce::ScopedLock lock(localChannelNamesLock);
        for (int i = 0; i < maxLocalChannels; ++i)
        {
            auto& name = localChannelNames[(size_t)i];
            if (name.isEmpty() || isDefaultLocalChannelName(name))
                name = buildDefaultLocalChannelName(i);
        }
    }

    numLocalChannels.store(clamped);
    effectiveLocalChannelCount.store(clamped);
    applyCodecPreference();
    syncLocalIntervalChannelConfig();

    // Post a local status message when transitioning into or out of multichannel
    if (previous != clamped)
    {
        juce::String msg;
        if (clamped > 1 && previous <= 1)
            msg = "MultiChannel mode enabled (" + juce::String(clamped) + " channels). Waiting for peer detection.";
        else if (clamped > 1)
            msg = "Local channels: " + juce::String(clamped) + ".";
        else
            msg = "MultiChannel mode disabled (single channel).";
        juce::ScopedLock lock(chatLock);
        chatHistory.add(msg);
        chatSenders.add("");
        chatRevision.fetch_add(1);
        if (chatHistory.size() > 100)
        {
            chatHistory.removeRange(0, chatHistory.size() - 100);
            chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
        }
    }

    // Immediately tell peers about the change so they update their expand buttons
    if (ninjamClient.GetStatus() == NJClient::NJC_STATUS_OK)
        broadcastOpusSyncSupport();
}

int NinjamVst3AudioProcessor::getNumLocalChannels() const
{
    return numLocalChannels.load();
}

int NinjamVst3AudioProcessor::getEffectiveLocalChannelCount() const
{
    const int configured = juce::jlimit(1, maxLocalChannels, numLocalChannels.load());
    const int cached = effectiveLocalChannelCount.load();
    if (cached > 0)
        return juce::jlimit(1, configured, cached);
    return configured;
}

int NinjamVst3AudioProcessor::getConfiguredLocalOpusWidth(int channel) const
{
    if (channel < 0 || channel >= maxLocalChannels)
        return 1;

    const int srcIndex = localChannelInputs[(size_t)channel].load(std::memory_order_relaxed);
    return srcIndex < 0 ? 2 : 1;
}

int NinjamVst3AudioProcessor::getConfiguredLocalOpusPackedChannelCount(int numVirtualChannels) const
{
    const int count = juce::jlimit(0, maxLocalChannels, numVirtualChannels);
    int packed = 0;
    for (int i = 0; i < count; ++i)
        packed += getConfiguredLocalOpusWidth(i);
    return juce::jmax(1, packed);
}

void NinjamVst3AudioProcessor::setLocalChannelName(int channel, const juce::String& name)
{
    if (channel < 0 || channel >= maxLocalChannels) return;
    { juce::ScopedLock lock(localChannelNamesLock); localChannelNames[(size_t)channel] = name; }
    syncLocalIntervalChannelConfig();
}

juce::String NinjamVst3AudioProcessor::getLocalChannelName(int channel) const
{
    if (channel < 0 || channel >= maxLocalChannels) return {};
    juce::ScopedLock lock(localChannelNamesLock);
    return localChannelNames[(size_t)channel];
}

void NinjamVst3AudioProcessor::setLocalChannelGain(int channel, float gain)
{
    if (channel < 0 || channel >= maxLocalChannels)
        return;
    localChannelGains[(size_t)channel].store(gain);
}

float NinjamVst3AudioProcessor::getLocalChannelGain(int channel) const
{
    if (channel < 0 || channel >= maxLocalChannels)
        return 1.0f;
    return localChannelGains[(size_t)channel].load();
}

void NinjamVst3AudioProcessor::setAutoTuneEnabled(bool enabled)
{
    autoTuneEnabled.store(enabled);
    if (autoTuneProcessor)
        autoTuneProcessor->setEnabled(enabled);
}

void NinjamVst3AudioProcessor::setAutoTuneQuality(int quality)
{
    autoTuneQuality.store(juce::jlimit(0, 1, quality));
    if (autoTuneProcessor)
        autoTuneProcessor->setQuality(quality == 1
            ? ninjamplus::PitchDetector::Quality::High
            : ninjamplus::PitchDetector::Quality::Low);
}

void NinjamVst3AudioProcessor::setAutoTuneScale(int scale)
{
    autoTuneScale.store(juce::jlimit(0, (int)ninjamplus::ScaleQuantizer::Scale::Count - 1, scale));
    if (autoTuneProcessor)
        autoTuneProcessor->setScale((ninjamplus::ScaleQuantizer::Scale)autoTuneScale.load());
}

void NinjamVst3AudioProcessor::setAutoTuneKey(int key)
{
    autoTuneKey.store(juce::jlimit(0, 11, key));
    if (autoTuneProcessor)
        autoTuneProcessor->setKey(autoTuneKey.load());
}

void NinjamVst3AudioProcessor::setAutoTuneSpeed(float speed)
{
    autoTuneSpeed.store(juce::jlimit(0.0f, 1.0f, speed));
    if (autoTuneProcessor)
        autoTuneProcessor->setCorrectionSpeed(autoTuneSpeed.load());
}

void NinjamVst3AudioProcessor::setLocalChannelInput(int channel, int inputIndex)
{
    if (channel < 0 || channel >= maxLocalChannels)
        return;
    localChannelInputs[(size_t)channel].store(inputIndex);
}

int NinjamVst3AudioProcessor::getLocalChannelInput(int channel) const
{
    if (channel < 0 || channel >= maxLocalChannels)
        return 0;
    return localChannelInputs[(size_t)channel].load();
}

void NinjamVst3AudioProcessor::setLocalChannelUsesLinkAudioInput(int channel, bool shouldUse)
{
    if (channel < 0 || channel >= maxLocalChannels)
        return;

    if (shouldUse)
        localChannelInputs[(size_t)channel].store(kLocalInputLinkAudioSentinel);
    else if (localChannelInputs[(size_t)channel].load() == kLocalInputLinkAudioSentinel)
        localChannelInputs[(size_t)channel].store(0);
}

bool NinjamVst3AudioProcessor::isLocalChannelUsingLinkAudioInput(int channel) const
{
    if (channel < 0 || channel >= maxLocalChannels)
        return false;
    return localChannelInputs[(size_t)channel].load() == kLocalInputLinkAudioSentinel;
}

float NinjamVst3AudioProcessor::getLocalChannelPeak(int channel) const
{
    if (channel < 0 || channel >= maxLocalChannels)
        return 0.0f;
    return localChannelPeaks[(size_t)channel].load();
}

float NinjamVst3AudioProcessor::getLocalChannelPeakLeft(int channel) const
{
    if (channel < 0 || channel >= maxLocalChannels)
        return 0.0f;
    return localChannelPeaksL[(size_t)channel].load();
}

float NinjamVst3AudioProcessor::getLocalChannelPeakRight(int channel) const
{
    if (channel < 0 || channel >= maxLocalChannels)
        return 0.0f;
    return localChannelPeaksR[(size_t)channel].load();
}

juce::String NinjamVst3AudioProcessor::getLocalChordLabel() const
{
    if (!isChordDetectionEnabled())
        return "Off";

    return chordAnalyzer ? chordAnalyzer->getLabel(BatchedChordAnalyzer::localTrackIndex) : "--";
}

double NinjamVst3AudioProcessor::getLocalChordCpuPercent() const
{
    if (!isChordDetectionEnabled())
        return 0.0;

    return chordAnalyzer ? chordAnalyzer->getCpuPercent(BatchedChordAnalyzer::localTrackIndex) : 0.0;
}

int NinjamVst3AudioProcessor::getLocalChordMemoryKb() const
{
    return chordAnalyzer ? chordAnalyzer->getMemoryKb(BatchedChordAnalyzer::localTrackIndex) : 0;
}

juce::String NinjamVst3AudioProcessor::getMasterChordLabel() const
{
    if (!isChordDetectionEnabled())
        return "Off";

    return chordAnalyzer ? chordAnalyzer->getLabel(BatchedChordAnalyzer::masterTrackIndex) : "--";
}

double NinjamVst3AudioProcessor::getMasterChordCpuPercent() const
{
    if (!isChordDetectionEnabled())
        return 0.0;

    return chordAnalyzer ? chordAnalyzer->getCpuPercent(BatchedChordAnalyzer::masterTrackIndex) : 0.0;
}

int NinjamVst3AudioProcessor::getMasterChordMemoryKb() const
{
    return chordAnalyzer ? chordAnalyzer->getMemoryKb(BatchedChordAnalyzer::masterTrackIndex) : 0;
}

std::vector<juce::String> NinjamVst3AudioProcessor::getMasterChordTimeline() const
{
    const juce::ScopedLock lock(masterChordTimelineLock);
    return masterChordTimeline;
}

juce::String NinjamVst3AudioProcessor::getUserChordLabel(int userIndex) const
{
    if (userIndex < 0 || userIndex >= maxRemoteChordUsers)
        return "--";

    if (!isChordDetectionEnabled() || !isUserChordDetectionEnabled(userIndex))
        return "Off";

    return chordAnalyzer ? chordAnalyzer->getLabel(BatchedChordAnalyzer::remoteTrackIndexForUser(userIndex)) : "--";
}

double NinjamVst3AudioProcessor::getUserChordCpuPercent(int userIndex) const
{
    if (userIndex < 0 || userIndex >= maxRemoteChordUsers)
        return 0.0;

    if (!isChordDetectionEnabled() || !isUserChordDetectionEnabled(userIndex))
        return 0.0;

    return chordAnalyzer ? chordAnalyzer->getCpuPercent(BatchedChordAnalyzer::remoteTrackIndexForUser(userIndex)) : 0.0;
}

void NinjamVst3AudioProcessor::setChordDetectionEnabled(bool enabled)
{
    chordDetectionEnabled.store(enabled, std::memory_order_relaxed);

    if (!enabled && chordAnalyzer)
    {
        chordAnalyzer->markAllNoInput();
        chordAnalyzer->resetAllTracks();
    }

    if (!enabled)
    {
        const juce::ScopedLock lock(masterChordTimelineLock);
        masterChordTimeline.clear();
        masterChordTimelineInterval = -1;
        masterChordTimelineBpi = 0;
    }
}

bool NinjamVst3AudioProcessor::isChordDetectionEnabled() const
{
    return chordDetectionEnabled.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setUserChordDetectionEnabled(int userIndex, bool enabled)
{
    if (userIndex < 0 || userIndex >= maxRemoteChordUsers)
        return;

    remoteChordDetectionEnabled[(size_t)userIndex].store(enabled, std::memory_order_relaxed);

    if (chordAnalyzer && chordAnalyzer->isPrepared())
        chordAnalyzer->resetTrack(BatchedChordAnalyzer::remoteTrackIndexForUser(userIndex));
}

bool NinjamVst3AudioProcessor::isUserChordDetectionEnabled(int userIndex) const
{
    if (userIndex < 0 || userIndex >= maxRemoteChordUsers)
        return false;

    return remoteChordDetectionEnabled[(size_t)userIndex].load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::updateMasterChordTimeline()
{
    const int bpi = juce::jlimit(1, 256, cachedNinjamBpi.load(std::memory_order_relaxed));
    const int interval = getDisplayIntervalIndex();
    const int transportLength = cachedNinjamTransportLen.load(std::memory_order_relaxed);
    const int transportPosition = cachedNinjamTransportPos.load(std::memory_order_relaxed);
    int beatIndex = 0;

    if (transportLength > 0)
    {
        const double progress = juce::jlimit(0.0, 0.999999, (double)juce::jlimit(0, transportLength - 1, transportPosition) / (double)transportLength);
        beatIndex = juce::jlimit(0, bpi - 1, (int)std::floor(progress * (double)bpi));
    }
    else
    {
        const double progress = juce::jlimit(0.0, 0.999999, (double)getIntervalProgress());
        beatIndex = juce::jlimit(0, bpi - 1, (int)std::floor(progress * (double)bpi));
    }

    const juce::String label = getMasterChordLabel();
    const bool hasChord = isChordDetectionEnabled()
                       && chordAnalyzer != nullptr
                       && chordAnalyzer->hasStrongMasterTimelineConfidence()
                       && label.isNotEmpty()
                       && label != "--"
                       && label != "Off"
                       && label != "Noise";

    const juce::ScopedLock lock(masterChordTimelineLock);
    if (masterChordTimelineInterval != interval
        || masterChordTimelineBpi != bpi
        || (int)masterChordTimeline.size() != bpi)
    {
        masterChordTimeline.assign((size_t)bpi, "--");
        masterChordTimelineInterval = interval;
        masterChordTimelineBpi = bpi;
    }

    if (hasChord)
    {
        if (label != lastMasterTimelineChordLabel)
        {
            masterChordTimeline[(size_t)beatIndex] = label;
            lastMasterTimelineChordLabel = label;
        }
    }
    else
    {
        lastMasterTimelineChordLabel.clear();
    }
}
int NinjamVst3AudioProcessor::getUserChordMemoryKb(int userIndex) const
{
    if (userIndex < 0 || userIndex >= maxRemoteChordUsers)
        return 0;

    return chordAnalyzer ? chordAnalyzer->getMemoryKb(BatchedChordAnalyzer::remoteTrackIndexForUser(userIndex)) : 0;
}
void NinjamVst3AudioProcessor::clearRemoteAudioTapBuffers()
{
    const juce::SpinLock::ScopedLockType lock(remoteAudioTapLock);
    for (int user = 0; user < maxRemoteChordUsers; ++user)
    {
        remoteAudioTapBuffers[(size_t)user].setSize(0, 0);
        remoteAudioTapWritePositions[(size_t)user] = 0;
        remoteAudioTapAvailableSamples[(size_t)user] = 0;
    }
}

bool NinjamVst3AudioProcessor::copyRemoteUserAudioForLooper(int userIndex, int numSamples)
{
    if (!isSamplePadsFeatureEnabled() || userIndex < 0 || userIndex >= maxRemoteChordUsers || numSamples <= 0)
        return false;

    if (samplePadRemoteLooperInputBuffer.getNumChannels() < 2
        || samplePadRemoteLooperInputBuffer.getNumSamples() < numSamples)
    {
        samplePadRemoteLooperInputBuffer.setSize(2, numSamples, false, true, true);
    }
    samplePadRemoteLooperInputBuffer.clear();

    const juce::SpinLock::ScopedLockType lock(remoteAudioTapLock);
    auto& source = remoteAudioTapBuffers[(size_t)userIndex];
    const int capacity = source.getNumSamples();
    const int available = juce::jmin(numSamples, remoteAudioTapAvailableSamples[(size_t)userIndex]);
    if (source.getNumChannels() < 2 || capacity <= 0 || available <= 0)
        return false;

    int sourcePosition = remoteAudioTapWritePositions[(size_t)userIndex] - available;
    while (sourcePosition < 0)
        sourcePosition += capacity;

    int targetPosition = numSamples - available;
    int remaining = available;
    while (remaining > 0)
    {
        const int chunk = juce::jmin(remaining, capacity - sourcePosition);
        samplePadRemoteLooperInputBuffer.copyFrom(0, targetPosition, source, 0, sourcePosition, chunk);
        samplePadRemoteLooperInputBuffer.copyFrom(1, targetPosition, source, 1, sourcePosition, chunk);
        targetPosition += chunk;
        sourcePosition = (sourcePosition + chunk) % capacity;
        remaining -= chunk;
    }

    return true;
}

void NinjamVst3AudioProcessor::RemoteChannelAudioTap_Callback(void* userData,
                                                              int useridx,
                                                              const char*,
                                                              int channelidx,
                                                              const float* interleaved,
                                                              int numChannels,
                                                              int numFrames,
                                                              int sampleRate)
{
    auto* self = static_cast<NinjamVst3AudioProcessor*>(userData);
    if (self == nullptr || interleaved == nullptr || numFrames <= 0)
        return;

    if (useridx < 0 || useridx >= maxRemoteChordUsers)
        return;

    // Channel 0 is the normal per-user mixdown, including the VST3 multichannel
    // mix slot. Tapping only this channel avoids double-feeding expanded peers.
    if (channelidx != 0)
        return;

    if (self->isRemoteOpusMultichannelPeer(useridx))
        return;

    if (self->isSamplePadsFeatureEnabled())
    {
        const juce::SpinLock::ScopedLockType lock(self->remoteAudioTapLock);
        auto& buffer = self->remoteAudioTapBuffers[(size_t)useridx];
        if (buffer.getNumChannels() < 2 || buffer.getNumSamples() != remoteAudioTapBufferSamples)
        {
            buffer.setSize(2, remoteAudioTapBufferSamples, false, true, true);
            buffer.clear();
            self->remoteAudioTapWritePositions[(size_t)useridx] = 0;
            self->remoteAudioTapAvailableSamples[(size_t)useridx] = 0;
        }

        const int framesToCopy = juce::jmin(numFrames, remoteAudioTapBufferSamples);
        const int sourceStart = numFrames - framesToCopy;
        int writePosition = self->remoteAudioTapWritePositions[(size_t)useridx];
        for (int frame = 0; frame < framesToCopy; ++frame)
        {
            const int sourceFrame = sourceStart + frame;
            const int sourceOffset = sourceFrame * numChannels;
            const float left = interleaved[sourceOffset];
            const float right = numChannels > 1 ? interleaved[sourceOffset + 1] : left;
            buffer.setSample(0, writePosition, left);
            buffer.setSample(1, writePosition, right);
            if (++writePosition >= remoteAudioTapBufferSamples)
                writePosition = 0;
        }

        self->remoteAudioTapWritePositions[(size_t)useridx] = writePosition;
        self->remoteAudioTapAvailableSamples[(size_t)useridx] =
            juce::jmin(remoteAudioTapBufferSamples,
                       self->remoteAudioTapAvailableSamples[(size_t)useridx] + framesToCopy);
    }

    // Session recorder: tap remote user stereo (non-Opus-multichannel)
    if (self->sessionRecorder.isRecording())
    {
        // Pass interleaved data directly — writeRemoteUserInterleaved handles it
        self->sessionRecorder.writeRemoteUserInterleaved(useridx, interleaved, numChannels, numFrames);
    }

    // Always update the LUFS meter regardless of chord analyzer state
    {
        auto& meter = self->userLufsMeters[(size_t)useridx];
        for (int frame = 0; frame < numFrames; ++frame)
        {
            const int offset = frame * numChannels;
            float mono = interleaved[offset];
            if (numChannels > 1)
                mono = 0.5f * (mono + interleaved[offset + 1]);
            meter.processSample(mono);
        }
        self->userLufsAvg[(size_t)useridx].store(meter.getCurrentLufs(), std::memory_order_relaxed);
    }

    if (self->chordAnalyzer == nullptr)
        return;

    const int trackIndex = BatchedChordAnalyzer::remoteTrackIndexForUser(useridx);
    if (!self->isChordDetectionEnabled() || !self->isUserChordDetectionEnabled(useridx))
    {
        self->chordAnalyzer->markNoInput(trackIndex);
        return;
    }

    self->chordAnalyzer->processInterleavedBlock(trackIndex, interleaved, numFrames, numChannels, sampleRate);
}

int NinjamVst3AudioProcessor::RemoteMultichannelTap_Callback(void* userData,
                                                             int useridx,
                                                             const char* username,
                                                             int channelidx,
                                                             const float* interleaved,
                                                             int numChannels,
                                                             int numFrames,
                                                             int sampleRate)
{
    auto* self = static_cast<NinjamVst3AudioProcessor*>(userData);
    if (self == nullptr || useridx < 0 || useridx >= maxRemoteChordUsers)
        return 0;

    const bool isOpusPeer = self->isRemoteOpusMultichannelPeer(useridx);
    const int carrierChannel = self->remoteOpusCarrierChannel[(size_t)useridx].load(std::memory_order_relaxed);
    if (!isOpusPeer || channelidx != carrierChannel)
        return 0;

    njplus_debug_log("TAP user=%d chan=%d carrier=%d frames=%d nch=%d data=%p mixOut=%p mixChans=%d",
                     useridx, channelidx, carrierChannel, numFrames, numChannels,
                     (void*)interleaved, (void*)self->remoteOpusMixOutputs, self->remoteOpusMixOutputChannels);

    if (interleaved == nullptr || numFrames <= 0 || numChannels <= 0)
        return 1;

    // Session recorder: tap remote Opus multichannel user (pre-user-volume, decoded)
    if (self->sessionRecorder.isRecording())
    {
        int packedCount = self->remoteOpusPackedChannelCount[(size_t)useridx].load(std::memory_order_relaxed);
        if (packedCount <= 0)
            packedCount = numChannels;
        self->sessionRecorder.writeRemoteUserMultichannel(useridx, interleaved, packedCount, numFrames);
    }

    int virtualCount = self->remoteOpusVirtualChannelCount[(size_t)useridx].load(std::memory_order_relaxed);
    virtualCount = juce::jlimit(1, maxLocalChannels, virtualCount);
    int packedIndex = 0;
    float combinedL = 0.0f;
    float combinedR = 0.0f;
    float sourceCombinedL = 0.0f;
    float sourceCombinedR = 0.0f;

    const float userVolume = self->remoteOpusUserVolume[(size_t)useridx].load(std::memory_order_relaxed);
    const float userPan = juce::jlimit(-1.0f, 1.0f, self->remoteOpusUserPan[(size_t)useridx].load(std::memory_order_relaxed));
    const bool userMuted = self->remoteOpusUserMute[(size_t)useridx].load(std::memory_order_relaxed);
    const bool userSolo = self->remoteOpusUserSolo[(size_t)useridx].load(std::memory_order_relaxed);
    const bool audible = !userMuted && (!self->remoteOpusSoloActiveThisBlock || userSolo);
    float userGainL = userVolume;
    float userGainR = userVolume;
    if (userPan > 0.0f) userGainL *= (1.0f - userPan);
    else if (userPan < 0.0f) userGainR *= (1.0f + userPan);

    for (int ch = 0; ch < virtualCount; ++ch)
    {
        const int width = juce::jlimit(1, 2, self->remoteOpusChannelWidths[(size_t)useridx][(size_t)ch].load(std::memory_order_relaxed));
        const float perGain = self->remoteOpusChannelGains[(size_t)useridx][(size_t)ch].load(std::memory_order_relaxed);
        float peak = 0.0f;

        for (int frame = 0; frame < numFrames; ++frame)
        {
            const int base = frame * numChannels + packedIndex;
            const float left = interleaved[base];
            const float right = width > 1 && (packedIndex + 1) < numChannels ? interleaved[base + 1] : left;
            const float monoPeak = juce::jmax(std::abs(left), std::abs(right));
            if (monoPeak > peak) peak = monoPeak;
            combinedL = juce::jmax(combinedL, std::abs(left * perGain * userGainL));
            combinedR = juce::jmax(combinedR, std::abs(right * perGain * userGainR));
            sourceCombinedL = juce::jmax(sourceCombinedL, std::abs(left));
            sourceCombinedR = juce::jmax(sourceCombinedR, std::abs(right));

            if (audible && self->remoteOpusMixOutputs != nullptr && self->remoteOpusMixOutputChannels > 0)
            {
                int outIndex = self->remoteOpusUserOutput[(size_t)useridx].load(std::memory_order_relaxed);
                const bool monoOut = (outIndex & 1024) != 0;
                int dest = outIndex & 1023;
                if (dest < 0) dest = 0;
                if (monoOut || self->remoteOpusMixOutputChannels < 2)
                {
                    if (dest < self->remoteOpusMixOutputChannels && self->remoteOpusMixOutputs[dest] != nullptr)
                        self->remoteOpusMixOutputs[dest][frame] += 0.5f * (left + right) * perGain * userVolume;
                }
                else
                {
                    const int leftOut = juce::jlimit(0, juce::jmax(0, self->remoteOpusMixOutputChannels - 1), dest);
                    const int rightOut = juce::jlimit(0, juce::jmax(0, self->remoteOpusMixOutputChannels - 1), dest + 1);
                    if (self->remoteOpusMixOutputs[leftOut] != nullptr)
                        self->remoteOpusMixOutputs[leftOut][frame] += left * perGain * userGainL;
                    if (self->remoteOpusMixOutputs[rightOut] != nullptr)
                        self->remoteOpusMixOutputs[rightOut][frame] += right * perGain * userGainR;
                }
            }
        }

        self->remoteOpusChannelPeaks[(size_t)useridx][(size_t)ch].store(peak, std::memory_order_relaxed);
        packedIndex += width;
        if (packedIndex >= numChannels)
            break;
    }

    self->remoteOpusCombinedPeakL[(size_t)useridx].store(combinedL, std::memory_order_relaxed);
    self->remoteOpusCombinedPeakR[(size_t)useridx].store(combinedR, std::memory_order_relaxed);
    self->remoteOpusSourcePeakL[(size_t)useridx].store(sourceCombinedL, std::memory_order_relaxed);
    self->remoteOpusSourcePeakR[(size_t)useridx].store(sourceCombinedR, std::memory_order_relaxed);

    {
        auto& meter = self->userLufsMeters[(size_t)useridx];
        for (int frame = 0; frame < numFrames; ++frame)
        {
            const int base = frame * numChannels;
            float mono = interleaved[base];
            if (numChannels > 1)
                mono = 0.5f * (mono + interleaved[base + 1]);
            meter.processSample(mono);
        }
        self->userLufsAvg[(size_t)useridx].store(meter.getCurrentLufs(), std::memory_order_relaxed);
    }

    if (self->chordAnalyzer != nullptr)
    {
        const int trackIndex = BatchedChordAnalyzer::remoteTrackIndexForUser(useridx);
        if (!self->isChordDetectionEnabled() || !self->isUserChordDetectionEnabled(useridx))
            self->chordAnalyzer->markNoInput(trackIndex);
        else
            self->chordAnalyzer->processInterleavedBlock(trackIndex, interleaved, numFrames, juce::jmin(2, numChannels), sampleRate);
    }

    return 1;
}

void NinjamVst3AudioProcessor::setLocalMonitorEnabled(bool enabled)
{
    localMonitorEnabled.store(enabled);
}

bool NinjamVst3AudioProcessor::isLocalMonitorEnabled() const
{
    return localMonitorEnabled.load();
}

void NinjamVst3AudioProcessor::setFxReverbEnabled(bool enabled)
{
    fxReverbEnabled.store(enabled);
}

bool NinjamVst3AudioProcessor::isFxReverbEnabled() const
{
    return fxReverbEnabled.load();
}

void NinjamVst3AudioProcessor::setFxDelayEnabled(bool enabled)
{
    fxDelayEnabled.store(enabled);
}

bool NinjamVst3AudioProcessor::isFxDelayEnabled() const
{
    return fxDelayEnabled.load();
}

void NinjamVst3AudioProcessor::setFxDelayMode(FxDelayMode mode)
{
    const int modeValue = mode == FxDelayMode::frippertronics ? (int)FxDelayMode::frippertronics
                                                              : (int)FxDelayMode::standard;
    const int previous = fxDelayMode.exchange(modeValue);
    if (previous != modeValue)
        fxDelayLowpassState.fill(0.0f);
}

NinjamVst3AudioProcessor::FxDelayMode NinjamVst3AudioProcessor::getFxDelayMode() const
{
    return fxDelayMode.load() == (int)FxDelayMode::frippertronics
        ? FxDelayMode::frippertronics
        : FxDelayMode::standard;
}

void NinjamVst3AudioProcessor::setFxReverbRoomSize(float roomSize)
{
    fxReverbRoomSize.store(juce::jlimit(0.0f, 1.0f, roomSize));
}

float NinjamVst3AudioProcessor::getFxReverbRoomSize() const
{
    return fxReverbRoomSize.load();
}

void NinjamVst3AudioProcessor::setFxReverbDamping(float damping)
{
    fxReverbDamping.store(juce::jlimit(0.0f, 1.0f, damping));
}

float NinjamVst3AudioProcessor::getFxReverbDamping() const
{
    return fxReverbDamping.load();
}

void NinjamVst3AudioProcessor::setFxReverbWetDryMix(float wetDryMix)
{
    fxReverbWetDryMix.store(juce::jlimit(0.0f, 1.0f, wetDryMix));
}

float NinjamVst3AudioProcessor::getFxReverbWetDryMix() const
{
    return fxReverbWetDryMix.load();
}

void NinjamVst3AudioProcessor::setFxReverbEarlyReflections(float earlyReflections)
{
    fxReverbEarlyReflections.store(juce::jlimit(0.0f, 1.0f, earlyReflections));
}

float NinjamVst3AudioProcessor::getFxReverbEarlyReflections() const
{
    return fxReverbEarlyReflections.load();
}

void NinjamVst3AudioProcessor::setFxReverbTail(float tail)
{
    fxReverbTail.store(juce::jlimit(0.0f, 1.0f, tail));
}

float NinjamVst3AudioProcessor::getFxReverbTail() const
{
    return fxReverbTail.load();
}

void NinjamVst3AudioProcessor::setFxDelayTimeMs(float timeMs)
{
    fxDelayTimeMs.store(juce::jlimit(20.0f, 10000.0f, timeMs));
}

float NinjamVst3AudioProcessor::getFxDelayTimeMs() const
{
    return fxDelayTimeMs.load();
}

void NinjamVst3AudioProcessor::setFxDelaySyncToHost(bool enabled)
{
    fxDelaySyncToHost.store(enabled);
}

bool NinjamVst3AudioProcessor::isFxDelaySyncToHost() const
{
    return fxDelaySyncToHost.load();
}

void NinjamVst3AudioProcessor::setFxDelayDivision(int division)
{
    if (division != 1 && division != 8 && division != 16)
        division = 8;
    fxDelayDivision.store(division);
}

int NinjamVst3AudioProcessor::getFxDelayDivision() const
{
    return fxDelayDivision.load();
}

void NinjamVst3AudioProcessor::setFxDelayPingPong(bool enabled)
{
    fxDelayPingPong.store(enabled);
}

bool NinjamVst3AudioProcessor::isFxDelayPingPong() const
{
    return fxDelayPingPong.load();
}

void NinjamVst3AudioProcessor::setFxDelayWetDryMix(float wetDryMix)
{
    fxDelayWetDryMix.store(juce::jlimit(0.0f, 1.0f, wetDryMix));
}

float NinjamVst3AudioProcessor::getFxDelayWetDryMix() const
{
    return fxDelayWetDryMix.load();
}

void NinjamVst3AudioProcessor::setFxDelayFeedback(float feedback)
{
    fxDelayFeedback.store(juce::jlimit(0.0f, 0.95f, feedback));
}

float NinjamVst3AudioProcessor::getFxDelayFeedback() const
{
    return fxDelayFeedback.load();
}

void NinjamVst3AudioProcessor::setLocalChannelReverbSend(int channel, float send)
{
    if (channel < 0 || channel >= maxLocalChannels)
        return;
    localChannelReverbSends[(size_t)channel].store(juce::jlimit(0.0f, 1.0f, send));
}

float NinjamVst3AudioProcessor::getLocalChannelReverbSend(int channel) const
{
    if (channel < 0 || channel >= maxLocalChannels)
        return 0.0f;
    return localChannelReverbSends[(size_t)channel].load();
}

void NinjamVst3AudioProcessor::setLocalChannelDelaySend(int channel, float send)
{
    if (channel < 0 || channel >= maxLocalChannels)
        return;
    localChannelDelaySends[(size_t)channel].store(juce::jlimit(0.0f, 1.0f, send));
}

float NinjamVst3AudioProcessor::getLocalChannelDelaySend(int channel) const
{
    if (channel < 0 || channel >= maxLocalChannels)
        return 0.0f;
    return localChannelDelaySends[(size_t)channel].load();
}

int NinjamVst3AudioProcessor::getBPI()
{
    return juce::jmax(1, cachedNinjamBpi.load(std::memory_order_relaxed));
}

float NinjamVst3AudioProcessor::getIntervalProgress()
{
    if (isTransportSyncEnabled() && (!hostWasPlaying.load() || syncWaitForInterval.load()))
        return 0.0f;

    int pos = 0;
    int length = 0;
    const juce::ScopedLock clientLock(ninjamClientLock);
    ninjamClient.GetPosition(&pos, &length);
    if (length > 0)
    {
        if (isTransportSyncEnabled() && hostWasPlaying.load())
        {
            int basePos = syncDisplayPositionOffset.load();
            int relativePos = pos - basePos;
            if (relativePos < 0)
                relativePos += length;
            return (float)relativePos / (float)length;
        }
        return (float)pos / (float)length;
    }
    return 0.0f;
}

float NinjamVst3AudioProcessor::getBPM()
{
    return juce::jmax(1.0f, cachedNinjamBpm.load(std::memory_order_relaxed));
}

int NinjamVst3AudioProcessor::getIntervalIndex() const
{
    return getDisplayIntervalIndex();
}

float NinjamVst3AudioProcessor::getLocalPeak() const
{
    return localPeak.load();
}

float NinjamVst3AudioProcessor::getLocalPeakLeft() const
{
    return localPeakL.load();
}

float NinjamVst3AudioProcessor::getLocalPeakRight() const
{
    return localPeakR.load();
}

void NinjamVst3AudioProcessor::sendSideSignal(const juce::String& target, const juce::String& type, const juce::String& payload)
{
    const char* tgt = target.isNotEmpty() ? target.toRawUTF8() : "*";
    const juce::ScopedLock clientLock(ninjamClientLock);
    ninjamClient.ChatMessage_Send("SIDE_SIGNAL", tgt, type.toRawUTF8(), payload.toRawUTF8());
}

void NinjamVst3AudioProcessor::sendIntervalSignal(const juce::String& type, const juce::String& payload, const juce::String& target)
{
    const juce::ScopedLock clientLock(ninjamClientLock);
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK) return;

    const bool isVdoSyncSignal = type == "intervalSyncTag"
                              || type == "intervalTransportProbe"
                              || type == "intervalTransportProbeAck"
                              || type == "videoTimingChange";
    if (isVdoSyncSignal
        && (!vdoVideoSyncEnabled.load(std::memory_order_relaxed)
            || ninjamZapVideoEnabled.load(std::memory_order_relaxed)))
        return;

    const bool useSideSignal = ninjamSideSignalServerSupported.load(std::memory_order_relaxed)
                            || ninjamClient.GetServerVideoSupported();
    if (useSideSignal)
        ninjamSideSignalServerSupported.store(true, std::memory_order_relaxed);

    // Wrap in {"sig":type, "data":payload} so the receiver knows the type.
    juce::DynamicObject::Ptr wrapper = new juce::DynamicObject();
    wrapper->setProperty("sig", type);
    wrapper->setProperty("data", payload);
    const juce::String msg = juce::JSON::toString(juce::var(wrapper.get()));

    if (useSideSignal)
    {
        const char* tgt = target.isNotEmpty() ? target.toRawUTF8() : "*";
        ninjamClient.ChatMessage_Send("SIDE_SIGNAL", tgt, type.toRawUTF8(), payload.toRawUTF8());
        return;
    }

    // All NJ+ raw metadata stays on the hidden control channel; channel 0 is audio-only.
    if (kSyncSignalChannelIndex >= serverMaxLocalChannelsCached.load(std::memory_order_relaxed))
        return;
    const int result = ninjamClient.SendRawIntervalItem(kSyncSignalChannelIndex, kSyncSignalFourcc, msg.toRawUTF8(), (int)msg.getNumBytesAsUTF8());
    if (result != 0)
        logIntervalPerf("interval sync raw send failed type=" + type + " result=" + juce::String(result));
}

void NinjamVst3AudioProcessor::setSpreadOutputsEnabled(bool shouldEnable)
{
    bool wasEnabled = spreadOutputsEnabled.load();
    spreadOutputsEnabled.store(shouldEnable);

    if (wasEnabled && !shouldEnable)
    {
        userOutputAssignment.clear();
        {
            const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
            remoteLinkAudioOutputPairs.clear();
            remoteLinkAudioSinks.clear();
        }

        int numUsers = ninjamClient.GetNumUsers();
        for (int userIdx = 0; userIdx < numUsers; ++userIdx)
        {
            for (int ch = 0; ch < 32; ++ch)
            {
                ninjamClient.SetUserChannelState(userIdx, ch,
                                                 false, false,
                                                 false, 0.0f,
                                                 false, 0.0f,
                                                 false, false,
                                                 false, false,
                                                 true, 0);
            }
        }
    }
}

bool NinjamVst3AudioProcessor::isSpreadOutputsEnabled() const
{
    return spreadOutputsEnabled.load();
}

void NinjamVst3AudioProcessor::setSpreadOutputStartPair(int pair)
{
    spreadOutputStartPair.store(juce::jlimit(0, 15, pair), std::memory_order_relaxed);
    userOutputAssignment.clear();
}

int NinjamVst3AudioProcessor::getSpreadOutputStartPair() const
{
    return spreadOutputStartPair.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setMobileHotspotModeEnabled(bool shouldEnable)
{
    mobileHotspotModeEnabled.store(shouldEnable, std::memory_order_relaxed);
    if (!shouldEnable)
        lastMobileHotspotHeartbeatSendMs = 0.0;
}

bool NinjamVst3AudioProcessor::isMobileHotspotModeEnabled() const
{
    return mobileHotspotModeEnabled.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setDpiScaleSetting(int setting)
{
    dpiScaleSetting.store(juce::jlimit(0, 5, setting));
}

int NinjamVst3AudioProcessor::getDpiScaleSetting() const
{
    return dpiScaleSetting.load(std::memory_order_relaxed);
}

float NinjamVst3AudioProcessor::getDpiScaleFactor() const
{
    const int setting = dpiScaleSetting.load(std::memory_order_relaxed);
    switch (setting)
    {
        case 1:  return 0.5f;   // 50%
        case 2:  return 0.75f;  // 75%
        case 3:  return 1.0f;   // 100%
        case 4:  return 1.25f;  // 125%
        case 5:  return 1.5f;   // 150%
        default: return 0.0f;   // auto — let JUCE decide
    }
}

void NinjamVst3AudioProcessor::setSshTunnelEnabled(bool shouldEnable)
{
    sshTunnelEnabled.store(shouldEnable, std::memory_order_relaxed);
    if (!shouldEnable)
        sshTunnel.stop();
}

bool NinjamVst3AudioProcessor::isSshTunnelEnabled() const
{
    return sshTunnelEnabled.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setSshTunnelHost(const juce::String& host)
{
    sshTunnelHost = host.trim();
}

juce::String NinjamVst3AudioProcessor::getSshTunnelHost() const
{
    return sshTunnelHost;
}

void NinjamVst3AudioProcessor::setSshTunnelPort(int port)
{
    sshTunnelPort.store(juce::jlimit(1, 65535, port), std::memory_order_relaxed);
}

int NinjamVst3AudioProcessor::getSshTunnelPort() const
{
    return sshTunnelPort.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setSshTunnelUser(const juce::String& user)
{
    sshTunnelUser = user.trim();
}

juce::String NinjamVst3AudioProcessor::getSshTunnelUser() const
{
    return sshTunnelUser;
}

void NinjamVst3AudioProcessor::setSshTunnelKeyFile(const juce::String& path)
{
    sshTunnelKeyFile = path.trim();
}

juce::String NinjamVst3AudioProcessor::getSshTunnelKeyFile() const
{
    return sshTunnelKeyFile;
}

juce::String NinjamVst3AudioProcessor::getSshTunnelLastError() const
{
    return sshTunnel.getLastError();
}

bool NinjamVst3AudioProcessor::isSshTunnelActive() const
{
    return sshTunnel.isActive();
}

void NinjamVst3AudioProcessor::setSamplePadVolume(int padIndex, float volume)
{
    if (padIndex < 0 || padIndex >= numSamplePads)
        return;
    samplePads[(size_t)padIndex].volume.store(juce::jlimit(0.0f, 2.0f, volume), std::memory_order_relaxed);
}

float NinjamVst3AudioProcessor::getSamplePadVolume(int padIndex) const
{
    if (padIndex < 0 || padIndex >= numSamplePads)
        return 1.0f;
    return samplePads[(size_t)padIndex].volume.load(std::memory_order_relaxed);
}

int NinjamVst3AudioProcessor::getCodecMode() const
{
    const int serverMaxLocalChannels = juce::jmax(1, serverMaxLocalChannelsCached.load(std::memory_order_relaxed));
    const bool opusLanesFit = serverMaxLocalChannels > kOpusMultichannelBaseIndex;
    const int maxFittedLocalChannels = opusLanesFit ? maxLocalChannels : 1;
    const int numCh = juce::jlimit(1, maxFittedLocalChannels, getEffectiveLocalChannelCount());
    const bool multiChanAuto = numCh > 1 && opusSyncAvailable.load() && opusLanesFit;
    if (!multiChanAuto)
        return 0;
    // Mixed mode: Vorbis ch0, hidden control ch1, Opus multichannel carrier ch2.
    return 1;
}

unsigned int NinjamVst3AudioProcessor::getVorbisMask() const
{
    return ninjamClient.GetCodecVorbisMask();
}

unsigned int NinjamVst3AudioProcessor::getOpusMask() const
{
    return ninjamClient.GetCodecOpusMask();
}

juce::String NinjamVst3AudioProcessor::translateText(const juce::String& text)
{
    juce::String targetCode;
    {
        juce::ScopedLock lock(chatLock);
        if (!autoTranslate)
            return text;

        targetCode = translateTargetLang.isNotEmpty() ? translateTargetLang : "en";
    }

    targetCode = resolveTranslateTargetLanguageCode(targetCode);
    if (targetCode.isEmpty())
        targetCode = "en";
    return translateTextForTarget(text, targetCode);
}

juce::String NinjamVst3AudioProcessor::translateTextForTarget(const juce::String& text, const juce::String& targetCode)
{
    auto fail = [this, &text](const juce::String& reason)
    {
        noteTranslationFailure(reason);
        return text;
    };

    if (text.trim().isEmpty())
        return text;

    juce::String translatedText;
    juce::String primaryError;
    if (tryTranslateWithFedilab(text, targetCode, translatedText, primaryError))
    {
        clearTranslationFailureState();
        return translatedText;
    }

    juce::String fallbackError;
    if (tryTranslateWithGoogleFallback(text, targetCode, translatedText, fallbackError))
    {
        clearTranslationFailureState();
        return translatedText;
    }

    juce::String combinedError = primaryError;
    if (combinedError.isNotEmpty() && fallbackError.isNotEmpty())
        combinedError << "; fallback: " << fallbackError;
    else if (combinedError.isEmpty())
        combinedError = fallbackError;

    if (combinedError.isEmpty())
        combinedError = "all translation services failed";

    return fail(combinedError);
}

void NinjamVst3AudioProcessor::enqueueAsyncTranslation(const juce::String& originalLine,
                                                       const juce::String& lineSender,
                                                       const juce::String& linePrefix,
                                                       const juce::String& lineBody)
{
    if (shouldSkipAutoChatTranslation(originalLine, lineBody) || asyncChatTranslationWorker == nullptr)
        return;

    juce::String preferredTarget;
    juce::uint64 configRevision = 0;
    {
        const juce::ScopedLock lock(chatLock);
        if (!autoTranslate)
            return;

        preferredTarget = translateTargetLang.isNotEmpty() ? translateTargetLang : "system";
        configRevision = translationConfigRevision.load(std::memory_order_relaxed);
    }

    juce::String targetCode = resolveTranslateTargetLanguageCode(preferredTarget);
    if (targetCode.isEmpty())
        targetCode = "en";

    AsyncChatTranslationWorker::Request request;
    request.originalLine = originalLine;
    request.lineSender = lineSender;
    request.linePrefix = linePrefix;
    request.lineBody = lineBody;
    request.targetCode = targetCode;
    request.configRevision = configRevision;
    asyncChatTranslationWorker->enqueue(std::move(request));
}

void NinjamVst3AudioProcessor::applyAsyncTranslatedChatLine(const juce::String& originalLine,
                                                            const juce::String& lineSender,
                                                            const juce::String& translatedLine,
                                                            juce::uint64 configRevision)
{
    const juce::ScopedLock lock(chatLock);
    if (!autoTranslate || configRevision != translationConfigRevision.load(std::memory_order_relaxed))
        return;

    for (int i = chatHistory.size(); --i >= 0;)
    {
        if (chatSenders[i] == lineSender && chatHistory[i] == originalLine)
        {
            if (chatHistory[i] != translatedLine)
            {
                chatHistory.set(i, translatedLine);
                chatRevision.fetch_add(1);
            }
            return;
        }
    }
}

std::vector<NinjamVst3AudioProcessor::PublicServerInfo> NinjamVst3AudioProcessor::getPublicServers() const
{
    std::vector<PublicServerInfo> copy;
    const juce::ScopedLock lock(serverListLock);
    copy = publicServers;
    return copy;
}

void NinjamVst3AudioProcessor::refreshPublicServers()
{
    std::vector<PublicServerInfo> result;

#if defined(_WIN32)
    const wchar_t* host = L"ninbot.com";
    const wchar_t* path = L"/app/servers.php";

    HINTERNET hSession = WinHttpOpen(L"NINJAMVST3/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS,
                                     0);
    if (!hSession)
        return;

    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        return;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect,
                                            L"GET",
                                            path,
                                            NULL,
                                            WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            0);
    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }

    BOOL ok = WinHttpSendRequest(hRequest,
                                 WINHTTP_NO_ADDITIONAL_HEADERS,
                                 0,
                                 WINHTTP_NO_REQUEST_DATA,
                                 0,
                                 0,
                                 0);
    if (!ok)
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }

    ok = WinHttpReceiveResponse(hRequest, NULL);
    if (!ok)
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }

    std::string response;
    DWORD dwSize = 0;
    do
    {
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0)
            break;

        std::string chunk;
        chunk.resize(dwSize);
        DWORD dwDownloaded = 0;
        if (!WinHttpReadData(hRequest, &chunk[0], dwSize, &dwDownloaded) || dwDownloaded == 0)
            break;

        response.append(chunk.data(), dwDownloaded);
    }
    while (dwSize > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (response.empty())
        return;

    juce::String jsonText = juce::String::fromUTF8(response.c_str(), (int)response.size());

    juce::var root;
    juce::Result parseError = juce::JSON::parse(jsonText, root);
    if (parseError.failed() || !root.isObject())
        return;

    auto* rootObj = root.getDynamicObject();
    if (!rootObj)
        return;

    juce::var serversVar = rootObj->getProperty("servers");
    if (!serversVar.isArray())
        return;

    auto* serversArray = serversVar.getArray();
    if (!serversArray)
        return;

    for (auto& serverVar : *serversArray)
    {
        if (!serverVar.isObject())
            continue;
        auto* obj = serverVar.getDynamicObject();
        if (!obj)
            continue;

        PublicServerInfo info;
        juce::String nameText = obj->getProperty("name").toString();
        info.name = nameText;

        int colon = nameText.lastIndexOfChar(':');
        if (colon > 0)
        {
            info.host = nameText.substring(0, colon);
            info.port = nameText.substring(colon + 1).getIntValue();
        }
        else
        {
            info.host = nameText;
            info.port = 2049;
        }

        info.bpi = obj->getProperty("bpi").toString().getIntValue();
        info.bpm = (float)obj->getProperty("bpm").toString().getDoubleValue();

        juce::var usersVar = obj->getProperty("users");
        if (usersVar.isArray() && usersVar.getArray() != nullptr)
        {
            info.userCount = usersVar.getArray()->size();
            info.userNames = extractPublicServerUserNames(usersVar);
        }
        else
        {
            info.userCount = obj->getProperty("user_count").toString().getIntValue();
        }

        info.userMax = obj->getProperty("user_max").toString().getIntValue();
        result.push_back(info);
    }
#else
    JNL_HTTPGet request(JNL_CONNECTION_AUTODNS, 16384, nullptr);
    request.addheader("User-Agent: NINJAMplus/1.0");
    request.addheader("Accept: application/json,*/*");
    request.connect("http://ninbot.com/app/servers.php");

    juce::MemoryOutputStream response;
    const double deadlineMs = juce::Time::getMillisecondCounterHiRes() + 8000.0;
    char buffer[4096] = {};

    for (;;)
    {
        const int runResult = request.run();

        for (;;)
        {
            const int available = request.bytes_available();
            if (available <= 0)
                break;

            const int bytesToRead = juce::jmin(available, (int)sizeof(buffer));
            const int bytesRead = request.get_bytes(buffer, bytesToRead);
            if (bytesRead <= 0)
                break;

            response.write(buffer, (size_t)bytesRead);
            if (response.getDataSize() > 4 * 1024 * 1024)
                return;
        }

        if (runResult == -1)
            return;
        if (runResult == 1)
            break;
        if (juce::Time::getMillisecondCounterHiRes() >= deadlineMs)
            return;

        juce::Thread::sleep(10);
    }

    const int replyCode = request.getreplycode();
    if (replyCode != 0 && (replyCode < 200 || replyCode >= 300))
        return;
    if (response.getDataSize() == 0)
        return;

    juce::String jsonText = juce::String::fromUTF8(static_cast<const char*>(response.getData()),
                                                   (int)response.getDataSize());

    juce::var root;
    juce::Result parseError = juce::JSON::parse(jsonText, root);
    if (parseError.failed() || !root.isObject())
        return;

    auto* rootObj = root.getDynamicObject();
    if (rootObj == nullptr)
        return;

    juce::var serversVar = rootObj->getProperty("servers");
    if (!serversVar.isArray())
        return;

    auto* serversArray = serversVar.getArray();
    if (serversArray == nullptr)
        return;

    for (auto& serverVar : *serversArray)
    {
        if (!serverVar.isObject())
            continue;
        auto* obj = serverVar.getDynamicObject();
        if (obj == nullptr)
            continue;

        PublicServerInfo info;
        juce::String nameText = obj->getProperty("name").toString();
        info.name = nameText;

        int colon = nameText.lastIndexOfChar(':');
        if (colon > 0)
        {
            info.host = nameText.substring(0, colon);
            info.port = nameText.substring(colon + 1).getIntValue();
        }
        else
        {
            info.host = nameText;
            info.port = 2049;
        }

        info.bpi = obj->getProperty("bpi").toString().getIntValue();
        info.bpm = (float)obj->getProperty("bpm").toString().getDoubleValue();

        juce::var usersVar = obj->getProperty("users");
        if (usersVar.isArray() && usersVar.getArray() != nullptr)
        {
            info.userCount = usersVar.getArray()->size();
            info.userNames = extractPublicServerUserNames(usersVar);
        }
        else
        {
            info.userCount = obj->getProperty("user_count").toString().getIntValue();
        }

        info.userMax = obj->getProperty("user_max").toString().getIntValue();
        result.push_back(info);
    }
#endif

    const juce::ScopedLock lock(serverListLock);
    publicServers.swap(result);
}

NinjamVst3AudioProcessor::~NinjamVst3AudioProcessor()
{
    if (zapChunkProcessingThread != nullptr)
    {
        zapChunkProcessingThread->signalThreadShouldExit();
        zapChunkProcessingThread->stopThread(2000);
        zapChunkProcessingThread.reset();
    }

    if (chordAnalyzer != nullptr)
        chordAnalyzer->stop();

    beginStandaloneShutdown();
    chordAnalyzer.reset();
    asyncChatTranslationWorker.reset();
    {
        const juce::ScopedLock launchLock(videoLaunchWorkerLock);
        if (videoLaunchFuture.valid())
            videoLaunchFuture.wait();
    }
    abletonLink.reset();
    JNL::close_socketlib();
}

void NinjamVst3AudioProcessor::beginStandaloneShutdown()
{
    stopTimer();
    if (samplePadBackgroundAlive)
        samplePadBackgroundAlive->store(false, std::memory_order_release);
    samplePadBackgroundPool.removeAllJobs(true, 4000);
    {
        const juce::ScopedLock lifecycleLock(ninjamAudioLifecycleLock);
        const juce::ScopedLock clientLock(ninjamClientLock);
        stopNinjamZapVideoTransportForDisconnect();
        ninjamClient.LicenseAgreementCallback = nullptr;
        ninjamClient.LicenseAgreement_User = nullptr;
        ninjamClient.ChatMessage_Callback = nullptr;
        ninjamClient.ChatMessage_User = nullptr;
        ninjamClient.IntervalMediaItem_Callback = nullptr;
        ninjamClient.IntervalMediaItem_User = nullptr;
        ninjamClient.IntervalChunkCallback = nullptr;
        ninjamClient.IntervalChunkCallbackUser = nullptr;
        ninjamClient.NewIntervalCallback = nullptr;
        ninjamClient.NewIntervalCallbackUser = nullptr;
        ninjamClient.PostNewIntervalCallback = nullptr;
        ninjamClient.PostNewIntervalCallbackUser = nullptr;
        ninjamClient.RemoteChannelAudioTap = nullptr;
        ninjamClient.RemoteChannelAudioTap_User = nullptr;

        ninjamClient.Disconnect();
    }

    if (asyncChatTranslationWorker)
        asyncChatTranslationWorker->stop(250);

    {
        const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
        abletonLinkSource.reset();
        abletonLinkSink.reset();
        remoteLinkAudioSinks.clear();
        remoteLinkAudioOutputPairs.clear();
    }

    linkAudioReceiveRing.reset();
    clearRemoteAudioTapBuffers();
    stopAdvancedVideoClient();
}

const juce::String NinjamVst3AudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool NinjamVst3AudioProcessor::acceptsMidi() const
{
    return true;
}

bool NinjamVst3AudioProcessor::producesMidi() const
{
    return true;
}

bool NinjamVst3AudioProcessor::isMidiEffect() const
{
    return false;
}

double NinjamVst3AudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int NinjamVst3AudioProcessor::getNumPrograms()
{
    return 1;
}

int NinjamVst3AudioProcessor::getCurrentProgram()
{
    return 0;
}

void NinjamVst3AudioProcessor::setCurrentProgram (int index)
{
}

const juce::String NinjamVst3AudioProcessor::getProgramName (int index)
{
    return {};
}

void NinjamVst3AudioProcessor::changeProgramName (int index, const juce::String& newName)
{
}

void NinjamVst3AudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    intervalSyncSampleCounter.store(0, std::memory_order_relaxed);
    cachedNinjamTransportPos.store(0, std::memory_order_relaxed);
    cachedNinjamTransportLen.store(0, std::memory_order_relaxed);
    cachedNinjamTransportSampleCounter.store(0, std::memory_order_relaxed);
    cachedNinjamBpi.store(juce::jmax(1, ninjamClient.GetBPI()), std::memory_order_relaxed);
    cachedNinjamBpm.store(juce::jmax(1.0f, (float)ninjamClient.GetActualBPM()), std::memory_order_relaxed);
    samplePadTransportInitialised = false;
    samplePadLastTransportPosition = 0;

    // Auto-tune processor (local channel 1)
    autoTuneProcessor = std::make_unique<ninjamplus::AutoTuneProcessor>();
    autoTuneProcessor->prepare(sampleRate, samplesPerBlock);
    autoTuneProcessor->setEnabled(autoTuneEnabled.load());
    autoTuneProcessor->setQuality(autoTuneQuality.load() == 1
        ? ninjamplus::PitchDetector::Quality::High
        : ninjamplus::PitchDetector::Quality::Low);
    autoTuneProcessor->setScale((ninjamplus::ScaleQuantizer::Scale)autoTuneScale.load());
    autoTuneProcessor->setKey(autoTuneKey.load());
    autoTuneProcessor->setCorrectionSpeed(autoTuneSpeed.load());
    samplePadLastTransportLength = 0;
    samplePadLastTransportBpi = 0;
    samplePadTransportInterval = 0;
    processingSampleRate = sampleRate > 1.0 ? sampleRate : 44100.0;
    lastBlockSize = juce::jmax(1, samplesPerBlock);
    masterLufsMeter.prepare(processingSampleRate);
    masterLufsMeterR.prepare(processingSampleRate);
    for (auto& m : userLufsMeters)
        m.prepare(processingSampleRate);
    resetMetronomeClickVoices();
    if (linkTimingState != nullptr)
        linkTimingState->reset();
    if (chordAnalyzer)
        chordAnalyzer->prepare(processingSampleRate);
    masterChordScratchBuffer.setSize(1, juce::jmax(1, samplesPerBlock), false, true, true);
    masterChordScratchBuffer.clear();

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = (juce::uint32) samplesPerBlock;
    spec.numChannels = (juce::uint32) getTotalNumOutputChannels();
    masterLimiter.prepare(spec);
    masterLimiter.setThreshold(limiterThresholdDb.load());
    masterLimiter.setRelease(limiterReleaseMs.load());
    masterLimiter.reset();

    fxReverb.reset();
    juce::Reverb::Parameters params;
    params.roomSize = fxReverbRoomSize.load();
    params.damping = 0.45f;
    params.width = 1.0f;
    params.wetLevel = 0.35f;
    params.dryLevel = 0.0f;
    params.freezeMode = 0.0f;
    fxReverb.setParameters(params);

    const int maxGlobalDelaySamples = juce::jmax(1, (int)std::ceil(processingSampleRate * 10.0));
    const int maxSamplePadDelaySamples = juce::jmax(1, (int)std::ceil(processingSampleRate * 2.5));
    fxDelayBuffer.setSize(2, maxGlobalDelaySamples, false, true, true);
    fxDelayBuffer.clear();
    fxDelayWritePosition = 0;
    fxDelayLowpassState.fill(0.0f);
    samplePadFxScratchBuffer.setSize(2, juce::jmax(1, samplesPerBlock), false, true, true);
    samplePadFxScratchBuffer.clear();
    samplePadDuckGainBuffer.assign((size_t)juce::jmax(1, samplesPerBlock), 1.0f);

    fxReverbInputBuffer.setSize(1, juce::jmax(1, samplesPerBlock), false, true, true);
    fxDelayInputBuffer.setSize(1, juce::jmax(1, samplesPerBlock), false, true, true);
    fxReturnBuffer.setSize(2, juce::jmax(1, samplesPerBlock), false, true, true);
    samplePadsRenderBuffer.setSize(2, juce::jmax(1, samplesPerBlock), false, true, true);
    samplePadsMonitorRenderBuffer.setSize(2, juce::jmax(1, samplesPerBlock), false, true, true);
    samplePadsOneShotRenderBuffer.setSize(2, juce::jmax(1, samplesPerBlock), false, true, true);
    samplePadsRenderBuffer.clear();
    samplePadsMonitorRenderBuffer.clear();
    samplePadsOneShotRenderBuffer.clear();
    samplePadsPeak.store(0.0f, std::memory_order_relaxed);

    juce::dsp::ProcessSpec sampleFxSpec;
    sampleFxSpec.sampleRate = sampleRate;
    sampleFxSpec.maximumBlockSize = (juce::uint32) juce::jmax(1, samplesPerBlock);
    sampleFxSpec.numChannels = 2;
    for (int pad = 0; pad < numSamplePads; ++pad)
    {
        for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
        {
            auto& dj = samplePadPerPadDjFilters[(size_t)pad][(size_t)slot];
            dj.prepare(sampleFxSpec);
            dj.setResonance(0.707f);
            dj.reset();

            auto& bp = samplePadPerPadDjBpFilters[(size_t)pad][(size_t)slot];
            bp.prepare(sampleFxSpec);
            bp.setType(juce::dsp::StateVariableTPTFilterType::bandpass);
            bp.setResonance(1.2f);
            bp.reset();

            auto& phaser = samplePadPerPadPhasers[(size_t)pad][(size_t)slot];
            phaser.prepare(sampleFxSpec);
            phaser.setDepth(0.72f);
            phaser.setCentreFrequency(950.0f);
            phaser.setFeedback(0.18f);
            phaser.setMix(0.0f);
            phaser.reset();

            auto& slotDelay = samplePadPerPadDelayBuffers[(size_t)pad][(size_t)slot];
            slotDelay.setSize(2, maxSamplePadDelaySamples, false, true, true);
            slotDelay.clear();
            samplePadPerPadDelayWritePositions[(size_t)pad][(size_t)slot] = 0;

            auto& reverb = samplePadPerPadReverbs[(size_t)pad][(size_t)slot];
            reverb.reset();
            reverb.setParameters(params);

            auto& slotInput = samplePadPerPadFxSlotInputBuffers[(size_t)pad][(size_t)slot];
            slotInput.setSize(2, juce::jmax(1, samplesPerBlock), false, true, true);
            slotInput.clear();
            auto& monitorDj = samplePadMonitorPerPadDjFilters[(size_t)pad][(size_t)slot];
            monitorDj.prepare(sampleFxSpec);
            monitorDj.setResonance(0.707f);
            monitorDj.reset();

            auto& monitorBp = samplePadMonitorPerPadDjBpFilters[(size_t)pad][(size_t)slot];
            monitorBp.prepare(sampleFxSpec);
            monitorBp.setType(juce::dsp::StateVariableTPTFilterType::bandpass);
            monitorBp.setResonance(1.2f);
            monitorBp.reset();

            auto& monitorPhaser = samplePadMonitorPerPadPhasers[(size_t)pad][(size_t)slot];
            monitorPhaser.prepare(sampleFxSpec);
            monitorPhaser.setDepth(0.72f);
            monitorPhaser.setCentreFrequency(950.0f);
            monitorPhaser.setFeedback(0.18f);
            monitorPhaser.setMix(0.0f);
            monitorPhaser.reset();

            auto& monitorSlotDelay = samplePadMonitorPerPadDelayBuffers[(size_t)pad][(size_t)slot];
            monitorSlotDelay.setSize(2, maxSamplePadDelaySamples, false, true, true);
            monitorSlotDelay.clear();
            samplePadMonitorPerPadDelayWritePositions[(size_t)pad][(size_t)slot] = 0;

            auto& monitorReverb = samplePadMonitorPerPadReverbs[(size_t)pad][(size_t)slot];
            monitorReverb.reset();
            monitorReverb.setParameters(params);

            auto& monitorSlotInput = samplePadMonitorPerPadFxSlotInputBuffers[(size_t)pad][(size_t)slot];
            monitorSlotInput.setSize(2, juce::jmax(1, samplesPerBlock), false, true, true);
            monitorSlotInput.clear();
        }
    }
    samplePadDuckOscillator.prepare(sampleFxSpec);
    samplePadDuckOscillator.setFrequency(1.0f, true);
    samplePadDuckOscillator.reset();

    linkAudioMaxNumSamples = (size_t) juce::jmax(8192, samplesPerBlock * 2);
    {
        const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
        if (abletonLinkSink != nullptr)
            abletonLinkSink->requestMaxNumSamples(linkAudioMaxNumSamples);
    }
}

void NinjamVst3AudioProcessor::releaseResources()
{
    if (linkTimingState != nullptr)
        linkTimingState->reset();

    if (chordAnalyzer != nullptr)
    {
        chordAnalyzer->markAllNoInput();
        chordAnalyzer->stop();
    }

    if (autoTuneProcessor)
        autoTuneProcessor->release();
}

bool NinjamVst3AudioProcessor::loadSamplePad(int padIndex, const juce::File& file)
{
    if (!isValidSamplePadIndex(padIndex) || !file.existsAsFile())
        return false;

    std::unique_ptr<juce::AudioFormatReader> reader(samplePadFormatManager.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels <= 0)
        return false;

    constexpr double maxSamplePadSeconds = 180.0;
    const double sourceRate = reader->sampleRate > 1.0 ? reader->sampleRate : 44100.0;
    const juce::int64 maxSamplesFromLength = (juce::int64)std::ceil(sourceRate * maxSamplePadSeconds);
    const juce::int64 samplesToRead64 = juce::jmin(reader->lengthInSamples, maxSamplesFromLength);
    if (samplesToRead64 <= 0 || samplesToRead64 > (juce::int64)std::numeric_limits<int>::max())
        return false;

    const int samplesToRead = (int)samplesToRead64;
    juce::AudioBuffer<float> loaded(2, samplesToRead);
    loaded.clear();

    const bool readRightChannel = reader->numChannels > 1;
    if (!reader->read(&loaded, 0, samplesToRead, 0, true, readRightChannel))
        return false;

    if (!readRightChannel)
        loaded.copyFrom(1, 0, loaded, 0, 0, samplesToRead);

    const auto bpmDetection = detectSampleBpmDetailed(loaded, sourceRate);
    const double detectedBpm = bpmDetection.normalisedBpm;

    {
        const juce::ScopedLock lock(samplePadsLock);
        auto& pad = samplePads[(size_t)padIndex];
        pad.sample = std::move(loaded);
        pad.originalSample = pad.sample;
        if (!pad.nameIsCustom)
            pad.name = file.getFileNameWithoutExtension();
        pad.file = file;
        pad.sourceSampleRate = sourceRate;
        pad.originalSourceSampleRate = sourceRate;
        pad.sourceBpm = detectedBpm;
        pad.rawSourceBpm = bpmDetection.rawBpm;
        pad.lastSyncedTargetBpm = 0.0;
        pad.bpmSyncApplied = false;
        pad.bpmSyncEnabled.store(detectedBpm > 1.0, std::memory_order_relaxed);
        pad.playbackSpeed.store((int)SamplePadPlaybackSpeed::normal, std::memory_order_relaxed);
        pad.recordedLoop = false;
        pad.loopLengthBeats = bpmDetection.loopLengthBeats;
        pad.recordLoopLengthBeatsOverride = 0;
        pad.playing.store(false, std::memory_order_relaxed);
        pad.playbackScheduled.store(false, std::memory_order_relaxed);
        for (auto& voice : pad.oneShotVoices)
        {
            voice.active = false;
            voice.position = 0.0;
        }
        pad.nextOneShotVoice = 0;
        pad.activeOneShotVoices.store(0, std::memory_order_relaxed);
        pad.recordArmed.store(false, std::memory_order_relaxed);
        pad.recordPendingStart.store(false, std::memory_order_relaxed);
        pad.recordPendingStop.store(false, std::memory_order_relaxed);
        pad.recordStartScheduled.store(false, std::memory_order_relaxed);
        pad.recording.store(false, std::memory_order_relaxed);
        pad.recordAutoStopAtScheduledEnd = false;
        pad.recordMatchBpiCanvas = false;
        pad.recordScheduledStartBeat = 0.0;
        pad.recordScheduledStopBeat = 0.0;
        pad.midiHoldActive = false;
        pad.midiHoldActionTriggered = false;
        pad.midiPadDown = false;
        pad.midiHoldStartMs = 0.0;
        pad.position.store(0.0, std::memory_order_relaxed);
    }

    // Auto-resync to NINJAM BPM if BPM sync was enabled by detection
    if (detectedBpm > 1.0 && isSamplePadsFeatureEnabled())
        resyncSamplePadToBpm(padIndex, (double)getBPM(), false);

    return true;
}

void NinjamVst3AudioProcessor::loadSamplePadAsync(int padIndex,
                                                  const juce::File& file,
                                                  std::function<void(bool, const juce::String&)> completion)
{
    if (!isValidSamplePadIndex(padIndex) || !file.existsAsFile() || !samplePadBackgroundAlive)
    {
        if (completion)
            completion(false, "That sample could not be loaded.");
        return;
    }

    const juce::uint64 requestSerial = samplePadLoadRequestSerial[(size_t)padIndex].fetch_add(1, std::memory_order_acq_rel) + 1;
    auto alive = samplePadBackgroundAlive;

    samplePadBackgroundPool.addJob(new SamplePadBackgroundJob("NINJAMSamplePadLoad",
        [this, alive, padIndex, file, requestSerial, completion = std::move(completion)]() mutable
        {
            if (!alive->load(std::memory_order_acquire)
                || samplePadLoadRequestSerial[(size_t)padIndex].load(std::memory_order_acquire) != requestSerial)
            {
                return juce::ThreadPoolJob::jobHasFinished;
            }

            PreparedSamplePadLoadData prepared;
            const bool loaded = prepareSamplePadLoadData(file, prepared);

            juce::MessageManager::callAsync(
                [this, alive, padIndex, requestSerial, loaded,
                 prepared = std::move(prepared), completion = std::move(completion)]() mutable
                {
                    if (!alive->load(std::memory_order_acquire)
                        || samplePadLoadRequestSerial[(size_t)padIndex].load(std::memory_order_acquire) != requestSerial)
                    {
                        return;
                    }

                    if (!loaded)
                    {
                        if (completion)
                            completion(false, "That sample could not be loaded.");
                        return;
                    }

                    samplePadResyncRequestSerial[(size_t)padIndex].fetch_add(1, std::memory_order_acq_rel);

                    // Pre-copy originalSample before acquiring the lock so the
                    // lock hold is O(1) moves only — no large buffer copies on
                    // the audio thread's contended mutex.
                    juce::AudioBuffer<float> preparedOriginal = prepared.sample;

                    {
                        const juce::ScopedLock lock(samplePadsLock);
                        auto& pad = samplePads[(size_t)padIndex];
                        pad.sample = std::move(prepared.sample);
                        pad.originalSample = std::move(preparedOriginal);
                        if (!pad.nameIsCustom)
                            pad.name = prepared.defaultName;
                        pad.file = prepared.file;
                        pad.sourceSampleRate = prepared.sourceRate;
                        pad.originalSourceSampleRate = prepared.sourceRate;
                        pad.sourceBpm = prepared.detectedBpm;
                        pad.rawSourceBpm = prepared.rawDetectedBpm;
                        pad.lastSyncedTargetBpm = 0.0;
                        pad.bpmSyncApplied = false;
                        pad.bpmSyncEnabled.store(prepared.detectedBpm > 1.0, std::memory_order_relaxed);
                        pad.playbackSpeed.store((int)SamplePadPlaybackSpeed::normal, std::memory_order_relaxed);
                        pad.recordedLoop = false;
                        pad.loopLengthBeats = prepared.loopLengthBeats;
                        pad.recordLoopLengthBeatsOverride = 0;
                        pad.playing.store(false, std::memory_order_relaxed);
                        pad.playbackScheduled.store(false, std::memory_order_relaxed);
                        for (auto& voice : pad.oneShotVoices)
                        {
                            voice.active = false;
                            voice.position = 0.0;
                        }
                        pad.nextOneShotVoice = 0;
                        pad.activeOneShotVoices.store(0, std::memory_order_relaxed);
                        pad.recordArmed.store(false, std::memory_order_relaxed);
                        pad.recordPendingStart.store(false, std::memory_order_relaxed);
                        pad.recordPendingStop.store(false, std::memory_order_relaxed);
                        pad.recordStartScheduled.store(false, std::memory_order_relaxed);
                        pad.recording.store(false, std::memory_order_relaxed);
                        pad.recordAutoStopAtScheduledEnd = false;
                        pad.recordMatchBpiCanvas = false;
                        pad.recordScheduledStartBeat = 0.0;
                        pad.recordScheduledStopBeat = 0.0;
                        pad.midiHoldActive = false;
                        pad.midiHoldActionTriggered = false;
                        pad.midiPadDown = false;
                        pad.midiHoldStartMs = 0.0;
                        pad.position.store(0.0, std::memory_order_relaxed);
                    }

                    // Auto-resync to NINJAM BPM if BPM sync was enabled by detection
                    if (prepared.detectedBpm > 1.0 && isSamplePadsFeatureEnabled())
                        resyncSamplePadToBpm(padIndex, (double)getBPM(), false);

                    if (completion)
                        completion(true, {});
                });

            return juce::ThreadPoolJob::jobHasFinished;
        }), true);
}

void NinjamVst3AudioProcessor::clearSamplePad(int padIndex)
{
    if (!isValidSamplePadIndex(padIndex))
        return;

    const juce::ScopedLock lock(samplePadsLock);
    auto& pad = samplePads[(size_t)padIndex];
    if (pad.sample.getNumSamples() > 0)
    {
        pad.undoClearSample = pad.sample;
        pad.undoClearOriginalSample = pad.originalSample;
        pad.undoClearName = pad.name;
        pad.undoClearFile = pad.file;
        pad.undoClearSourceSampleRate = pad.sourceSampleRate;
        pad.undoClearOriginalSourceSampleRate = pad.originalSourceSampleRate;
        pad.undoClearSourceBpm = pad.sourceBpm;
        pad.undoClearLastSyncedTargetBpm = pad.lastSyncedTargetBpm;
        pad.undoClearNameIsCustom = pad.nameIsCustom;
        pad.undoClearBpmSyncApplied = pad.bpmSyncApplied;
        pad.undoClearLoop = pad.loop.load(std::memory_order_relaxed);
        pad.undoClearReverse = pad.reverse.load(std::memory_order_relaxed);
        pad.undoClearMatchBpi = pad.matchBpi.load(std::memory_order_relaxed);
        pad.undoClearBpmSyncEnabled = pad.bpmSyncEnabled.load(std::memory_order_relaxed);
        pad.undoClearPlaybackSpeed = sanitizeSamplePadPlaybackSpeed(pad.playbackSpeed.load(std::memory_order_relaxed));
        pad.undoClearDuckRoute = pad.duckRoute.load(std::memory_order_relaxed);
        for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
            pad.undoClearFxSlotRoutes[(size_t)slot] = pad.fxSlotRoutes[(size_t)slot].load(std::memory_order_relaxed);
        pad.undoClearLoopAnchorBeat = pad.loopAnchorBeat;
        pad.undoClearRecordedStartBeatInInterval = pad.recordedStartBeatInInterval;
        pad.undoClearLoopLengthBeats = pad.loopLengthBeats;
        pad.undoClearRecordedLoop = pad.recordedLoop;
        pad.canUndoClear = true;
    }

    pad.sample.setSize(0, 0);
    pad.originalSample.setSize(0, 0);
    pad.name.clear();
    pad.nameIsCustom = false;
    pad.file = juce::File{};
    pad.sourceSampleRate = 44100.0;
    pad.originalSourceSampleRate = 44100.0;
    pad.sourceBpm = 0.0;
    pad.lastSyncedTargetBpm = 0.0;
    pad.bpmSyncApplied = false;
    pad.loop.store(false, std::memory_order_relaxed);
    pad.reverse.store(false, std::memory_order_relaxed);
    pad.matchBpi.store(false, std::memory_order_relaxed);
    pad.bpmSyncEnabled.store(true, std::memory_order_relaxed);
    pad.playbackSpeed.store((int)SamplePadPlaybackSpeed::normal, std::memory_order_relaxed);
    pad.pendingPlaybackSpeed.store(-1, std::memory_order_relaxed);
    pad.duckRoute.store(false, std::memory_order_relaxed);
    for (auto& route : pad.fxSlotRoutes)
        route.store(false, std::memory_order_relaxed);
    pad.playing.store(false, std::memory_order_relaxed);
    pad.playbackScheduled.store(false, std::memory_order_relaxed);
    for (auto& voice : pad.oneShotVoices)
    {
        voice.active = false;
        voice.position = 0.0;
    }
    pad.nextOneShotVoice = 0;
    pad.activeOneShotVoices.store(0, std::memory_order_relaxed);
    pad.recordArmed.store(false, std::memory_order_relaxed);
    pad.recordPendingStart.store(false, std::memory_order_relaxed);
    pad.recordPendingStop.store(false, std::memory_order_relaxed);
    pad.recordStartScheduled.store(false, std::memory_order_relaxed);
    pad.recording.store(false, std::memory_order_relaxed);
    pad.position.store(0.0, std::memory_order_relaxed);
    pad.loopLengthBeats = 0;
    pad.recordLoopLengthBeatsOverride = 0;
    pad.loopAnchorBeat = 0.0;
    pad.recordedStartBeatInInterval = 0.0;
    pad.scheduledStartBeat = 0.0;
    pad.recordScheduledStartBeat = 0.0;
    pad.recordScheduledStopBeat = 0.0;
    pad.recordAutoStopAtScheduledEnd = false;
    pad.recordMatchBpiCanvas = false;
    pad.recordedLoop = false;
    pad.recordBuffer.setSize(0, 0);
    pad.recordWritePosition = 0;
    pad.recordStartBeat = 0.0;
    pad.midiHoldActive = false;
    pad.midiHoldActionTriggered = false;
    pad.midiPadDown = false;
    pad.midiHoldStartMs = 0.0;
}

void NinjamVst3AudioProcessor::clearAllSamplePads()
{
    for (int pad = 0; pad < numSamplePads; ++pad)
        clearSamplePad(pad);
}

void NinjamVst3AudioProcessor::resetSamplePadSettings()
{
    samplePadsVolume.store(1.0f, std::memory_order_relaxed);
    samplePadsLimiterEnabled.store(false, std::memory_order_relaxed);
    samplePadsDuckEnabled.store(false, std::memory_order_relaxed);
    samplePadsDuckShape.store((int)SamplePadDuckShape::smoothPump, std::memory_order_relaxed);
    samplePadsDuckLength.store((int)SamplePadDuckLength::quarter, std::memory_order_relaxed);
    samplePadsUseDefaultFx.store(true, std::memory_order_relaxed);
    samplePadMonitorModeEnabled.store(false, std::memory_order_relaxed);
    samplePadsPeak.store(0.0f, std::memory_order_relaxed);

    for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
    {
        samplePadFxSlotTypes[(size_t)slot].store((int)getDefaultSamplePadFxType(slot), std::memory_order_relaxed);
        samplePadFxSlotAmounts[(size_t)slot].store(0.0f, std::memory_order_relaxed);
        for (int targetSlot = 0; targetSlot < numSamplePadFxSlots; ++targetSlot)
            samplePadFxSlotChainRoutes[(size_t)slot][(size_t)targetSlot].store(false, std::memory_order_relaxed);
    }

    {
        const juce::ScopedLock lock(samplePadsLock);
        for (auto& pad : samplePads)
        {
            pad.duckRoute.store(false, std::memory_order_relaxed);
            pad.playbackSpeed.store((int)SamplePadPlaybackSpeed::normal, std::memory_order_relaxed);
            pad.bpmSyncApplied = false;
            pad.lastSyncedTargetBpm = 0.0;
            for (auto& route : pad.fxSlotRoutes)
                route.store(false, std::memory_order_relaxed);
        }
    }

    const double bpm = (double)getBPM();
    if (bpm > 1.0)
        resyncLoopedSamplePadsToBpm(bpm);
}

void NinjamVst3AudioProcessor::setSamplePadsFeatureEnabled(bool shouldEnable)
{
    const bool wasEnabled = samplePadsFeatureEnabled.exchange(shouldEnable, std::memory_order_acq_rel);
    if (wasEnabled == shouldEnable)
        return;

    if (!shouldEnable)
        stopAllSamplePadRuntimeActivity();
}

bool NinjamVst3AudioProcessor::isSamplePadsFeatureEnabled() const
{
    return samplePadsFeatureEnabled.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::stopAllSamplePadRuntimeActivity()
{
    for (auto& serial : samplePadResyncRequestSerial)
        serial.fetch_add(1, std::memory_order_acq_rel);

    samplePadsPeak.store(0.0f, std::memory_order_relaxed);
    clearRemoteAudioTapBuffers();

    const juce::ScopedLock lock(samplePadsLock);
    for (auto& pad : samplePads)
    {
        pad.playing.store(false, std::memory_order_relaxed);
        pad.playbackScheduled.store(false, std::memory_order_relaxed);
        for (auto& voice : pad.oneShotVoices)
        {
            voice.active = false;
            voice.position = 0.0;
        }
        pad.nextOneShotVoice = 0;
        pad.activeOneShotVoices.store(0, std::memory_order_relaxed);
        pad.recordArmed.store(false, std::memory_order_relaxed);
        pad.recordPendingStart.store(false, std::memory_order_relaxed);
        pad.recordPendingStop.store(false, std::memory_order_relaxed);
        pad.recordStartScheduled.store(false, std::memory_order_relaxed);
        pad.recording.store(false, std::memory_order_relaxed);
        pad.recordLoopLengthBeatsOverride = 0;
        pad.recordScheduledStartBeat = 0.0;
        pad.recordScheduledStopBeat = 0.0;
        pad.recordAutoStopAtScheduledEnd = false;
        pad.recordMatchBpiCanvas = false;
        pad.recordBuffer.setSize(0, 0);
        pad.recordWritePosition = 0;
        pad.recordStartBeat = 0.0;
        pad.midiHoldActive = false;
        pad.midiHoldActionTriggered = false;
        pad.midiPadDown = false;
        pad.midiHoldStartMs = 0.0;
    }
}

void NinjamVst3AudioProcessor::undoSamplePadClear(int padIndex)
{
    if (!isValidSamplePadIndex(padIndex))
        return;

    const juce::ScopedLock lock(samplePadsLock);
    auto& pad = samplePads[(size_t)padIndex];
    if (!pad.canUndoClear || pad.undoClearSample.getNumSamples() <= 0)
        return;

    pad.sample = pad.undoClearSample;
    pad.originalSample = pad.undoClearOriginalSample;
    pad.name = pad.undoClearName;
    pad.file = pad.undoClearFile;
    pad.sourceSampleRate = pad.undoClearSourceSampleRate;
    pad.originalSourceSampleRate = pad.undoClearOriginalSourceSampleRate;
    pad.sourceBpm = pad.undoClearSourceBpm;
    pad.lastSyncedTargetBpm = pad.undoClearLastSyncedTargetBpm;
    pad.nameIsCustom = pad.undoClearNameIsCustom;
    pad.bpmSyncApplied = pad.undoClearBpmSyncApplied;
    pad.loop.store(pad.undoClearLoop, std::memory_order_relaxed);
    pad.reverse.store(pad.undoClearReverse, std::memory_order_relaxed);
    pad.matchBpi.store(pad.undoClearMatchBpi, std::memory_order_relaxed);
    pad.bpmSyncEnabled.store(pad.undoClearBpmSyncEnabled, std::memory_order_relaxed);
    pad.playbackSpeed.store((int)pad.undoClearPlaybackSpeed, std::memory_order_relaxed);
    pad.duckRoute.store(pad.undoClearDuckRoute, std::memory_order_relaxed);
    for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
        pad.fxSlotRoutes[(size_t)slot].store(pad.undoClearFxSlotRoutes[(size_t)slot], std::memory_order_relaxed);
    pad.loopAnchorBeat = pad.undoClearLoopAnchorBeat;
    pad.recordedStartBeatInInterval = pad.undoClearRecordedStartBeatInInterval;
    pad.loopLengthBeats = pad.undoClearLoopLengthBeats;
    pad.recordedLoop = pad.undoClearRecordedLoop;

    pad.playing.store(false, std::memory_order_relaxed);
    pad.playbackScheduled.store(false, std::memory_order_relaxed);
    for (auto& voice : pad.oneShotVoices)
    {
        voice.active = false;
        voice.position = 0.0;
    }
    pad.nextOneShotVoice = 0;
    pad.activeOneShotVoices.store(0, std::memory_order_relaxed);
    pad.recordArmed.store(false, std::memory_order_relaxed);
    pad.recordPendingStart.store(false, std::memory_order_relaxed);
    pad.recordPendingStop.store(false, std::memory_order_relaxed);
    pad.recordStartScheduled.store(false, std::memory_order_relaxed);
    pad.recording.store(false, std::memory_order_relaxed);
    pad.position.store(0.0, std::memory_order_relaxed);
    pad.recordBuffer.setSize(0, 0);
    pad.recordWritePosition = 0;
    pad.recordStartBeat = 0.0;
    pad.recordScheduledStartBeat = 0.0;
    pad.recordScheduledCountdownBeats = 0.0;
    pad.recordScheduledStopBeat = 0.0;
    pad.recordLoopLengthBeatsOverride = 0;
    pad.recordAutoStopAtScheduledEnd = false;
    pad.recordMatchBpiCanvas = false;
    pad.midiHoldActive = false;
    pad.midiHoldActionTriggered = false;
    pad.midiPadDown = false;
    pad.midiHoldStartMs = 0.0;
}

bool NinjamVst3AudioProcessor::canUndoSamplePadClear(int padIndex) const
{
    if (!isValidSamplePadIndex(padIndex))
        return false;

    const juce::ScopedLock lock(samplePadsLock);
    const auto& pad = samplePads[(size_t)padIndex];
    return pad.canUndoClear && pad.undoClearSample.getNumSamples() > 0;
}

void NinjamVst3AudioProcessor::triggerSamplePad(int padIndex)
{
    if (!isSamplePadsFeatureEnabled() || !isValidSamplePadIndex(padIndex))
        return;

    const juce::ScopedLock lock(samplePadsLock);
    auto& pad = samplePads[(size_t)padIndex];
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    if (nowMs - pad.lastAcceptedPressMs < samplePadPressDebounceMs)
        return;
    pad.lastAcceptedPressMs = nowMs;
    pad.triggerFlashCounter.fetch_add(1, std::memory_order_relaxed);
    const int length = pad.sample.getNumSamples();
    if (pad.recording.load(std::memory_order_relaxed))
    {
        pad.recordPendingStop.store(true, std::memory_order_relaxed);
        return;
    }

    if (pad.recordStartScheduled.load(std::memory_order_relaxed))
    {
        const int bpi = juce::jmax(1, getBPI());
        const bool matchBpi = pad.matchBpi.load(std::memory_order_relaxed);
        pad.recordScheduledStartBeat = 0.0;
        pad.recordScheduledCountdownBeats = 0.0;
        pad.recordScheduledStopBeat = 0.0;
        pad.recordLoopLengthBeatsOverride = matchBpi ? bpi : 0;
        pad.recordAutoStopAtScheduledEnd = false;
        pad.recordMatchBpiCanvas = false;
        pad.recordPendingStart.store(true, std::memory_order_relaxed);
        pad.recordPendingStop.store(false, std::memory_order_relaxed);
        pad.recordStartScheduled.store(false, std::memory_order_relaxed);
        pad.playing.store(false, std::memory_order_relaxed);
        pad.playbackScheduled.store(false, std::memory_order_relaxed);
        for (auto& voice : pad.oneShotVoices)
        {
            voice.active = false;
            voice.position = 0.0;
        }
        pad.nextOneShotVoice = 0;
        pad.activeOneShotVoices.store(0, std::memory_order_relaxed);
        return;
    }

    if (pad.recordArmed.load(std::memory_order_relaxed))
    {
        const int bpi = juce::jmax(1, getBPI());
        const double currentBeat = (double)intervalIndex.load(std::memory_order_relaxed) * (double)bpi
            + (double)juce::jlimit(0.0f, 1.0f, getIntervalProgress()) * (double)bpi;
        const double candidate = nextSamplePadGridBeat(currentBeat, bpi);
        const bool matchBpi = pad.matchBpi.load(std::memory_order_relaxed);

        pad.recordScheduledStartBeat = candidate;
        pad.recordScheduledCountdownBeats = samplePadRecordBarBeats;
        pad.recordScheduledStopBeat = 0.0;
        pad.recordLoopLengthBeatsOverride = matchBpi ? bpi : 0;
        pad.recordAutoStopAtScheduledEnd = false;
        pad.recordMatchBpiCanvas = false;
        pad.recordPendingStart.store(false, std::memory_order_relaxed);
        pad.recordPendingStop.store(false, std::memory_order_relaxed);
        pad.recordStartScheduled.store(true, std::memory_order_relaxed);
        pad.playing.store(false, std::memory_order_relaxed);
        pad.playbackScheduled.store(false, std::memory_order_relaxed);
        for (auto& voice : pad.oneShotVoices)
        {
            voice.active = false;
            voice.position = 0.0;
        }
        pad.nextOneShotVoice = 0;
        pad.activeOneShotVoices.store(0, std::memory_order_relaxed);
        return;
    }

    if (length <= 0)
        return;

    const bool oneShotMode = !pad.loop.load(std::memory_order_relaxed) && !pad.recordedLoop;
    const bool routeNewVoiceToLocal = !samplePadMonitorModeEnabled.load(std::memory_order_relaxed);
    if (oneShotMode)
    {
        const bool reverse = pad.reverse.load(std::memory_order_relaxed);
        pad.playing.store(false, std::memory_order_relaxed);
        pad.playbackScheduled.store(false, std::memory_order_relaxed);
        pad.position.store(reverse ? juce::jmax(0.0, (double)length - 1.0) : 0.0, std::memory_order_relaxed);

        auto& voice = pad.oneShotVoices[(size_t)pad.nextOneShotVoice];
        voice.active = true;
        voice.routeToLocal = routeNewVoiceToLocal;
        voice.position = reverse ? juce::jmax(0.0, (double)length - 1.0) : 0.0;
        pad.nextOneShotVoice = (pad.nextOneShotVoice + 1) % samplePadOneShotVoiceCount;

        int activeVoices = 0;
        for (const auto& candidateVoice : pad.oneShotVoices)
            if (candidateVoice.active)
                ++activeVoices;
        pad.activeOneShotVoices.store(activeVoices, std::memory_order_relaxed);
        return;
    }

    if (pad.playing.load(std::memory_order_relaxed)
        || pad.playbackScheduled.load(std::memory_order_relaxed))
    {
        pad.playing.store(false, std::memory_order_relaxed);
        pad.playbackScheduled.store(false, std::memory_order_relaxed);
        return;
    }

    if (pad.loop.load(std::memory_order_relaxed))
    {
        const int bpi = juce::jmax(1, getBPI());

        // Use the same timing source as the looper recording (intervalIndex + getIntervalProgress)
        // since the looper recording starts in sync and on time.
        const double currentBeat = (double)intervalIndex.load(std::memory_order_relaxed) * (double)bpi
            + (double)juce::jlimit(0.0f, 1.0f, getIntervalProgress()) * (double)bpi;

        const bool syncBpiEnabled = pad.bpmSyncEnabled.load(std::memory_order_relaxed);
        const bool matchBpiEnabled = pad.matchBpi.load(std::memory_order_relaxed);

        // When Sync BPI or Match BPI is enabled, cue to the next interval boundary (BPI 1).
        if (syncBpiEnabled || matchBpiEnabled || pad.recordedLoop)
        {
            const double startOffset = (matchBpiEnabled || pad.recordedLoop)
                ? pad.recordedStartBeatInInterval
                : 0.0;

            pad.scheduledStartBeat = nextSamplePadIntervalStartBeat(currentBeat, bpi) + startOffset;
            pad.scheduledPlaybackRouteToLocal = routeNewVoiceToLocal;
            pad.playbackScheduled.store(true, std::memory_order_relaxed);
            return;
        }

        // No sync enabled — schedule to the next loop-length boundary.
        // Determine the loop length in beats from the sample length and raw source BPM.
        double loopLengthBeats = (double)juce::jmax(1, pad.loopLengthBeats);
        if (pad.loopLengthBeats <= 0 && pad.rawSourceBpm > 1.0 && pad.sourceSampleRate > 1.0)
        {
            const double sampleSeconds = (double)length / pad.sourceSampleRate;
            loopLengthBeats = juce::jmax(1.0, sampleSeconds * pad.rawSourceBpm / 60.0);
            loopLengthBeats = juce::jmax(1.0, std::round(loopLengthBeats));
        }
        else if (pad.loopLengthBeats <= 0 && pad.sourceBpm > 1.0 && pad.sourceSampleRate > 1.0)
        {
            const double sampleSeconds = (double)length / pad.sourceSampleRate;
            loopLengthBeats = juce::jmax(1.0, sampleSeconds * pad.sourceBpm / 60.0);
            loopLengthBeats = juce::jmax(1.0, std::round(loopLengthBeats));
        }

        // Account for playback speed (half speed doubles, double speed halves)
        const auto padSpeed = sanitizeSamplePadPlaybackSpeed(pad.playbackSpeed.load(std::memory_order_relaxed));
        if (padSpeed == SamplePadPlaybackSpeed::half)
            loopLengthBeats *= 2.0;
        else if (padSpeed == SamplePadPlaybackSpeed::doubleSpeed)
            loopLengthBeats *= 0.5;
        loopLengthBeats = juce::jmax(1.0, std::round(loopLengthBeats));

        const double anchor = pad.loopAnchorBeat > 0.0 ? pad.loopAnchorBeat : 0.0;
        double loops = std::ceil((currentBeat - anchor) / loopLengthBeats);
        if (!std::isfinite(loops) || loops < 0.0)
            loops = 0.0;
        double candidate = anchor + loops * loopLengthBeats;
        if (candidate <= currentBeat + 0.0001)
            candidate += loopLengthBeats;
        candidate = std::round(candidate);
        pad.scheduledStartBeat = candidate;
        pad.scheduledPlaybackRouteToLocal = routeNewVoiceToLocal;
        pad.playbackScheduled.store(true, std::memory_order_relaxed);
        return;
    }

    const bool reverse = pad.reverse.load(std::memory_order_relaxed);
    pad.position.store(reverse ? juce::jmax(0.0, (double)length - 1.0) : 0.0, std::memory_order_relaxed);
    pad.mainVoiceRouteToLocal.store(routeNewVoiceToLocal, std::memory_order_relaxed);
    pad.playing.store(true, std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::stopSamplePad(int padIndex)
{
    if (!isValidSamplePadIndex(padIndex))
        return;

    const juce::ScopedLock lock(samplePadsLock);
    auto& pad = samplePads[(size_t)padIndex];
    pad.playing.store(false, std::memory_order_relaxed);
    pad.playbackScheduled.store(false, std::memory_order_relaxed);
    for (auto& voice : pad.oneShotVoices)
    {
        voice.active = false;
        voice.position = 0.0;
    }
    pad.nextOneShotVoice = 0;
    pad.activeOneShotVoices.store(0, std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setSamplePadRecordArmed(int padIndex, bool shouldArm)
{
    if (!isValidSamplePadIndex(padIndex) || (!isSamplePadsFeatureEnabled() && shouldArm))
        return;

    const juce::ScopedLock lock(samplePadsLock);
    auto& pad = samplePads[(size_t)padIndex];
    pad.recordArmed.store(shouldArm, std::memory_order_relaxed);
    if (!shouldArm)
    {
        pad.recordPendingStart.store(false, std::memory_order_relaxed);
        pad.recordStartScheduled.store(false, std::memory_order_relaxed);
        pad.recordScheduledCountdownBeats = 0.0;
        pad.recordLoopLengthBeatsOverride = 0;
        pad.recordAutoStopAtScheduledEnd = false;
        pad.recordMatchBpiCanvas = false;
        pad.midiHoldActive = false;
        pad.midiHoldActionTriggered = false;
        pad.midiPadDown = false;
        pad.midiHoldStartMs = 0.0;
        if (pad.recording.load(std::memory_order_relaxed))
            pad.recordPendingStop.store(true, std::memory_order_relaxed);
    }
}

void NinjamVst3AudioProcessor::armSamplePadLooper(int padIndex, bool matchBpi)
{
    if (!isSamplePadsFeatureEnabled() || !isValidSamplePadIndex(padIndex))
        return;

    const juce::ScopedLock lock(samplePadsLock);
    auto& pad = samplePads[(size_t)padIndex];
    if (pad.recording.load(std::memory_order_relaxed))
        return;

    pad.matchBpi.store(matchBpi, std::memory_order_relaxed);
    pad.loop.store(true, std::memory_order_relaxed);
    pad.recordArmed.store(true, std::memory_order_relaxed);
    pad.recordPendingStart.store(false, std::memory_order_relaxed);
    pad.recordPendingStop.store(false, std::memory_order_relaxed);
    pad.recordScheduledStartBeat = 0.0;
    pad.recordScheduledStopBeat = 0.0;
    pad.recordLoopLengthBeatsOverride = 0;
    pad.recordAutoStopAtScheduledEnd = false;
    pad.recordMatchBpiCanvas = false;
    pad.midiHoldActive = false;
    pad.midiHoldActionTriggered = false;
    pad.midiPadDown = false;
    pad.midiHoldStartMs = 0.0;
    pad.recordStartScheduled.store(false, std::memory_order_relaxed);
    for (auto& voice : pad.oneShotVoices)
    {
        voice.active = false;
        voice.position = 0.0;
    }
    pad.nextOneShotVoice = 0;
    pad.activeOneShotVoices.store(0, std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::scheduleSamplePadBpiRecordStartAtNextInterval(int padIndex)
{
    if (!isSamplePadsFeatureEnabled() || !isValidSamplePadIndex(padIndex))
        return;

    const int bpi = juce::jmax(1, getBPI());
    const double currentBeat = (double)intervalIndex.load(std::memory_order_relaxed) * (double)bpi
        + (double)juce::jlimit(0.0f, 1.0f, getIntervalProgress()) * (double)bpi;
    const double candidate = nextSamplePadIntervalStartBeat(currentBeat, bpi);

    const juce::ScopedLock lock(samplePadsLock);
    auto& pad = samplePads[(size_t)padIndex];
    if (pad.recording.load(std::memory_order_relaxed))
        return;

    pad.matchBpi.store(true, std::memory_order_relaxed);
    pad.loop.store(true, std::memory_order_relaxed);
    pad.recordArmed.store(true, std::memory_order_relaxed);
    pad.recordPendingStart.store(false, std::memory_order_relaxed);
    pad.recordPendingStop.store(false, std::memory_order_relaxed);
    pad.recordScheduledStartBeat = candidate;
    pad.recordScheduledCountdownBeats = (double)bpi;
    pad.recordScheduledStopBeat = 0.0;
    pad.recordLoopLengthBeatsOverride = bpi;
    pad.recordAutoStopAtScheduledEnd = false;
    pad.recordMatchBpiCanvas = false;
    pad.midiHoldActive = false;
    pad.midiHoldActionTriggered = false;
    pad.midiPadDown = false;
    pad.midiHoldStartMs = 0.0;
    pad.recordStartScheduled.store(true, std::memory_order_relaxed);
    pad.playing.store(false, std::memory_order_relaxed);
    pad.playbackScheduled.store(false, std::memory_order_relaxed);
    for (auto& voice : pad.oneShotVoices)
    {
        voice.active = false;
        voice.position = 0.0;
    }
    pad.nextOneShotVoice = 0;
    pad.activeOneShotVoices.store(0, std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::isSamplePadRecordArmed(int padIndex) const
{
    return isValidSamplePadIndex(padIndex)
        && samplePads[(size_t)padIndex].recordArmed.load(std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::isSamplePadRecording(int padIndex) const
{
    return isValidSamplePadIndex(padIndex)
        && samplePads[(size_t)padIndex].recording.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setSamplePadMatchBpiEnabled(int padIndex, bool shouldEnable)
{
    if (!isValidSamplePadIndex(padIndex))
        return;

    samplePads[(size_t)padIndex].matchBpi.store(shouldEnable, std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::isSamplePadMatchBpiEnabled(int padIndex) const
{
    return isValidSamplePadIndex(padIndex)
        && samplePads[(size_t)padIndex].matchBpi.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setSamplePadBpmSyncEnabled(int padIndex, bool shouldEnable)
{
    if (!isValidSamplePadIndex(padIndex))
        return;

    samplePads[(size_t)padIndex].bpmSyncEnabled.store(shouldEnable, std::memory_order_relaxed);

    if (!shouldEnable)
    {
        // Restore the original unstretched sample so playback returns to native speed
        const juce::ScopedLock lock(samplePadsLock);
        auto& pad = samplePads[(size_t)padIndex];
        if (pad.originalSample.getNumSamples() > 0)
        {
            pad.sample = pad.originalSample;
            pad.sourceSampleRate = pad.originalSourceSampleRate;
            pad.lastSyncedTargetBpm = 0.0;
            pad.bpmSyncApplied = false;
            pad.playbackSpeed.store((int)SamplePadPlaybackSpeed::normal, std::memory_order_relaxed);
            pad.pendingPlaybackSpeed.store(-1, std::memory_order_relaxed);
        }
    }
    else if (isSamplePadsFeatureEnabled())
        resyncSamplePadToBpm(padIndex, (double)getBPM(), false);
}

bool NinjamVst3AudioProcessor::isSamplePadBpmSyncEnabled(int padIndex) const
{
    return isValidSamplePadIndex(padIndex)
        && samplePads[(size_t)padIndex].bpmSyncEnabled.load(std::memory_order_relaxed);
}

double NinjamVst3AudioProcessor::getSamplePadSourceBpm(int padIndex) const
{
    if (!isValidSamplePadIndex(padIndex))
        return 0.0;
    return samplePads[(size_t)padIndex].sourceBpm;
}

void NinjamVst3AudioProcessor::setSamplePadSourceBpm(int padIndex, double newBpm)
{
    if (!isValidSamplePadIndex(padIndex) || newBpm <= 1.0 || !std::isfinite(newBpm))
        return;

    {
        const juce::ScopedLock lock(samplePadsLock);
        auto& pad = samplePads[(size_t)padIndex];
        if (pad.originalSample.getNumSamples() <= 0)
            return;

        // Recalculate loop length in beats from the new BPM and the original sample duration
        const double durationSeconds = (double)pad.originalSample.getNumSamples() / pad.originalSourceSampleRate;
        double correctedBpm = newBpm;
        int correctedLoopBeats = 0;
        correctTempoOctave(newBpm, durationSeconds, correctedBpm, correctedLoopBeats);

        pad.sourceBpm = correctedBpm;
        pad.rawSourceBpm = correctedBpm;
        pad.loopLengthBeats = correctedLoopBeats;
        pad.bpmSyncEnabled.store(true, std::memory_order_relaxed);
        pad.bpmSyncApplied = false;
        pad.lastSyncedTargetBpm = 0.0;
    }

    // Trigger a resync to the current NINJAM BPM with the corrected values
    resyncSamplePadToBpm(padIndex, (double)getBPM(), true);
}

void NinjamVst3AudioProcessor::setSamplePadPlaybackSpeed(int padIndex, SamplePadPlaybackSpeed speed)
{
    if (!isValidSamplePadIndex(padIndex))
        return;

    speed = sanitizeSamplePadPlaybackSpeed((int)speed);
    {
        const juce::ScopedLock lock(samplePadsLock);
        auto& pad = samplePads[(size_t)padIndex];

        // If the pad is currently playing a loop, defer the speed change until
        // the loop wraps around to avoid a jarring mid-loop pitch/tempo jump.
        if (pad.loop.load(std::memory_order_relaxed)
            && pad.playing.load(std::memory_order_relaxed)
            && !pad.recordedLoop)
        {
            pad.pendingPlaybackSpeed.store((int)speed, std::memory_order_relaxed);
            return;
        }

        pad.playbackSpeed.store((int)speed, std::memory_order_relaxed);
        pad.pendingPlaybackSpeed.store(-1, std::memory_order_relaxed);
        pad.bpmSyncEnabled.store(true, std::memory_order_relaxed);
        pad.bpmSyncApplied = false;
        pad.lastSyncedTargetBpm = 0.0;
    }
    if (isSamplePadsFeatureEnabled())
        resyncSamplePadToBpm(padIndex, (double)getBPM(), true);
}

NinjamVst3AudioProcessor::SamplePadPlaybackSpeed NinjamVst3AudioProcessor::getSamplePadPlaybackSpeed(int padIndex) const
{
    if (!isValidSamplePadIndex(padIndex))
        return SamplePadPlaybackSpeed::normal;

    return sanitizeSamplePadPlaybackSpeed(samplePads[(size_t)padIndex].playbackSpeed.load(std::memory_order_relaxed));
}

void NinjamVst3AudioProcessor::resyncSamplePadToNinjamBpm(int padIndex)
{
    enqueueSamplePadResyncJob(padIndex, (double)getBPM(), true);
}

void NinjamVst3AudioProcessor::requestSamplePadResyncToNinjamBpm(int padIndex, bool force)
{
    enqueueSamplePadResyncJob(padIndex, (double)getBPM(), force);
}

void NinjamVst3AudioProcessor::syncSamplePadLoopToBeat(int padIndex)
{
    if (!isSamplePadsFeatureEnabled() || !isValidSamplePadIndex(padIndex))
        return;

    const double targetBpm = juce::jmax(1.0, (double)getBPM());
    juce::AudioBuffer<float> original;
    double sourceRate = 44100.0;

    {
        const juce::ScopedLock lock(samplePadsLock);
        auto& pad = samplePads[(size_t)padIndex];
        if (pad.sample.getNumSamples() <= 0 || pad.recording.load(std::memory_order_relaxed))
            return;

        if (pad.originalSample.getNumSamples() <= 0)
        {
            pad.originalSample = pad.sample;
            pad.originalSourceSampleRate = pad.sourceSampleRate;
        }

        original = pad.originalSample;
        sourceRate = pad.originalSourceSampleRate > 1.0 ? pad.originalSourceSampleRate : pad.sourceSampleRate;
    }

    if (original.getNumSamples() <= 0 || sourceRate <= 1.0 || targetBpm <= 1.0)
        return;

    const double durationSeconds = (double)original.getNumSamples() / sourceRate;
    if (durationSeconds <= 0.0 || !std::isfinite(durationSeconds))
        return;

    const double capturedBeatsAtCurrentTempo = durationSeconds * targetBpm / 60.0;
    const int loopBeats = quantiseSamplePadFreeLoopBeats(capturedBeatsAtCurrentTempo);
    const double inferredSourceBpm = 60.0 * (double)loopBeats / durationSeconds;
    if (inferredSourceBpm <= 1.0 || !std::isfinite(inferredSourceBpm))
        return;

    {
        const juce::ScopedLock lock(samplePadsLock);
        auto& pad = samplePads[(size_t)padIndex];
        if (pad.sample.getNumSamples() <= 0 || pad.recording.load(std::memory_order_relaxed))
            return;

        pad.sourceBpm = inferredSourceBpm;
        pad.loopLengthBeats = loopBeats;
        pad.loop.store(true, std::memory_order_relaxed);
        pad.bpmSyncEnabled.store(true, std::memory_order_relaxed);
        pad.bpmSyncApplied = false;
        pad.lastSyncedTargetBpm = 0.0;
    }

    enqueueSamplePadResyncJob(padIndex, targetBpm, true);
}

void NinjamVst3AudioProcessor::undoSamplePadBpmResync(int padIndex)
{
    if (!isValidSamplePadIndex(padIndex))
        return;

    const juce::ScopedLock lock(samplePadsLock);
    auto& pad = samplePads[(size_t)padIndex];
    if (pad.originalSample.getNumSamples() <= 0)
        return;

    pad.sample = pad.originalSample;
    pad.sourceSampleRate = pad.originalSourceSampleRate;
    pad.lastSyncedTargetBpm = 0.0;
    pad.bpmSyncApplied = false;
    pad.bpmSyncEnabled.store(false, std::memory_order_relaxed);
    for (auto& voice : pad.oneShotVoices)
    {
        voice.active = false;
        voice.position = 0.0;
    }
    pad.nextOneShotVoice = 0;
    pad.activeOneShotVoices.store(0, std::memory_order_relaxed);
    const int length = pad.sample.getNumSamples();
    if (length > 0)
        pad.position.store(juce::jlimit(0.0, (double)length - 1.0, pad.position.load(std::memory_order_relaxed)),
                           std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::canUndoSamplePadBpmResync(int padIndex) const
{
    if (!isValidSamplePadIndex(padIndex))
        return false;

    const juce::ScopedLock lock(samplePadsLock);
    const auto& pad = samplePads[(size_t)padIndex];
    return pad.bpmSyncApplied && pad.originalSample.getNumSamples() > 0;
}

void NinjamVst3AudioProcessor::setSamplePadLoopEnabled(int padIndex, bool shouldLoop)
{
    if (!isValidSamplePadIndex(padIndex))
        return;

    {
        const juce::ScopedLock lock(samplePadsLock);
        auto& pad = samplePads[(size_t)padIndex];
        pad.loop.store(shouldLoop, std::memory_order_relaxed);
        if (shouldLoop)
        {
            for (auto& voice : pad.oneShotVoices)
            {
                voice.active = false;
                voice.position = 0.0;
            }
            pad.nextOneShotVoice = 0;
            pad.activeOneShotVoices.store(0, std::memory_order_relaxed);
        }
    }
    if (shouldLoop && isSamplePadsFeatureEnabled())
        resyncSamplePadToBpm(padIndex, (double)getBPM(), false);
}

bool NinjamVst3AudioProcessor::isSamplePadLoopEnabled(int padIndex) const
{
    return isValidSamplePadIndex(padIndex)
        && samplePads[(size_t)padIndex].loop.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setSamplePadReverseEnabled(int padIndex, bool shouldReverse)
{
    if (!isValidSamplePadIndex(padIndex))
        return;

    auto& pad = samplePads[(size_t)padIndex];
    pad.reverse.store(shouldReverse, std::memory_order_relaxed);
    if (pad.playing.load(std::memory_order_relaxed))
    {
        const juce::ScopedLock lock(samplePadsLock);
        const int length = pad.sample.getNumSamples();
        if (length > 0)
            pad.position.store(shouldReverse ? (double)length - 1.0 : 0.0, std::memory_order_relaxed);
    }
}

bool NinjamVst3AudioProcessor::isSamplePadReverseEnabled(int padIndex) const
{
    return isValidSamplePadIndex(padIndex)
        && samplePads[(size_t)padIndex].reverse.load(std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::hasSamplePadSample(int padIndex) const
{
    if (!isValidSamplePadIndex(padIndex))
        return false;

    const juce::ScopedLock lock(samplePadsLock);
    return samplePads[(size_t)padIndex].sample.getNumSamples() > 0;
}

juce::String NinjamVst3AudioProcessor::getSamplePadName(int padIndex) const
{
    if (!isValidSamplePadIndex(padIndex))
        return {};

    const juce::ScopedLock lock(samplePadsLock);
    const auto& pad = samplePads[(size_t)padIndex];
    return pad.name.isNotEmpty() ? pad.name : getDefaultSamplePadName(padIndex);
}

void NinjamVst3AudioProcessor::setSamplePadName(int padIndex, const juce::String& name)
{
    if (!isValidSamplePadIndex(padIndex))
        return;

    const juce::ScopedLock lock(samplePadsLock);
    auto& pad = samplePads[(size_t)padIndex];
    const auto trimmed = name.trim();
    if (trimmed.isEmpty() || trimmed == getDefaultSamplePadName(padIndex))
    {
        pad.name.clear();
        pad.nameIsCustom = false;
    }
    else
    {
        pad.name = trimmed.substring(0, 32);
        pad.nameIsCustom = true;
    }
}

int NinjamVst3AudioProcessor::getSamplePadLoopLengthBeats(int padIndex) const
{
    if (!isValidSamplePadIndex(padIndex))
        return 0;

    const juce::ScopedLock lock(samplePadsLock);
    const auto& pad = samplePads[(size_t)padIndex];
    if (pad.sample.getNumSamples() <= 0 || !pad.loop.load(std::memory_order_relaxed))
        return 0;

    return juce::jmax(0, pad.loopLengthBeats);
}

float NinjamVst3AudioProcessor::getSamplePadLoopProgress(int padIndex) const
{
    if (!isValidSamplePadIndex(padIndex))
        return 0.0f;

    const juce::ScopedLock lock(samplePadsLock);
    const auto& pad = samplePads[(size_t)padIndex];
    const int length = pad.sample.getNumSamples();
    if (length <= 1 || !pad.playing.load(std::memory_order_relaxed))
        return 0.0f;

    const double denominator = juce::jmax(1.0, (double)length - 1.0);
    double progress = pad.position.load(std::memory_order_relaxed) / denominator;
    if (pad.reverse.load(std::memory_order_relaxed))
        progress = 1.0 - progress;

    return juce::jlimit(0.0f, 1.0f, (float)progress);
}

float NinjamVst3AudioProcessor::getSamplePadRecordStartCountdownProgress(int padIndex) const
{
    if (!isValidSamplePadIndex(padIndex) || !isSamplePadsFeatureEnabled())
        return 0.0f;

    const int bpi = juce::jmax(1, cachedNinjamBpi.load(std::memory_order_relaxed));
    const int transportPosition = cachedNinjamTransportPos.load(std::memory_order_relaxed);
    const int transportLength = cachedNinjamTransportLen.load(std::memory_order_relaxed);
    const double intervalProgress = transportLength > 0
        ? juce::jlimit(0.0, 1.0, (double)transportPosition / (double)transportLength)
        : 0.0;
    const double currentBeat = (double)intervalIndex.load(std::memory_order_relaxed) * (double)bpi
        + intervalProgress * (double)bpi;

    const juce::ScopedLock lock(samplePadsLock);
    const auto& pad = samplePads[(size_t)padIndex];
    if (!pad.recordStartScheduled.load(std::memory_order_relaxed))
        return 0.0f;

    const double countdownBeats = pad.recordScheduledCountdownBeats > 0.0
        ? pad.recordScheduledCountdownBeats
        : (pad.matchBpi.load(std::memory_order_relaxed) ? (double)bpi : samplePadRecordBarBeats);
    const double countdownStartBeat = pad.recordScheduledStartBeat - juce::jmax(0.0001, countdownBeats);
    return juce::jlimit(0.0f, 1.0f, (float)((currentBeat - countdownStartBeat) / juce::jmax(0.0001, countdownBeats)));
}

int NinjamVst3AudioProcessor::getSamplePadRecordStartCountdownBeats(int padIndex) const
{
    if (!isValidSamplePadIndex(padIndex) || !isSamplePadsFeatureEnabled())
        return 0;

    const int bpi = juce::jmax(1, cachedNinjamBpi.load(std::memory_order_relaxed));
    const juce::ScopedLock lock(samplePadsLock);
    const auto& pad = samplePads[(size_t)padIndex];
    if (!pad.recordStartScheduled.load(std::memory_order_relaxed))
        return 0;

    const double countdownBeats = pad.recordScheduledCountdownBeats > 0.0
        ? pad.recordScheduledCountdownBeats
        : (pad.matchBpi.load(std::memory_order_relaxed) ? (double)bpi : samplePadRecordBarBeats);
    return juce::jmax(1, (int)std::llround(countdownBeats));
}

int NinjamVst3AudioProcessor::getSamplePadTriggerFlashCounter(int padIndex) const
{
    if (!isValidSamplePadIndex(padIndex))
        return 0;

    return samplePads[(size_t)padIndex].triggerFlashCounter.load(std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::triggerSamplePadForMidiNote(int noteNumber)
{
    if (!isSamplePadsFeatureEnabled())
        return false;

    const int padIndex = samplePadIndexForMidiNoteNumber(noteNumber);

    if (!isValidSamplePadIndex(padIndex))
        return false;

    triggerSamplePad(padIndex);
    return true;
}

bool NinjamVst3AudioProcessor::handleSamplePadMidiNote(int noteNumber, bool isNoteOn)
{
    if (!isSamplePadsFeatureEnabled())
        return false;

    return handleSamplePadMidiPadState(samplePadIndexForMidiNoteNumber(noteNumber), isNoteOn);
}

bool NinjamVst3AudioProcessor::handleSamplePadMidiPadState(int padIndex, bool isDown)
{
    if (!isSamplePadsFeatureEnabled() || !isValidSamplePadIndex(padIndex))
        return false;

    if (!isDown)
    {
        bool shouldTrigger = false;
        bool shouldArmNormalLooper = false;
        {
            const juce::ScopedLock lock(samplePadsLock);
            auto& pad = samplePads[(size_t)padIndex];
            if (!pad.midiPadDown && !pad.midiHoldActive)
                return true;
            if (!pad.midiHoldActive)
            {
                pad.midiPadDown = false;
                return true;
            }

            const double heldMs = juce::Time::getMillisecondCounterHiRes() - pad.midiHoldStartMs;
            shouldArmNormalLooper = !pad.midiHoldActionTriggered && heldMs >= samplePadHoldScheduleMs;
            shouldTrigger = !pad.midiHoldActionTriggered && !shouldArmNormalLooper;
            pad.midiPadDown = false;
            pad.midiHoldActive = false;
            pad.midiHoldActionTriggered = false;
            pad.midiHoldStartMs = 0.0;
        }
        if (shouldArmNormalLooper)
            scheduleSamplePadBpiRecordStartAtNextInterval(padIndex);
        if (shouldTrigger)
            triggerSamplePad(padIndex);
        return true;
    }

    bool shouldTriggerImmediately = false;
    {
        const juce::ScopedLock lock(samplePadsLock);
        auto& pad = samplePads[(size_t)padIndex];
        if (pad.midiPadDown)
            return true;

        pad.midiPadDown = true;
        pad.triggerFlashCounter.fetch_add(1, std::memory_order_relaxed);

        if (pad.recording.load(std::memory_order_relaxed)
            || pad.recordStartScheduled.load(std::memory_order_relaxed))
        {
            shouldTriggerImmediately = true;
        }
        else
        {
            pad.midiHoldActive = true;
            pad.midiHoldActionTriggered = false;
            pad.midiHoldStartMs = juce::Time::getMillisecondCounterHiRes();
        }
    }

    if (shouldTriggerImmediately)
        triggerSamplePad(padIndex);

    return true;
}

void NinjamVst3AudioProcessor::setSamplePadVolume(float gain)
{
    samplePadsVolume.store(juce::jlimit(0.0f, 2.0f, gain), std::memory_order_relaxed);
}

float NinjamVst3AudioProcessor::getSamplePadVolume() const
{
    return samplePadsVolume.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setSamplePadLimiterEnabled(bool shouldEnable)
{
    samplePadsLimiterEnabled.store(shouldEnable, std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::isSamplePadLimiterEnabled() const
{
    return samplePadsLimiterEnabled.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setSamplePadDuckEnabled(bool shouldEnable)
{
    samplePadsDuckEnabled.store(shouldEnable, std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::isSamplePadDuckEnabled() const
{
    return samplePadsDuckEnabled.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setSamplePadDuckShape(SamplePadDuckShape shape)
{
    samplePadsDuckShape.store((int)sanitizeSamplePadDuckShape((int)shape), std::memory_order_relaxed);
}

NinjamVst3AudioProcessor::SamplePadDuckShape NinjamVst3AudioProcessor::getSamplePadDuckShape() const
{
    return sanitizeSamplePadDuckShape(samplePadsDuckShape.load(std::memory_order_relaxed));
}

void NinjamVst3AudioProcessor::setSamplePadDuckLength(SamplePadDuckLength length)
{
    samplePadsDuckLength.store((int)sanitizeSamplePadDuckLength((int)length), std::memory_order_relaxed);
}

NinjamVst3AudioProcessor::SamplePadDuckLength NinjamVst3AudioProcessor::getSamplePadDuckLength() const
{
    return sanitizeSamplePadDuckLength(samplePadsDuckLength.load(std::memory_order_relaxed));
}

void NinjamVst3AudioProcessor::setSamplePadsUseDefaultFx(bool shouldUse)
{
    samplePadsUseDefaultFx.store(shouldUse, std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::getSamplePadsUseDefaultFx() const
{
    return samplePadsUseDefaultFx.load(std::memory_order_relaxed);
}
void NinjamVst3AudioProcessor::setSamplePadMonitorModeEnabled(bool shouldEnable)
{
    samplePadMonitorModeEnabled.store(shouldEnable, std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::isSamplePadMonitorModeEnabled() const
{
    return samplePadMonitorModeEnabled.load(std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::isSamplePadFxSlotInUseForLocalRoute(int slotIndex) const
{
    if (!isValidSamplePadFxSlot(slotIndex))
        return false;

    const juce::ScopedLock lock(samplePadsLock);
    for (const auto& pad : samplePads)
    {
        const bool localMainActive =
            (pad.playing.load(std::memory_order_relaxed)
                && pad.mainVoiceRouteToLocal.load(std::memory_order_relaxed))
            || (pad.playbackScheduled.load(std::memory_order_relaxed)
                && pad.scheduledPlaybackRouteToLocal);

        bool localOneShotActive = false;
        for (const auto& voice : pad.oneShotVoices)
        {
            if (voice.active && voice.routeToLocal)
            {
                localOneShotActive = true;
                break;
            }
        }

        if ((localMainActive || localOneShotActive)
            && pad.fxSlotRoutes[(size_t)slotIndex].load(std::memory_order_relaxed))
        {
            return true;
        }
    }

    return false;
}

void NinjamVst3AudioProcessor::setSamplePadDuckRouteEnabled(int padIndex, bool shouldEnable)
{
    if (!isValidSamplePadIndex(padIndex))
        return;

    samplePads[(size_t)padIndex].duckRoute.store(shouldEnable, std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::isSamplePadDuckRouteEnabled(int padIndex) const
{
    if (!isValidSamplePadIndex(padIndex))
        return false;

    return samplePads[(size_t)padIndex].duckRoute.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setSamplePadFxSlotRouteEnabled(int padIndex, int slotIndex, bool shouldEnable)
{
    if (!isValidSamplePadIndex(padIndex) || !isValidSamplePadFxSlot(slotIndex))
        return;

    samplePads[(size_t)padIndex].fxSlotRoutes[(size_t)slotIndex].store(shouldEnable,
                                                                       std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::isSamplePadFxSlotRouteEnabled(int padIndex, int slotIndex) const
{
    if (!isValidSamplePadIndex(padIndex) || !isValidSamplePadFxSlot(slotIndex))
        return false;

    return samplePads[(size_t)padIndex].fxSlotRoutes[(size_t)slotIndex].load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setSamplePadFxSlotToSlotRouteEnabled(int sourceSlotIndex,
                                                                     int targetSlotIndex,
                                                                     bool shouldEnable)
{
    if (!isValidSamplePadFxSlot(sourceSlotIndex) || !isValidSamplePadFxSlot(targetSlotIndex))
        return;

    if (shouldEnable && !canRouteSamplePadFxSlotToSlot(sourceSlotIndex, targetSlotIndex))
        return;

    samplePadFxSlotChainRoutes[(size_t)sourceSlotIndex][(size_t)targetSlotIndex].store(shouldEnable,
                                                                                       std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::isSamplePadFxSlotToSlotRouteEnabled(int sourceSlotIndex, int targetSlotIndex) const
{
    if (!isValidSamplePadFxSlot(sourceSlotIndex) || !isValidSamplePadFxSlot(targetSlotIndex))
        return false;

    return samplePadFxSlotChainRoutes[(size_t)sourceSlotIndex][(size_t)targetSlotIndex].load(std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::canRouteSamplePadFxSlotToSlot(int sourceSlotIndex, int targetSlotIndex) const
{
    if (!isValidSamplePadFxSlot(sourceSlotIndex)
        || !isValidSamplePadFxSlot(targetSlotIndex)
        || sourceSlotIndex == targetSlotIndex)
        return false;

    if (isSamplePadFxSlotToSlotRouteEnabled(sourceSlotIndex, targetSlotIndex))
        return true;

    std::array<bool, numSamplePadFxSlots> visited {};
    std::array<int, numSamplePadFxSlots> stack {};
    int stackSize = 0;
    visited[(size_t)sourceSlotIndex] = true;
    stack[(size_t)stackSize++] = sourceSlotIndex;

    while (stackSize > 0)
    {
        const int slot = stack[(size_t)--stackSize];
        if (slot == targetSlotIndex)
            return false;

        for (int other = 0; other < numSamplePadFxSlots; ++other)
        {
            if (visited[(size_t)other])
                continue;

            const bool connected = samplePadFxSlotChainRoutes[(size_t)slot][(size_t)other].load(std::memory_order_relaxed)
                || samplePadFxSlotChainRoutes[(size_t)other][(size_t)slot].load(std::memory_order_relaxed);
            if (!connected)
                continue;

            visited[(size_t)other] = true;
            stack[(size_t)stackSize++] = other;
        }
    }

    return true;
}

float NinjamVst3AudioProcessor::getSamplePadPeak() const
{
    return samplePadsPeak.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setSamplePadFxSlotType(int slotIndex, SamplePadFxType type)
{
    if (!isValidSamplePadFxSlot(slotIndex))
        return;

    samplePadFxSlotTypes[(size_t)slotIndex].store((int)sanitizeSamplePadFxType((int)type),
                                                  std::memory_order_relaxed);
}

NinjamVst3AudioProcessor::SamplePadFxType NinjamVst3AudioProcessor::getSamplePadFxSlotType(int slotIndex) const
{
    if (!isValidSamplePadFxSlot(slotIndex))
        return SamplePadFxType::reverb;

    return sanitizeSamplePadFxType(samplePadFxSlotTypes[(size_t)slotIndex].load(std::memory_order_relaxed));
}

void NinjamVst3AudioProcessor::setSamplePadFxSlotAmount(int slotIndex, float amount)
{
    if (!isValidSamplePadFxSlot(slotIndex))
        return;

    samplePadFxSlotAmounts[(size_t)slotIndex].store(juce::jlimit(0.0f, 1.0f, amount),
                                                    std::memory_order_relaxed);
}

float NinjamVst3AudioProcessor::getSamplePadFxSlotAmount(int slotIndex) const
{
    if (!isValidSamplePadFxSlot(slotIndex))
        return 0.0f;

    return samplePadFxSlotAmounts[(size_t)slotIndex].load(std::memory_order_relaxed);
}

juce::File NinjamVst3AudioProcessor::getSamplePadBanksDirectory() const
{
    auto dir = getNinjamplusSettingsDirectory().getChildFile("samples");
    dir.createDirectory();
    return dir;
}

juce::File NinjamVst3AudioProcessor::getSamplePadBankDirectory(const juce::String& bankName) const
{
    const auto safeName = sanitiseSamplePadBankName(bankName);
    if (safeName.isEmpty())
        return {};

    return getSamplePadBanksDirectory().getChildFile(safeName);
}

juce::StringArray NinjamVst3AudioProcessor::getSamplePadBankNames() const
{
    juce::StringArray names;
    const auto banksDir = getSamplePadBanksDirectory();
    const auto dirs = banksDir.findChildFiles(juce::File::findDirectories, false);
    for (const auto& dir : dirs)
        if (dir.getChildFile("pad.cfg").existsAsFile())
            names.add(dir.getFileName());

    names.sort(true);
    return names;
}

bool NinjamVst3AudioProcessor::saveSamplePadBank(const juce::String& bankName, juce::String& errorMessage)
{
    errorMessage.clear();
    const auto safeName = sanitiseSamplePadBankName(bankName);
    if (safeName.isEmpty())
    {
        errorMessage = "Enter a bank name.";
        return false;
    }

    const auto bankDir = getSamplePadBankDirectory(safeName);
    if (!bankDir.createDirectory())
    {
        errorMessage = "Could not create bank folder:\n" + bankDir.getFullPathName();
        return false;
    }

    struct PadSnapshot
    {
        bool hasSample = false;
        juce::AudioBuffer<float> sample;
        juce::String name;
        bool nameIsCustom = false;
        bool loop = false;
        bool reverse = false;
        bool matchBpi = false;
        bool bpmSyncEnabled = true;
        SamplePadPlaybackSpeed playbackSpeed = SamplePadPlaybackSpeed::normal;
        bool duckRoute = false;
        std::array<bool, numSamplePadFxSlots> fxSlotRoutes {};
        bool recordedLoop = false;
        bool bpmSyncApplied = false;
        int loopLengthBeats = 0;
        double sourceSampleRate = 44100.0;
        double sourceBpm = 0.0;
        double lastSyncedTargetBpm = 0.0;
    };

    std::array<PadSnapshot, numSamplePads> snapshots;
    {
        const juce::ScopedLock lock(samplePadsLock);
        for (int pad = 0; pad < numSamplePads; ++pad)
        {
            const auto& source = samplePads[(size_t)pad];
            auto& snapshot = snapshots[(size_t)pad];
            snapshot.hasSample = source.sample.getNumSamples() > 0;
            if (snapshot.hasSample)
                snapshot.sample = source.sample;
            snapshot.name = source.name;
            snapshot.nameIsCustom = source.nameIsCustom;
            snapshot.loop = source.loop.load(std::memory_order_relaxed);
            snapshot.reverse = source.reverse.load(std::memory_order_relaxed);
            snapshot.matchBpi = source.matchBpi.load(std::memory_order_relaxed);
            snapshot.bpmSyncEnabled = source.bpmSyncEnabled.load(std::memory_order_relaxed);
            snapshot.playbackSpeed = sanitizeSamplePadPlaybackSpeed(source.playbackSpeed.load(std::memory_order_relaxed));
            snapshot.duckRoute = source.duckRoute.load(std::memory_order_relaxed);
            for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
                snapshot.fxSlotRoutes[(size_t)slot] = source.fxSlotRoutes[(size_t)slot].load(std::memory_order_relaxed);
            snapshot.recordedLoop = source.recordedLoop;
            snapshot.bpmSyncApplied = source.bpmSyncApplied;
            snapshot.loopLengthBeats = source.loopLengthBeats;
            snapshot.sourceSampleRate = source.sourceSampleRate > 1.0 ? source.sourceSampleRate : 44100.0;
            snapshot.sourceBpm = source.sourceBpm;
            snapshot.lastSyncedTargetBpm = source.lastSyncedTargetBpm;
        }
    }

    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("version", 1);
    root->setProperty("name", safeName);
    root->setProperty("savedAt", juce::Time::getCurrentTime().toISO8601(true));
    root->setProperty("duck", isSamplePadDuckEnabled());
    root->setProperty("duckShape", (int)getSamplePadDuckShape());
    root->setProperty("duckLength", (int)getSamplePadDuckLength());
    root->setProperty("useDefaultFx", getSamplePadsUseDefaultFx());

    juce::Array<juce::var> fxSlotArray;
    for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
    {
        juce::DynamicObject::Ptr slotObj = new juce::DynamicObject();
        slotObj->setProperty("index", slot);
        slotObj->setProperty("type", (int)getSamplePadFxSlotType(slot));
        slotObj->setProperty("amount", (double)getSamplePadFxSlotAmount(slot));
        fxSlotArray.add(juce::var(slotObj.get()));
    }
    root->setProperty("fxSlots", juce::var(fxSlotArray));

    juce::Array<juce::var> fxChainRouteArray;
    for (int sourceSlot = 0; sourceSlot < numSamplePadFxSlots; ++sourceSlot)
    {
        for (int targetSlot = 0; targetSlot < numSamplePadFxSlots; ++targetSlot)
        {
            if (!isSamplePadFxSlotToSlotRouteEnabled(sourceSlot, targetSlot))
                continue;

            juce::DynamicObject::Ptr routeObj = new juce::DynamicObject();
            routeObj->setProperty("source", sourceSlot);
            routeObj->setProperty("target", targetSlot);
            fxChainRouteArray.add(juce::var(routeObj.get()));
        }
    }
    root->setProperty("fxChainRoutes", juce::var(fxChainRouteArray));

    juce::Array<juce::var> padArray;
    for (int pad = 0; pad < numSamplePads; ++pad)
    {
        const auto& snapshot = snapshots[(size_t)pad];
        juce::DynamicObject::Ptr padObj = new juce::DynamicObject();
        padObj->setProperty("index", pad);
        padObj->setProperty("hasSample", snapshot.hasSample);
        padObj->setProperty("name", snapshot.name);
        padObj->setProperty("nameIsCustom", snapshot.nameIsCustom);
        padObj->setProperty("loop", snapshot.loop);
        padObj->setProperty("reverse", snapshot.reverse);
        padObj->setProperty("matchBpi", snapshot.matchBpi);
        padObj->setProperty("bpmSyncEnabled", snapshot.bpmSyncEnabled);
        padObj->setProperty("playbackSpeed", (int)snapshot.playbackSpeed);
        padObj->setProperty("duckRoute", snapshot.duckRoute);
        for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
            padObj->setProperty("fxSlotRoute" + juce::String(slot), snapshot.fxSlotRoutes[(size_t)slot]);
        padObj->setProperty("recordedLoop", snapshot.recordedLoop);
        padObj->setProperty("bpmSyncApplied", snapshot.bpmSyncApplied);
        padObj->setProperty("loopLengthBeats", snapshot.loopLengthBeats);
        padObj->setProperty("sourceSampleRate", snapshot.sourceSampleRate);

        double sourceBpm = snapshot.sourceBpm;
        if (snapshot.loopLengthBeats > 0
            && snapshot.sample.getNumSamples() > 0
            && snapshot.sourceSampleRate > 1.0)
        {
            const double durationSeconds = (double)snapshot.sample.getNumSamples() / snapshot.sourceSampleRate;
            if (durationSeconds > 0.0)
                sourceBpm = 60.0 * (double)snapshot.loopLengthBeats / durationSeconds;
        }
        else if (snapshot.bpmSyncApplied && snapshot.lastSyncedTargetBpm > 1.0)
        {
            sourceBpm = snapshot.lastSyncedTargetBpm;
        }
        padObj->setProperty("sourceBpm", sourceBpm);

        if (snapshot.hasSample)
        {
            const juce::String sampleName = "pad" + juce::String(pad + 1).paddedLeft('0', 2) + ".wav";
            const auto sampleFile = bankDir.getChildFile(sampleName);
            if (!writeSamplePadWavFile(sampleFile, snapshot.sample, snapshot.sourceSampleRate))
            {
                errorMessage = "Could not write sample:\n" + sampleFile.getFullPathName();
                return false;
            }
            padObj->setProperty("sample", sampleName);
        }

        padArray.add(juce::var(padObj.get()));
    }

    root->setProperty("pads", juce::var(padArray));

    const auto cfgFile = bankDir.getChildFile("pad.cfg");
    if (!cfgFile.replaceWithText(juce::JSON::toString(juce::var(root.get()), true)))
    {
        errorMessage = "Could not write bank config:\n" + cfgFile.getFullPathName();
        return false;
    }

    return true;
}

void NinjamVst3AudioProcessor::saveSamplePadBankAsync(const juce::String& bankName,
                                                      std::function<void(bool, const juce::String&, const juce::String&)> completion)
{
    if (!samplePadBackgroundAlive)
    {
        if (completion)
            completion(false, "Sampler background worker unavailable.", {});
        return;
    }

    const juce::String safeName = sanitiseSamplePadBankName(bankName);
    if (safeName.isEmpty())
    {
        if (completion)
            completion(false, "Enter a bank name.", {});
        return;
    }

    const juce::uint64 requestSerial = samplePadBankSaveRequestSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto alive = samplePadBackgroundAlive;

    samplePadBackgroundPool.addJob(new SamplePadBackgroundJob("NINJAMSamplePadBankSave",
        [this, alive, bankName, safeName, requestSerial, completion = std::move(completion)]() mutable
        {
            if (!alive->load(std::memory_order_acquire)
                || samplePadBankSaveRequestSerial.load(std::memory_order_acquire) != requestSerial)
            {
                return juce::ThreadPoolJob::jobHasFinished;
            }

            juce::String error;
            const bool saved = saveSamplePadBank(bankName, error);

            juce::MessageManager::callAsync(
                [alive, requestSerial, saved, error, safeName, completion = std::move(completion), this]() mutable
                {
                    if (!alive->load(std::memory_order_acquire)
                        || samplePadBankSaveRequestSerial.load(std::memory_order_acquire) != requestSerial)
                    {
                        return;
                    }

                    if (completion)
                        completion(saved, error, saved ? safeName : juce::String());
                });

            return juce::ThreadPoolJob::jobHasFinished;
        }), true);
}

bool NinjamVst3AudioProcessor::loadSamplePadBank(const juce::File& bankDirectory, juce::String& errorMessage)
{
    errorMessage.clear();
    if (!bankDirectory.isDirectory())
    {
        errorMessage = "Choose a sample bank folder.";
        return false;
    }

    const auto cfgFile = bankDirectory.getChildFile("pad.cfg");
    if (!cfgFile.existsAsFile())
    {
        errorMessage = "No pad.cfg found in:\n" + bankDirectory.getFullPathName();
        return false;
    }

    juce::var rootVar;
    const auto parseResult = juce::JSON::parse(cfgFile.loadFileAsString(), rootVar);
    if (parseResult.failed())
    {
        errorMessage = "Could not parse pad.cfg:\n" + parseResult.getErrorMessage();
        return false;
    }

    auto* rootObj = rootVar.getDynamicObject();
    if (rootObj == nullptr)
    {
        errorMessage = "pad.cfg is not a bank config.";
        return false;
    }

    auto* pads = rootObj->getProperty("pads").getArray();
    if (pads == nullptr)
    {
        errorMessage = "pad.cfg does not contain pad data.";
        return false;
    }

    setSamplePadDuckEnabled(rootObj->hasProperty("duck") ? (bool)rootObj->getProperty("duck") : false);
    setSamplePadDuckShape(rootObj->hasProperty("duckShape")
        ? sanitizeSamplePadDuckShape((int)rootObj->getProperty("duckShape"))
        : SamplePadDuckShape::smoothPump);
    setSamplePadDuckLength(rootObj->hasProperty("duckLength")
        ? sanitizeSamplePadDuckLength((int)rootObj->getProperty("duckLength"))
        : SamplePadDuckLength::quarter);
    setSamplePadsUseDefaultFx(rootObj->hasProperty("useDefaultFx") ? (bool)rootObj->getProperty("useDefaultFx") : true);
    for (int sourceSlot = 0; sourceSlot < numSamplePadFxSlots; ++sourceSlot)
        for (int targetSlot = 0; targetSlot < numSamplePadFxSlots; ++targetSlot)
            setSamplePadFxSlotToSlotRouteEnabled(sourceSlot, targetSlot, false);

    if (auto* fxSlots = rootObj->getProperty("fxSlots").getArray())
    {
        for (const auto& slotVar : *fxSlots)
        {
            auto* slotObj = slotVar.getDynamicObject();
            if (slotObj == nullptr)
                continue;

            const int slotIndex = (int)slotObj->getProperty("index");
            if (!isValidSamplePadFxSlot(slotIndex))
                continue;

            setSamplePadFxSlotType(slotIndex,
                                   sanitizeSamplePadFxType(slotObj->hasProperty("type")
                                       ? (int)slotObj->getProperty("type")
                                       : 0));
            setSamplePadFxSlotAmount(slotIndex,
                                     juce::jlimit(0.0f, 1.0f, slotObj->hasProperty("amount")
                                         ? (float)(double)slotObj->getProperty("amount")
                                         : 0.0f));
        }
    }

    if (auto* fxChainRoutes = rootObj->getProperty("fxChainRoutes").getArray())
    {
        for (const auto& routeVar : *fxChainRoutes)
        {
            auto* routeObj = routeVar.getDynamicObject();
            if (routeObj == nullptr)
                continue;

            const int sourceSlot = routeObj->hasProperty("source") ? (int)routeObj->getProperty("source") : -1;
            const int targetSlot = routeObj->hasProperty("target") ? (int)routeObj->getProperty("target") : -1;
            setSamplePadFxSlotToSlotRouteEnabled(sourceSlot, targetSlot, true);
        }
    }

    for (int pad = 0; pad < numSamplePads; ++pad)
        clearSamplePad(pad);

    for (const auto& padVar : *pads)
    {
        auto* padObj = padVar.getDynamicObject();
        if (padObj == nullptr)
            continue;

        const int padIndex = (int)padObj->getProperty("index");
        if (!isValidSamplePadIndex(padIndex))
            continue;

        const bool hasSample = (bool)padObj->getProperty("hasSample");
        const juce::String sampleName = padObj->getProperty("sample").toString();
        if (hasSample && sampleName.isNotEmpty())
        {
            const auto sampleFile = bankDirectory.getChildFile(sampleName);
            if (!loadSamplePad(padIndex, sampleFile))
            {
                errorMessage = "Could not load sample:\n" + sampleFile.getFullPathName();
                return false;
            }
        }

        const juce::ScopedLock lock(samplePadsLock);
        auto& pad = samplePads[(size_t)padIndex];
        pad.name = padObj->getProperty("name").toString();
        pad.nameIsCustom = (bool)padObj->getProperty("nameIsCustom");
        pad.loop.store((bool)padObj->getProperty("loop"), std::memory_order_relaxed);
        pad.reverse.store((bool)padObj->getProperty("reverse"), std::memory_order_relaxed);
        pad.matchBpi.store((bool)padObj->getProperty("matchBpi"), std::memory_order_relaxed);
        pad.bpmSyncEnabled.store((bool)padObj->getProperty("bpmSyncEnabled"), std::memory_order_relaxed);
        pad.playbackSpeed.store((int)sanitizeSamplePadPlaybackSpeed(padObj->hasProperty("playbackSpeed")
                                  ? (int)padObj->getProperty("playbackSpeed")
                                  : (int)SamplePadPlaybackSpeed::normal),
                                std::memory_order_relaxed);
        pad.duckRoute.store(padObj->hasProperty("duckRoute") ? (bool)padObj->getProperty("duckRoute") : false,
                            std::memory_order_relaxed);
        for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
        {
            pad.fxSlotRoutes[(size_t)slot].store(padObj->hasProperty("fxSlotRoute" + juce::String(slot))
                                                     ? (bool)padObj->getProperty("fxSlotRoute" + juce::String(slot))
                                                     : false,
                                                 std::memory_order_relaxed);
        }
        pad.recordedLoop = (bool)padObj->getProperty("recordedLoop");
        pad.bpmSyncApplied = false;
        pad.loopLengthBeats = (int)padObj->getProperty("loopLengthBeats");
        const double sourceBpm = (double)padObj->getProperty("sourceBpm");
        if (sourceBpm > 1.0 && std::isfinite(sourceBpm))
            pad.sourceBpm = sourceBpm;
        pad.lastSyncedTargetBpm = 0.0;
        pad.originalSample = pad.sample;
        pad.originalSourceSampleRate = pad.sourceSampleRate;
        pad.playing.store(false, std::memory_order_relaxed);
        pad.playbackScheduled.store(false, std::memory_order_relaxed);
        for (auto& voice : pad.oneShotVoices)
        {
            voice.active = false;
            voice.position = 0.0;
        }
        pad.nextOneShotVoice = 0;
        pad.activeOneShotVoices.store(0, std::memory_order_relaxed);
        pad.recordArmed.store(false, std::memory_order_relaxed);
        pad.recordPendingStart.store(false, std::memory_order_relaxed);
        pad.recordPendingStop.store(false, std::memory_order_relaxed);
        pad.recordStartScheduled.store(false, std::memory_order_relaxed);
        pad.recording.store(false, std::memory_order_relaxed);
        pad.recordScheduledStartBeat = 0.0;
        pad.recordScheduledStopBeat = 0.0;
        pad.recordLoopLengthBeatsOverride = 0;
        pad.recordAutoStopAtScheduledEnd = false;
        pad.recordMatchBpiCanvas = false;
        pad.recordBuffer.setSize(0, 0);
        pad.recordWritePosition = 0;
        pad.recordStartBeat = 0.0;
        pad.midiHoldActive = false;
        pad.midiHoldActionTriggered = false;
        pad.midiPadDown = false;
        pad.midiHoldStartMs = 0.0;
        pad.position.store(0.0, std::memory_order_relaxed);
    }

    return true;
}

void NinjamVst3AudioProcessor::loadSamplePadBankAsync(const juce::File& bankDirectory,
                                                      std::function<void(bool, const juce::String&)> completion)
{
    if (!samplePadBackgroundAlive)
    {
        if (completion)
            completion(false, "Sampler background worker unavailable.");
        return;
    }

    const juce::uint64 requestSerial = samplePadBankLoadRequestSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
    for (auto& serial : samplePadLoadRequestSerial)
        serial.fetch_add(1, std::memory_order_acq_rel);
    for (auto& serial : samplePadResyncRequestSerial)
        serial.fetch_add(1, std::memory_order_acq_rel);
    auto alive = samplePadBackgroundAlive;

    samplePadBackgroundPool.addJob(new SamplePadBackgroundJob("NINJAMSamplePadBankLoad",
        [this, alive, bankDirectory, requestSerial, completion = std::move(completion)]() mutable
        {
            if (!alive->load(std::memory_order_acquire)
                || samplePadBankLoadRequestSerial.load(std::memory_order_acquire) != requestSerial)
            {
                return juce::ThreadPoolJob::jobHasFinished;
            }

            juce::String error;
            const bool loaded = loadSamplePadBank(bankDirectory, error);

            juce::MessageManager::callAsync(
                [alive, requestSerial, loaded, error, completion = std::move(completion), this]() mutable
                {
                    if (!alive->load(std::memory_order_acquire)
                        || samplePadBankLoadRequestSerial.load(std::memory_order_acquire) != requestSerial)
                    {
                        return;
                    }

                    if (completion)
                        completion(loaded, error);
                });

            return juce::ThreadPoolJob::jobHasFinished;
        }), true);
}

void NinjamVst3AudioProcessor::requestLoopedSamplePadsResync(double targetBpm)
{
    if (!isSamplePadsFeatureEnabled() || targetBpm <= 1.0 || !std::isfinite(targetBpm))
        return;

    std::array<bool, numSamplePads> padsToResync {};
    {
        const juce::ScopedLock lock(samplePadsLock);
        for (int padIndex = 0; padIndex < numSamplePads; ++padIndex)
        {
            const auto& pad = samplePads[(size_t)padIndex];
            if (pad.sample.getNumSamples() <= 0
                || !pad.bpmSyncEnabled.load(std::memory_order_relaxed)
                || pad.recording.load(std::memory_order_relaxed))
            {
                continue;
            }

            const auto playbackSpeed = sanitizeSamplePadPlaybackSpeed(pad.playbackSpeed.load(std::memory_order_relaxed));
            const double effectiveTargetBpm = targetBpm * samplePadPlaybackSpeedMultiplier(playbackSpeed);
            if (effectiveTargetBpm <= 1.0 || !std::isfinite(effectiveTargetBpm))
                continue;
            if (pad.bpmSyncApplied && std::abs(pad.lastSyncedTargetBpm - effectiveTargetBpm) < 0.05)
                continue;

            padsToResync[(size_t)padIndex] = true;
        }
    }

    for (int pad = 0; pad < numSamplePads; ++pad)
        if (padsToResync[(size_t)pad])
            enqueueSamplePadResyncJob(pad, targetBpm, false);
}

void NinjamVst3AudioProcessor::enqueueSamplePadResyncJob(int padIndex, double targetBpm, bool force)
{
    if (!isSamplePadsFeatureEnabled()
        || !isValidSamplePadIndex(padIndex)
        || targetBpm <= 1.0
        || !std::isfinite(targetBpm)
        || !samplePadBackgroundAlive)
    {
        return;
    }

    const juce::uint64 requestSerial = samplePadResyncRequestSerial[(size_t)padIndex].fetch_add(1, std::memory_order_acq_rel) + 1;
    auto alive = samplePadBackgroundAlive;

    samplePadBackgroundPool.addJob(new SamplePadBackgroundJob("NINJAMSamplePadResync",
        [this, alive, padIndex, targetBpm, force, requestSerial]()
        {
            if (!alive->load(std::memory_order_acquire)
                || !isSamplePadsFeatureEnabled()
                || samplePadResyncRequestSerial[(size_t)padIndex].load(std::memory_order_acquire) != requestSerial)
            {
                return juce::ThreadPoolJob::jobHasFinished;
            }

            juce::AudioBuffer<float> original;
            double sourceRate = 44100.0;
            double sourceBpm = 0.0;
            double rawSourceBpm = 0.0;
            int loopLengthBeats = 0;
            SamplePadPlaybackSpeed playbackSpeed = SamplePadPlaybackSpeed::normal;
            double effectiveTargetBpm = 0.0;

            {
                const juce::ScopedLock lock(samplePadsLock);
                if (!isSamplePadsFeatureEnabled()
                    || samplePadResyncRequestSerial[(size_t)padIndex].load(std::memory_order_acquire) != requestSerial)
                    return juce::ThreadPoolJob::jobHasFinished;

                auto& pad = samplePads[(size_t)padIndex];
                if (pad.sample.getNumSamples() <= 0
                    || !pad.bpmSyncEnabled.load(std::memory_order_relaxed)
                    || pad.recording.load(std::memory_order_relaxed))
                {
                    return juce::ThreadPoolJob::jobHasFinished;
                }

                if (pad.originalSample.getNumSamples() > 0)
                {
                    original = pad.originalSample;
                    sourceRate = pad.originalSourceSampleRate > 1.0 ? pad.originalSourceSampleRate : pad.sourceSampleRate;
                }
                else
                {
                    original = pad.sample;
                    sourceRate = pad.sourceSampleRate;
                }

                sourceRate = sourceRate > 1.0 ? sourceRate : 44100.0;
                sourceBpm = pad.sourceBpm;
                rawSourceBpm = pad.rawSourceBpm;
                loopLengthBeats = pad.loopLengthBeats;
                playbackSpeed = sanitizeSamplePadPlaybackSpeed(pad.playbackSpeed.load(std::memory_order_relaxed));
                effectiveTargetBpm = targetBpm * samplePadPlaybackSpeedMultiplier(playbackSpeed);
                if (!force && pad.bpmSyncApplied && std::abs(pad.lastSyncedTargetBpm - effectiveTargetBpm) < 0.05)
                    return juce::ThreadPoolJob::jobHasFinished;
            }

            if (original.getNumSamples() <= 0
                || !isSamplePadsFeatureEnabled()
                || samplePadResyncRequestSerial[(size_t)padIndex].load(std::memory_order_acquire) != requestSerial)
            {
                return juce::ThreadPoolJob::jobHasFinished;
            }

            if (sourceBpm <= 1.0 || !std::isfinite(sourceBpm))
            {
                const auto redetect = detectSampleBpmDetailed(original, sourceRate);
                sourceBpm = redetect.normalisedBpm;
                rawSourceBpm = redetect.rawBpm;
                if (loopLengthBeats <= 0)
                    loopLengthBeats = redetect.loopLengthBeats;
            }

            if (sourceBpm <= 1.0 || !std::isfinite(sourceBpm)
                || effectiveTargetBpm <= 1.0 || !std::isfinite(effectiveTargetBpm))
            {
                return juce::ThreadPoolJob::jobHasFinished;
            }

            if (!isSamplePadsFeatureEnabled()
                || samplePadResyncRequestSerial[(size_t)padIndex].load(std::memory_order_acquire) != requestSerial)
            {
                return juce::ThreadPoolJob::jobHasFinished;
            }

            // Calculate target samples from loop length in beats, not BPM ratio.
            // This ensures the stretched sample is exactly loopLengthBeats beats at the target BPM,
            // so it loops perfectly in sync with the NINJAM interval.
            int targetSamples = 0;
            if (loopLengthBeats > 0)
            {
                const double secondsPerBeat = 60.0 / effectiveTargetBpm;
                targetSamples = juce::jmax(1, (int) std::llround((double)loopLengthBeats * secondsPerBeat * sourceRate));
            }
            else
            {
                // Fallback: use BPM ratio if loop length wasn't detected
                const double ratio = sourceBpm / effectiveTargetBpm;
                targetSamples = juce::jmax(1, (int) std::llround((double) original.getNumSamples() * ratio));
            }
            juce::AudioBuffer<float> synced = stretchLoopWithSignalsmith(original, sourceRate, targetSamples);
            if (synced.getNumSamples() <= 0)
                return juce::ThreadPoolJob::jobHasFinished;

            juce::MessageManager::callAsync(
                [this, alive, padIndex, requestSerial,
                 synced = std::move(synced), sourceRate, sourceBpm, effectiveTargetBpm]() mutable
                {
                    if (!alive->load(std::memory_order_acquire)
                        || !isSamplePadsFeatureEnabled()
                        || samplePadResyncRequestSerial[(size_t)padIndex].load(std::memory_order_acquire) != requestSerial)
                    {
                        return;
                    }

                    const juce::ScopedLock lock(samplePadsLock);
                    if (!isSamplePadsFeatureEnabled()
                        || samplePadResyncRequestSerial[(size_t)padIndex].load(std::memory_order_acquire) != requestSerial)
                        return;

                    auto& pad = samplePads[(size_t)padIndex];
                    if (pad.sample.getNumSamples() <= 0
                        || !pad.bpmSyncEnabled.load(std::memory_order_relaxed)
                        || pad.recording.load(std::memory_order_relaxed))
                    {
                        return;
                    }

                    pad.sample = std::move(synced);
                    pad.sourceSampleRate = sourceRate;
                    pad.sourceBpm = sourceBpm;
                    pad.lastSyncedTargetBpm = effectiveTargetBpm;
                    pad.bpmSyncApplied = true;
                    for (auto& voice : pad.oneShotVoices)
                    {
                        voice.active = false;
                        voice.position = 0.0;
                    }
                    pad.nextOneShotVoice = 0;
                    pad.activeOneShotVoices.store(0, std::memory_order_relaxed);
                    const int length = pad.sample.getNumSamples();
                    pad.position.store(juce::jlimit(0.0,
                                                    juce::jmax(0.0, (double) length - 1.0),
                                                    pad.position.load(std::memory_order_relaxed)),
                                       std::memory_order_relaxed);
                });

            return juce::ThreadPoolJob::jobHasFinished;
        }), true);
}

void NinjamVst3AudioProcessor::resyncLoopedSamplePadsToBpm(double targetBpm)
{
    if (!isSamplePadsFeatureEnabled() || targetBpm <= 1.0 || !std::isfinite(targetBpm))
        return;

    for (int pad = 0; pad < numSamplePads; ++pad)
        resyncSamplePadToBpm(pad, targetBpm, false);
}

void NinjamVst3AudioProcessor::resyncSamplePadToBpm(int padIndex, double targetBpm, bool force)
{
    if (!isSamplePadsFeatureEnabled() || !isValidSamplePadIndex(padIndex) || targetBpm <= 1.0 || !std::isfinite(targetBpm))
        return;

    juce::AudioBuffer<float> original;
    double sourceRate = 44100.0;
    double sourceBpm = 0.0;
    double rawSourceBpm = 0.0;
    int loopLengthBeats = 0;
    SamplePadPlaybackSpeed playbackSpeed = SamplePadPlaybackSpeed::normal;
    double effectiveTargetBpm = 0.0;
    bool shouldSync = false;

    {
        const juce::ScopedLock lock(samplePadsLock);
        auto& pad = samplePads[(size_t)padIndex];
        if (pad.sample.getNumSamples() <= 0
            || !pad.bpmSyncEnabled.load(std::memory_order_relaxed)
            || pad.recording.load(std::memory_order_relaxed))
            return;

        if (pad.originalSample.getNumSamples() <= 0)
        {
            pad.originalSample = pad.sample;
            pad.originalSourceSampleRate = pad.sourceSampleRate;
        }

        original = pad.originalSample;
        sourceRate = pad.originalSourceSampleRate > 1.0 ? pad.originalSourceSampleRate : pad.sourceSampleRate;
        sourceBpm = pad.sourceBpm;
        rawSourceBpm = pad.rawSourceBpm;
        loopLengthBeats = pad.loopLengthBeats;
        playbackSpeed = sanitizeSamplePadPlaybackSpeed(pad.playbackSpeed.load(std::memory_order_relaxed));
        effectiveTargetBpm = targetBpm * samplePadPlaybackSpeedMultiplier(playbackSpeed);
        if (!force && pad.bpmSyncApplied && std::abs(pad.lastSyncedTargetBpm - effectiveTargetBpm) < 0.05)
            return;
        shouldSync = true;
    }

    if (!shouldSync || original.getNumSamples() <= 0)
        return;

    if (sourceBpm <= 1.0 || !std::isfinite(sourceBpm))
    {
        const auto redetect = detectSampleBpmDetailed(original, sourceRate);
        sourceBpm = redetect.normalisedBpm;
        rawSourceBpm = redetect.rawBpm;
        if (loopLengthBeats <= 0)
            loopLengthBeats = redetect.loopLengthBeats;
    }

    if (sourceBpm <= 1.0 || !std::isfinite(sourceBpm))
        return;

    if (effectiveTargetBpm <= 1.0 || !std::isfinite(effectiveTargetBpm))
        return;

    if (!isSamplePadsFeatureEnabled())
        return;

    // Calculate target samples from loop length in beats, not BPM ratio.
    int targetSamples = 0;
    if (loopLengthBeats > 0)
    {
        const double secondsPerBeat = 60.0 / effectiveTargetBpm;
        targetSamples = juce::jmax(1, (int)std::llround((double)loopLengthBeats * secondsPerBeat * sourceRate));
    }
    else
    {
        const double ratio = sourceBpm / effectiveTargetBpm;
        targetSamples = juce::jmax(1, (int)std::llround((double)original.getNumSamples() * ratio));
    }
    juce::AudioBuffer<float> synced = stretchLoopWithSignalsmith(original, sourceRate, targetSamples);
    if (synced.getNumSamples() <= 0)
        return;

    const juce::ScopedLock lock(samplePadsLock);
    auto& pad = samplePads[(size_t)padIndex];
    if (pad.sample.getNumSamples() <= 0
        || !pad.bpmSyncEnabled.load(std::memory_order_relaxed)
        || pad.recording.load(std::memory_order_relaxed))
        return;

    pad.sample = std::move(synced);
    pad.sourceSampleRate = sourceRate;
    pad.sourceBpm = sourceBpm;
    pad.lastSyncedTargetBpm = effectiveTargetBpm;
    pad.bpmSyncApplied = true;
    for (auto& voice : pad.oneShotVoices)
    {
        voice.active = false;
        voice.position = 0.0;
    }
    pad.nextOneShotVoice = 0;
    pad.activeOneShotVoices.store(0, std::memory_order_relaxed);
    const int length = pad.sample.getNumSamples();
    pad.position.store(juce::jlimit(0.0, juce::jmax(0.0, (double)length - 1.0),
                                    pad.position.load(std::memory_order_relaxed)),
                       std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::updateSamplePadTransport(int transportPosition, int transportLength, int bpi)
{
    const int safeLength = juce::jmax(0, transportLength);
    const int safeBpi = juce::jmax(1, bpi);
    if (!samplePadTransportInitialised
        || safeLength != samplePadLastTransportLength
        || safeBpi != samplePadLastTransportBpi)
    {
        samplePadTransportInitialised = true;
        samplePadTransportInterval = (long long)intervalIndex.load(std::memory_order_relaxed);
        samplePadLastTransportPosition = transportPosition;
        samplePadLastTransportLength = safeLength;
        samplePadLastTransportBpi = safeBpi;
        return;
    }

    if (safeLength > 0)
    {
        const int delta = transportPosition - samplePadLastTransportPosition;
        const int wrapThreshold = juce::jmax(1, safeLength / 2);
        if (delta < -wrapThreshold)
            ++samplePadTransportInterval;
        else if (delta > wrapThreshold)
            samplePadTransportInterval = (long long)intervalIndex.load(std::memory_order_relaxed);
    }
    else if (transportPosition < samplePadLastTransportPosition)
    {
        ++samplePadTransportInterval;
    }

    samplePadLastTransportPosition = transportPosition;
}

double NinjamVst3AudioProcessor::getSamplePadBlockStartBeat(int transportPosition,
                                                            int transportLength,
                                                            int bpi,
                                                            double& samplesPerBeat)
{
    const int safeBpi = juce::jmax(1, bpi);
    if (transportLength > 0)
        samplesPerBeat = juce::jmax(1.0, (double)transportLength / (double)safeBpi);
    else
    {
        const double bpm = juce::jmax(1.0, (double)getBPM());
        samplesPerBeat = juce::jmax(1.0, (60.0 / bpm) * juce::jmax(1.0, processingSampleRate));
    }

    updateSamplePadTransport(transportPosition, transportLength, safeBpi);
    const double beatInInterval = transportLength > 0
        ? juce::jlimit(0.0, (double)safeBpi, (double)transportPosition / samplesPerBeat)
        : 0.0;
    return (double)samplePadTransportInterval * (double)safeBpi + beatInInterval;
}

void NinjamVst3AudioProcessor::updateSamplePadMidiHolds(double currentBeat, int bpi)
{
    if (!isSamplePadsFeatureEnabled())
        return;

    const int safeBpi = juce::jmax(1, bpi);
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const double safeCurrentBeat = std::isfinite(currentBeat) ? currentBeat : 0.0;
    const double candidate = nextSamplePadIntervalStartBeat(safeCurrentBeat, safeBpi);

    const juce::ScopedLock lock(samplePadsLock);
    for (auto& pad : samplePads)
    {
        if (!pad.midiHoldActive
            || pad.midiHoldActionTriggered
            || nowMs - pad.midiHoldStartMs < samplePadHoldScheduleMs
            || pad.recording.load(std::memory_order_relaxed)
            || pad.recordStartScheduled.load(std::memory_order_relaxed))
        {
            continue;
        }

        pad.matchBpi.store(true, std::memory_order_relaxed);
        pad.loop.store(true, std::memory_order_relaxed);
        pad.recordArmed.store(true, std::memory_order_relaxed);
        pad.recordPendingStart.store(false, std::memory_order_relaxed);
        pad.recordPendingStop.store(false, std::memory_order_relaxed);
        pad.recordScheduledStartBeat = candidate;
        pad.recordScheduledCountdownBeats = (double)safeBpi;
        pad.recordScheduledStopBeat = 0.0;
        pad.recordLoopLengthBeatsOverride = safeBpi;
        pad.recordAutoStopAtScheduledEnd = false;
        pad.recordMatchBpiCanvas = false;
        pad.recordStartScheduled.store(true, std::memory_order_relaxed);
        pad.playing.store(false, std::memory_order_relaxed);
        pad.playbackScheduled.store(false, std::memory_order_relaxed);
        for (auto& voice : pad.oneShotVoices)
        {
            voice.active = false;
            voice.position = 0.0;
        }
        pad.nextOneShotVoice = 0;
        pad.activeOneShotVoices.store(0, std::memory_order_relaxed);
        pad.midiHoldActionTriggered = true;
    }
}

void NinjamVst3AudioProcessor::processSamplePadLooperRecording(int numSamples,
                                                               double blockStartBeat,
                                                               double samplesPerBeat,
                                                               int bpi,
                                                               int totalAvailableInputChannels,
                                                               int localLeftIndex,
                                                               int localRightIndex)
{
    if (!isSamplePadsFeatureEnabled() || numSamples <= 0)
        return;

    bool hasRecordingWork = false;
    {
        const juce::ScopedLock lock(samplePadsLock);
        for (const auto& pad : samplePads)
        {
            if (pad.recording.load(std::memory_order_relaxed)
                || pad.recordPendingStart.load(std::memory_order_relaxed)
                || pad.recordPendingStop.load(std::memory_order_relaxed)
                || pad.recordStartScheduled.load(std::memory_order_relaxed))
            {
                hasRecordingWork = true;
                break;
            }
        }
    }
    if (!hasRecordingWork)
        return;

    const float* srcL = nullptr;
    const float* srcR = nullptr;
    const int input = samplePadLooperInput.load(std::memory_order_relaxed);
    if (input == looperInputSamplePads)
    {
        if (samplePadsOneShotRenderBuffer.getNumChannels() >= 2
            && samplePadsOneShotRenderBuffer.getNumSamples() >= numSamples)
        {
            srcL = samplePadsOneShotRenderBuffer.getReadPointer(0);
            srcR = samplePadsOneShotRenderBuffer.getReadPointer(1);
        }
    }
    else if (isLooperInputRemoteUser(input))
    {
        const int remoteUserIndex = remoteUserIndexForLooperInput(input);
        if (copyRemoteUserAudioForLooper(remoteUserIndex, numSamples))
        {
            srcL = samplePadRemoteLooperInputBuffer.getReadPointer(0);
            srcR = samplePadRemoteLooperInputBuffer.getReadPointer(1);
        }
    }
    else if (input == looperInputLocalChannel)
    {
        // Prefer explicit source indices for stereo local capture when provided by caller.
        if (localLeftIndex >= 0 && localLeftIndex < totalAvailableInputChannels
            && tempInputBuffer.getNumSamples() >= numSamples
            && tempInputBuffer.getNumChannels() > localLeftIndex)
        {
            srcL = tempInputBuffer.getReadPointer(localLeftIndex);
        }
        else if (localChannelBuffer.getNumChannels() > 0 && localChannelBuffer.getNumSamples() >= numSamples)
        {
            srcL = localChannelBuffer.getReadPointer(0);
        }

        if (localRightIndex >= 0 && localRightIndex < totalAvailableInputChannels
            && tempInputBuffer.getNumSamples() >= numSamples
            && tempInputBuffer.getNumChannels() > localRightIndex)
        {
            srcR = tempInputBuffer.getReadPointer(localRightIndex);
        }
        else if (localChannelBuffer.getNumChannels() > 1 && localChannelBuffer.getNumSamples() >= numSamples)
        {
            srcR = localChannelBuffer.getReadPointer(1);
        }

        if (srcL == nullptr)
            srcL = srcR;
        if (srcR == nullptr)
            srcR = srcL;
    }
    else if (input >= 0)
    {
        if (input < totalAvailableInputChannels && tempInputBuffer.getNumSamples() >= numSamples)
        {
            srcL = tempInputBuffer.getReadPointer(input);
            srcR = srcL;
        }
    }
    else
    {
        const int pairIndex = -1 - input;
        const int left = pairIndex * 2;
        const int right = left + 1;
        if (left >= 0 && left < totalAvailableInputChannels && tempInputBuffer.getNumSamples() >= numSamples)
        {
            srcL = tempInputBuffer.getReadPointer(left);
            srcR = (right >= 0 && right < totalAvailableInputChannels) ? tempInputBuffer.getReadPointer(right) : srcL;
        }
    }

    if (srcL == nullptr)
        return;
    if (srcR == nullptr)
        srcR = srcL;

    const double safeSamplesPerBeat = juce::jmax(1.0, samplesPerBeat);
    const int safeBpi = juce::jmax(1, bpi);
    const double blockEndBeat = blockStartBeat + (double)numSamples / safeSamplesPerBeat;

    for (auto& pad : samplePads)
    {
        const juce::ScopedLock padLock(samplePadsLock);
        bool startedThisBlock = false;
        int recordCopyStart = 0;

        auto beginRecording = [&](double startBeat, int startSample)
        {
            const bool matchBpi = pad.matchBpi.load(std::memory_order_relaxed);
            const int expectedBeats = matchBpi
                ? safeBpi
                : pad.recordLoopLengthBeatsOverride > 0
                    ? pad.recordLoopLengthBeatsOverride
                    : safeBpi;
            const int initialCapacity = matchBpi
                ? juce::jmax(1, (int)std::llround(safeSamplesPerBeat * (double)safeBpi))
                : juce::jmax(numSamples * 4,
                              (int)std::ceil(safeSamplesPerBeat * (double)expectedBeats) + numSamples);
            pad.recordBuffer.setSize(2, initialCapacity, false, true, true);
            pad.recordBuffer.clear();
            pad.recordWritePosition = 0;
            pad.recordStartBeat = startBeat;
            pad.recordedStartBeatInInterval = positiveModulo(startBeat, (double)safeBpi);
            pad.recordMatchBpiCanvas = matchBpi;
            pad.recording.store(true, std::memory_order_relaxed);
            pad.recordArmed.store(true, std::memory_order_relaxed);
            pad.playing.store(false, std::memory_order_relaxed);
            pad.playbackScheduled.store(false, std::memory_order_relaxed);
            pad.recordStartScheduled.store(false, std::memory_order_relaxed);
            pad.recordScheduledCountdownBeats = 0.0;
            for (auto& voice : pad.oneShotVoices)
            {
                voice.active = false;
                voice.position = 0.0;
            }
            pad.nextOneShotVoice = 0;
            pad.activeOneShotVoices.store(0, std::memory_order_relaxed);
            startedThisBlock = true;
            recordCopyStart = juce::jlimit(0, numSamples, startSample);
        };

        auto appendRecordingSamples = [&](int sourceOffset, int samplesToAppend)
        {
            if (samplesToAppend <= 0)
                return;

            if (pad.recordMatchBpiCanvas)
            {
                const int targetSamples = pad.recordBuffer.getNumSamples();
                if (targetSamples <= 0)
                    return;

                int remaining = samplesToAppend;
                int sourcePosition = sourceOffset;
                while (remaining > 0)
                {
                    const double sourceBeat = blockStartBeat + (double)sourcePosition / safeSamplesPerBeat;
                    const double beatPhase = positiveModulo(sourceBeat, (double)safeBpi);
                    int targetOffset = (int)std::llround((beatPhase / (double)safeBpi) * (double)targetSamples);
                    targetOffset = targetSamples > 0 ? targetOffset % targetSamples : 0;
                    const int chunk = juce::jmin(remaining, targetSamples - targetOffset);
                    if (chunk <= 0)
                        break;

                    pad.recordBuffer.copyFrom(0, targetOffset, srcL + sourcePosition, chunk);
                    pad.recordBuffer.copyFrom(1, targetOffset, srcR + sourcePosition, chunk);
                    sourcePosition += chunk;
                    remaining -= chunk;
                    pad.recordWritePosition += chunk;
                }
                return;
            }

            const int writePos = pad.recordWritePosition;
            const int requiredSamples = writePos + samplesToAppend;
            if (pad.recordBuffer.getNumSamples() < requiredSamples)
            {
                const int newSize = juce::jmax(requiredSamples, pad.recordBuffer.getNumSamples() * 2 + samplesToAppend);
                pad.recordBuffer.setSize(2, newSize, true, true, false);
            }

            pad.recordBuffer.copyFrom(0, writePos, srcL + sourceOffset, samplesToAppend);
            pad.recordBuffer.copyFrom(1, writePos, srcR + sourceOffset, samplesToAppend);
            pad.recordWritePosition = requiredSamples;
        };

        auto finishRecording = [&](double currentBeat)
        {
            const int capturedSamples = pad.recordWritePosition;
            if (capturedSamples > 0)
            {
                const bool matchBpi = pad.matchBpi.load(std::memory_order_relaxed);
                const bool matchBpiCanvas = pad.recordMatchBpiCanvas;
                const double capturedBeats = juce::jmax(1.0 / safeSamplesPerBeat, (double)capturedSamples / safeSamplesPerBeat);
                const int quantisedBeats = matchBpiCanvas
                    ? safeBpi
                    : pad.recordLoopLengthBeatsOverride > 0
                        ? pad.recordLoopLengthBeatsOverride
                        : matchBpi
                            ? juce::jmax(1, (int)std::llround(capturedBeats))
                            : quantiseSamplePadFreeLoopBeats(capturedBeats);
                const int targetSamples = juce::jmax(1, (int)std::llround((double)quantisedBeats * safeSamplesPerBeat));
                if (samplePadLoopFinishBuffer.getNumChannels() < 2 || samplePadLoopFinishBuffer.getNumSamples() < targetSamples)
                    samplePadLoopFinishBuffer.setSize(2, targetSamples, false, true, true);
                samplePadLoopFinishBuffer.clear(0, targetSamples);
                samplePadLoopFinishBuffer.clear(1, targetSamples);

                const int samplesToCopy = juce::jmin(pad.recordBuffer.getNumSamples(), targetSamples);
                samplePadLoopFinishBuffer.copyFrom(0, 0, pad.recordBuffer, 0, 0, samplesToCopy);
                samplePadLoopFinishBuffer.copyFrom(1, 0, pad.recordBuffer, 1, 0, samplesToCopy);

                pad.sample.setSize(2, targetSamples, false, true, true);
                pad.sample.copyFrom(0, 0, samplePadLoopFinishBuffer, 0, 0, targetSamples);
                pad.sample.copyFrom(1, 0, samplePadLoopFinishBuffer, 1, 0, targetSamples);
                pad.originalSample = pad.sample;
                pad.file = juce::File{};
                pad.sourceSampleRate = juce::jmax(1.0, processingSampleRate);
                pad.originalSourceSampleRate = pad.sourceSampleRate;
                pad.sourceBpm = juce::jmax(1.0, (double)getBPM());
                pad.lastSyncedTargetBpm = pad.sourceBpm;
                pad.bpmSyncApplied = false;
                pad.bpmSyncEnabled.store(true, std::memory_order_relaxed);
                pad.loop.store(true, std::memory_order_relaxed);
                pad.recordedLoop = true;
                pad.loopLengthBeats = quantisedBeats;
                pad.loopAnchorBeat = matchBpiCanvas
                    ? std::floor(pad.recordStartBeat / (double)safeBpi) * (double)safeBpi
                    : pad.recordStartBeat;
                pad.recordedStartBeatInInterval = matchBpiCanvas
                    ? 0.0
                    : positiveModulo(pad.recordStartBeat, (double)safeBpi);

                if (matchBpiCanvas)
                {
                    const double phase = positiveModulo(currentBeat, (double)safeBpi) / (double)safeBpi;
                    const double position = phase * (double)targetSamples;
                    if (pad.reverse.load(std::memory_order_relaxed))
                        pad.position.store(juce::jlimit(0.0, (double)targetSamples - 1.0, (double)targetSamples - 1.0 - position),
                                           std::memory_order_relaxed);
                    else
                        pad.position.store(juce::jlimit(0.0, (double)targetSamples - 1.0, position),
                                           std::memory_order_relaxed);
                    pad.mainVoiceRouteToLocal.store(!samplePadMonitorModeEnabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
                    pad.playing.store(true, std::memory_order_relaxed);
                    pad.playbackScheduled.store(false, std::memory_order_relaxed);
                }
                else if (matchBpi)
                {
                    pad.position.store(0.0, std::memory_order_relaxed);
                    pad.playing.store(false, std::memory_order_relaxed);
                    double interval = std::floor(currentBeat / (double)safeBpi);
                    double candidate = interval * (double)safeBpi + pad.recordedStartBeatInInterval;
                    if (candidate < currentBeat - 0.0001)
                        candidate += (double)safeBpi;
                    pad.scheduledStartBeat = candidate;
                    pad.scheduledPlaybackRouteToLocal = !samplePadMonitorModeEnabled.load(std::memory_order_relaxed);
                    pad.playbackScheduled.store(true, std::memory_order_relaxed);
                }
                else
                {
                    pad.position.store(0.0, std::memory_order_relaxed);
                    pad.playing.store(false, std::memory_order_relaxed);
                    const double loopLengthBeats = (double)quantisedBeats;
                    double loops = std::ceil((currentBeat - pad.loopAnchorBeat) / loopLengthBeats);
                    if (!std::isfinite(loops) || loops < 0.0)
                        loops = 0.0;
                    double candidate = pad.loopAnchorBeat + loops * loopLengthBeats;
                    if (candidate < currentBeat - 0.0001)
                        candidate += loopLengthBeats;
                    pad.scheduledStartBeat = candidate;
                    pad.scheduledPlaybackRouteToLocal = !samplePadMonitorModeEnabled.load(std::memory_order_relaxed);
                    pad.playbackScheduled.store(true, std::memory_order_relaxed);
                }
            }

            pad.recording.store(false, std::memory_order_relaxed);
            pad.recordArmed.store(false, std::memory_order_relaxed);
            pad.recordStartScheduled.store(false, std::memory_order_relaxed);
            pad.recordBuffer.setSize(0, 0);
            pad.recordWritePosition = 0;
            pad.recordLoopLengthBeatsOverride = 0;
            pad.recordScheduledStartBeat = 0.0;
            pad.recordScheduledCountdownBeats = 0.0;
            pad.recordScheduledStopBeat = 0.0;
            pad.recordAutoStopAtScheduledEnd = false;
            pad.recordMatchBpiCanvas = false;
        };

        if (!pad.recording.load(std::memory_order_relaxed))
        {
            if (pad.recordPendingStart.exchange(false, std::memory_order_relaxed))
                beginRecording(blockStartBeat, 0);
            else if (pad.recordStartScheduled.load(std::memory_order_relaxed)
                     && pad.recordScheduledStartBeat < blockEndBeat)
            {
                constexpr double missedScheduledStartToleranceBeats = 0.02;
                if (pad.recordScheduledStartBeat < blockStartBeat - missedScheduledStartToleranceBeats)
                {
                    const double countdownBeats = pad.recordScheduledCountdownBeats > 0.0
                        ? pad.recordScheduledCountdownBeats
                        : (pad.matchBpi.load(std::memory_order_relaxed) ? (double)safeBpi : samplePadRecordBarBeats);
                    pad.recordScheduledStartBeat = countdownBeats >= (double)safeBpi - 0.0001
                        ? nextSamplePadIntervalStartBeat(blockStartBeat, safeBpi)
                        : nextSamplePadGridBeat(blockStartBeat, safeBpi);
                }
                else
                {
                    const int startSample = juce::jlimit(0, numSamples,
                        (int)std::llround((pad.recordScheduledStartBeat - blockStartBeat) * safeSamplesPerBeat));
                    beginRecording(pad.recordScheduledStartBeat, startSample);
                }
            }
        }

        const bool manualStopRequested = pad.recordPendingStop.exchange(false, std::memory_order_relaxed);
        if (manualStopRequested && pad.recording.load(std::memory_order_relaxed) && !startedThisBlock)
        {
            finishRecording(blockStartBeat);
            continue;
        }

        if (!pad.recording.load(std::memory_order_relaxed))
            continue;

        int recordCopyEnd = numSamples;
        bool autoStopReached = false;
        double stopBeat = blockEndBeat;
        if (pad.recordAutoStopAtScheduledEnd
            && pad.recordScheduledStopBeat > 0.0
            && pad.recordScheduledStopBeat <= blockEndBeat + 0.0001)
        {
            recordCopyEnd = juce::jlimit(recordCopyStart, numSamples,
                (int)std::llround((pad.recordScheduledStopBeat - blockStartBeat) * safeSamplesPerBeat));
            autoStopReached = true;
            stopBeat = pad.recordScheduledStopBeat;
        }

        appendRecordingSamples(recordCopyStart, recordCopyEnd - recordCopyStart);

        if (manualStopRequested || autoStopReached)
        {
            finishRecording(manualStopRequested ? blockEndBeat : stopBeat);
            continue;
        }
    }
}

bool NinjamVst3AudioProcessor::renderSamplePads(int numSamples,
                                                double blockStartBeat,
                                                double samplesPerBeat,
                                                int bpi)
{
    if (!isSamplePadsFeatureEnabled() || numSamples <= 0)
    {
        samplePadsPeak.store(0.0f, std::memory_order_relaxed);
        return false;
    }

    bool hasRenderWork = false;
    {
        const juce::ScopedLock lock(samplePadsLock);
        for (const auto& pad : samplePads)
        {
            bool hasActiveOneShot = pad.activeOneShotVoices.load(std::memory_order_relaxed) > 0;
            if (!hasActiveOneShot)
            {
                for (const auto& voice : pad.oneShotVoices)
                {
                    if (voice.active)
                    {
                        hasActiveOneShot = true;
                        break;
                    }
                }
            }

            if (pad.playing.load(std::memory_order_relaxed)
                || pad.playbackScheduled.load(std::memory_order_relaxed)
                || hasActiveOneShot)
            {
                hasRenderWork = true;
                break;
            }
        }
    }
    if (!hasRenderWork)
    {
        auto clearOrResizeStereoBuffer = [numSamples](juce::AudioBuffer<float>& target)
        {
            if (target.getNumChannels() < 2 || target.getNumSamples() < numSamples)
                target.setSize(2, numSamples, false, true, true);
            target.clear(0, 0, numSamples);
            target.clear(1, 0, numSamples);
        };
        clearOrResizeStereoBuffer(samplePadsRenderBuffer);
        clearOrResizeStereoBuffer(samplePadsMonitorRenderBuffer);
        clearOrResizeStereoBuffer(samplePadsOneShotRenderBuffer);
        samplePadsPeak.store(0.0f, std::memory_order_relaxed);
        return false;
    }

    if (samplePadsRenderBuffer.getNumChannels() < 2 || samplePadsRenderBuffer.getNumSamples() < numSamples)
        samplePadsRenderBuffer.setSize(2, numSamples, false, true, true);
    if (samplePadsOneShotRenderBuffer.getNumChannels() < 2 || samplePadsOneShotRenderBuffer.getNumSamples() < numSamples)
        samplePadsOneShotRenderBuffer.setSize(2, numSamples, false, true, true);
    if (samplePadsMonitorRenderBuffer.getNumChannels() < 2 || samplePadsMonitorRenderBuffer.getNumSamples() < numSamples)
        samplePadsMonitorRenderBuffer.setSize(2, numSamples, false, true, true);
    samplePadsRenderBuffer.clear();
    samplePadsMonitorRenderBuffer.clear();
    samplePadsOneShotRenderBuffer.clear();
    for (int pad = 0; pad < numSamplePads; ++pad)
    {
        for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
        {
            auto& slotBuffer = samplePadPerPadFxSlotInputBuffers[(size_t)pad][(size_t)slot];
            if (slotBuffer.getNumChannels() < 2 || slotBuffer.getNumSamples() < numSamples)
                slotBuffer.setSize(2, numSamples, false, true, true);
            slotBuffer.clear();
            auto& monitorSlotBuffer = samplePadMonitorPerPadFxSlotInputBuffers[(size_t)pad][(size_t)slot];
            if (monitorSlotBuffer.getNumChannels() < 2 || monitorSlotBuffer.getNumSamples() < numSamples)
                monitorSlotBuffer.setSize(2, numSamples, false, true, true);
            monitorSlotBuffer.clear();
        }
    }

    const double targetRate = processingSampleRate > 1.0 ? processingSampleRate : juce::jmax(1.0, getSampleRate());
    const double safeSamplesPerBeat = juce::jmax(1.0, samplesPerBeat);
    const double blockEndBeat = blockStartBeat + (double)numSamples / safeSamplesPerBeat;
    float* outL = samplePadsRenderBuffer.getWritePointer(0);
    float* outR = samplePadsRenderBuffer.getWritePointer(1);
    float* monitorOutL = samplePadsMonitorRenderBuffer.getWritePointer(0);
    float* monitorOutR = samplePadsMonitorRenderBuffer.getWritePointer(1);
    float* oneShotOutL = samplePadsOneShotRenderBuffer.getWritePointer(0);
    float* oneShotOutR = samplePadsOneShotRenderBuffer.getWritePointer(1);
    const bool globalDuckEnabled = samplePadsDuckEnabled.load(std::memory_order_relaxed);
    // Global Duck is the master enable. Only pads with D enabled get the duck effect.
    // When global Duck is off, no ducking happens at all.
    bool anyPadNeedsDuck = false;
    if (globalDuckEnabled)
    {
        for (const auto& p : samplePads)
        {
            if (p.duckRoute.load(std::memory_order_relaxed))
            {
                anyPadNeedsDuck = true;
                break;
            }
        }
    }

    const float* duckGains = nullptr;
    if (anyPadNeedsDuck)
    {
        if (samplePadDuckGainBuffer.size() < (size_t)numSamples)
            samplePadDuckGainBuffer.resize((size_t)numSamples, 1.0f);

        const double bpm = juce::jmax(1.0, (double)getBPM());
        const auto duckShape = getSamplePadDuckShape();
        const auto duckLength = getSamplePadDuckLength();
        samplePadDuckOscillator.setFrequency((float)(bpm / 60.0), false);

        // The duck shapes already use smoothDuckStep (Hermite interpolation) which
        // produces a smooth curve. We only need minimal smoothing to prevent clicks
        // from the hardGate shape's sharp transitions. Using 0.5ms attack / 2ms release
        // preserves the shape character while eliminating artifacts.
        const double sr = processingSampleRate > 1.0 ? processingSampleRate : juce::jmax(1.0, getSampleRate());
        const float attackCoeff = 1.0f - std::exp(-1.0f / (0.0005f * (float)sr));  // 0.5ms attack
        const float releaseCoeff = 1.0f - std::exp(-1.0f / (0.002f * (float)sr));  // 2ms release

        for (int i = 0; i < numSamples; ++i)
        {
            (void)samplePadDuckOscillator.processSample(0.0f);
            const float target =
                getSamplePadDuckTargetForBeat(blockStartBeat + (double)i / safeSamplesPerBeat,
                                              duckShape,
                                              duckLength);

            // Minimal smoothing: only enough to prevent clicks on sharp transitions
            const float coeff = target < smoothedDuckGain ? attackCoeff : releaseCoeff;
            smoothedDuckGain += (target - smoothedDuckGain) * coeff;
            samplePadDuckGainBuffer[(size_t)i] = smoothedDuckGain;
        }
        duckGains = samplePadDuckGainBuffer.data();
    }
    else
    {
        // No ducking needed — reset smoothed gain to 1.0
        smoothedDuckGain = 1.0f;
    }

    {
        std::array<bool, numSamplePadFxSlots> localRouteFxSlotsInUse {};
        {
            const juce::ScopedLock lock(samplePadsLock);
            for (const auto& activePad : samplePads)
            {
            const bool localMainActive =
                (activePad.playing.load(std::memory_order_relaxed)
                    && activePad.mainVoiceRouteToLocal.load(std::memory_order_relaxed))
                || (activePad.playbackScheduled.load(std::memory_order_relaxed)
                    && activePad.scheduledPlaybackRouteToLocal);

            bool localOneShotActive = false;
            for (const auto& voice : activePad.oneShotVoices)
            {
                if (voice.active && voice.routeToLocal)
                {
                    localOneShotActive = true;
                    break;
                }
            }

            if (!localMainActive && !localOneShotActive)
                continue;

            for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
                localRouteFxSlotsInUse[(size_t)slot] = localRouteFxSlotsInUse[(size_t)slot]
                    || activePad.fxSlotRoutes[(size_t)slot].load(std::memory_order_relaxed);
            }
        }
        for (int padIndex = 0; padIndex < numSamplePads; ++padIndex)
        {
            const juce::ScopedLock padLock(samplePadsLock);
            auto& pad = samplePads[(size_t)padIndex];
            const int length = pad.sample.getNumSamples();
            if (length <= 0)
            {
                pad.playing.store(false, std::memory_order_relaxed);
                for (auto& voice : pad.oneShotVoices)
                {
                    voice.active = false;
                    voice.position = 0.0;
                }
                pad.nextOneShotVoice = 0;
                pad.activeOneShotVoices.store(0, std::memory_order_relaxed);
                continue;
            }

            const float* srcL = pad.sample.getReadPointer(0);
            const float* srcR = pad.sample.getNumChannels() > 1 ? pad.sample.getReadPointer(1) : srcL;
            const bool reverse = pad.reverse.load(std::memory_order_relaxed);
            const bool loop = pad.loop.load(std::memory_order_relaxed);
            const double lengthD = (double)length;
            const auto padSpeed = sanitizeSamplePadPlaybackSpeed(pad.playbackSpeed.load(std::memory_order_relaxed));
            const bool bpmSynced = pad.bpmSyncEnabled.load(std::memory_order_relaxed) && pad.bpmSyncApplied;
            // When BPM sync is active, the sample is already pre-stretched for the speed.
            // Only apply the speed multiplier to the step when BPM sync is off.
            double speedMultiplier = 1.0;
            if (!bpmSynced)
            {
                if (padSpeed == SamplePadPlaybackSpeed::half) speedMultiplier = 0.5;
                else if (padSpeed == SamplePadPlaybackSpeed::doubleSpeed) speedMultiplier = 2.0;
            }
            const double step = juce::jmax(0.000001, (pad.sourceSampleRate / targetRate) * speedMultiplier);
            const bool duckThisPad = globalDuckEnabled && pad.duckRoute.load(std::memory_order_relaxed);
            const float padVolume = pad.volume.load(std::memory_order_relaxed);
            std::array<bool, numSamplePadFxSlots> routedFxSlots {};
            bool hasRoutedFxSlot = false;
            for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
            {
                const bool routed = pad.fxSlotRoutes[(size_t)slot].load(std::memory_order_relaxed);
                routedFxSlots[(size_t)slot] = routed;
                hasRoutedFxSlot = hasRoutedFxSlot || routed;
            }

            int startSample = 0;
            bool mainVoiceActive = pad.playing.load(std::memory_order_relaxed);
            if (!mainVoiceActive
                && pad.playbackScheduled.load(std::memory_order_relaxed)
                && pad.scheduledStartBeat < blockEndBeat)
            {
                // Missed-start recovery: if the scheduled beat is in the past (we missed it
                // between blocks), reschedule to the next interval boundary rather than
                // starting late. Same approach as the looper recording.
                constexpr double missedScheduledStartToleranceBeats = 0.02;
                if (pad.scheduledStartBeat < blockStartBeat - missedScheduledStartToleranceBeats)
                {
                    pad.scheduledStartBeat = nextSamplePadIntervalStartBeat(blockStartBeat, bpi);
                }
                else
                {
                    startSample = juce::jlimit(0, numSamples - 1,
                                               (int)std::llround((pad.scheduledStartBeat - blockStartBeat) * safeSamplesPerBeat));
                    pad.playbackScheduled.store(false, std::memory_order_relaxed);
                    pad.mainVoiceRouteToLocal.store(pad.scheduledPlaybackRouteToLocal, std::memory_order_relaxed);
                    pad.position.store(reverse ? juce::jmax(0.0, lengthD - 1.0) : 0.0, std::memory_order_relaxed);
                    pad.playing.store(true, std::memory_order_relaxed);
                    mainVoiceActive = true;
                }
            }

            if (mainVoiceActive)
            {
                double pos = juce::jlimit(0.0, juce::jmax(0.0, lengthD - 1.0), pad.position.load(std::memory_order_relaxed));
                bool stillPlaying = true;
                const bool routeToLocal = pad.mainVoiceRouteToLocal.load(std::memory_order_relaxed);
                float* routeOutL = routeToLocal ? outL : monitorOutL;
                float* routeOutR = routeToLocal ? outR : monitorOutR;
                auto& routeFxSlotInputBuffers = routeToLocal ? samplePadPerPadFxSlotInputBuffers : samplePadMonitorPerPadFxSlotInputBuffers;

                for (int i = startSample; i < numSamples; ++i)
                {
                    const int index0 = juce::jlimit(0, length - 1, (int)std::floor(pos));
                    const int index1 = juce::jmin(index0 + 1, length - 1);
                    const float frac = (float)(pos - (double)index0);
                    const float gain = (duckThisPad ? duckGains[i] : 1.0f) * padVolume;
                    const float sampleL = (srcL[index0] + (srcL[index1] - srcL[index0]) * frac) * gain;
                    const float sampleR = (srcR[index0] + (srcR[index1] - srcR[index0]) * frac) * gain;
                    routeOutL[i] += sampleL;
                    routeOutR[i] += sampleR;
                    if (hasRoutedFxSlot)
                    {
                        for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
                        {
                            if (!routedFxSlots[(size_t)slot] || (!routeToLocal && localRouteFxSlotsInUse[(size_t)slot]))
                                continue;
                            auto& fxBuffer = routeFxSlotInputBuffers[(size_t)padIndex][(size_t)slot];
                            fxBuffer.addSample(0, i, sampleL);
                            fxBuffer.addSample(1, i, sampleR);
                        }
                    }

                    pos += reverse ? -step : step;
                    if (reverse)
                    {
                        if (pos < 0.0)
                        {
                            if (loop)
                            {
                                while (pos < 0.0)
                                    pos += lengthD;
                                // Apply deferred speed change at loop boundary
                                const int pendingSpeed = pad.pendingPlaybackSpeed.load(std::memory_order_relaxed);
                                if (pendingSpeed >= 0)
                                {
                                    pad.pendingPlaybackSpeed.store(-1, std::memory_order_relaxed);
                                    pad.playbackSpeed.store(pendingSpeed, std::memory_order_relaxed);
                                    pad.bpmSyncApplied = false;
                                    pad.lastSyncedTargetBpm = 0.0;
                                    samplePadPendingSpeedResync[(size_t)padIndex].store(true, std::memory_order_relaxed);
                                }
                            }
                            else
                            {
                                pos = 0.0;
                                stillPlaying = false;
                                break;
                            }
                        }
                    }
                    else if (pos >= lengthD)
                    {
                        if (loop)
                        {
                            while (pos >= lengthD)
                                pos -= lengthD;
                            // Apply deferred speed change at loop boundary
                            const int pendingSpeed = pad.pendingPlaybackSpeed.load(std::memory_order_relaxed);
                            if (pendingSpeed >= 0)
                            {
                                pad.pendingPlaybackSpeed.store(-1, std::memory_order_relaxed);
                                pad.playbackSpeed.store(pendingSpeed, std::memory_order_relaxed);
                                pad.bpmSyncApplied = false;
                                pad.lastSyncedTargetBpm = 0.0;
                                samplePadPendingSpeedResync[(size_t)padIndex].store(true, std::memory_order_relaxed);
                            }
                        }
                        else
                        {
                            pos = juce::jmax(0.0, lengthD - 1.0);
                            stillPlaying = false;
                            break;
                        }
                    }
                }

                pad.position.store(pos, std::memory_order_relaxed);
                pad.playing.store(stillPlaying, std::memory_order_relaxed);
            }

            int activeOneShotVoices = 0;
            for (auto& voice : pad.oneShotVoices)
            {
                if (!voice.active)
                    continue;

                double pos = juce::jlimit(0.0, juce::jmax(0.0, lengthD - 1.0), voice.position);
                bool stillActive = true;
                const bool routeToLocal = voice.routeToLocal;
                float* routeOutL = routeToLocal ? outL : monitorOutL;
                float* routeOutR = routeToLocal ? outR : monitorOutR;
                auto& routeFxSlotInputBuffers = routeToLocal ? samplePadPerPadFxSlotInputBuffers : samplePadMonitorPerPadFxSlotInputBuffers;

                for (int i = 0; i < numSamples; ++i)
                {
                    const int index0 = juce::jlimit(0, length - 1, (int)std::floor(pos));
                    const int index1 = juce::jmin(index0 + 1, length - 1);
                    const float frac = (float)(pos - (double)index0);
                    const float gain = (duckThisPad ? duckGains[i] : 1.0f) * padVolume;
                    const float sampleL = (srcL[index0] + (srcL[index1] - srcL[index0]) * frac) * gain;
                    const float sampleR = (srcR[index0] + (srcR[index1] - srcR[index0]) * frac) * gain;
                    routeOutL[i] += sampleL;
                    routeOutR[i] += sampleR;
                    if (routeToLocal)
                    {
                        oneShotOutL[i] += sampleL;
                        oneShotOutR[i] += sampleR;
                    }
                    if (hasRoutedFxSlot)
                    {
                        for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
                        {
                            if (!routedFxSlots[(size_t)slot] || (!routeToLocal && localRouteFxSlotsInUse[(size_t)slot]))
                                continue;
                            auto& fxBuffer = routeFxSlotInputBuffers[(size_t)padIndex][(size_t)slot];
                            fxBuffer.addSample(0, i, sampleL);
                            fxBuffer.addSample(1, i, sampleR);
                        }
                    }

                    pos += reverse ? -step : step;
                    if (reverse)
                    {
                        if (pos < 0.0)
                        {
                            pos = 0.0;
                            stillActive = false;
                            break;
                        }
                    }
                    else if (pos >= lengthD)
                    {
                        pos = juce::jmax(0.0, lengthD - 1.0);
                        stillActive = false;
                        break;
                    }
                }

                voice.position = pos;
                voice.active = stillActive;
                if (stillActive)
                    ++activeOneShotVoices;
            }

            pad.activeOneShotVoices.store(activeOneShotVoices, std::memory_order_relaxed);
        }
    }

    const float volume = samplePadsVolume.load(std::memory_order_relaxed);
    for (int ch = 0; ch < 2; ++ch)
    {
        float* data = samplePadsRenderBuffer.getWritePointer(ch);
        float* oneShotData = samplePadsOneShotRenderBuffer.getWritePointer(ch);
        float* monitorData = samplePadsMonitorRenderBuffer.getWritePointer(ch);
        for (int i = 0; i < numSamples; ++i)
        {
            data[i] *= volume;
            oneShotData[i] *= volume;
            monitorData[i] *= volume;
        }
    }
    if (volume != 1.0f)
    {
        for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
        {
            for (int pad = 0; pad < numSamplePads; ++pad)
            {
                samplePadPerPadFxSlotInputBuffers[(size_t)pad][(size_t)slot].applyGain(0, 0, numSamples, volume);
                samplePadPerPadFxSlotInputBuffers[(size_t)pad][(size_t)slot].applyGain(1, 0, numSamples, volume);
                samplePadMonitorPerPadFxSlotInputBuffers[(size_t)pad][(size_t)slot].applyGain(0, 0, numSamples, volume);
                samplePadMonitorPerPadFxSlotInputBuffers[(size_t)pad][(size_t)slot].applyGain(1, 0, numSamples, volume);
            }
        }
    }

    applySamplePadInsertFx(numSamples, blockStartBeat, samplesPerBeat, bpi);

    const bool limiter = samplePadsLimiterEnabled.load(std::memory_order_relaxed);
    constexpr float limiterThreshold = 0.79432823f; // -2 dBFS
    float peak = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
    {
        float* data = samplePadsRenderBuffer.getWritePointer(ch);
        float* oneShotData = samplePadsOneShotRenderBuffer.getWritePointer(ch);
        float* monitorData = samplePadsMonitorRenderBuffer.getWritePointer(ch);
        for (int i = 0; i < numSamples; ++i)
        {
            float v = data[i];
            if (limiter)
                v = juce::jlimit(-limiterThreshold, limiterThreshold, v);

            data[i] = v;

            float oneShotV = oneShotData[i];
            if (limiter)
                oneShotV = juce::jlimit(-limiterThreshold, limiterThreshold, oneShotV);
            oneShotData[i] = oneShotV;

            float monitorV = monitorData[i];
            if (limiter)
                monitorV = juce::jlimit(-limiterThreshold, limiterThreshold, monitorV);
            monitorData[i] = monitorV;

            peak = juce::jmax(peak, juce::jmax(std::abs(v), std::abs(monitorV)));
        }
    }

    samplePadsPeak.store(peak, std::memory_order_relaxed);
    return peak > 0.000001f;
}

void NinjamVst3AudioProcessor::processSamplePadInsertFxRoute(int numSamples,
                                                             juce::AudioBuffer<float>& renderBuffer,
                                                             SamplePadFxBufferBank& slotInputBuffers,
                                                             SamplePadFilterBank& djFilters,
                                                             SamplePadFilterBank& djBpFilters,
                                                             SamplePadPhaserBank& phasers,
                                                             SamplePadReverbBank& reverbs,
                                                             SamplePadFxBufferBank& delayBuffers,
                                                             SamplePadDelayWritePositionBank& delayWritePositions,
                                                             double,
                                                             double,
                                                             int)
{
    if (numSamples <= 0
        || renderBuffer.getNumChannels() < 2
        || renderBuffer.getNumSamples() < numSamples)
        return;

    const double sampleRate = processingSampleRate > 1.0 ? processingSampleRate : juce::jmax(1.0, getSampleRate());
    const double bpm = juce::jmax(1.0, (double)getBPM());
    static constexpr double samplePadDjFilterHpMaxHz = 5200.0;
    if (samplePadFxScratchBuffer.getNumChannels() < 2 || samplePadFxScratchBuffer.getNumSamples() < numSamples)
        samplePadFxScratchBuffer.setSize(2, numSamples, false, true, true);

    std::array<std::array<bool, numSamplePadFxSlots>, numSamplePadFxSlots> chainRoutes {};
    std::array<bool, numSamplePadFxSlots> slotHasInput {};
    std::array<bool, numSamplePadFxSlots> slotHasDownstream {};
    for (int sourceSlot = 0; sourceSlot < numSamplePadFxSlots; ++sourceSlot)
    {
        for (int targetSlot = 0; targetSlot < numSamplePadFxSlots; ++targetSlot)
        {
            const bool routed = samplePadFxSlotChainRoutes[(size_t)sourceSlot][(size_t)targetSlot].load(std::memory_order_relaxed);
            chainRoutes[(size_t)sourceSlot][(size_t)targetSlot] = routed;
            slotHasDownstream[(size_t)sourceSlot] = slotHasDownstream[(size_t)sourceSlot] || routed;
        }
    }

    std::array<int, numSamplePadFxSlots> fxProcessOrder {};
    std::array<int, numSamplePadFxSlots> indegree {};
    std::array<bool, numSamplePadFxSlots> ordered {};
    for (int sourceSlot = 0; sourceSlot < numSamplePadFxSlots; ++sourceSlot)
        for (int targetSlot = 0; targetSlot < numSamplePadFxSlots; ++targetSlot)
            if (chainRoutes[(size_t)sourceSlot][(size_t)targetSlot])
                ++indegree[(size_t)targetSlot];

    int orderCount = 0;
    for (int pass = 0; pass < numSamplePadFxSlots; ++pass)
    {
        int nextSlot = -1;
        for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
        {
            if (!ordered[(size_t)slot] && indegree[(size_t)slot] == 0)
            {
                nextSlot = slot;
                break;
            }
        }

        if (nextSlot < 0)
            break;

        ordered[(size_t)nextSlot] = true;
        fxProcessOrder[(size_t)orderCount++] = nextSlot;
        for (int targetSlot = 0; targetSlot < numSamplePadFxSlots; ++targetSlot)
            if (chainRoutes[(size_t)nextSlot][(size_t)targetSlot])
                --indegree[(size_t)targetSlot];
    }

    for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
        if (!ordered[(size_t)slot])
            fxProcessOrder[(size_t)orderCount++] = slot;

    auto copySlotDry = [this, numSamples](const juce::AudioBuffer<float>& slotBuffer)
    {
        samplePadFxScratchBuffer.copyFrom(0, 0, slotBuffer, 0, 0, numSamples);
        samplePadFxScratchBuffer.copyFrom(1, 0, slotBuffer, 1, 0, numSamples);
    };

    auto addSlotDeltaToOutput = [this, numSamples, &renderBuffer](const juce::AudioBuffer<float>& slotBuffer)
    {
        float* outL = renderBuffer.getWritePointer(0);
        float* outR = renderBuffer.getWritePointer(1);
        const float* dryL = samplePadFxScratchBuffer.getReadPointer(0);
        const float* dryR = samplePadFxScratchBuffer.getReadPointer(1);
        const float* wetL = slotBuffer.getReadPointer(0);
        const float* wetR = slotBuffer.getReadPointer(1);
        for (int i = 0; i < numSamples; ++i)
        {
            outL[i] += wetL[i] - dryL[i];
            outR[i] += wetR[i] - dryR[i];
        }
    };

    for (int padIndex = 0; padIndex < numSamplePads; ++padIndex)
    {
        for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
            slotHasInput[(size_t)slot] = samplePads[(size_t)padIndex].fxSlotRoutes[(size_t)slot].load(std::memory_order_relaxed);

        for (int orderIndex = 0; orderIndex < numSamplePadFxSlots; ++orderIndex)
        {
            const int slot = fxProcessOrder[(size_t)orderIndex];
            const auto type = getSamplePadFxSlotType(slot);
            const float amount = juce::jlimit(0.0f, 1.0f, getSamplePadFxSlotAmount(slot));
            if (!slotHasInput[(size_t)slot])
                continue;
            if (amount <= 0.0001f && !slotHasDownstream[(size_t)slot])
                continue;

            auto& slotBuffer = slotInputBuffers[(size_t)padIndex][(size_t)slot];
            if (slotBuffer.getNumChannels() < 2 || slotBuffer.getNumSamples() < numSamples)
                continue;

            if (amount > 0.0001f)
            {
                switch (type)
                {
                case SamplePadFxType::djFilter:
                {
                    if (amount > 0.49f && amount < 0.51f)
                        break;

                    copySlotDry(slotBuffer);
                    auto& filter = djFilters[(size_t)padIndex][(size_t)slot];
                    if (amount < 0.5f)
                    {
                        filter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
                        filter.setCutoffFrequency((float)mapNormalisedToLogFrequency((double)amount / 0.5, 80.0, 18000.0));
                    }
                    else
                    {
                        filter.setType(juce::dsp::StateVariableTPTFilterType::highpass);
                        filter.setCutoffFrequency((float)mapNormalisedToLogFrequency(((double)amount - 0.5) / 0.5,
                                                                                     35.0,
                                                                                     samplePadDjFilterHpMaxHz));
                    }

                    for (int i = 0; i < numSamples; ++i)
                    {
                        slotBuffer.setSample(0, i, filter.processSample(0, slotBuffer.getSample(0, i)));
                        slotBuffer.setSample(1, i, filter.processSample(1, slotBuffer.getSample(1, i)));
                    }
                    addSlotDeltaToOutput(slotBuffer);
                    break;
                }

                case SamplePadFxType::djFilterHp:
                {
                    copySlotDry(slotBuffer);
                    auto& filter = djFilters[(size_t)padIndex][(size_t)slot];
                    filter.setType(juce::dsp::StateVariableTPTFilterType::highpass);
                    filter.setCutoffFrequency((float)mapNormalisedToLogFrequency(amount, 35.0, samplePadDjFilterHpMaxHz));
                    for (int i = 0; i < numSamples; ++i)
                    {
                        slotBuffer.setSample(0, i, filter.processSample(0, slotBuffer.getSample(0, i)));
                        slotBuffer.setSample(1, i, filter.processSample(1, slotBuffer.getSample(1, i)));
                    }
                    addSlotDeltaToOutput(slotBuffer);
                    break;
                }

                case SamplePadFxType::djFilterLp:
                {
                    copySlotDry(slotBuffer);
                    auto& filter = djFilters[(size_t)padIndex][(size_t)slot];
                    filter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
                    filter.setCutoffFrequency((float)mapNormalisedToLogFrequency(1.0 - (double)amount, 80.0, 18000.0));
                    for (int i = 0; i < numSamples; ++i)
                    {
                        slotBuffer.setSample(0, i, filter.processSample(0, slotBuffer.getSample(0, i)));
                        slotBuffer.setSample(1, i, filter.processSample(1, slotBuffer.getSample(1, i)));
                    }
                    addSlotDeltaToOutput(slotBuffer);
                    break;
                }

                case SamplePadFxType::djFilterBp:
                {
                    copySlotDry(slotBuffer);
                    auto& filter = djBpFilters[(size_t)padIndex][(size_t)slot];
                    filter.setCutoffFrequency((float)mapNormalisedToLogFrequency(amount, 120.0, 9000.0));
                    filter.setResonance(1.35f);
                    for (int i = 0; i < numSamples; ++i)
                    {
                        const float dryL = slotBuffer.getSample(0, i);
                        const float dryR = slotBuffer.getSample(1, i);
                        const float wetL = filter.processSample(0, dryL);
                        const float wetR = filter.processSample(1, dryR);
                        slotBuffer.setSample(0, i, dryL + (wetL - dryL) * amount);
                        slotBuffer.setSample(1, i, dryR + (wetR - dryR) * amount);
                    }
                    addSlotDeltaToOutput(slotBuffer);
                    break;
                }

                case SamplePadFxType::phaser:
                case SamplePadFxType::phaserHalf:
                {
                    copySlotDry(slotBuffer);
                    auto& phaser = phasers[(size_t)padIndex][(size_t)slot];
                    const double beatsPerCycle = type == SamplePadFxType::phaserHalf ? 2.0 : 1.0;
                    phaser.setRate((float)juce::jlimit(0.02, 12.0, bpm / (60.0 * beatsPerCycle)));
                    phaser.setMix(amount * 0.5f);
                    auto block = juce::dsp::AudioBlock<float>(slotBuffer).getSubBlock(0, (size_t)numSamples);
                    juce::dsp::ProcessContextReplacing<float> context(block);
                    phaser.process(context);
                    addSlotDeltaToOutput(slotBuffer);
                    break;
                }

                case SamplePadFxType::reverb:
                {
                    float* mono = samplePadFxScratchBuffer.getWritePointer(0);
                    const float* left = slotBuffer.getReadPointer(0);
                    const float* right = slotBuffer.getReadPointer(1);
                    for (int i = 0; i < numSamples; ++i)
                        mono[i] = 0.5f * (left[i] + right[i]);

                    juce::Reverb::Parameters params;
                    params.roomSize = juce::jlimit(0.35f, 0.98f, 0.35f + amount * 0.63f);
                    params.damping = juce::jlimit(0.25f, 0.62f, 0.58f - amount * 0.24f);
                    params.width = 1.0f;
                    params.wetLevel = 1.0f;
                    params.dryLevel = 0.0f;
                    params.freezeMode = 0.0f;
                    auto& reverb = reverbs[(size_t)padIndex][(size_t)slot];
                    reverb.setParameters(params);
                    reverb.processMono(mono, numSamples);

                    const float gain = amount * 0.72f;
                    float* outL = renderBuffer.getWritePointer(0);
                    float* outR = renderBuffer.getWritePointer(1);
                    for (int i = 0; i < numSamples; ++i)
                    {
                        const float dryL = left[i];
                        const float dryR = right[i];
                        const float wet = mono[i] * gain;
                        outL[i] += wet;
                        outR[i] += wet;
                        slotBuffer.setSample(0, i, dryL + wet);
                        slotBuffer.setSample(1, i, dryR + wet);
                    }
                    break;
                }

                case SamplePadFxType::delay:
                case SamplePadFxType::delayQuarter:
                case SamplePadFxType::delayQuarterPingPong:
                {
                    auto& delayBuffer = delayBuffers[(size_t)padIndex][(size_t)slot];
                    if (delayBuffer.getNumSamples() <= 1)
                        break;

                    const int delayBufferSamples = delayBuffer.getNumSamples();
                    const int division = type == SamplePadFxType::delay ? 8 : 4;
                    const double targetDelaySeconds = bpm > 1.0
                        ? (60.0 / bpm) * (4.0 / (double)division)
                        : 0.35;

                    const int delaySamples = juce::jlimit(1, delayBufferSamples - 1, (int)std::round(targetDelaySeconds * sampleRate));
                    const float feedback = juce::jlimit(0.0f, 0.92f, fxDelayFeedback.load(std::memory_order_relaxed));
                    const bool pingPong = type == SamplePadFxType::delayQuarterPingPong;
                    const float wetGain = amount * 0.75f;
                    float* delayMemoryL = delayBuffer.getWritePointer(0);
                    float* delayMemoryR = delayBuffer.getWritePointer(1);
                    const float* slotL = slotBuffer.getReadPointer(0);
                    const float* slotR = slotBuffer.getReadPointer(1);
                    float* outL = renderBuffer.getWritePointer(0);
                    float* outR = renderBuffer.getWritePointer(1);
                    int writePos = delayWritePositions[(size_t)padIndex][(size_t)slot];

                    for (int i = 0; i < numSamples; ++i)
                    {
                        int readPos = writePos - delaySamples;
                        if (readPos < 0)
                            readPos += delayBufferSamples;

                        const float readL = delayMemoryL[readPos];
                        const float readR = delayMemoryR[readPos];
                        const float dryL = slotL[i];
                        const float dryR = slotR[i];
                        const float input = 0.5f * (dryL + dryR);
                        const float wetL = readL * wetGain;
                        const float wetR = readR * wetGain;

                        outL[i] += wetL;
                        outR[i] += wetR;
                        slotBuffer.setSample(0, i, dryL + wetL);
                        slotBuffer.setSample(1, i, dryR + wetR);

                        if (pingPong)
                        {
                            delayMemoryL[writePos] = input + readR * feedback;
                            delayMemoryR[writePos] = input + readL * feedback;
                        }
                        else
                        {
                            const float monoDelay = 0.5f * (readL + readR);
                            delayMemoryL[writePos] = input + monoDelay * feedback;
                            delayMemoryR[writePos] = input + monoDelay * feedback;
                        }

                        if (++writePos >= delayBufferSamples)
                            writePos = 0;
                    }

                    delayWritePositions[(size_t)padIndex][(size_t)slot] = writePos;
                    break;
                }
            }
        }

        for (int targetSlot = 0; targetSlot < numSamplePadFxSlots; ++targetSlot)
        {
            if (!chainRoutes[(size_t)slot][(size_t)targetSlot])
                continue;

            auto& targetBuffer = slotInputBuffers[(size_t)padIndex][(size_t)targetSlot];
            if (targetBuffer.getNumChannels() < 2 || targetBuffer.getNumSamples() < numSamples)
                continue;

            targetBuffer.addFrom(0, 0, slotBuffer, 0, 0, numSamples);
            targetBuffer.addFrom(1, 0, slotBuffer, 1, 0, numSamples);
            slotHasInput[(size_t)targetSlot] = true;
        }
    }
    }
}


void NinjamVst3AudioProcessor::applySamplePadInsertFx(int numSamples,
                                                      double blockStartBeat,
                                                      double samplesPerBeat,
                                                      int bpi)
{
    processSamplePadInsertFxRoute(numSamples,
                                  samplePadsRenderBuffer,
                                  samplePadPerPadFxSlotInputBuffers,
                                  samplePadPerPadDjFilters,
                                  samplePadPerPadDjBpFilters,
                                  samplePadPerPadPhasers,
                                  samplePadPerPadReverbs,
                                  samplePadPerPadDelayBuffers,
                                  samplePadPerPadDelayWritePositions,
                                  blockStartBeat,
                                  samplesPerBeat,
                                  bpi);

    processSamplePadInsertFxRoute(numSamples,
                                  samplePadsMonitorRenderBuffer,
                                  samplePadMonitorPerPadFxSlotInputBuffers,
                                  samplePadMonitorPerPadDjFilters,
                                  samplePadMonitorPerPadDjBpFilters,
                                  samplePadMonitorPerPadPhasers,
                                  samplePadMonitorPerPadReverbs,
                                  samplePadMonitorPerPadDelayBuffers,
                                  samplePadMonitorPerPadDelayWritePositions,
                                  blockStartBeat,
                                  samplesPerBeat,
                                  bpi);
}
float NinjamVst3AudioProcessor::getSamplePadFxSendAmount(SamplePadFxType type) const
{
    float amount = 0.0f;
    for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
    {
        const auto slotType = sanitizeSamplePadFxType(samplePadFxSlotTypes[(size_t)slot].load(std::memory_order_relaxed));
        if (slotType == type)
            amount = juce::jlimit(0.0f, 1.0f,
                                  amount + samplePadFxSlotAmounts[(size_t)slot].load(std::memory_order_relaxed));
    }
    return amount;
}

bool NinjamVst3AudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    auto mainIn = layouts.getMainInputChannelSet();
    if (mainIn.isDisabled())
        return false;
    if (mainIn != juce::AudioChannelSet::stereo()
        && mainIn != juce::AudioChannelSet::mono())
        return false;

    for (int i = 1; i < layouts.inputBuses.size(); ++i)
    {
        if (!layouts.inputBuses[i].isDisabled()
            && layouts.inputBuses[i] != juce::AudioChannelSet::stereo()
            && layouts.inputBuses[i] != juce::AudioChannelSet::mono())
            return false;
    }

    for (int i = 1; i < layouts.outputBuses.size(); ++i)
    {
        if (!layouts.outputBuses[i].isDisabled() && layouts.outputBuses[i] != juce::AudioChannelSet::stereo())
            return false;
    }

    return true;
}

bool NinjamVst3AudioProcessor::canAddBus (bool isInput) const
{
    const int maxBuses = isInput ? maxAudioInputBuses : maxAudioOutputBuses;
    return getBusCount (isInput) < maxBuses;
}

bool NinjamVst3AudioProcessor::canRemoveBus (bool isInput) const
{
    return getBusCount (isInput) > 1;
}

bool NinjamVst3AudioProcessor::canApplyBusCountChange (bool isInput, bool isAddingBuses, BusProperties& outNewBusProperties)
{
    if (! isAddingBuses)
        return canRemoveBus (isInput);

    if (! canAddBus (isInput))
        return false;

    const int newBusNumber = getBusCount (isInput) + 1;
    outNewBusProperties.busName = juce::String (isInput ? "Input " : "Output ") + juce::String (newBusNumber);
    outNewBusProperties.defaultLayout = juce::AudioChannelSet::stereo();
    outNewBusProperties.isActivatedByDefault = true;
    return true;
}

void NinjamVst3AudioProcessor::showServerLicenseAgreementAsync(const juce::String& serverName,
                                                               const juce::String& licenseText,
                                                               const juce::String& settingsKey)
{
    auto* editor = dynamic_cast<juce::Component*>(getActiveEditor());
    juce::Component::SafePointer<juce::Component> safeEditor(editor);

    if (safeEditor == nullptr)
    {
        serverLicenseDialogActive.store(false, std::memory_order_release);
        return;
    }

    const juce::String reconnectHost = pendingConnectHost.trim().isNotEmpty() ? pendingConnectHost.trim()
                                                                              : serverName.trim();
    const juce::String reconnectUser = pendingConnectOriginalUser;
    const juce::String reconnectPass = pendingConnectPass;

    auto finishDecision = [this, safeEditor, reconnectHost, reconnectUser, reconnectPass, settingsKey](int finalResult,
                                                                                                      bool rememberAlways) mutable
    {
        if (safeEditor == nullptr)
            return;

        serverLicenseDialogActive.store(false, std::memory_order_release);

        if (finalResult == serverLicenseResultAgree)
        {
            if (rememberAlways)
            {
                juce::PropertiesFile settings(makeNinjamplusSettingsOptions());
                settings.setValue(settingsKey, true);
                settings.saveIfNeeded();
            }
            else
            {
                pendingServerLicenseApprovalKey = settingsKey;
            }

            if (reconnectHost.trim().isNotEmpty())
                connectToServer(reconnectHost, reconnectUser, reconnectPass);

            return;
        }

        const juce::String current = currentServer.trim();
        const juce::String pending = pendingConnectHost.trim();
        if (reconnectHost.trim().isEmpty()
            || current.equalsIgnoreCase(reconnectHost.trim())
            || pending.equalsIgnoreCase(reconnectHost.trim()))
            disconnectFromServer();
    };

    launchServerLicenseAgreementDialog(serverName, licenseText,
        [this, safeEditor, finishDecision](int result) mutable
        {
            if (safeEditor == nullptr)
                return;

            if (result == serverLicenseResultAgreeAlways)
            {
                launchServerLicenseAlwaysConfirmDialog(
                    [safeEditor, finishDecision](int confirmResult) mutable
                    {
                        if (safeEditor == nullptr)
                            return;

                        const bool agreedAlways = confirmResult == serverLicenseResultAgree;
                        finishDecision(agreedAlways ? serverLicenseResultAgree : serverLicenseResultDisagree,
                                       agreedAlways);
                    });
                return;
            }

            finishDecision(result == serverLicenseResultAgree ? serverLicenseResultAgree
                                                              : serverLicenseResultDisagree,
                           false);
        });
}

int NinjamVst3AudioProcessor::handleServerLicenseAgreement(const juce::String& licenseText)
{
    const juce::String trimmedLicense = licenseText.trim();
    if (trimmedLicense.isEmpty())
        return 1;

    if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        disconnectAfterLicenseRejected.store(true, std::memory_order_release);
        return 0;
    }

    juce::String serverName = currentServer.trim();
    if (serverName.isEmpty())
        serverName = pendingConnectHost.trim();

    const juce::String serverKey = normaliseServerLicenseServerKey(serverName);
    const juce::String settingsKey = makeServerLicenseAlwaysSettingsKey(serverKey, trimmedLicense);

    if (pendingServerLicenseApprovalKey == settingsKey)
    {
        pendingServerLicenseApprovalKey.clear();
        return 1;
    }

    juce::PropertiesFile settings(makeNinjamplusSettingsOptions());
    if (settings.getBoolValue(settingsKey, false))
        return 1;

    if (!serverLicenseDialogActive.exchange(true, std::memory_order_acq_rel))
        showServerLicenseAgreementAsync(serverName, trimmedLicense, settingsKey);

    disconnectAfterLicenseRejected.store(true, std::memory_order_release);
    return 0;
}

int NinjamVst3AudioProcessor::LicenseAgreementCallback(void* userData, const char* licensetext)
{
    if (auto* processor = static_cast<NinjamVst3AudioProcessor*>(userData))
        return processor->handleServerLicenseAgreement(juce::String::fromUTF8(licensetext != nullptr ? licensetext : ""));

    return 0;
}

void NinjamVst3AudioProcessor::processSyncSignal(const juce::String& sender, const juce::String& type, const juce::String& payload)
{
    const bool isVdoSyncSignal = type == "intervalSyncTag"
                              || type == "intervalTransportProbe"
                              || type == "intervalTransportProbeAck"
                              || type == "videoTimingChange";
    if (isVdoSyncSignal
        && (!vdoVideoSyncEnabled.load(std::memory_order_relaxed)
            || ninjamZapVideoEnabled.load(std::memory_order_relaxed)))
        return;

    if (type == "chatAttachment")
    {
        juce::String payloadUserId;
        juce::String appFamily;
        juce::String kind = "link";
        juce::String url;
        const juce::var parsed = juce::JSON::parse(payload);
        if (auto* obj = parsed.getDynamicObject())
        {
            if (obj->hasProperty("userId"))
                payloadUserId = obj->getProperty("userId").toString();
            if (obj->hasProperty("appFamily"))
                appFamily = obj->getProperty("appFamily").toString();
            if (obj->hasProperty("kind"))
                kind = obj->getProperty("kind").toString();
            if (obj->hasProperty("url"))
                url = obj->getProperty("url").toString();
        }

        if (appFamily.isNotEmpty() && appFamily != opusSyncAppFamily)
            return;

        const juce::String senderKey = normaliseOpusPeerId(payloadUserId.isNotEmpty() ? payloadUserId : sender);
        const juce::String localUserKey = normaliseOpusPeerId(currentUser);
        if (senderKey.isEmpty() || senderKey == localUserKey || !isHttpOrHttpsChatUrl(url))
            return;

        juce::String senderLabel = normaliseChatTargetNick(sender);
        if (senderLabel.isEmpty())
            senderLabel = senderKey;

        {
            juce::ScopedLock lock(chatLock);
            chatHistory.add(makeRichChatLine(senderLabel, kind, url.trim()));
            chatSenders.add(senderLabel);
            chatRevision.fetch_add(1);
            trimChatArrays(chatHistory, chatSenders);
        }
        return;
    }
    if (type == "vdoRoom")
    {
        juce::String payloadUserId;
        juce::String appFamily;
        juce::String scope;
        juce::String serverKey;
        juce::String room;
        const juce::var parsed = juce::JSON::parse(payload);
        if (auto* obj = parsed.getDynamicObject())
        {
            payloadUserId = obj->getProperty("userId").toString();
            appFamily = obj->getProperty("appFamily").toString();
            scope = obj->getProperty("scope").toString();
            serverKey = obj->getProperty("serverKey").toString().trim().toLowerCase();
            room = makeUrlSafeVdoRoomToken(obj->getProperty("room").toString());
        }

        if (appFamily.isNotEmpty() && appFamily != opusSyncAppFamily)
            return;

        const juce::String localUserKey = normaliseOpusPeerId(currentUser);
        const juce::String senderKey = normaliseOpusPeerId(payloadUserId.isNotEmpty() ? payloadUserId : sender);
        if (senderKey.isNotEmpty() && senderKey == localUserKey)
            return;

        const VdoRoomResolution currentResolution = resolveVdoRoomForServer(currentServer);
        const bool isCurrentSessionPayload = scope == "currentNinjamSession";
        if ((!isCurrentSessionPayload && serverKey.isEmpty()) || currentResolution.serverKey.isEmpty() || room.isEmpty())
            return;

        // This signal arrived through the server to which we are currently connected.
        // Apply it to the local endpoint key even when peers used different aliases for
        // that server, such as localhost/127.0.0.1 and its LAN IP address.
        noteRemoteVideoRoomActivity();

        if (rememberVdoRoomNameForServer(currentResolution.serverKey, room, false))
        {
            juce::String senderLabel = normaliseChatTargetNick(sender);
            if (senderLabel.isEmpty())
                senderLabel = senderKey.isNotEmpty() ? senderKey : "Another NINJAMplus user";
            addSystemChatLine(senderLabel + " set VDO room to " + room + ".");
        }
        return;
    }
    if (type == "chatStyle")
    {
        juce::String payloadUserId;
        juce::String appFamily;
        juce::String colourKey;
        const juce::var parsed = juce::JSON::parse(payload);
        if (auto* obj = parsed.getDynamicObject())
        {
            if (obj->hasProperty("userId"))
                payloadUserId = obj->getProperty("userId").toString();
            if (obj->hasProperty("appFamily"))
                appFamily = obj->getProperty("appFamily").toString();
            if (obj->hasProperty("colourKey"))
                colourKey = obj->getProperty("colourKey").toString();
        }

        if (appFamily.isNotEmpty() && appFamily != opusSyncAppFamily)
            return;

        const juce::String senderKey = normaliseOpusPeerId(payloadUserId.isNotEmpty() ? payloadUserId : sender);
        const juce::String localUserKey = normaliseOpusPeerId(currentUser);
        if (senderKey.isEmpty() || senderKey == localUserKey)
            return;

        const juce::String normalisedColourKey = normaliseChatColourKey(colourKey);
        bool changed = false;
        {
            const juce::ScopedLock lock(chatStyleLock);
            changed = chatColourKeyByUser[senderKey] != normalisedColourKey;
            chatColourKeyByUser[senderKey] = normalisedColourKey;
        }

        if (changed)
            chatRevision.fetch_add(1);
        return;
    }
    if (type == "zapVideoTiming")
    {
        juce::String payloadUserId;
        juce::String appFamily;
        int channelIndex = -1;
        double captureQueueMs = 0.0;
        double encodeMs = 0.0;
        const juce::var parsed = juce::JSON::parse(payload);
        if (auto* obj = parsed.getDynamicObject())
        {
            payloadUserId = obj->getProperty("userId").toString();
            appFamily = obj->getProperty("appFamily").toString();
            channelIndex = (int)obj->getProperty("channelIndex");
            captureQueueMs = (double)obj->getProperty("captureQueueMs");
            encodeMs = (double)obj->getProperty("encodeMs");
        }

        if (appFamily.isNotEmpty() && appFamily != opusSyncAppFamily)
            return;

        const juce::String senderName = payloadUserId.isNotEmpty() ? payloadUserId : sender;
        const juce::String localUserKey = normaliseOpusPeerId(currentUser);
        if (senderName.isEmpty()
            || normaliseOpusPeerId(senderName) == localUserKey
            || channelIndex < 0)
            return;

        noteRemoteVideoRoomActivity();

        ZapVideoSenderTiming timing;
        timing.captureQueueMs = juce::jlimit(0.0, 500.0, captureQueueMs);
        timing.encodeMs = juce::jlimit(0.0, 500.0, encodeMs);
        timing.updatedMs = juce::Time::getMillisecondCounterHiRes();
        const juce::ScopedLock lock(zapVideoFrameLock);
        zapVideoSenderTimingByStream[senderName + ":" + juce::String(channelIndex)] = timing;
        return;
    }
    if (type == "intervalTransportProbe")
    {
        juce::String payloadUserId;
        juce::String probeId;
        const juce::var parsed = juce::JSON::parse(payload);
        if (auto* obj = parsed.getDynamicObject())
        {
            if (obj->hasProperty("userId"))
                payloadUserId = obj->getProperty("userId").toString();
            if (obj->hasProperty("probeId"))
                probeId = obj->getProperty("probeId").toString();
        }
        const juce::String senderKey = normaliseOpusPeerId(payloadUserId.isNotEmpty() ? payloadUserId : sender);
        const juce::String localUserKey = normaliseOpusPeerId(currentUser);
        if (probeId.isEmpty() || sender.isEmpty() || senderKey.isEmpty() || senderKey == localUserKey)
            return;

        juce::DynamicObject::Ptr ackObj = new juce::DynamicObject();
        ackObj->setProperty("type", "intervalTransportProbeAck");
        ackObj->setProperty("userId", localUserKey.isNotEmpty() ? localUserKey : currentUser);
        ackObj->setProperty("probeId", probeId);
        ackObj->setProperty("eventId", "transportProbeAck:" + probeId + ":" + juce::String(++sideSignalEventCounter));
        sendIntervalSignal("intervalTransportProbeAck", juce::JSON::toString(juce::var(ackObj.get())));
        return;
    }
    if (type == "intervalTransportProbeAck")
    {
        juce::String payloadUserId;
        juce::String probeId;
        const juce::var parsed = juce::JSON::parse(payload);
        if (auto* obj = parsed.getDynamicObject())
        {
            if (obj->hasProperty("userId"))
                payloadUserId = obj->getProperty("userId").toString();
            if (obj->hasProperty("probeId"))
                probeId = obj->getProperty("probeId").toString();
        }

        const juce::String senderKey = normaliseOpusPeerId(payloadUserId.isNotEmpty() ? payloadUserId : sender);
        const juce::String localUserKey = normaliseOpusPeerId(currentUser);
        if (probeId.isEmpty() || senderKey.isEmpty() || senderKey == localUserKey)
            return;

        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        const juce::ScopedLock lock(intervalSyncAnnouncementLock);
        auto sentIt = pendingTransportProbeSentMsById.find(probeId);
        if (sentIt == pendingTransportProbeSentMsById.end())
            return;

        const double rttMs = nowMs - sentIt->second;
        pendingTransportProbeSentMsById.erase(sentIt);
        if (!std::isfinite(rttMs) || rttMs <= 0.0 || rttMs > 4000.0)
            return;

        const int measuredRouteMs = juce::jlimit(0, 3000, (int)std::llround(rttMs * 0.5));
        const auto priorIt = remoteServerRouteLatencyMsByUser.find(senderKey);
        const int smoothedRouteMs = priorIt != remoteServerRouteLatencyMsByUser.end()
            ? juce::jlimit(0, 3000, (int)std::llround((double)priorIt->second * 0.7 + (double)measuredRouteMs * 0.3))
            : measuredRouteMs;

        remoteServerRouteLatencyMsByUser[senderKey] = smoothedRouteMs;
        lastRemoteRouteProbeSeenMsByUser[senderKey] = nowMs;
        const juce::String canonicalSenderKey = canonicalDelayUserKey(senderKey);
        if (canonicalSenderKey.isNotEmpty())
        {
            remoteServerRouteLatencyMsByUser[canonicalSenderKey] = smoothedRouteMs;
            lastRemoteRouteProbeSeenMsByUser[canonicalSenderKey] = nowMs;
        }
        return;
    }
    if (type == "midiRelay")
    {
        juce::String payloadUserId;
        MidiControllerEvent event;
        const juce::var parsed = juce::JSON::parse(payload);
        if (auto* obj = parsed.getDynamicObject())
        {
            if (obj->hasProperty("userId")) payloadUserId = obj->getProperty("userId").toString();
            if (obj->hasProperty("isController")) event.isController = (bool)obj->getProperty("isController");
            if (obj->hasProperty("midiChannel")) event.midiChannel = (int)obj->getProperty("midiChannel");
            if (obj->hasProperty("number")) event.number = (int)obj->getProperty("number");
            if (obj->hasProperty("value")) event.value = (int)obj->getProperty("value");
            if (obj->hasProperty("normalized")) event.normalized = (float)(double)obj->getProperty("normalized");
            if (obj->hasProperty("isNoteOn")) event.isNoteOn = (bool)obj->getProperty("isNoteOn");
        }

        const juce::String senderKey = normaliseOpusPeerId(payloadUserId.isNotEmpty() ? payloadUserId : sender);
        const juce::String localUserKey = normaliseOpusPeerId(currentUser);
        if (senderKey.isEmpty() || senderKey == localUserKey)
            return;

        event.midiChannel = juce::jlimit(1, 16, event.midiChannel);
        event.number = juce::jlimit(0, 127, event.number);
        event.value = juce::jlimit(0, 127, event.value);
        event.normalized = juce::jlimit(0.0f, 1.0f, event.normalized);

        bool acceptForLearn = false;
        const juce::String learnSource = getMidiLearnInputDeviceId();
        if (learnSource == "__learn_relay__" || learnSource == "__learn_relay__:*")
        {
            acceptForLearn = true;
        }
        else if (learnSource.startsWith("__learn_relay__:"))
        {
            const juce::String desired = learnSource.fromFirstOccurrenceOf("__learn_relay__:", false, false).trim();
            if (desired.isEmpty() || desired == "*")
                acceptForLearn = true;
            else
                acceptForLearn = normaliseOpusPeerId(desired) == senderKey;
        }

        if (acceptForLearn)
        {
            const juce::SpinLock::ScopedLockType learnLock(midiEventQueueLock);
            pendingMidiControllerEvents.push_back(event);
            if (pendingMidiControllerEvents.size() > 512)
                pendingMidiControllerEvents.erase(pendingMidiControllerEvents.begin(), pendingMidiControllerEvents.begin() + (long long)(pendingMidiControllerEvents.size() - 512));
        }

        const juce::SpinLock::ScopedLockType lock(inboundMidiRelayQueueLock);
        pendingInboundMidiRelayEvents.push_back(event);
        if (pendingInboundMidiRelayEvents.size() > 512)
            pendingInboundMidiRelayEvents.erase(pendingInboundMidiRelayEvents.begin(), pendingInboundMidiRelayEvents.begin() + (long long)(pendingInboundMidiRelayEvents.size() - 512));
        return;
    }
    if (type == "oscRelay")
    {
        juce::String payloadUserId;
        OscRelayEvent event;
        const juce::var parsed = juce::JSON::parse(payload);
        if (auto* obj = parsed.getDynamicObject())
        {
            if (obj->hasProperty("userId")) payloadUserId = obj->getProperty("userId").toString();
            if (obj->hasProperty("address")) event.address = obj->getProperty("address").toString();
            if (obj->hasProperty("normalized")) event.normalized = (float)(double)obj->getProperty("normalized");
            if (obj->hasProperty("binaryOn")) event.binaryOn = (bool)obj->getProperty("binaryOn");
        }

        const juce::String senderKey = normaliseOpusPeerId(payloadUserId.isNotEmpty() ? payloadUserId : sender);
        const juce::String localUserKey = normaliseOpusPeerId(currentUser);
        if (senderKey.isEmpty() || senderKey == localUserKey)
            return;

        event.senderKey = senderKey;
        event.normalized = juce::jlimit(0.0f, 1.0f, event.normalized);
        if (event.address.isEmpty())
            return;

        const juce::SpinLock::ScopedLockType lock(inboundOscRelayQueueLock);
        pendingInboundOscRelayEvents.push_back(event);
        if (pendingInboundOscRelayEvents.size() > 512)
            pendingInboundOscRelayEvents.erase(pendingInboundOscRelayEvents.begin(), pendingInboundOscRelayEvents.begin() + (long long)(pendingInboundOscRelayEvents.size() - 512));
        return;
    }
    if (type == "localInputSelect")
    {
        const juce::var parsed = juce::JSON::parse(payload);
        if (auto* obj = parsed.getDynamicObject())
        {
            const int channel = obj->hasProperty("channel") ? (int)obj->getProperty("channel") : -1;
            const int inputIndex = obj->hasProperty("inputIndex") ? (int)obj->getProperty("inputIndex") : 0;
            if (channel >= 0 && channel < maxLocalChannels)
                setLocalChannelInput(channel, inputIndex);
        }
        return;
    }
    if (type == "videoTimingChange")
    {
        juce::String payloadUserId;
        juce::String appFamily;
        juce::String eventId;
        double previousBpm = 0.0;
        double newBpm = 0.0;
        int bpi = 0;
        int timingDelayDeltaMs = 0;
        const juce::var parsed = juce::JSON::parse(payload);
        if (auto* obj = parsed.getDynamicObject())
        {
            payloadUserId = obj->getProperty("userId").toString();
            appFamily = obj->getProperty("appFamily").toString();
            eventId = obj->getProperty("eventId").toString();
            previousBpm = (double)obj->getProperty("previousBpm");
            newBpm = (double)obj->getProperty("bpm");
            bpi = (int)obj->getProperty("bpi");
            timingDelayDeltaMs = (int)obj->getProperty("timingDelayDeltaMs");
        }

        if (appFamily.isNotEmpty() && appFamily != opusSyncAppFamily)
            return;

        const juce::String senderKey = normaliseOpusPeerId(payloadUserId.isNotEmpty() ? payloadUserId : sender);
        const juce::String localUserKey = normaliseOpusPeerId(currentUser);
        if (senderKey.isEmpty() || senderKey == localUserKey || !consumeVideoTimingChangeEvent(eventId))
            return;

        noteRemoteVideoRoomActivity();

        if (videoHelperRunning.load())
        {
            const juce::String canonicalSenderKey = canonicalDelayUserKey(senderKey);
            const juce::ScopedLock lock(intervalSyncAnnouncementLock);
            const auto refreshId = ++videoBufferRefreshCounter;
            remoteVideoBufferRefreshIdByUser[senderKey] = refreshId;
            if (canonicalSenderKey.isNotEmpty())
                remoteVideoBufferRefreshIdByUser[canonicalSenderKey] = refreshId;
            lastIntervalHelperPayloadWriteMs = 0.0;
        }

        return;
    }
    if (type == "intervalSyncTag")
    {
        juce::String tag;
        juce::String payloadUserId;
        int remoteInterval = -1;
        int remoteIntervalAbsolute = -1;
        int remoteServerLatencyMs = -1;
        int remoteBpi = 0;
        int remoteBeat = -1;
        double sendOffsetMs = 0.0;
        const juce::var parsed = juce::JSON::parse(payload);
        if (auto* obj = parsed.getDynamicObject())
        {
            if (obj->hasProperty("tag"))
                tag = obj->getProperty("tag").toString();
            if (obj->hasProperty("userId"))
                payloadUserId = obj->getProperty("userId").toString();
            if (obj->hasProperty("intervalIndex"))
                remoteInterval = (int)obj->getProperty("intervalIndex");
            if (obj->hasProperty("intervalAbsolute"))
                remoteIntervalAbsolute = (int)obj->getProperty("intervalAbsolute");
            if (obj->hasProperty("serverLatencyMs"))
                remoteServerLatencyMs = (int)obj->getProperty("serverLatencyMs");
            if (obj->hasProperty("bpi"))
                remoteBpi = (int)obj->getProperty("bpi");
            if (obj->hasProperty("beatIndex"))
                remoteBeat = (int)obj->getProperty("beatIndex");
            if (obj->hasProperty("sendOffsetMs"))
                sendOffsetMs = (double)obj->getProperty("sendOffsetMs");
        }
        const int localInterval = getIntervalIndex();
        juce::String status = "Interval Tag " + sender;
        if (remoteInterval >= 0)
        {
            const int delta = remoteInterval - localInterval;
            status << " remoteInt " << juce::String(remoteInterval)
                   << " localInt " << juce::String(localInterval)
                   << " d=" << juce::String(delta);
        }
        if (remoteBeat >= 0 && remoteBpi > 0)
            status << " beat " << juce::String(remoteBeat + 1) << "/" << juce::String(remoteBpi);
        if (tag.isNotEmpty())
            status << " tag " << tag;
        setIntervalSyncStatusText(status);

        const int localBpi = juce::jmax(1, getBPI());
        const int markerBpi = remoteBpi > 0 ? remoteBpi : localBpi;
        if (remoteInterval >= 0 && isIntervalSyncMarkerBeat(remoteBeat, markerBpi))
        {
            const juce::String localUserKey = normaliseOpusPeerId(currentUser);
            const juce::String senderKey = normaliseOpusPeerId(payloadUserId.isNotEmpty() ? payloadUserId : sender);
            if (senderKey.isNotEmpty() && senderKey != localUserKey)
            {
                const double signalSeenMs = juce::Time::getMillisecondCounterHiRes();
                // If this is a redundant retransmission (sendOffsetMs > 0),
                // adjust the received time back by the offset so the buffer
                // calculation is based on when the original was sent, not
                // when this duplicate arrived.
                const double effectiveSignalSeenMs = signalSeenMs - sendOffsetMs;
                noteRemoteVideoRoomActivity(effectiveSignalSeenMs);
                {
                    const juce::ScopedLock lock(intervalSyncAnnouncementLock);
                    lastRemoteIntervalSignalSeenMsByUser[senderKey] = effectiveSignalSeenMs;
                    const juce::String canonicalSenderKey = canonicalDelayUserKey(senderKey);
                    if (canonicalSenderKey.isNotEmpty())
                        lastRemoteIntervalSignalSeenMsByUser[canonicalSenderKey] = effectiveSignalSeenMs;
                }
                const bool bpiMatches = (remoteBpi <= 0 || remoteBpi == localBpi);
                if (!bpiMatches)
                    return;
                const int remoteMarkerBeat = getIntervalSyncMarkerBeatForBeat(remoteBeat, localBpi);
                const int remoteSourceInterval = remoteIntervalAbsolute >= 0 ? remoteIntervalAbsolute : remoteInterval;
                const long long remoteMarkerKey = makeIntervalSyncMarkerKey(remoteSourceInterval, remoteMarkerBeat);
                bool shouldStorePending = false;
                const juce::String displaySender = sender.isNotEmpty() ? sender : (payloadUserId.isNotEmpty() ? payloadUserId : senderKey);
                const long long receivedSampleCount = intervalSyncSampleCounter.load(std::memory_order_relaxed);

                // Check if this sync tag is from an interval that has already passed.
                // If the tag's interval is more than 1 behind our current interval, it
                // arrived too late to be useful for this interval's calculation.
                // We still update signal-seen time and server latency (so the indicator
                // knows we're hearing from this user), but we don't store it as pending
                // because processing it at the next boundary would give a buffer that's
                // off by a full interval. The existing firm delay persists instead.
                const int localIntervalNow = getDisplayIntervalIndex();
                const bool tagIsLate = (remoteIntervalAbsolute >= 0 && localIntervalNow >= 0
                                        && remoteIntervalAbsolute < localIntervalNow - 1);

                {
                    const juce::ScopedLock lock(intervalSyncAnnouncementLock);
                    const int clampedRemoteServerLatencyMs = remoteServerLatencyMs >= 0 ? juce::jlimit(0, 3000, remoteServerLatencyMs) : -1;
                    if (clampedRemoteServerLatencyMs >= 0)
                    {
                        lastRemoteServerLatencyMsByUser[senderKey] = clampedRemoteServerLatencyMs;
                        const juce::String canonicalSenderKey = canonicalDelayUserKey(senderKey);
                        if (canonicalSenderKey.isNotEmpty())
                            lastRemoteServerLatencyMsByUser[canonicalSenderKey] = clampedRemoteServerLatencyMs;
                    }
                    auto it = lastAnnouncedRemoteIntervalByUser.find(senderKey);
                    if (it != lastAnnouncedRemoteIntervalByUser.end() && remoteMarkerKey + intervalSyncMarkerKeyBeatStride < it->second)
                        remoteLatencyAverageByUser.erase(senderKey);
                    if (it == lastAnnouncedRemoteIntervalByUser.end() || it->second != remoteMarkerKey)
                    {
                        // Only store as pending if the tag is not from a past interval.
                        // Late tags would produce a buffer offset by a full interval.
                        shouldStorePending = !tagIsLate;
                        // Only record the marker key if we actually stored it as pending.
                        // If we skip a late tag, we want redundant retransmissions
                        // (which may arrive in time) to still have a chance.
                        if (shouldStorePending)
                            lastAnnouncedRemoteIntervalByUser[senderKey] = remoteMarkerKey;
                    }
                }

                if (shouldStorePending)
                {
                    const juce::ScopedLock lock(intervalSyncAnnouncementLock);
                    const juce::String pendingKey = senderKey + ":" + juce::String((juce::int64)remoteMarkerKey);
                    auto& pending = pendingRemoteIntervalStartsByUser[pendingKey];
                    pending.remoteInterval = remoteInterval;
                    pending.remoteIntervalAbsolute = remoteIntervalAbsolute;
                    pending.remoteBeat = remoteMarkerBeat;
                    pending.remoteBpi = remoteBpi;
                    pending.remoteServerLatencyMs = remoteServerLatencyMs >= 0 ? juce::jlimit(0, 3000, remoteServerLatencyMs) : -1;
                    auto routeIt = remoteServerRouteLatencyMsByUser.find(senderKey);
                    if (routeIt != remoteServerRouteLatencyMsByUser.end())
                        pending.serverRouteLatencyMs = juce::jmax(0, routeIt->second);
                    else
                    {
                        const juce::String canonicalSenderKey = canonicalDelayUserKey(senderKey);
                        if (canonicalSenderKey.isNotEmpty())
                        {
                            auto canonicalRouteIt = remoteServerRouteLatencyMsByUser.find(canonicalSenderKey);
                            if (canonicalRouteIt != remoteServerRouteLatencyMsByUser.end())
                                pending.serverRouteLatencyMs = juce::jmax(0, canonicalRouteIt->second);
                        }
                    }
                    pending.senderKey = senderKey;
                    pending.displaySender = displaySender;
                    pending.receivedSampleCount = receivedSampleCount;
                    pending.receivedAtMs = effectiveSignalSeenMs;
                }
            }
        }
        return;
    }
}

void NinjamVst3AudioProcessor::ChatMessage_Callback(void* userData, NJClient* inst, const char** parms, int nparms)
{
    auto* self = static_cast<NinjamVst3AudioProcessor*>(userData);
    auto processOpusSyncSupport = [self](const juce::String& sender, const juce::String& payload, juce::String* outEventId) -> bool
    {
        juce::var parsed = juce::JSON::parse(payload);
        bool supportsOpus = false;
        bool multiChanEnabled = false;
        int peerNumChannels = 1;
        int peerOpusBaseChannel = 1;
        int peerPackedChannelCount = 0;
        std::array<int, maxLocalChannels> peerChannelWidths {};
        juce::StringArray peerChannelNames;
        juce::String userId = normaliseOpusPeerId(sender);
        juce::String clientId;
        juce::String appFamily;
        int handshakeVersion = 0;
        juce::String runtimeFormat;
        juce::String pluginVersion;
        if (auto* obj = parsed.getDynamicObject())
        {
            const juce::String supports = obj->getProperty("supportsOpus").toString();
            supportsOpus = supports == "1" || supports.equalsIgnoreCase("true");
            const juce::String enabledStr = obj->getProperty("enabled").toString();
            multiChanEnabled = enabledStr == "1" || enabledStr.equalsIgnoreCase("true");
            const juce::var numChVar = obj->getProperty("numChannels");
            if (!numChVar.isVoid()) peerNumChannels = juce::jmax(1, (int)numChVar);
            const juce::var packedVar = obj->getProperty("packedChannelCount");
            if (!packedVar.isVoid()) peerPackedChannelCount = juce::jmax(0, (int)packedVar);
            const juce::var opusBaseVar = obj->getProperty("opusBaseChannel");
            if (!opusBaseVar.isVoid()) peerOpusBaseChannel = juce::jlimit(1, 31, (int)opusBaseVar);
            if (auto* widthsArray = obj->getProperty("channelWidths").getArray())
            {
                for (int i = 0; i < juce::jmin(maxLocalChannels, widthsArray->size()); ++i)
                    peerChannelWidths[(size_t)i] = juce::jlimit(1, 2, (int)widthsArray->getReference(i));
            }
            if (auto* namesArray = obj->getProperty("channelNames").getArray())
            {
                for (const auto& item : *namesArray)
                    peerChannelNames.add(item.toString());
            }
            juce::String payloadUserId = obj->getProperty("userId").toString();
            if (payloadUserId.isNotEmpty())
                userId = normaliseOpusPeerId(payloadUserId);
            clientId = obj->getProperty("clientId").toString().trim();
            appFamily = obj->getProperty("appFamily").toString().trim();
            handshakeVersion = (int)obj->getProperty("handshakeVersion");
            runtimeFormat = obj->getProperty("runtimeFormat").toString().trim();
            pluginVersion = obj->getProperty("pluginVersion").toString().trim();
            if (outEventId != nullptr)
                *outEventId = obj->getProperty("eventId").toString();
        }
        else
            return false;

        const bool isLocalClient = clientId.isNotEmpty() ? (clientId == self->opusSyncInstanceId)
                                                          : (userId == normaliseOpusPeerId(self->currentUser));
        const bool sameAppFamily = appFamily.isEmpty() || appFamily == opusSyncAppFamily;
        const bool compatibleHandshake = handshakeVersion <= 0 || handshakeVersion == opusSyncHandshakeVersion;
        const juce::String peerKey = clientId.isNotEmpty() ? clientId : userId;
        if (peerKey.isNotEmpty() && userId.isNotEmpty() && !isLocalClient)
        {
            bool recognizedNow = false;
            juce::String recognizedMessage;
            {
                juce::ScopedLock lock(self->opusSyncPeerLock);
                if (supportsOpus && sameAppFamily && compatibleHandshake)
                {
                    const bool wasKnown = self->opusSyncPeers.find(peerKey) != self->opusSyncPeers.end();
                    auto& peer = self->opusSyncPeers[peerKey];
                    const bool wasMultiChan = peer.multiChanEnabled;
                    peer.userId = userId;
                    peer.supportsOpus = true;
                    peer.multiChanEnabled = multiChanEnabled;
                    peer.numChannels = peerNumChannels;
                    peer.opusBaseChannel = peerOpusBaseChannel;
                    peer.packedChannelCount = peerPackedChannelCount;
                    peer.channelWidths = peerChannelWidths;
                    peer.channelNames = peerChannelNames;
                    peer.appFamily = appFamily;
                    peer.handshakeVersion = handshakeVersion;
                    peer.runtimeFormat = runtimeFormat;
                    peer.pluginVersion = pluginVersion;
                    peer.lastSeenMs = juce::Time::getMillisecondCounterHiRes();
                    njplus_debug_log("PEERBROADCAST sender='%s' userId='%s' supports=%d enabled=%d numCh=%d packed=%d base=%d", sender.toRawUTF8(), userId.toRawUTF8(), supportsOpus, multiChanEnabled, peerNumChannels, peerPackedChannelCount, peerOpusBaseChannel);
                    juce::String peerLabel = sender.isNotEmpty() ? sender : userId;
                    if (!wasKnown)
                    {
                        juce::String peerInfo = peer.runtimeFormat;
                        if (peer.pluginVersion.isNotEmpty())
                        {
                            if (peerInfo.isNotEmpty())
                                peerInfo << " ";
                            peerInfo << peer.pluginVersion;
                        }
                        recognizedMessage = "Multi Client Detected: " + peerLabel;
                        if (peerInfo.isNotEmpty())
                            recognizedMessage << " (" << peerInfo << ")";
                        if (multiChanEnabled)
                            recognizedMessage << " [MultiChannel ON]";
                        recognizedNow = true;
                    }
                    else if (multiChanEnabled && !wasMultiChan)
                    {
                        recognizedMessage = "MultiChannel Detected: " + peerLabel;
                        recognizedNow = true;
                    }
                    else if (!multiChanEnabled && wasMultiChan)
                    {
                        recognizedMessage = "MultiChannel Off: " + peerLabel;
                        recognizedNow = true;
                    }
                }
                else
                    self->opusSyncPeers.erase(peerKey);
            }
            if (recognizedNow)
            {
                juce::MessageManager::callAsync([self, recognizedMessage]()
                {
                    juce::ScopedLock lock(self->chatLock);
                    self->chatHistory.add(recognizedMessage);
                    self->chatSenders.add("");
                    self->chatRevision.fetch_add(1);
                    if (self->chatHistory.size() > 100)
                    {
                        self->chatHistory.removeRange(0, self->chatHistory.size() - 100);
                        self->chatSenders.removeRange(0, juce::jmax(0, self->chatSenders.size() - 100));
                    }
                });
            }
        }
        return true;
    };
    auto processInboundSideSignal = [self, &processOpusSyncSupport](const juce::String& sender, const juce::String& type, const juce::String& payload, juce::String* outEventId) -> bool
    {
        if (type == "mobileHotspotKeepalive")
            return true;
        if (type == "opusSyncSupport")
            return processOpusSyncSupport(sender, payload, outEventId);
        juce::ignoreUnused(outEventId);
        self->processSyncSignal(sender, type, payload);
        return true;
    };
    // nparms is the static array size (always 5); count only non-null entries
    {
        int actualNparms = 0;
        while (actualNparms < nparms && parms[actualNparms] != nullptr)
            ++actualNparms;
        nparms = actualNparms;
    }
    if (nparms > 0)
    {
        auto paramUtf8 = [](const char* raw) -> juce::String
        {
            return raw != nullptr ? juce::String::fromUTF8(raw) : juce::String();
        };

        juce::String cmd = paramUtf8(parms[0]);
        auto applyServerCaps = [self](const juce::String& capsText)
        {
            const juce::String caps = capsText.toLowerCase();
            const bool hasHiddenSignalCap = caps.contains("video_signal_v2")
                                         || caps.contains("pro_video_v2");
            const bool hasOpusSyncCap = caps.contains("opus_sync_v2")
                                     || caps.contains("hd_audio_v2")
                                     || caps.contains("hd_sync_v2");
            if (hasHiddenSignalCap)
                self->ninjamSideSignalServerSupported.store(true, std::memory_order_relaxed);
            self->opusSyncServerSupported.store(hasOpusSyncCap);
        };
        juce::String line;
        if (cmd == "SERVER_CAPS" && nparms >= 2)
        {
            applyServerCaps(paramUtf8(parms[1]));
            return;
        }
        bool isSideSignalCmd = (cmd == "SIDE_SIGNAL_FROM" && nparms >= 4)
                               || (cmd == "SIDE_SIGNAL" && nparms >= 4)
                               || (cmd == "VIDEO_SIGNAL_FROM" && nparms >= 4)
                               || (cmd == "VIDEO_SIGNAL" && nparms >= 4);
        if (isSideSignalCmd)
        {
            juce::String sender;
            juce::String type;
            juce::String payload;
            sender = nparms >= 2 ? paramUtf8(parms[1]) : juce::String();
            type = nparms >= 3 ? paramUtf8(parms[nparms - 2]) : juce::String();
            payload = nparms >= 2 ? paramUtf8(parms[nparms - 1]) : juce::String();
            if (type.isEmpty() || payload.isEmpty())
                return;

            processInboundSideSignal(sender, type, payload, nullptr);
            return;
        }
        if ((cmd == "MSG" || cmd == "PRIVMSG") && nparms >= 3)
        {
            const juce::String sender = paramUtf8(parms[1]);
            const juce::String messageText = paramUtf8(parms[2]);
            const juce::String trimmedText = messageText.trim();
            if (sender == "*" && trimmedText.startsWithIgnoreCase("SERVER_CAPS"))
            {
                juce::String capsText = trimmedText.fromFirstOccurrenceOf("SERVER_CAPS", false, true).trim();
                if (capsText.startsWithChar(':'))
                    capsText = capsText.substring(1).trim();
                applyServerCaps(capsText);
                return;
            }
            if (messageText.startsWith(opusSyncChatPrefix))
            {
                const juce::String payload = messageText.fromFirstOccurrenceOf(opusSyncChatPrefix, false, false);
                if (processOpusSyncSupport(sender, payload, nullptr))
                {
                    return;
                }
            }
            bool isSideSignalChat = messageText.startsWith(sideSignalChatPrefix);
            if (isSideSignalChat)
            {
                const char* signalPrefix = sideSignalChatPrefix;
                const juce::String wrapperJson = messageText.fromFirstOccurrenceOf(signalPrefix, false, false);
                juce::var wrapped = juce::JSON::parse(wrapperJson);
                if (auto* wrappedObj = wrapped.getDynamicObject())
                {
                    const juce::String type = wrappedObj->getProperty("type").toString();
                    const juce::String payload = wrappedObj->getProperty("payload").toString();
                    if (type.isNotEmpty() && payload.isNotEmpty())
                    {
                            if (processInboundSideSignal(sender, type, payload, nullptr))
                            {
                                return;
                            }
                    }
                }
            }
        }

        auto cleanName = [&paramUtf8](const char* raw) -> juce::String {
            return normaliseChatTargetNick(paramUtf8(raw));
        };

        juce::String lineSender;
        juce::String linePrefix;
        juce::String lineBody;
        bool shouldTranslateBody = false;
        if (cmd == "MSG" && nparms >= 3)
        {
            // Suppress server echo of our own messages
            if (normaliseChatTargetNick(paramUtf8(parms[1])) == normaliseChatTargetNick(self->currentUser))
                return;
            juce::String name = cleanName(parms[1]);
            linePrefix = name + ": ";
            lineBody = paramUtf8(parms[2]);
            line = linePrefix + lineBody;
            lineSender = name;
            shouldTranslateBody = true;
        }
        else if (cmd == "PRIVMSG" && nparms >= 3)
        {
            juce::String name = cleanName(parms[1]);
            linePrefix = "(Private) " + name + ": ";
            lineBody = paramUtf8(parms[2]);
            line = linePrefix + lineBody;
            lineSender = name;
            shouldTranslateBody = true;
        }
        else if (cmd == "TOPIC" && nparms >= 2)
            line = "Topic: " + paramUtf8(parms[1]);
        else if (cmd == "JOIN" && nparms >= 2)
        {
            line = cleanName(parms[1]) + " has joined.";
        }
        else if (cmd == "PART" && nparms >= 2)
             line = cleanName(parms[1]) + " has left.";
        else
        {
            line = cmd;
            for (int i=1; i<nparms; ++i)
                if (parms[i]) line += " " + paramUtf8(parms[i]);
        }
        {
            const juce::String stored = line;
            // Defer chat history insertion to the message thread to avoid chatLock
            // contention between the audio thread and UI reads.
            juce::MessageManager::callAsync([self, stored, lineSender]()
            {
                juce::ScopedLock lock(self->chatLock);
                self->chatHistory.add(stored);
                self->chatSenders.add(lineSender);
                self->chatRevision.fetch_add(1);
                if (self->chatHistory.size() > 100)
                {
                    self->chatHistory.removeRange(0, self->chatHistory.size() - 100);
                    self->chatSenders.removeRange(0, juce::jmax(0, self->chatSenders.size() - 100));
                }
            });
        }

        if (shouldTranslateBody)
            self->enqueueAsyncTranslation(line, lineSender, linePrefix, lineBody);

    }
}

void NinjamVst3AudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    juce::AudioPlayHead::CurrentPositionInfo hostInfoAtBlock;
    bool gotHostPosition = false;
    if (auto* playHead = getPlayHead())
    {
        juce::AudioPlayHead::CurrentPositionInfo info;
        if (copyPlayHeadPositionToCurrentInfo(*playHead, info))
        {
            gotHostPosition = true;
            hostInfoAtBlock = info;
            const juce::ScopedLock lock(transportLock);
            lastHostPosition = info;
            lastHostPositionValid.store(true, std::memory_order_relaxed);
        }
        else
        {
            lastHostPositionValid.store(false, std::memory_order_relaxed);
        }
    }
    else
    {
        lastHostPositionValid.store(false, std::memory_order_relaxed);
    }

    const int numSamples = buffer.getNumSamples();
    std::chrono::microseconds filteredLinkBufferTime { 0 };
    if (linkTimingState != nullptr)
    {
        filteredLinkBufferTime = linkTimingState->hostTimeFilter.sampleTimeToHostTime(linkTimingState->nextSampleTime);
        linkTimingState->nextSampleTime += (double) numSamples;
    }

    const SyncMode syncModeAtBlock = getSyncMode();
    const bool linkTransportMode = (syncModeAtBlock == SyncMode::abletonLink);
    const bool anyLinkActive = linkTransportMode || isLinkAudioEnabled();
    std::optional<ableton::LinkAudio::SessionState> linkSessionState;
    std::chrono::microseconds linkBufferTime { 0 };
    const double linkQuantum = juce::jmax(1.0, (double) getBPI());
    const double linkStartQuantum = juce::jmax(1.0, juce::jmin(linkQuantum, linkAudioQuantumBeats));
    constexpr double linkAudioQuantum = linkAudioQuantumBeats;
    bool gotLinkState = false;
    bool linkPlayingAtBlock = false;
    double linkTempoAtBlock = 0.0;
    double linkPhaseAtBlock = 0.0;
    int linkPeersAtBlock = 0;
    if (anyLinkActive && abletonLink != nullptr)
    {
        linkSessionState.emplace(abletonLink->captureAudioSessionState());
        linkBufferTime = linkTimingState != nullptr ? filteredLinkBufferTime : abletonLink->clock().micros();
        linkPlayingAtBlock = linkSessionState->isPlaying();
        linkTempoAtBlock = linkSessionState->tempo();
        linkPhaseAtBlock = linkSessionState->phaseAtTime(linkBufferTime, linkQuantum);
        linkPeersAtBlock = (int) abletonLink->numPeers();
        gotLinkState = true;
        {
            const juce::ScopedLock lock(linkTransportStateLock);
            lastLinkTempo = linkTempoAtBlock;
            lastLinkPhaseBeats = linkPhaseAtBlock;
            lastLinkPeerCount = linkPeersAtBlock;
            lastLinkIsPlaying = linkPlayingAtBlock;
        }
    }

    const long long blockStartSampleCounter = intervalSyncSampleCounter.fetch_add((long long)numSamples, std::memory_order_relaxed);
    lastBlockSize = juce::jmax(1, numSamples);
    const bool useHostMidiForLearn = getMidiLearnInputDeviceId().isEmpty();
    const bool useHostMidiForRelay = getMidiRelayInputDeviceId().isEmpty();
    const bool samplePadsEnabledAtBlock = isSamplePadsFeatureEnabled();
    const bool useHostMidiForSamplePads = samplePadsEnabledAtBlock && getSamplePadsMidiInputDeviceId().isEmpty();
    {
        const juce::SpinLock::ScopedLockType midiQueueLock(midiEventQueueLock);
        const juce::SpinLock::ScopedLockType relayQueueLock(outboundMidiRelayQueueLock);
        for (const auto metadata : midiMessages)
        {
            const auto& msg = metadata.getMessage();
            if (msg.isController())
            {
                MidiControllerEvent event;
                event.isController = true;
                event.midiChannel = msg.getChannel();
                event.number = msg.getControllerNumber();
                event.value = msg.getControllerValue();
                event.normalized = (float)event.value / 127.0f;
                event.isNoteOn = event.value >= 64;
                if (useHostMidiForLearn)
                    pendingMidiControllerEvents.push_back(event);
                if (useHostMidiForRelay)
                    pendingOutboundMidiRelayEvents.push_back(event);
            }
            else if (msg.isNoteOnOrOff())
            {
                MidiControllerEvent event;
                event.isController = false;
                event.midiChannel = msg.getChannel();
                event.number = msg.getNoteNumber();
                event.value = msg.getVelocity();
                event.normalized = msg.isNoteOn() ? ((float)event.value / 127.0f) : 0.0f;
                event.isNoteOn = msg.isNoteOn();
                if (useHostMidiForSamplePads)
                    handleSamplePadMidiNote(event.number, event.isNoteOn);
                if (useHostMidiForLearn)
                    pendingMidiControllerEvents.push_back(event);
                if (useHostMidiForRelay)
                    pendingOutboundMidiRelayEvents.push_back(event);
            }
        }
        if (pendingMidiControllerEvents.size() > 512)
            pendingMidiControllerEvents.erase(pendingMidiControllerEvents.begin(), pendingMidiControllerEvents.begin() + (long long)(pendingMidiControllerEvents.size() - 512));
        if (pendingOutboundMidiRelayEvents.size() > 512)
            pendingOutboundMidiRelayEvents.erase(pendingOutboundMidiRelayEvents.begin(), pendingOutboundMidiRelayEvents.begin() + (long long)(pendingOutboundMidiRelayEvents.size() - 512));
    }
    if (!samplePadsEnabledAtBlock)
        samplePadsPeak.store(0.0f, std::memory_order_relaxed);
    injectInboundMidiRelayEvents(midiMessages);

    int totalInputChannels = 0;
    int numInputBuses = getBusCount(true);
    for (int bus = 0; bus < numInputBuses; ++bus)
    {
        int busChans = getChannelCountOfBus(true, bus);
        if (busChans <= 0)
            continue;
        totalInputChannels += busChans;
    }

    bool anyLocalUsesLinkAudioInput = false;
    for (int channel = 0; channel < maxLocalChannels; ++channel)
    {
        if (localChannelInputs[(size_t) channel].load() == kLocalInputLinkAudioSentinel)
        {
            anyLocalUsesLinkAudioInput = true;
            break;
        }
    }

    int blockLinkAudioNumChannels = 0;
    int blockLinkAudioNumFrames = 0;
    if (isLinkAudioEnabled() && isLinkAudioReceiveEnabled())
    {
        const size_t requiredSamples = (size_t)numSamples * 2u;
        if (blockLinkAudioSamples.size() < requiredSamples)
            blockLinkAudioSamples.resize(requiredSamples, 0.0f);
        else
            std::fill_n(blockLinkAudioSamples.data(), requiredSamples, 0.0f);
        blockLinkAudioNumChannels = 2;
        blockLinkAudioNumFrames = numSamples;

        const size_t maxBufferedFrames = (size_t) (numSamples <= 64
            ? juce::jmax(numSamples * 16, 1024)
            : juce::jmax(numSamples * 4, 512));
        const size_t availableFrames = linkAudioReceiveRing.available();
        if (availableFrames > maxBufferedFrames)
            linkAudioReceiveRing.discard(availableFrames - maxBufferedFrames);

        const size_t framesRead = linkAudioReceiveRing.readInterleaved(blockLinkAudioSamples.data(), (size_t)numSamples);
        if (framesRead == 0)
        {
            blockLinkAudioNumChannels = 0;
            blockLinkAudioNumFrames = 0;
        }
    }
    else
    {
        linkAudioReceiveRing.reset();
    }

    const int linkInputChannels = blockLinkAudioNumChannels > 0 ? juce::jlimit(1, 2, blockLinkAudioNumChannels) : 0;
    const int totalAvailableInputChannels = totalInputChannels + linkInputChannels;

    if (tempInputBuffer.getNumChannels() < totalAvailableInputChannels || tempInputBuffer.getNumSamples() < numSamples)
        tempInputBuffer.setSize(totalAvailableInputChannels, numSamples, false, false, true);

    int inputChanIndex = 0;
    for (int bus = 0; bus < numInputBuses; ++bus)
    {
        auto busBuffer = getBusBuffer(buffer, true, bus);
        int busChans = busBuffer.getNumChannels();
        if (busChans <= 0)
            continue;
        for (int ch = 0; ch < busChans; ++ch)
        {
            if (inputChanIndex < totalInputChannels)
            {
                tempInputBuffer.copyFrom(inputChanIndex, 0, busBuffer, ch, 0, numSamples);
                ++inputChanIndex;
            }
        }
    }

    if (linkInputChannels > 0)
    {
        for (int channel = 0; channel < linkInputChannels; ++channel)
            tempInputBuffer.clear(totalInputChannels + channel, 0, numSamples);

        const int framesToCopy = juce::jmin(numSamples, blockLinkAudioNumFrames);
        for (int sampleIndex = 0; sampleIndex < framesToCopy; ++sampleIndex)
        {
            const int sourceOffset = sampleIndex * blockLinkAudioNumChannels;
            tempInputBuffer.setSample(totalInputChannels, sampleIndex, blockLinkAudioSamples[(size_t) sourceOffset]);
            if (linkInputChannels > 1)
            {
                const int rightSource = sourceOffset + juce::jmin(1, blockLinkAudioNumChannels - 1);
                tempInputBuffer.setSample(totalInputChannels + 1, sampleIndex, blockLinkAudioSamples[(size_t) rightSource]);
            }
        }
    }

    if (localChannelBuffer.getNumChannels() < maxLocalChannels || localChannelBuffer.getNumSamples() < numSamples)
        localChannelBuffer.setSize(maxLocalChannels, numSamples, false, false, true);
    if (voiceChannelBuffer.getNumChannels() < 1 || voiceChannelBuffer.getNumSamples() < numSamples)
        voiceChannelBuffer.setSize(1, numSamples, false, false, true);

    int requestedLocal = numLocalChannels.load();
    const int serverMaxLocalChannelsForBlock = juce::jmax(1, serverMaxLocalChannelsCached.load(std::memory_order_relaxed));
    const bool opusSchemeFits = serverMaxLocalChannelsForBlock > kOpusMultichannelBaseIndex;
    const int maxEffectiveLocalChannels = opusSchemeFits ? maxLocalChannels : 1;
    // The opus multistream carries all configured virtual channels packed into the single
    // carrier lane (ch2). They do not consume additional NINJAM channels, so the virtual
    // count must not be capped by how many input channels the device provides - sources can
    // be shared or left silent for channels beyond the physical inputs.
    int actualLocal = juce::jlimit(1, maxEffectiveLocalChannels, requestedLocal);
    effectiveLocalChannelCount.store(actualLocal, std::memory_order_relaxed);
    std::array<int, maxLocalChannels> monitorSourceLeft{};
    std::array<int, maxLocalChannels> monitorSourceRight{};
    std::array<bool, maxLocalChannels> monitorStereo{};
    monitorSourceLeft.fill(-1);
    monitorSourceRight.fill(-1);
    monitorStereo.fill(false);

    const int samplePadBpi = samplePadsEnabledAtBlock ? juce::jmax(1, getBPI()) : 1;
    double samplePadSamplesPerBeat = 1.0;
    double samplePadBlockStartBeat = 0.0;
    if (samplePadsEnabledAtBlock)
    {
        const long long cachedTransportSampleCounter = cachedNinjamTransportSampleCounter.load(std::memory_order_acquire);
        int samplePadTransportPosition = cachedNinjamTransportPos.load(std::memory_order_relaxed);
        const int samplePadTransportLength = cachedNinjamTransportLen.load(std::memory_order_relaxed);
        if (samplePadTransportLength > 0)
        {
            if (blockStartSampleCounter > cachedTransportSampleCounter)
            {
                const long long elapsedSamples = blockStartSampleCounter - cachedTransportSampleCounter;
                samplePadTransportPosition = (int)(((long long)samplePadTransportPosition + elapsedSamples)
                                                   % (long long)samplePadTransportLength);
            }

            samplePadBlockStartBeat = getSamplePadBlockStartBeat(samplePadTransportPosition,
                                                                 samplePadTransportLength,
                                                                 samplePadBpi,
                                                                 samplePadSamplesPerBeat);
        }
        else
        {
            const double bpm = juce::jmax(1.0, (double)getBPM());
            samplePadSamplesPerBeat = juce::jmax(1.0, (60.0 / bpm) * juce::jmax(1.0, processingSampleRate));
            samplePadBlockStartBeat = (double)blockStartSampleCounter / samplePadSamplesPerBeat;
        }
    }

    if (samplePadsEnabledAtBlock)
        updateSamplePadMidiHolds(samplePadBlockStartBeat, samplePadBpi);

    bool samplePadsNeedLocalSlot = false;
    if (samplePadsEnabledAtBlock)
    {
        const juce::ScopedLock lock(samplePadsLock);
        for (const auto& pad : samplePads)
        {
            bool hasActiveOneShot = pad.activeOneShotVoices.load(std::memory_order_relaxed) > 0;
            if (!hasActiveOneShot)
            {
                for (const auto& voice : pad.oneShotVoices)
                {
                    if (voice.active)
                    {
                        hasActiveOneShot = true;
                        break;
                    }
                }
            }

            if (pad.recordArmed.load(std::memory_order_relaxed)
                || pad.recording.load(std::memory_order_relaxed)
                || pad.recordPendingStart.load(std::memory_order_relaxed)
                || pad.recordPendingStop.load(std::memory_order_relaxed)
                || pad.recordStartScheduled.load(std::memory_order_relaxed)
                || pad.playbackScheduled.load(std::memory_order_relaxed)
                || pad.playing.load(std::memory_order_relaxed)
                || hasActiveOneShot)
            {
                samplePadsNeedLocalSlot = true;
                break;
            }
        }
    }

    if (samplePadsNeedLocalSlot && actualLocal <= 0)
        actualLocal = 1;

    bool samplePadsActiveThisBlock = false;
    bool fedChordAnalyzer = false;
    float globalLocalMax = 0.0f;
    float globalLocalMaxL = 0.0f;
    float globalLocalMaxR = 0.0f;
    for (int ch = 0; ch < actualLocal; ++ch)
    {
        int srcIndex = localChannelInputs[(size_t)ch].load();
        int leftSource = -1;
        int rightSource = -1;

        if (srcIndex == kLocalInputLinkAudioSentinel)
        {
            const int left = totalInputChannels;
            const int right = linkInputChannels > 1 ? totalInputChannels + 1 : left;

            localChannelBuffer.clear(ch, 0, numSamples);
            if (left < totalAvailableInputChannels)
                localChannelBuffer.addFrom(ch, 0, tempInputBuffer, left, 0, numSamples, linkInputChannels > 1 ? 0.5f : 1.0f);
            if (linkInputChannels > 1 && right < totalAvailableInputChannels)
                localChannelBuffer.addFrom(ch, 0, tempInputBuffer, right, 0, numSamples, 0.5f);

            leftSource = linkInputChannels > 0 ? left : -1;
            rightSource = linkInputChannels > 1 ? right : leftSource;
            monitorStereo[(size_t)ch] = (linkInputChannels > 1);
        }
        else if (srcIndex >= 0)
        {
            if (srcIndex >= totalInputChannels)
                srcIndex = juce::jlimit(0, totalInputChannels - 1, srcIndex);

            int left = juce::jlimit(0, juce::jmax(totalInputChannels - 1, 0), srcIndex);
            int right = left;

            localChannelBuffer.clear(ch, 0, numSamples);
            if (left < totalInputChannels)
                localChannelBuffer.copyFrom(ch, 0, tempInputBuffer, left, 0, numSamples);

            leftSource = left;
            rightSource = right;
        }
        else
        {
            int pairIndex = -1 - srcIndex;
            int left = pairIndex * 2;
            int right = left + 1;

            if (left < 0 || left >= totalInputChannels)
                left = juce::jlimit(0, juce::jmax(totalInputChannels - 1, 0), left);
            if (right < 0 || right >= totalInputChannels)
                right = left;

            localChannelBuffer.clear(ch, 0, numSamples);
            if (left < totalInputChannels)
                localChannelBuffer.addFrom(ch, 0, tempInputBuffer, left, 0, numSamples, 0.5f);
            if (right < totalInputChannels)
                localChannelBuffer.addFrom(ch, 0, tempInputBuffer, right, 0, numSamples, 0.5f);

            leftSource = left;
            rightSource = right;
            monitorStereo[(size_t)ch] = (right != left);
        }

        monitorSourceLeft[(size_t)ch] = leftSource;
        monitorSourceRight[(size_t)ch] = rightSource;

        float gain = localChannelGains[(size_t)ch].load();
        if (gain != 1.0f)
            localChannelBuffer.applyGain(ch, 0, numSamples, gain);

        // Auto-tune on local channel 1 (ch == 0) — mono processing
        if (ch == 0 && autoTuneProcessor != nullptr && autoTuneEnabled.load(std::memory_order_relaxed))
        {
            float* channelData = localChannelBuffer.getWritePointer(ch);
            autoTuneProcessor->process(channelData, numSamples);
        }

        if (ch == 0 && samplePadsEnabledAtBlock)
        {
            const bool looperCapturesSamplePads =
                samplePadLooperInput.load(std::memory_order_relaxed) == looperInputSamplePads;
            if (looperCapturesSamplePads)
            {
                samplePadsActiveThisBlock = renderSamplePads(numSamples,
                                                             samplePadBlockStartBeat,
                                                             samplePadSamplesPerBeat,
                                                             samplePadBpi);
            }

            processSamplePadLooperRecording(numSamples,
                                            samplePadBlockStartBeat,
                                            samplePadSamplesPerBeat,
                                            samplePadBpi,
                                            totalAvailableInputChannels,
                                            leftSource,
                                            rightSource);
            if (!looperCapturesSamplePads)
            {
                samplePadsActiveThisBlock = renderSamplePads(numSamples,
                                                             samplePadBlockStartBeat,
                                                             samplePadSamplesPerBeat,
                                                             samplePadBpi);
            }

            if (samplePadsActiveThisBlock)
            {
                const float* padL = samplePadsRenderBuffer.getReadPointer(0);
                const float* padR = samplePadsRenderBuffer.getReadPointer(1);
                float* local = localChannelBuffer.getWritePointer(0);
                for (int i = 0; i < numSamples; ++i)
                    local[i] += 0.5f * (padL[i] + padR[i]);
            }
        }

        if (ch == 0 && chordAnalyzer && isChordDetectionEnabled())
        {
            chordAnalyzer->processBlock(BatchedChordAnalyzer::localTrackIndex,
                                        localChannelBuffer.getReadPointer(ch),
                                        numSamples);
            fedChordAnalyzer = true;
        }

        const float* data = localChannelBuffer.getReadPointer(ch);
        float localMax = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            float a = std::abs(data[i]);
            if (a > localMax)
                localMax = a;
        }

        float localMaxL = 0.0f;
        float localMaxR = 0.0f;

        if (leftSource >= 0 && leftSource < totalAvailableInputChannels)
        {
            const float* leftData = tempInputBuffer.getReadPointer(leftSource);
            for (int i = 0; i < numSamples; ++i)
            {
                float a = std::abs(leftData[i] * gain);
                if (a > localMaxL)
                    localMaxL = a;
            }
        }

        if (rightSource >= 0 && rightSource < totalAvailableInputChannels)
        {
            const float* rightData = tempInputBuffer.getReadPointer(rightSource);
            for (int i = 0; i < numSamples; ++i)
            {
                float a = std::abs(rightData[i] * gain);
                if (a > localMaxR)
                    localMaxR = a;
            }
        }

        if (ch == 0 && samplePadsActiveThisBlock)
        {
            localMaxL = juce::jmax(localMaxL, localMax);
            localMaxR = juce::jmax(localMaxR, localMax);
        }

        localChannelPeaks[(size_t)ch].store(localMax);
        localChannelPeaksL[(size_t)ch].store(localMaxL);
        localChannelPeaksR[(size_t)ch].store(localMaxR);
        if (localMax > globalLocalMax)
            globalLocalMax = localMax;
        if (localMaxL > globalLocalMaxL)
            globalLocalMaxL = localMaxL;
        if (localMaxR > globalLocalMaxR)
            globalLocalMaxR = localMaxR;
    }

    voiceChannelBuffer.clear(0, 0, numSamples);
    int voiceSourceLeft = -1;
    int voiceSourceRight = -1;
    const int selectedVoiceInput = voiceChannelInput.load(std::memory_order_relaxed);

    if (selectedVoiceInput == kLocalInputLinkAudioSentinel)
    {
        const int left = totalInputChannels;
        const int right = linkInputChannels > 1 ? totalInputChannels + 1 : left;
        if (linkInputChannels > 0 && left < totalAvailableInputChannels)
            voiceChannelBuffer.addFrom(0, 0, tempInputBuffer, left, 0, numSamples, linkInputChannels > 1 ? 0.5f : 1.0f);
        if (linkInputChannels > 1 && right < totalAvailableInputChannels)
            voiceChannelBuffer.addFrom(0, 0, tempInputBuffer, right, 0, numSamples, 0.5f);
        voiceSourceLeft = linkInputChannels > 0 ? left : -1;
        voiceSourceRight = linkInputChannels > 1 ? right : voiceSourceLeft;
    }
    else if (selectedVoiceInput >= 0)
    {
        if (totalInputChannels > 0)
        {
            const int left = juce::jlimit(0, totalInputChannels - 1, selectedVoiceInput);
            voiceChannelBuffer.copyFrom(0, 0, tempInputBuffer, left, 0, numSamples);
            voiceSourceLeft = left;
            voiceSourceRight = left;
        }
    }
    else if (totalInputChannels > 0)
    {
        const int pairIndex = -1 - selectedVoiceInput;
        int left = pairIndex * 2;
        int right = left + 1;
        if (left < 0 || left >= totalInputChannels)
            left = juce::jlimit(0, totalInputChannels - 1, left);
        if (right < 0 || right >= totalInputChannels)
            right = left;
        voiceChannelBuffer.addFrom(0, 0, tempInputBuffer, left, 0, numSamples, right != left ? 0.5f : 1.0f);
        if (right != left)
            voiceChannelBuffer.addFrom(0, 0, tempInputBuffer, right, 0, numSamples, 0.5f);
        voiceSourceLeft = left;
        voiceSourceRight = right;
    }

    const float voiceGain = voiceChannelGain.load(std::memory_order_relaxed);
    if (voiceGain != 1.0f)
        voiceChannelBuffer.applyGain(0, 0, numSamples, voiceGain);

    const float* voiceData = voiceChannelBuffer.getReadPointer(0);
    float voiceMax = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        const float a = std::abs(voiceData[i]);
        if (a > voiceMax)
            voiceMax = a;
    }

    float voiceMaxL = 0.0f;
    float voiceMaxR = 0.0f;
    if (voiceSourceLeft >= 0 && voiceSourceLeft < totalAvailableInputChannels)
    {
        const float* leftData = tempInputBuffer.getReadPointer(voiceSourceLeft);
        for (int i = 0; i < numSamples; ++i)
        {
            const float a = std::abs(leftData[i] * voiceGain);
            if (a > voiceMaxL)
                voiceMaxL = a;
        }
    }
    if (voiceSourceRight >= 0 && voiceSourceRight < totalAvailableInputChannels)
    {
        const float* rightData = tempInputBuffer.getReadPointer(voiceSourceRight);
        for (int i = 0; i < numSamples; ++i)
        {
            const float a = std::abs(rightData[i] * voiceGain);
            if (a > voiceMaxR)
                voiceMaxR = a;
        }
    }
    if (voiceSourceRight < 0)
        voiceMaxR = voiceMaxL;
    voiceChannelPeak.store(voiceMax, std::memory_order_relaxed);
    voiceChannelPeakL.store(voiceMaxL, std::memory_order_relaxed);
    voiceChannelPeakR.store(voiceMaxR, std::memory_order_relaxed);

    for (int ch = actualLocal; ch < maxLocalChannels; ++ch)
    {
        localChannelBuffer.clear(ch, 0, numSamples);
        localChannelPeaks[(size_t)ch].store(0.0f);
        localChannelPeaksL[(size_t)ch].store(0.0f);
        localChannelPeaksR[(size_t)ch].store(0.0f);
    }

    localPeak.store(globalLocalMax);
    localPeakL.store(globalLocalMaxL);
    localPeakR.store(globalLocalMaxR);

    // Session recorder: tap local channels (post-gain, post-AutoTune)
    if (sessionRecorder.isRecording())
    {
        for (int ch = 0; ch < actualLocal; ++ch)
            sessionRecorder.writeLocalChannel(ch, localChannelBuffer.getReadPointer(ch), numSamples);
    }

    if (chordAnalyzer && !fedChordAnalyzer)
        chordAnalyzer->markNoInput(BatchedChordAnalyzer::localTrackIndex);

    const bool reverbOn = fxReverbEnabled.load();
    const bool delayOn = fxDelayEnabled.load();
    const bool fxSendActive = reverbOn || delayOn;

    if (fxTransmitBuffer.getNumSamples() < numSamples)
        fxTransmitBuffer.setSize(1, numSamples, false, true, true);
    if (fxReturnBuffer.getNumSamples() < numSamples)
        fxReturnBuffer.setSize(2, numSamples, false, true, true);
    fxTransmitBuffer.clear();
    fxReturnBuffer.clear();

    if (fxSendActive)
    {
        if (fxReverbInputBuffer.getNumSamples() < numSamples)
            fxReverbInputBuffer.setSize(1, numSamples, false, true, true);
        if (fxDelayInputBuffer.getNumSamples() < numSamples)
            fxDelayInputBuffer.setSize(1, numSamples, false, true, true);

        fxReverbInputBuffer.clear();
        fxDelayInputBuffer.clear();

        const int activeLocal = juce::jmin(actualLocal, numLocalChannels.load());
        const bool removePadsFromDefaultFxSends = samplePadsActiveThisBlock && !samplePadsUseDefaultFx.load(std::memory_order_relaxed);
        const float* padSendL = removePadsFromDefaultFxSends ? samplePadsRenderBuffer.getReadPointer(0) : nullptr;
        const float* padSendR = removePadsFromDefaultFxSends ? samplePadsRenderBuffer.getReadPointer(1) : nullptr;
        for (int ch = 0; ch < activeLocal; ++ch)
        {
            const float reverbSend = localChannelReverbSends[(size_t)ch].load();
            const float delaySend = localChannelDelaySends[(size_t)ch].load();
            if (reverbSend <= 0.0001f && delaySend <= 0.0001f)
                continue;

            const float* src = localChannelBuffer.getReadPointer(ch);
            float* reverbDst = fxReverbInputBuffer.getWritePointer(0);
            float* delayDst = fxDelayInputBuffer.getWritePointer(0);
            for (int i = 0; i < numSamples; ++i)
            {
                float v = src[i];
                if (ch == 0 && removePadsFromDefaultFxSends)
                    v -= 0.5f * (padSendL[i] + padSendR[i]);

                if (reverbSend > 0.0001f)
                    reverbDst[i] += v * reverbSend;
                if (delaySend > 0.0001f)
                    delayDst[i] += v * delaySend;
            }
        }

        float* fxSendMono = fxTransmitBuffer.getWritePointer(0);
        float* fxLeft = fxReturnBuffer.getWritePointer(0);
        float* fxRight = fxReturnBuffer.getWritePointer(1);

        if (reverbOn)
        {
            juce::Reverb::Parameters params;
            params.roomSize = fxReverbRoomSize.load();
            params.damping = fxReverbDamping.load();
            params.width = 1.0f;
            params.wetLevel = 1.0f;
            params.dryLevel = 0.0f;
            params.freezeMode = 0.0f;
            fxReverb.setParameters(params);

            const float wetDryMix = fxReverbWetDryMix.load();
            const float earlyAmount = fxReverbEarlyReflections.load();
            const float tailAmount = fxReverbTail.load();
            const float* reverbIn = fxReverbInputBuffer.getReadPointer(0);
            float* revMono = fxReverbInputBuffer.getWritePointer(0);
            fxReverb.processMono(revMono, numSamples);
            for (int i = 0; i < numSamples; ++i)
            {
                const float early = reverbIn[i] * earlyAmount;
                const float tail = revMono[i] * tailAmount;
                const float wet = early + tail;
                const float mixed = wet * wetDryMix + reverbIn[i] * (1.0f - wetDryMix);
                const float out = mixed * 0.8f;
                fxLeft[i] += out;
                fxRight[i] += out;
                fxSendMono[i] += out * 0.5f;
            }
        }

        if (delayOn)
        {
            const int delayBufferSamples = fxDelayBuffer.getNumSamples();
            if (delayBufferSamples > 1)
            {
                const int division = fxDelayDivision.load();
                const double bpm = (double)getBPM();
                double targetDelaySeconds = fxDelayTimeMs.load() / 1000.0;
                if (fxDelaySyncToHost.load() && bpm > 1.0)
                    targetDelaySeconds = (60.0 / bpm) * (4.0 / (double)division);
                const int delaySamples = juce::jlimit(1, delayBufferSamples - 1, (int)std::round(targetDelaySeconds * processingSampleRate));

                const bool frippertronics = getFxDelayMode() == FxDelayMode::frippertronics;
                const bool pingPong = fxDelayPingPong.load();
                const float feedback = juce::jlimit(0.0f, 0.95f, fxDelayFeedback.load());
                const float wetDryMix = juce::jlimit(0.0f, 1.0f, fxDelayWetDryMix.load());
                const float delayWet = wetDryMix * 0.8f;
                const float tapeCutoff = 3600.0f;
                const float tapeLowpassAlpha = 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi * tapeCutoff
                                                              / (float)juce::jmax(1.0, processingSampleRate));

                float* delayMemoryL = fxDelayBuffer.getWritePointer(0);
                float* delayMemoryR = fxDelayBuffer.getWritePointer(1);
                const float* delayIn = fxDelayInputBuffer.getReadPointer(0);

                int writePos = fxDelayWritePosition;
                float lowpassL = fxDelayLowpassState[0];
                float lowpassR = fxDelayLowpassState[1];
                for (int i = 0; i < numSamples; ++i)
                {
                    int readPos = writePos - delaySamples;
                    if (readPos < 0)
                        readPos += delayBufferSamples;

                    const float readL = delayMemoryL[readPos];
                    const float readR = delayMemoryR[readPos];
                    const float input = delayIn[i];
                    const float wetL = readL * delayWet;
                    const float wetR = readR * delayWet;

                    fxLeft[i] += wetL;
                    fxRight[i] += wetR;
                    fxSendMono[i] += (wetL + wetR) * 0.25f;

                    if (frippertronics)
                    {
                        lowpassL += tapeLowpassAlpha * (readL - lowpassL);
                        lowpassR += tapeLowpassAlpha * (readR - lowpassR);

                        if (pingPong)
                        {
                            delayMemoryL[writePos] = input + lowpassR * feedback;
                            delayMemoryR[writePos] = input + lowpassL * feedback;
                        }
                        else
                        {
                            delayMemoryL[writePos] = input + lowpassL * feedback;
                            delayMemoryR[writePos] = input + lowpassR * feedback;
                        }
                    }
                    else if (pingPong)
                    {
                        delayMemoryL[writePos] = input + readR * feedback;
                        delayMemoryR[writePos] = input + readL * feedback;
                    }
                    else
                    {
                        const float mono = 0.5f * (readL + readR);
                        delayMemoryL[writePos] = input + mono * feedback;
                        delayMemoryR[writePos] = input + mono * feedback;
                    }

                    ++writePos;
                    if (writePos >= delayBufferSamples)
                        writePos = 0;
                }
                fxDelayWritePosition = writePos;
                fxDelayLowpassState[0] = lowpassL;
                fxDelayLowpassState[1] = lowpassR;
            }
        }
    }

    // Determine active encoding mode:
    // - multiChanAuto: >1 local channels + NJ+ peers -> Vorbis ch0, control ch1, Opus ch2
    // - otherwise:     Vorbis only, single channel (mix folded into ch0 above)
    const int serverMaxLocalChannelsForAudio = serverMaxLocalChannelsForBlock;
    const int configuredLocalChannelsForAudio = juce::jmax(1, actualLocal);
    const bool opusLanesFitAudio = serverMaxLocalChannelsForAudio > kOpusMultichannelBaseIndex;
    const bool multiChanAuto = configuredLocalChannelsForAudio > 1 && opusSyncAvailable.load() && isTransmittingLocal() && opusLanesFitAudio;
    {
        static int s_blockLogCounter = 0;
        static int s_lastBlockState = -1;
        const int blockState = (multiChanAuto ? 1 : 0) | (opusSyncAvailable.load() ? 2 : 0)
                             | (isTransmittingLocal() ? 4 : 0) | (serverMaxLocalChannelsForAudio << 8);
        if ((++s_blockLogCounter & 255) == 0 || blockState != s_lastBlockState)
        {
            s_lastBlockState = blockState;
            njplus_debug_log("BLOCK configured=%d actualLocal=%d opusSync=%d transmit=%d serverMax=%d lanesFit=%d multiChanAuto=%d packed=%d",
                             configuredLocalChannelsForAudio, actualLocal, (int)opusSyncAvailable.load(), (int)isTransmittingLocal(),
                             serverMaxLocalChannelsForAudio, opusLanesFitAudio, multiChanAuto,
                             getConfiguredLocalOpusPackedChannelCount(configuredLocalChannelsForAudio));
        }
    }

    if (!multiChanAuto && actualLocal > 1)
    {
        float* dst = localChannelBuffer.getWritePointer(0);
        for (int ch = 1; ch < actualLocal; ++ch)
        {
            const float* src = localChannelBuffer.getReadPointer(ch);
            for (int s = 0; s < numSamples; ++s)
                dst[s] += src[s];
        }
    }

    if (!multiChanAuto && fxSendActive)
        localChannelBuffer.addFrom(0, 0, fxTransmitBuffer, 0, 0, numSamples);

    const bool singleStereoLocal = !multiChanAuto
        && actualLocal == 1
        && monitorStereo[0]
        && isTransmittingLocal();

    if (multiChanAuto)
    {
        if (localMixBuffer.getNumChannels() < 2 || localMixBuffer.getNumSamples() < numSamples)
            localMixBuffer.setSize(2, numSamples, false, true, true);

        localMixBuffer.clear();
        for (int ch = 0; ch < actualLocal; ++ch)
        {
            const float* src = localChannelBuffer.getReadPointer(ch);
            localMixBuffer.addFrom(0, 0, src, numSamples);
            localMixBuffer.addFrom(1, 0, src, numSamples);
        }
        if (fxSendActive)
        {
            localMixBuffer.addFrom(0, 0, fxTransmitBuffer, 0, 0, numSamples);
            localMixBuffer.addFrom(1, 0, fxTransmitBuffer, 0, 0, numSamples);
        }

        const int packedChannelCount = getConfiguredLocalOpusPackedChannelCount(actualLocal);
        if (localOpusPackedBuffer.getNumChannels() < packedChannelCount || localOpusPackedBuffer.getNumSamples() < numSamples)
            localOpusPackedBuffer.setSize(packedChannelCount, numSamples, false, true, true);
        localOpusPackedBuffer.clear();

        int packedIndex = 0;
        for (int ch = 0; ch < actualLocal; ++ch)
        {
            const int width = getConfiguredLocalOpusWidth(ch);
            const float* mono = localChannelBuffer.getReadPointer(ch);
            if (width <= 1)
            {
                localOpusPackedBuffer.copyFrom(packedIndex++, 0, mono, numSamples);
                continue;
            }

            const int leftSource = monitorSourceLeft[(size_t)ch];
            const int rightSource = monitorSourceRight[(size_t)ch];
            if (leftSource >= 0 && leftSource < totalAvailableInputChannels)
                localOpusPackedBuffer.addFrom(packedIndex, 0, tempInputBuffer, leftSource, 0, numSamples, localChannelGains[(size_t)ch].load());
            else
                localOpusPackedBuffer.copyFrom(packedIndex, 0, mono, numSamples);

            if (rightSource >= 0 && rightSource < totalAvailableInputChannels)
                localOpusPackedBuffer.addFrom(packedIndex + 1, 0, tempInputBuffer, rightSource, 0, numSamples, localChannelGains[(size_t)ch].load());
            else
                localOpusPackedBuffer.copyFrom(packedIndex + 1, 0, localOpusPackedBuffer, packedIndex, 0, numSamples);

            packedIndex += 2;
        }
    }

    float* inputs[32] = {};
    int actualInputChannels;
    if (multiChanAuto)
    {
        // Channel 0 stays on the legacy Vorbis mixdown; the remaining lanes carry the Opus multichannel payload.
        const int n = getConfiguredLocalOpusPackedChannelCount(configuredLocalChannelsForAudio);
        inputs[0] = localMixBuffer.getWritePointer(0);
        inputs[1] = localMixBuffer.getWritePointer(1);
        for (int i = 0; i < n; ++i)
            inputs[2 + i] = localOpusPackedBuffer.getWritePointer(i);
        actualInputChannels = 2 + n;
    }
    else
    {
        if (singleStereoLocal)
        {
            if (localMixBuffer.getNumChannels() < 2 || localMixBuffer.getNumSamples() < numSamples)
                localMixBuffer.setSize(2, numSamples, false, true, true);

            const int sourceLeft = monitorSourceLeft[0];
            const int sourceRight = monitorSourceRight[0];
            const float gain = localChannelGains[0].load();

            localMixBuffer.clear();
            if (sourceLeft >= 0 && sourceLeft < totalAvailableInputChannels)
                localMixBuffer.copyFrom(0, 0, tempInputBuffer, sourceLeft, 0, numSamples);
            if (sourceRight >= 0 && sourceRight < totalAvailableInputChannels)
                localMixBuffer.copyFrom(1, 0, tempInputBuffer, sourceRight, 0, numSamples);
            else if (sourceLeft >= 0 && sourceLeft < totalAvailableInputChannels)
                localMixBuffer.copyFrom(1, 0, tempInputBuffer, sourceLeft, 0, numSamples);

            if (gain != 1.0f)
                localMixBuffer.applyGain(gain);

            if (fxSendActive)
            {
                localMixBuffer.addFrom(0, 0, fxTransmitBuffer, 0, 0, numSamples);
                localMixBuffer.addFrom(1, 0, fxTransmitBuffer, 0, 0, numSamples);
            }

            inputs[0] = localMixBuffer.getWritePointer(0);
            inputs[1] = localMixBuffer.getWritePointer(1);
            actualInputChannels = 2;
        }
        else
        {
            inputs[0] = localChannelBuffer.getWritePointer(0);
            actualInputChannels = 1;
        }
    }

    if (voiceChatMode && actualInputChannels < (int)std::size(inputs))
        inputs[actualInputChannels++] = voiceChannelBuffer.getWritePointer(0);

    float* outputs[32];
    int totalOutputChannels = 0;
    int numOutputBuses = getBusCount(false);
    for (int bus = 0; bus < numOutputBuses; ++bus)
    {
        int busChans = getChannelCountOfBus(false, bus);
        if (busChans <= 0)
            continue;
        totalOutputChannels += busChans;
    }

    int actualOutputChannels = juce::jmin(totalOutputChannels, 32);

    int outputChanIndex = 0;
    for (int bus = 0; bus < numOutputBuses; ++bus)
    {
        auto busBuffer = getBusBuffer(buffer, false, bus);
        int busChans = busBuffer.getNumChannels();
        if (busChans <= 0)
            continue;
        for (int ch = 0; ch < busChans; ++ch)
        {
            if (outputChanIndex < actualOutputChannels)
            {
                outputs[outputChanIndex] = busBuffer.getWritePointer(ch);
                ++outputChanIndex;
            }
        }
    }

    for (int bus = 0; bus < numOutputBuses; ++bus)
    {
        auto busBuffer = getBusBuffer(buffer, false, bus);
        int busChans = busBuffer.getNumChannels();
        if (busChans <= 0)
            continue;
        for (int ch = 0; ch < busChans; ++ch)
            busBuffer.clear(ch, 0, numSamples);
    }

    bool gateForSync = false;
    bool runMonitorOnly = false;
    if (syncModeAtBlock == SyncMode::host)
    {
        bool hostValid = gotHostPosition;
        bool hostPlaying = hostValid && hostInfoAtBlock.isPlaying;
        const bool waitingForRestart = syncAwaitingHostRestart.load();

        bool prev = hostWasPlaying.load();
        if (!hostValid || !hostPlaying)
        {
            hostWasPlaying.store(false);
            syncAwaitingHostRestart.store(false);
            syncWaitForInterval.store(false);
            syncTargetInterval.store(-1);
            syncDisplayPositionOffset.store(0);
            syncHostPhaseOffsetSamples.store(0);
        }
        else if (waitingForRestart)
        {
            hostWasPlaying.store(false);
            gateForSync = true;
            syncDisplayPositionOffset.store(0);
            syncHostPhaseOffsetSamples.store(0);
        }
        else if (!prev)
        {
            hostWasPlaying.store(true);
            primeSyncTransportStart(&hostInfoAtBlock);
            syncWaitForInterval.store(false);
            syncTargetInterval.store(-1);
            syncDisplayIntervalOffset.store(intervalIndex.load());
        }

        if (!hostValid || !hostPlaying)
        {
            gateForSync = true;
        }
        runMonitorOnly = gateForSync;
    }
    else if (syncModeAtBlock == SyncMode::abletonLink)
    {
        const bool linkPlaying = gotLinkState && linkPlayingAtBlock;
        const bool prev = linkWasPlaying.load();
        const bool waitingForRestart = syncAwaitingHostRestart.load();

        if (!gotLinkState || !linkPlaying)
        {
            hostWasPlaying.store(false);
            linkWasPlaying.store(false);
            syncAwaitingHostRestart.store(false);
            syncWaitForInterval.store(false);
            syncTargetInterval.store(-1);
            syncDisplayPositionOffset.store(0);
            syncHostPhaseOffsetSamples.store(0);
            gateForSync = true;
        }
        else if (waitingForRestart)
        {
            hostWasPlaying.store(false);
            linkWasPlaying.store(false);
            syncDisplayPositionOffset.store(0);
            syncHostPhaseOffsetSamples.store(0);
            gateForSync = true;
        }
        else
        {
            if (!prev)
            {
                const auto requestedStartTime = linkSessionState->timeForIsPlaying();
                const auto alignedStartTime = getNextLinkQuantumTime(*linkSessionState,
                                                                     requestedStartTime,
                                                                     linkStartQuantum,
                                                                     linkTempoAtBlock);
                if (alignedStartTime > linkBufferTime)
                {
                    hostWasPlaying.store(false);
                    linkWasPlaying.store(false);
                    syncWaitForInterval.store(false);
                    syncTargetInterval.store(-1);
                    syncDisplayPositionOffset.store(0);
                    syncHostPhaseOffsetSamples.store(0);
                    gateForSync = true;
                }
                else
                {
                    hostWasPlaying.store(true);
                    linkWasPlaying.store(true);
                    const double startPhaseBeats = linkSessionState->phaseAtTime(alignedStartTime, linkQuantum);
                    primeLinkTransportStart(startPhaseBeats, linkQuantum, linkTempoAtBlock);
                    syncWaitForInterval.store(false);
                    syncTargetInterval.store(-1);
                    syncDisplayIntervalOffset.store(intervalIndex.load());
                }
            }
            else
            {
                hostWasPlaying.store(true);
                linkWasPlaying.store(true);
            }
        }

        runMonitorOnly = gateForSync;
    }
    else
    {
        hostWasPlaying.store(false);
        linkWasPlaying.store(false);
        syncWaitForInterval.store(false);
        syncTargetInterval.store(-1);
        syncDisplayIntervalOffset.store(0);
        syncDisplayPositionOffset.store(0);
        syncHostPhaseOffsetSamples.store(0);
    }

    const bool monitorEnabled = localMonitorEnabled.load();
    const bool transmitEnabled = isTransmittingLocal();

    // Feed local input to the engine for normal transmit or independent voice chat.
    // Monitoring is handled below with explicit per-channel routing so stereo doesn't collapse when transmit toggles.
    const bool allowEngineLocalInput = transmitEnabled || voiceChatMode;
    float** engineInputs = allowEngineLocalInput ? inputs : nullptr;
    int engineInputChannels = allowEngineLocalInput ? actualInputChannels : 0;
    remoteOpusMixOutputs = outputs;
    remoteOpusMixOutputChannels = actualOutputChannels;
    remoteOpusSoloActiveThisBlock = false;
    for (int i = 0; i < maxRemoteChordUsers; ++i)
    {
        if (remoteOpusPeerActive[(size_t)i].load(std::memory_order_relaxed)
            && remoteOpusUserSolo[(size_t)i].load(std::memory_order_relaxed))
        {
            remoteOpusSoloActiveThisBlock = true;
            break;
        }
    }
    bool ninjamAudioProcessed = false;
    int metronomeTransportStartPosition = 0;
    int metronomeTransportLength = 0;
    int metronomeTransportBpi = 0;
    {
        const juce::ScopedTryLock lifecycleLock(ninjamAudioLifecycleLock);
        if (lifecycleLock.isLocked())
        {
            if (syncModeAtBlock == SyncMode::host
                && gotHostPosition
                && hostInfoAtBlock.isPlaying
                && hostWasPlaying.load()
                && !gateForSync)
            {
                int currentPosition = 0;
                int intervalLength = 0;
                ninjamClient.GetPosition(&currentPosition, &intervalLength);
                const int hostPhasePosition = computeHostIntervalPhasePositionSamples(hostInfoAtBlock,
                                                                                      getSampleRate(),
                                                                                      ninjamClient.GetBPI(),
                                                                                      intervalLength);
                if (hostPhasePosition >= 0)
                {
                    const int targetPosition = normaliseSignedIntervalPosition(
                        hostPhasePosition + syncHostPhaseOffsetSamples.load(),
                        intervalLength);
                    const int phaseError = shortestIntervalPhaseError(targetPosition, currentPosition, intervalLength);
                    const int correctionThreshold = juce::jmax(64, (int)std::llround(getSampleRate() * 0.0015));
                    const int maxCorrectionPerBlock = juce::jmax(correctionThreshold * 2,
                                                                 (int)std::llround(getSampleRate() * 0.025));
                    const int maxTrustedError = juce::jmax(maxCorrectionPerBlock,
                                                           (int)std::llround(getSampleRate() * 0.5));

                    if (std::abs(phaseError) >= correctionThreshold
                        && std::abs(phaseError) <= maxTrustedError)
                    {
                        const int correction = juce::jlimit(-maxCorrectionPerBlock, maxCorrectionPerBlock, phaseError);
                        ninjamClient.SetTransportPosition(currentPosition + correction);
                    }
                }
            }
            ninjamClient.GetPosition(&metronomeTransportStartPosition, &metronomeTransportLength);
            metronomeTransportBpi = juce::jmax(1, ninjamClient.GetBPI());
            ninjamClient.AudioProc(engineInputs, engineInputChannels, outputs, actualOutputChannels, numSamples, (int)getSampleRate(), runMonitorOnly);
            if (metronomeTransportLength <= 0)
            {
                int metronomeTransportEndPosition = 0;
                ninjamClient.GetPosition(&metronomeTransportEndPosition, &metronomeTransportLength);
                metronomeTransportStartPosition = metronomeTransportLength > 0
                    ? (metronomeTransportEndPosition - numSamples + metronomeTransportLength) % metronomeTransportLength
                    : 0;
            }
            ninjamAudioProcessed = true;
        }
    }
    if (ninjamAudioProcessed)
        mixSelectedMetronomeIntoOutputs(outputs, actualOutputChannels, numSamples, getSampleRate(), metronomeTransportStartPosition, metronomeTransportLength, metronomeTransportBpi, runMonitorOnly);

    int numOutputBusesOut = getBusCount(false);
    if (gateForSync)
    {
        for (int bus = 0; bus < numOutputBusesOut; ++bus)
        {
            auto busBuffer = getBusBuffer(buffer, false, bus);
            const int busChans = busBuffer.getNumChannels();
            for (int ch = 0; ch < busChans; ++ch)
                busBuffer.clear(ch, 0, numSamples);
        }
    }

    if (linkSessionState.has_value() && isLinkAudioEnabled() && isLinkAudioSendEnabled())
    {
        const juce::SpinLock::ScopedTryLockType endpointLock(linkAudioEndpointLock);
        if (endpointLock.isLocked())
        {
            const double beatsAtBufferBegin = linkSessionState->beatAtTime(linkBufferTime, linkAudioQuantum);
            for (auto& pair : remoteLinkAudioOutputPairs)
            {
                auto sinkIt = remoteLinkAudioSinks.find(pair.first);
                if (sinkIt == remoteLinkAudioSinks.end() || sinkIt->second == nullptr)
                    continue;

                const int leftChannel = pair.second * 2;
                const int rightChannel = leftChannel + 1;
                if (leftChannel < 0 || rightChannel >= actualOutputChannels)
                    continue;
                if (outputs[leftChannel] == nullptr || outputs[rightChannel] == nullptr)
                    continue;

                sinkIt->second->requestMaxNumSamples((size_t) juce::jmax(2, numSamples * 2));
                ableton::LinkAudioSink::BufferHandle sinkBuffer(*sinkIt->second);
                if (!sinkBuffer || sinkBuffer.samples == nullptr || sinkBuffer.maxNumSamples < (size_t) numSamples * 2u)
                    continue;

                for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
                {
                    const float leftSample = juce::jlimit(-1.0f, 1.0f, outputs[leftChannel][sampleIndex]);
                    const float rightSample = juce::jlimit(-1.0f, 1.0f, outputs[rightChannel][sampleIndex]);
                    sinkBuffer.samples[(size_t) sampleIndex * 2u] = ableton::util::floatToInt16(leftSample);
                    sinkBuffer.samples[(size_t) sampleIndex * 2u + 1u] = ableton::util::floatToInt16(rightSample);
                }

                sinkBuffer.commit(*linkSessionState,
                                  beatsAtBufferBegin,
                                  linkAudioQuantum,
                                  (size_t) numSamples,
                                  2u,
                                  (uint32_t) std::llround(juce::jmax(1.0, getSampleRate())));

                juce::FloatVectorOperations::clear(outputs[leftChannel], numSamples);
                juce::FloatVectorOperations::clear(outputs[rightChannel], numSamples);
            }
        }
    }

    if (monitorEnabled)
    {
        if (numOutputBusesOut > 0)
        {
            auto mainBus = getBusBuffer(buffer, false, 0);
            int outChans = mainBus.getNumChannels();
            int numLocal = juce::jmin(numLocalChannels.load(), maxLocalChannels);
            for (int ch = 0; ch < numLocal; ++ch)
            {
                const int outLeft = ch * 2;
                const int outRight = outLeft + 1;
                if (outChans <= 0)
                    break;
                const int sourceLeft = monitorSourceLeft[(size_t)ch];
                const int sourceRight = monitorSourceRight[(size_t)ch];
                const float gain = localChannelGains[(size_t)ch].load();
                if (sourceLeft < 0 || sourceLeft >= totalAvailableInputChannels)
                    continue;

                if (monitorStereo[(size_t)ch] && sourceRight >= 0 && sourceRight < totalAvailableInputChannels)
                {
                    if (outLeft < outChans)
                        mainBus.addFrom(outLeft, 0, tempInputBuffer, sourceLeft, 0, numSamples, gain);
                    if (outRight < outChans)
                        mainBus.addFrom(outRight, 0, tempInputBuffer, sourceRight, 0, numSamples, gain);
                    else if (outLeft == 0 && outChans == 1)
                    {
                        mainBus.addFrom(0, 0, tempInputBuffer, sourceLeft, 0, numSamples, gain * 0.5f);
                        mainBus.addFrom(0, 0, tempInputBuffer, sourceRight, 0, numSamples, gain * 0.5f);
                    }
                }
                else
                {
                    if (outLeft < outChans)
                        mainBus.addFrom(outLeft, 0, tempInputBuffer, sourceLeft, 0, numSamples, gain);
                    if (outRight < outChans)
                        mainBus.addFrom(outRight, 0, tempInputBuffer, sourceLeft, 0, numSamples, gain);
                    else if (outLeft == 0 && outChans == 1)
                        mainBus.addFrom(0, 0, tempInputBuffer, sourceLeft, 0, numSamples, gain);
                }
            }
        }
    }

    if (monitorEnabled && voiceChatMode && numOutputBusesOut > 0)
    {
        auto mainBus = getBusBuffer(buffer, false, 0);
        const int outChans = mainBus.getNumChannels();
        const int voiceOutputIndex = juce::jmin(juce::jmax(1, actualLocal), maxLocalChannels);
        const int outLeft = voiceOutputIndex * 2;
        const int outRight = outLeft + 1;
        if (outLeft < outChans)
            mainBus.addFrom(outLeft, 0, voiceChannelBuffer, 0, 0, numSamples);
        if (outRight < outChans)
            mainBus.addFrom(outRight, 0, voiceChannelBuffer, 0, 0, numSamples);
        else if (outLeft == 0 && outChans == 1)
            mainBus.addFrom(0, 0, voiceChannelBuffer, 0, 0, numSamples);
    }
    if (monitorEnabled && samplePadsActiveThisBlock && numOutputBusesOut > 0)
    {
        auto mainBus = getBusBuffer(buffer, false, 0);
        const int outChans = mainBus.getNumChannels();
        const float sampleMonitorGain = localChannelGains[0].load(std::memory_order_relaxed);
        if (outChans >= 2)
        {
            mainBus.addFrom(0, 0, samplePadsRenderBuffer, 0, 0, numSamples, sampleMonitorGain);
            mainBus.addFrom(1, 0, samplePadsRenderBuffer, 1, 0, numSamples, sampleMonitorGain);
            mainBus.addFrom(0, 0, samplePadsMonitorRenderBuffer, 0, 0, numSamples, sampleMonitorGain);
            mainBus.addFrom(1, 0, samplePadsMonitorRenderBuffer, 1, 0, numSamples, sampleMonitorGain);
        }
        else if (outChans == 1)
        {
            const float* padL = samplePadsRenderBuffer.getReadPointer(0);
            const float* padR = samplePadsRenderBuffer.getReadPointer(1);
            const float* monitorPadL = samplePadsMonitorRenderBuffer.getReadPointer(0);
            const float* monitorPadR = samplePadsMonitorRenderBuffer.getReadPointer(1);
            float* out = mainBus.getWritePointer(0);
            for (int i = 0; i < numSamples; ++i)
                out[i] += sampleMonitorGain * 0.5f * (padL[i] + padR[i] + monitorPadL[i] + monitorPadR[i]);
        }
    }

    int mtcPos = 0;
    int mtcLength = 0;
    {
        const juce::ScopedTryLock clientLock(ninjamClientLock);
        if (clientLock.isLocked())
            ninjamClient.GetPosition(&mtcPos, &mtcLength);
    }
    emitMidiTimecode(midiMessages, numSamples, mtcPos, mtcLength);
    remoteOpusMixOutputs = nullptr;
    remoteOpusMixOutputChannels = 0;
    remoteOpusSoloActiveThisBlock = false;

    if (linkInputChannels > 0 && !anyLocalUsesLinkAudioInput)
    {
        auto mainBus = getBusBuffer(buffer, false, 0);
        const int outputChannels = mainBus.getNumChannels();
        if (outputChannels > 0)
        {
            const int linkLeft = totalInputChannels;
            const int linkRight = linkInputChannels > 1 ? totalInputChannels + 1 : linkLeft;
            if (outputChannels > 1)
            {
                mainBus.addFrom(0, 0, tempInputBuffer, linkLeft, 0, numSamples);
                mainBus.addFrom(1, 0, tempInputBuffer, linkRight, 0, numSamples);
            }
            else if (outputChannels == 1)
            {
                const float foldGain = linkInputChannels > 1 ? 0.5f : 1.0f;
                mainBus.addFrom(0, 0, tempInputBuffer, linkLeft, 0, numSamples, foldGain);
                if (linkInputChannels > 1)
                mainBus.addFrom(0, 0, tempInputBuffer, linkRight, 0, numSamples, 0.5f);
            }
        }
    }

    if (numOutputBusesOut > 0 && fxSendActive)
    {
        auto mainBus = getBusBuffer(buffer, false, 0);
        const int mainChans = mainBus.getNumChannels();
        if (mainChans >= 2)
        {
            mainBus.addFrom(0, 0, fxReturnBuffer, 0, 0, numSamples);
            mainBus.addFrom(1, 0, fxReturnBuffer, 1, 0, numSamples);
        }
        else if (mainChans == 1)
        {
            const float* l = fxReturnBuffer.getReadPointer(0);
            const float* r = fxReturnBuffer.getReadPointer(1);
            float* monoOut = mainBus.getWritePointer(0);
            for (int i = 0; i < numSamples; ++i)
                monoOut[i] += 0.5f * (l[i] + r[i]);
        }
    }

    float masterGain = masterOutputGain.load();
    if (masterGain != 1.0f)
    {
        for (int bus = 0; bus < numOutputBusesOut; ++bus)
        {
            auto busBuffer = getBusBuffer(buffer, false, bus);
            int busChans = busBuffer.getNumChannels();
            for (int ch = 0; ch < busChans; ++ch)
                busBuffer.applyGain(ch, 0, numSamples, masterGain);
        }
    }

    bool limiter = dspLimiterEnabled.load() && (limiterThresholdDb.load() < 0.0f);
    if (limiter)
    {
        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::ProcessContextReplacing<float> context(block);
        masterLimiter.process(context);
    }

    bool softClip = softLimiterEnabled.load();
    float maxSample = 0.0f;
    float maxSampleL = 0.0f;
    float maxSampleR = 0.0f;
    for (int bus = 0; bus < numOutputBusesOut; ++bus)
    {
        auto busBuffer = getBusBuffer(buffer, false, bus);
        int busChans = busBuffer.getNumChannels();
        for (int ch = 0; ch < busChans; ++ch)
        {
            float* data = busBuffer.getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i)
            {
                float v = data[i];
                if (softClip)
                    v = softClipSample(v);
                float a = std::abs(v);
                if (a > maxSample)
                    maxSample = a;
                if (bus == 0 && ch == 0 && a > maxSampleL)
                    maxSampleL = a;
                if (bus == 0 && ch == 1 && a > maxSampleR)
                    maxSampleR = a;
                data[i] = v;
            }
        }
    }

    // Session recorder: tap master stereo mix (post-limiter, post-softclip)
    if (sessionRecorder.isRecording() && numOutputBusesOut > 0)
    {
        auto mainBus = getBusBuffer(buffer, false, 0);
        const int mainChans = mainBus.getNumChannels();
        if (mainChans >= 2)
            sessionRecorder.writeMasterBlock(mainBus.getReadPointer(0), mainBus.getReadPointer(1), numSamples);
        else if (mainChans == 1)
            sessionRecorder.writeMasterBlock(mainBus.getReadPointer(0), mainBus.getReadPointer(0), numSamples);
    }

    if (numOutputBusesOut > 0)
    {
        auto mainBus = getBusBuffer(buffer, false, 0);
        if (mainBus.getNumChannels() == 1)
            maxSampleR = maxSampleL;
        else if (mainBus.getNumChannels() == 0)
        {
            maxSampleL = maxSample;
            maxSampleR = maxSample;
        }
    }
    else
    {
        maxSampleL = maxSample;
        maxSampleR = maxSample;
    }

    if (chordAnalyzer != nullptr && chordAnalyzer->isPrepared())
    {
        if (isChordDetectionEnabled() && numOutputBusesOut > 0 && numSamples > 0)
        {
            auto mainBus = getBusBuffer(buffer, false, 0);
            const int mainChannels = mainBus.getNumChannels();
            if (mainChannels > 0)
            {
                if (masterChordScratchBuffer.getNumChannels() < 1 || masterChordScratchBuffer.getNumSamples() < numSamples)
                    masterChordScratchBuffer.setSize(1, numSamples, false, false, true);

                float* mixed = masterChordScratchBuffer.getWritePointer(0);
                const float* left = mainBus.getReadPointer(0);
                if (mainChannels > 1)
                {
                    const float* right = mainBus.getReadPointer(1);
                    for (int i = 0; i < numSamples; ++i)
                        mixed[i] = 0.5f * (left[i] + right[i]);
                }
                else
                {
                    std::memcpy(mixed, left, (size_t)numSamples * sizeof(float));
                }

                chordAnalyzer->processBlock(BatchedChordAnalyzer::masterTrackIndex,
                                            mixed,
                                            numSamples,
                                            juce::roundToInt(getSampleRate()));
            }
            else
            {
                chordAnalyzer->markNoInput(BatchedChordAnalyzer::masterTrackIndex);
            }
        }
        else
        {
            chordAnalyzer->markNoInput(BatchedChordAnalyzer::masterTrackIndex);
        }
    }

    if (linkSessionState.has_value() && isLinkAudioEnabled() && isLinkAudioSendEnabled() && numOutputBusesOut > 0)
    {
        const juce::SpinLock::ScopedTryLockType endpointLock(linkAudioEndpointLock);
        if (endpointLock.isLocked() && abletonLinkSink != nullptr)
        {
            abletonLinkSink->requestMaxNumSamples((size_t) juce::jmax(2, numSamples * 2));
            ableton::LinkAudioSink::BufferHandle sinkBuffer(*abletonLinkSink);
            if (sinkBuffer)
            {
                auto mainBus = getBusBuffer(buffer, false, 0);
                const int mainBusChannels = mainBus.getNumChannels();
                if (mainBusChannels > 0)
                {
                    const float* left = mainBus.getReadPointer(0);
                    const float* right = mainBus.getReadPointer(juce::jmin(1, mainBusChannels - 1));
                    const size_t requiredSamples = (size_t) numSamples * 2u;
                    if (sinkBuffer.maxNumSamples >= requiredSamples)
                    {
                        for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
                        {
                            const float leftSample = juce::jlimit(-1.0f, 1.0f, left[sampleIndex]);
                            const float rightSample = juce::jlimit(-1.0f, 1.0f, right[sampleIndex]);
                            sinkBuffer.samples[(size_t) sampleIndex * 2u] = ableton::util::floatToInt16(leftSample);
                            sinkBuffer.samples[(size_t) sampleIndex * 2u + 1u] = ableton::util::floatToInt16(rightSample);
                        }

                        const double beatsAtBufferBegin = linkSessionState->beatAtTime(linkBufferTime, linkAudioQuantum);
                        sinkBuffer.commit(*linkSessionState,
                                          beatsAtBufferBegin,
                                          linkAudioQuantum,
                                          (size_t) numSamples,
                                          2u,
                                          (uint32_t) std::llround(juce::jmax(1.0, getSampleRate())));
                    }
                }
            }
        }
    }

    masterPeak.store(maxSample);
    masterPeakL.store(maxSampleL);
    masterPeakR.store(maxSampleR);

    {
        auto busBuffer = getBusBuffer(buffer, false, 0);
        int busChans = busBuffer.getNumChannels();
        if (busChans > 0)
        {
            const float* ch0 = busBuffer.getReadPointer(0);
            const float* ch1 = busChans > 1 ? busBuffer.getReadPointer(1) : ch0;
            float peakLufs = -70.0f;
            for (int i = 0; i < numSamples; ++i)
            {
                float mono = 0.5f * (ch0[i] + ch1[i]);
                masterLufsMeter.processSample(mono);
                if (busChans > 1)
                {
                    float monoR = 0.5f * (ch0[i] + ch1[i]);
                    masterLufsMeterR.processSample(monoR);
                }
            }
            float avgL = masterLufsMeter.getCurrentLufs();
            float avgR = masterLufsMeterR.getCurrentLufs();
            masterLufsAvg.store(juce::jmax(avgL, avgR), std::memory_order_relaxed);
            masterLufsPeak.store(juce::jmax(masterLufsPeak.load(std::memory_order_relaxed),
                                            juce::jmax(avgL, avgR)), std::memory_order_relaxed);
        }
    }
}

// Called from NJClient::on_new_interval() in the AUDIO THREAD at sample-accurate timing.
void NinjamVst3AudioProcessor::NewIntervalCallback_cb(void* userData, NJClient* /*inst*/)
{
    auto* self = static_cast<NinjamVst3AudioProcessor*>(userData);
    if (self == nullptr
        || !self->ninjamZapServerVideoSupported.load(std::memory_order_relaxed)
        || !self->ninjamZapCameraSendEnabled.load(std::memory_order_relaxed))
        return;

    self->requestNinjamZapVideoIntervalRotateFromAudioThread();
}

void NinjamVst3AudioProcessor::PostNewIntervalCallback_cb(void* userData, NJClient* /*inst*/)
{
    auto* self = static_cast<NinjamVst3AudioProcessor*>(userData);
    if (self == nullptr
        || !self->ninjamZapServerVideoSupported.load(std::memory_order_relaxed)
        || !self->ninjamZapVideoEnabled.load(std::memory_order_relaxed)
        || !self->ninjamZapVideoPlaybackWorkPending.load(std::memory_order_acquire))
        return;

    self->pendingNinjamZapVideoPlaybackBoundaryMs.store(0.0, std::memory_order_release);
    self->pendingNinjamZapVideoPlaybackSwap.store(true, std::memory_order_release);
}

void NinjamVst3AudioProcessor::IntervalChunkCallback_cb(void* userData, NJClient* /*inst*/,
    const char* username, int chidx, unsigned int fourcc,
    const unsigned char* guid, const void* data, int dataLen, int flags)
{
    if (!isNinjamZapVideoFourcc(fourcc))
        return;

    auto* self = static_cast<NinjamVst3AudioProcessor*>(userData);
    if (self == nullptr || !self->ninjamZapVideoEnabled.load(std::memory_order_relaxed))
        return;

    // On the audio thread: do minimal work — copy raw data and enqueue for background processing.
    PendingZapChunk chunk;
    chunk.username = username != nullptr ? std::string(username) : std::string();
    chunk.chidx = chidx;
    chunk.fourcc = fourcc;
    if (guid != nullptr)
        std::memcpy(chunk.guid.data(), guid, 16);
    if (dataLen > 0 && data != nullptr)
        chunk.data.assign(static_cast<const unsigned char*>(data),
                          static_cast<const unsigned char*>(data) + dataLen);
    chunk.flags = flags;
    chunk.receivedMs = juce::Time::getMillisecondCounterHiRes();
    self->noteRemoteVideoRoomActivity(chunk.receivedMs);

    {
        const juce::ScopedLock lock(self->pendingZapChunksLock);
        self->pendingZapChunks.push_back(std::move(chunk));
    }
}

void NinjamVst3AudioProcessor::processPendingZapChunks()
{
    std::vector<PendingZapChunk> localQueue;
    {
        const juce::ScopedLock lock(pendingZapChunksLock);
        localQueue.swap(pendingZapChunks);
    }
    for (const auto& chunk : localQueue)
        processSingleZapChunk(chunk);
}

void NinjamVst3AudioProcessor::processSingleZapChunk(const PendingZapChunk& chunk)
{
    const auto codec = getNinjamZapVideoCodec(chunk.fourcc);
    const juce::String sender = juce::String::fromUTF8(chunk.username.c_str());
    const juce::String streamKey = sender + ":" + juce::String(chunk.chidx);
    const juce::String reassemblyKey = streamKey + ":" + guidToHexString(chunk.guid.data());
    std::vector<juce::MemoryBlock> chunks;

    if (reassemblyKey.isNotEmpty())
    {
        const juce::ScopedLock lock(ninjamZapVideoChunkLock);
        auto& reassembler = ninjamZapVideoChunkReassemblers[reassemblyKey];
        if (!chunk.data.empty())
            chunks = reassembler.pushBytes(chunk.data.data(), chunk.data.size());
    }

    if (!ninjamZapVideoReceivedNotice.exchange(true, std::memory_order_relaxed))
    {
        juce::String message = "Receiving NINJAMZap video transport";
        if (sender.isNotEmpty())
            message << " from " << sender;
        message << " on channel " << (chunk.chidx + 1)
                << " as " << ninjamplus::zap::getCodecName(codec)
                << " (" << (int)chunk.data.size() << " bytes first fragment";
        if (!chunks.empty())
            message << ", " << (int)chunks.size() << " complete Zap chunk" << (chunks.size() == 1 ? "" : "s");
        message << ").";
        message << " Browser decode path active.";
        addSystemChatLine(message);
    }

    if (!chunks.empty())
    {
        for (const auto& completedChunk : chunks)
        {
            ninjamplus::zap::SyncMarker marker;
            bool markerAlreadySeen = false;
            {
                const juce::ScopedLock lock(ninjamZapVideoChunkLock);
                auto markerSeenIt = ninjamZapVideoMarkerSeenByReassemblyKey.find(reassemblyKey);
                markerAlreadySeen = markerSeenIt != ninjamZapVideoMarkerSeenByReassemblyKey.end()
                    && markerSeenIt->second;
            }

            if (!markerAlreadySeen
                && ninjamplus::zap::parseSyncMarkerPayload(completedChunk.getData(), completedChunk.getSize(), marker))
            {
                const juce::String audioGuidHex = guidToHexString(marker.audioGuid.data());
                {
                    const juce::ScopedLock lock(ninjamZapVideoChunkLock);
                    ninjamZapVideoAudioGuidByReassemblyKey[reassemblyKey] = audioGuidHex;
                    ninjamZapVideoMarkerIntervalByReassemblyKey[reassemblyKey] = (int)marker.intervalCounter;
                    ninjamZapVideoMarkerSeenByReassemblyKey[reassemblyKey] = true;
                }
                continue;
            }

            if (codec == ninjamplus::zap::VideoCodec::mjpeg
                || codec == ninjamplus::zap::VideoCodec::h264
                || codec == ninjamplus::zap::VideoCodec::vp8
                || codec == ninjamplus::zap::VideoCodec::vp9)
            {
                juce::String audioGuidHex;
                int markerInterval = -1;
                {
                    const juce::ScopedLock lock(ninjamZapVideoChunkLock);
                    auto guidIt = ninjamZapVideoAudioGuidByReassemblyKey.find(reassemblyKey);
                    if (guidIt != ninjamZapVideoAudioGuidByReassemblyKey.end())
                        audioGuidHex = guidIt->second;
                    auto intervalIt = ninjamZapVideoMarkerIntervalByReassemblyKey.find(reassemblyKey);
                    if (intervalIt != ninjamZapVideoMarkerIntervalByReassemblyKey.end())
                        markerInterval = intervalIt->second;
                }

                ZapVideoDecodeJob job;
                job.streamKey = streamKey;
                job.sender = sender;
                job.audioGuidHex = audioGuidHex;
                job.markerInterval = markerInterval;
                job.channelIndex = chunk.chidx;
                job.codec = codec;
                job.payload.append(completedChunk.getData(), completedChunk.getSize());
                job.receivedMs = chunk.receivedMs;
                job.queuedMs = juce::Time::getMillisecondCounterHiRes();
                {
                    const juce::ScopedLock lock(zapVideoFrameLock);
                    auto timingIt = zapVideoSenderTimingByStream.find(streamKey);
                    if (timingIt != zapVideoSenderTimingByStream.end()
                        && job.queuedMs - timingIt->second.updatedMs <= 5000.0)
                    {
                        job.senderCaptureQueueMs = timingIt->second.captureQueueMs;
                        job.senderEncodeMs = timingIt->second.encodeMs;
                    }
                }
                publishBrowserDecodedZapVideoFrame(job);
            }
        }
    }

    if ((chunk.flags & 1) != 0 && reassemblyKey.isNotEmpty())
    {
        const juce::ScopedLock lock(ninjamZapVideoChunkLock);
        ninjamZapVideoChunkReassemblers.erase(reassemblyKey);
        ninjamZapVideoAudioGuidByReassemblyKey.erase(reassemblyKey);
        ninjamZapVideoMarkerIntervalByReassemblyKey.erase(reassemblyKey);
        ninjamZapVideoMarkerSeenByReassemblyKey.erase(reassemblyKey);
    }
}

void NinjamVst3AudioProcessor::IntervalMediaItem_Callback(void* userData, NJClient* /*inst*/,
    const char* username, int /*chidx*/, unsigned int fourcc,
    const unsigned char* /*guid*/, const void* data, int dataLen)
{
    if (!username || !data || dataLen <= 0) return;
    if (isNinjamZapVideoFourcc(fourcc)) return;

    auto* self = static_cast<NinjamVst3AudioProcessor*>(userData);
    PendingMediaItem item;
    item.sender = juce::String::fromUTF8(username);
    item.fourcc = fourcc;
    item.data.append(data, static_cast<size_t>(dataLen));
    {
        const juce::SpinLock::ScopedLockType lock(self->pendingMediaItemLock);
        self->pendingMediaItems.push_back(std::move(item));
    }
    self->pendingMediaItemsReady.store(true, std::memory_order_release);
}

void NinjamVst3AudioProcessor::processPendingMediaItems()
{
    if (!pendingMediaItemsReady.exchange(false, std::memory_order_acquire))
        return;

    std::vector<PendingMediaItem> items;
    {
        const juce::SpinLock::ScopedLockType lock(pendingMediaItemLock);
        items.swap(pendingMediaItems);
    }

    for (auto& item : items)
    {
        const juce::String sender = item.sender;
        const unsigned int fourcc = item.fourcc;
        const auto* dataPtr = static_cast<const char*>(item.data.getData());
        const int dataLen = static_cast<int>(item.data.getSize());

        if (fourcc == kSyncSignalFourcc)
        {
            const juce::String msg    = juce::String::fromUTF8(dataPtr, dataLen);
            const juce::var parsed    = juce::JSON::parse(msg);
            if (auto* obj = parsed.getDynamicObject())
            {
                const juce::String type    = obj->getProperty("sig").toString();
                const juce::String payload = obj->getProperty("data").toString();
                if (type == "mobileHotspotKeepalive")
                    continue;
                if (type.isNotEmpty() && payload.isNotEmpty())
                    processSyncSignal(sender, type, payload);
            }
            continue;
        }
        if (fourcc != kOpusSyncFourcc) continue;
        const juce::String payload = juce::String::fromUTF8(dataPtr, dataLen);

        juce::var parsed = juce::JSON::parse(payload);
        bool supportsOpus = false;
        bool multiChanEnabled = false;
        int peerNumChannels = 1;
        int peerOpusBaseChannel = 1;
        int peerPackedChannelCount = 0;
        std::array<int, maxLocalChannels> peerChannelWidths {};
        juce::StringArray peerChannelNames;
        juce::String userId = normaliseOpusPeerId(sender);
        juce::String clientId;
        juce::String appFamily;
        int handshakeVersion = 0;
        juce::String runtimeFormat;
        juce::String pluginVersion;
        if (auto* obj = parsed.getDynamicObject())
        {
            const juce::String supports = obj->getProperty("supportsOpus").toString();
            supportsOpus = supports == "1" || supports.equalsIgnoreCase("true");
            const juce::String enabledStr = obj->getProperty("enabled").toString();
            multiChanEnabled = enabledStr == "1" || enabledStr.equalsIgnoreCase("true");
            const juce::var numChVar = obj->getProperty("numChannels");
            if (!numChVar.isVoid()) peerNumChannels = juce::jmax(1, (int)numChVar);
            const juce::var packedVar = obj->getProperty("packedChannelCount");
            if (!packedVar.isVoid()) peerPackedChannelCount = juce::jmax(0, (int)packedVar);
            const juce::var opusBaseVar = obj->getProperty("opusBaseChannel");
            if (!opusBaseVar.isVoid()) peerOpusBaseChannel = juce::jlimit(1, 31, (int)opusBaseVar);
            if (auto* widthsArray = obj->getProperty("channelWidths").getArray())
            {
                for (int i = 0; i < juce::jmin(maxLocalChannels, widthsArray->size()); ++i)
                    peerChannelWidths[(size_t)i] = juce::jlimit(1, 2, (int)widthsArray->getReference(i));
            }
            if (auto* namesArray = obj->getProperty("channelNames").getArray())
            {
                for (const auto& n : *namesArray)
                    peerChannelNames.add(n.toString());
            }
            juce::String payloadUserId = obj->getProperty("userId").toString();
            if (payloadUserId.isNotEmpty())
                userId = normaliseOpusPeerId(payloadUserId);
            clientId = obj->getProperty("clientId").toString().trim();
            appFamily = obj->getProperty("appFamily").toString().trim();
            handshakeVersion = (int)obj->getProperty("handshakeVersion");
            runtimeFormat = obj->getProperty("runtimeFormat").toString().trim();
            pluginVersion = obj->getProperty("pluginVersion").toString().trim();
        }
        else { continue; }

        const bool isLocalClient = clientId.isNotEmpty() ? (clientId == opusSyncInstanceId)
                                                           : (userId == normaliseOpusPeerId(currentUser));
        const bool sameAppFamily = appFamily.isEmpty() || appFamily == opusSyncAppFamily;
        const bool compatibleHandshake = handshakeVersion <= 0 || handshakeVersion == opusSyncHandshakeVersion;
        const juce::String peerKey = clientId.isNotEmpty() ? clientId : userId;
        if (peerKey.isEmpty() || userId.isEmpty() || isLocalClient) continue;

        bool recognizedNow = false;
        juce::String recognizedMessage;
        {
            juce::ScopedLock lock(opusSyncPeerLock);
            if (supportsOpus && sameAppFamily && compatibleHandshake)
            {
                const bool wasKnown = opusSyncPeers.find(peerKey) != opusSyncPeers.end();
                auto& peer = opusSyncPeers[peerKey];
                const bool wasMultiChan = peer.multiChanEnabled;
                peer.userId = userId;
                peer.supportsOpus = true;
                peer.multiChanEnabled = multiChanEnabled;
                peer.numChannels = peerNumChannels;
                peer.opusBaseChannel = peerOpusBaseChannel;
                peer.packedChannelCount = peerPackedChannelCount;
                peer.channelWidths = peerChannelWidths;
                peer.channelNames = peerChannelNames;
                peer.appFamily = appFamily;
                peer.handshakeVersion = handshakeVersion;
                peer.runtimeFormat = runtimeFormat;
                peer.pluginVersion = pluginVersion;
                peer.lastSeenMs = juce::Time::getMillisecondCounterHiRes();
                const juce::String peerLabel = sender.isNotEmpty() ? sender : userId;
                if (!wasKnown)
                {
                    juce::String peerInfo = peer.runtimeFormat;
                    if (peer.pluginVersion.isNotEmpty())
                    {
                        if (peerInfo.isNotEmpty()) peerInfo << " ";
                        peerInfo << peer.pluginVersion;
                    }
                    recognizedMessage = "Multi Client Detected: " + peerLabel;
                    if (peerInfo.isNotEmpty()) recognizedMessage << " (" << peerInfo << ")";
                    if (multiChanEnabled) recognizedMessage << " [MultiChannel ON]";
                    recognizedNow = true;
                }
                else if (multiChanEnabled && !wasMultiChan)
                {
                    recognizedMessage = "MultiChannel Detected: " + peerLabel;
                    recognizedNow = true;
                }
                else if (!multiChanEnabled && wasMultiChan)
                {
                    recognizedMessage = "MultiChannel Off: " + peerLabel;
                    recognizedNow = true;
                }
            }
            else
                opusSyncPeers.erase(peerKey);
        }
        if (recognizedNow)
        {
            juce::ScopedLock lock(chatLock);
            chatHistory.add(recognizedMessage);
            chatSenders.add("");
            chatRevision.fetch_add(1);
            if (chatHistory.size() > 100)
            {
                chatHistory.removeRange(0, chatHistory.size() - 100);
                chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
            }
        }
    }
}

void NinjamVst3AudioProcessor::setSyncToHost(bool shouldSync)
{
    setSyncMode(shouldSync ? SyncMode::host : SyncMode::off);
}

NinjamVst3AudioProcessor::SyncMode NinjamVst3AudioProcessor::getSyncMode() const
{
    return syncMode.load(std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::isTransportSyncEnabled() const
{
    return getSyncMode() != SyncMode::off;
}

void NinjamVst3AudioProcessor::setSyncMode(SyncMode newMode)
{
    syncMode.store(newMode, std::memory_order_relaxed);
    hostWasPlaying.store(false);
    linkWasPlaying.store(false);
    bool hostIsPlayingNow = false;
    bool linkIsPlayingNow = false;
    double linkPhaseBeats = 0.0;
    const double linkQuantum = juce::jmax(1.0, (double) getBPI());

    if (newMode == SyncMode::host)
    {
        const juce::ScopedLock lock(transportLock);
        hostIsPlayingNow = lastHostPositionValid.load(std::memory_order_relaxed)
            && lastHostPosition.isPlaying;
    }

    refreshAbletonLinkActivation();

    if (newMode == SyncMode::abletonLink && abletonLink != nullptr)
    {
        auto sessionState = abletonLink->captureAppSessionState();
        const auto now = abletonLink->clock().micros();
        linkIsPlayingNow = sessionState.isPlaying();
        linkPhaseBeats = sessionState.phaseAtTime(now, linkQuantum);
        const juce::ScopedLock lock(linkTransportStateLock);
        lastLinkTempo = sessionState.tempo();
        lastLinkPhaseBeats = linkPhaseBeats;
        lastLinkPeerCount = (int) abletonLink->numPeers();
        lastLinkIsPlaying = linkIsPlayingNow;
    }

    const bool waitForRestart = (newMode == SyncMode::host && hostIsPlayingNow)
        || (newMode == SyncMode::abletonLink && linkIsPlayingNow);

    syncAwaitingHostRestart.store(waitForRestart);
    syncWaitForInterval.store(false);
    syncTargetInterval.store(-1);
    syncDisplayIntervalOffset.store(intervalIndex.load());
    syncDisplayPositionOffset.store(0);

    if (newMode == SyncMode::host)
        primeSyncTransportStart();
    else if (newMode == SyncMode::abletonLink && linkIsPlayingNow)
    {
        hostWasPlaying.store(false);
        linkWasPlaying.store(false);
    }
}

bool NinjamVst3AudioProcessor::isSyncToHostEnabled() const
{
    return getSyncMode() == SyncMode::host;
}

bool NinjamVst3AudioProcessor::isAbletonLinkTransportEnabled() const
{
    return getSyncMode() == SyncMode::abletonLink;
}

void NinjamVst3AudioProcessor::setSyncStartCompensationMs(float ms)
{
    syncStartCompensationMs.store(juce::jlimit(0.0f, 250.0f, ms));
}

float NinjamVst3AudioProcessor::getSyncStartCompensationMs() const
{
    return syncStartCompensationMs.load();
}

int NinjamVst3AudioProcessor::getSyncStartCompensationSamples() const
{
    const double sampleRate = getSampleRate();
    if (sampleRate <= 1.0)
        return 0;

    const double compensationSamples = (double) syncStartCompensationMs.load() * sampleRate / 1000.0;
    return juce::jmax(0, (int) std::llround(compensationSamples));
}

void NinjamVst3AudioProcessor::primeSyncTransportStart(const juce::AudioPlayHead::CurrentPositionInfo* hostInfo)
{
    const juce::ScopedTryLock clientLock(ninjamClientLock);
    if (!clientLock.isLocked())
        return;

    ninjamClient.ResetTransportPhase();
    ninjamClient.ResetLocalBroadcastState();

    int intervalLength = 0;
    ninjamClient.GetPosition(nullptr, &intervalLength);

    int startPositionSamples = getSyncStartCompensationSamples();
    if (hostInfo != nullptr)
        startPositionSamples += computeJamTabaHostSyncStartPositionSamples(*hostInfo, getSampleRate());

    const int displayOffset = normaliseSignedIntervalPosition(startPositionSamples, intervalLength);

    ninjamClient.SetTransportPosition(displayOffset);
    syncDisplayPositionOffset.store(displayOffset);
    int hostPhasePosition = -1;
    if (hostInfo != nullptr)
        hostPhasePosition = computeHostIntervalPhasePositionSamples(*hostInfo, getSampleRate(), ninjamClient.GetBPI(), intervalLength);
    syncHostPhaseOffsetSamples.store(hostPhasePosition >= 0
        ? normaliseSignedIntervalPosition(displayOffset - hostPhasePosition, intervalLength)
        : 0);
}

void NinjamVst3AudioProcessor::primeLinkTransportStart(double phaseBeats, double quantum, double tempoBpm)
{
    const juce::ScopedTryLock clientLock(ninjamClientLock);
    if (!clientLock.isLocked())
        return;

    ninjamClient.ResetTransportPhase();
    ninjamClient.ResetLocalBroadcastState();

    int intervalLength = 0;
    ninjamClient.GetPosition(nullptr, &intervalLength);
    if (intervalLength <= 0)
        return;

    int startPositionSamples = computeLinkSyncStartPositionSamples(phaseBeats,
                                                                   quantum,
                                                                   tempoBpm,
                                                                   getSampleRate());
    startPositionSamples += getSyncStartCompensationSamples();

    const int displayOffset = normaliseSignedIntervalPosition(startPositionSamples, intervalLength);
    ninjamClient.SetTransportPosition(displayOffset);
    syncDisplayPositionOffset.store(displayOffset);
}

bool NinjamVst3AudioProcessor::getHostPosition(juce::AudioPlayHead::CurrentPositionInfo& info) const
{
    const juce::ScopedLock lock(transportLock);
    if (!lastHostPositionValid.load(std::memory_order_relaxed))
        return false;

    info = lastHostPosition;
    return true;
}

void NinjamVst3AudioProcessor::setLinkAudioEnabled(bool shouldEnable)
{
    linkAudioEnabled.store(shouldEnable, std::memory_order_relaxed);
    refreshAbletonLinkActivation();
}

bool NinjamVst3AudioProcessor::isLinkAudioEnabled() const
{
    return linkAudioEnabled.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setLinkAudioSendEnabled(bool shouldEnable)
{
    linkAudioSendEnabled.store(shouldEnable, std::memory_order_relaxed);
    rebuildLinkAudioEndpoints();
}

bool NinjamVst3AudioProcessor::isLinkAudioSendEnabled() const
{
    return linkAudioSendEnabled.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setLinkAudioReceiveEnabled(bool shouldEnable)
{
    linkAudioReceiveEnabled.store(shouldEnable, std::memory_order_relaxed);
    rebuildLinkAudioEndpoints();
}

bool NinjamVst3AudioProcessor::isLinkAudioReceiveEnabled() const
{
    return linkAudioReceiveEnabled.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setLinkAudioReceiveSelection(const juce::String& channelKey)
{
    {
        const juce::ScopedLock lock(linkAudioSelectionLock);
        linkAudioReceiveSelection = channelKey.trim();
    }
    rebuildLinkAudioEndpoints();
}

juce::String NinjamVst3AudioProcessor::getLinkAudioReceiveSelection() const
{
    const juce::ScopedLock lock(linkAudioSelectionLock);
    return linkAudioReceiveSelection;
}

std::vector<NinjamVst3AudioProcessor::LinkAudioChannelInfo> NinjamVst3AudioProcessor::getLinkAudioAvailableChannels() const
{
    std::vector<LinkAudioChannelInfo> result;
    if (abletonLink == nullptr || !isLinkAudioEnabled())
        return result;

    const juce::String localPeerName = getLinkPeerName();
    for (const auto& channel : abletonLink->channels())
    {
        LinkAudioChannelInfo info;
        info.name = juce::String::fromUTF8(channel.name.c_str()).trim();
        info.peerName = juce::String::fromUTF8(channel.peerName.c_str()).trim();
        if (info.name.isEmpty())
            info.name = "Audio";
        if (info.peerName.isEmpty())
            info.peerName = "Link";
        if (info.peerName == localPeerName)
            continue;
        info.key = buildLinkAudioChannelKey(info.peerName, info.name);
        result.push_back(std::move(info));
    }

    return result;
}

double NinjamVst3AudioProcessor::getLinkTempoBpm() const
{
    const juce::ScopedLock lock(linkTransportStateLock);
    return lastLinkTempo;
}

bool NinjamVst3AudioProcessor::isLinkTransportPlaying() const
{
    const juce::ScopedLock lock(linkTransportStateLock);
    return lastLinkIsPlaying;
}

int NinjamVst3AudioProcessor::getLinkPeerCount() const
{
    const juce::ScopedLock lock(linkTransportStateLock);
    return lastLinkPeerCount;
}

void NinjamVst3AudioProcessor::refreshAbletonLinkActivation()
{
    if (abletonLink == nullptr)
        abletonLink = std::make_unique<ableton::LinkAudio>(120.0, getLinkPeerName().toStdString());

    abletonLink->setPeerName(getLinkPeerName().toStdString());

    const bool transportEnabled = isAbletonLinkTransportEnabled();
    const bool audioEnabled = isLinkAudioEnabled();

    abletonLink->enable(transportEnabled || audioEnabled);
    abletonLink->enableStartStopSync(transportEnabled);
    abletonLink->enableLinkAudio(audioEnabled);

    if (!transportEnabled)
    {
        linkWasPlaying.store(false);
        const juce::ScopedLock lock(linkTransportStateLock);
        lastLinkIsPlaying = false;
    }

    lastLinkAudioEndpointRefreshMs = 0.0;
    rebuildLinkAudioEndpoints();
}

void NinjamVst3AudioProcessor::rebuildLinkAudioEndpoints()
{
    std::unique_ptr<ableton::LinkAudioSink> newSink;
    std::unique_ptr<ableton::LinkAudioSource> newSource;
    std::map<juce::String, std::unique_ptr<ableton::LinkAudioSink>> newRemoteSinks;

    if (abletonLink != nullptr && isLinkAudioEnabled())
    {
        if (isLinkAudioSendEnabled())
            newSink = std::make_unique<ableton::LinkAudioSink>(*abletonLink, std::string("Main Mix"), linkAudioMaxNumSamples);

        if (isLinkAudioSendEnabled())
        {
            const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
            for (const auto& pair : remoteLinkAudioOutputPairs)
            {
                const std::string channelName = pair.first.trim().isNotEmpty()
                    ? pair.first.trim().toStdString()
                    : std::string("Remote");
                newRemoteSinks.emplace(pair.first,
                                       std::make_unique<ableton::LinkAudioSink>(*abletonLink, channelName, linkAudioMaxNumSamples));
            }
        }

        if (isLinkAudioReceiveEnabled())
        {
            const juce::String selectedKey = getLinkAudioReceiveSelection();
            if (selectedKey.isNotEmpty())
            {
                for (const auto& channel : abletonLink->channels())
                {
                    const juce::String peerName = juce::String::fromUTF8(channel.peerName.c_str()).trim();
                    const juce::String channelName = juce::String::fromUTF8(channel.name.c_str()).trim();
                    if (buildLinkAudioChannelKey(peerName, channelName) != selectedKey)
                        continue;

                    newSource = std::make_unique<ableton::LinkAudioSource>(
                        *abletonLink,
                        channel.id,
                        [this](ableton::LinkAudioSource::BufferHandle bufferHandle)
                        {
                            if (bufferHandle.samples == nullptr)
                                return;

                            const size_t totalSamples = bufferHandle.info.numChannels * bufferHandle.info.numFrames;
                            if (totalSamples == 0)
                                return;

                            constexpr size_t batchSize = 512;
                            float left[batchSize] {};
                            float right[batchSize] {};
                            const size_t sourceChannels = bufferHandle.info.numChannels;
                            const bool isStereo = sourceChannels >= 2;
                            size_t framesLeft = bufferHandle.info.numFrames;
                            size_t sourceFrameOffset = 0;

                            while (framesLeft > 0)
                            {
                                const size_t framesThisBatch = juce::jmin(framesLeft, batchSize);
                                for (size_t frame = 0; frame < framesThisBatch; ++frame)
                                {
                                    const size_t sourceIndex = (sourceFrameOffset + frame) * sourceChannels;
                                    left[frame] = ableton::util::int16ToFloat<float>(bufferHandle.samples[sourceIndex]);
                                    right[frame] = isStereo
                                        ? ableton::util::int16ToFloat<float>(bufferHandle.samples[sourceIndex + 1u])
                                        : left[frame];
                                }

                                const size_t writtenFrames = linkAudioReceiveRing.write(left, right, framesThisBatch);
                                linkAudioFramesReceived.fetch_add((juce::uint64)writtenFrames, std::memory_order_relaxed);
                                if (writtenFrames < framesThisBatch)
                                    linkAudioFramesDropped.fetch_add((juce::uint64)(framesThisBatch - writtenFrames), std::memory_order_relaxed);

                                sourceFrameOffset += framesThisBatch;
                                framesLeft -= framesThisBatch;
                            }
                        });
                    break;
                }
            }
        }
    }

    const bool hasNewReceiveEndpoint = newSource != nullptr;
    const bool receiveEndpointChanged = hasNewReceiveEndpoint || (abletonLinkSource != nullptr && newSource == nullptr);
    {
        const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
        abletonLinkSink = std::move(newSink);
        abletonLinkSource = std::move(newSource);
        remoteLinkAudioSinks = std::move(newRemoteSinks);
    }

    if (receiveEndpointChanged || !hasNewReceiveEndpoint)
        linkAudioReceiveRing.reset();

    if (hasNewReceiveEndpoint)
        linkAudioReceiveSelectedMissingSinceMs = 0.0;
}

void NinjamVst3AudioProcessor::mixReceivedLinkAudioIntoBuffer(juce::AudioBuffer<float>& buffer, int numSamples)
{
    juce::ignoreUnused(buffer, numSamples);
}

juce::String NinjamVst3AudioProcessor::buildLinkAudioChannelKey(const juce::String& peerName, const juce::String& channelName) const
{
    return peerName.trim() + "::" + channelName.trim();
}

juce::String NinjamVst3AudioProcessor::getLinkPeerName() const
{
    const juce::String user = currentUser.trim();
    return user.isNotEmpty() ? (juce::String(JucePlugin_Name) + " " + user) : juce::String(JucePlugin_Name);
}

void NinjamVst3AudioProcessor::setMtcOutputEnabled(bool shouldEnable)
{
    mtcOutputEnabled.store(shouldEnable);
}

bool NinjamVst3AudioProcessor::isMtcOutputEnabled() const
{
    return mtcOutputEnabled.load();
}

void NinjamVst3AudioProcessor::setMtcFrameRate(int fps)
{
    int mapped = 30;
    if (fps == 24 || fps == 25 || fps == 30 || fps == 2997)
        mapped = fps;
    mtcFrameRateFps.store(mapped);
}

int NinjamVst3AudioProcessor::getMtcFrameRate() const
{
    return mtcFrameRateFps.load();
}

bool NinjamVst3AudioProcessor::isStandaloneInstance() const
{
    return isStandaloneWrapper();
}

std::vector<NinjamVst3AudioProcessor::MidiControllerEvent> NinjamVst3AudioProcessor::popPendingMidiControllerEvents()
{
    std::vector<MidiControllerEvent> events;
    const juce::SpinLock::ScopedLockType midiQueueLock(midiEventQueueLock);
    events.swap(pendingMidiControllerEvents);
    return events;
}

std::vector<NinjamVst3AudioProcessor::OscRelayEvent> NinjamVst3AudioProcessor::popPendingOscRelayEvents()
{
    std::vector<OscRelayEvent> events;
    {
        const juce::SpinLock::ScopedLockType lock(inboundOscRelayQueueLock);
        events.swap(pendingInboundOscRelayEvents);
    }

    if (events.empty())
        return {};

    const juce::String learnSource = getMidiLearnInputDeviceId();
    if (!(learnSource == "__learn_relay__" || learnSource.startsWith("__learn_relay__:")))
        return {};

    if (learnSource == "__learn_relay__" || learnSource == "__learn_relay__:*")
        return events;

    const juce::String desired = learnSource.fromFirstOccurrenceOf("__learn_relay__:", false, false).trim();
    if (desired.isEmpty() || desired == "*")
        return events;

    const juce::String desiredKey = normaliseOpusPeerId(desired);
    if (desiredKey.isEmpty())
        return {};

    std::vector<OscRelayEvent> filtered;
    filtered.reserve(events.size());
    for (const auto& e : events)
        if (e.senderKey == desiredKey)
            filtered.push_back(e);
    return filtered;
}

void NinjamVst3AudioProcessor::setMidiRelayTarget(const juce::String& targetUser)
{
    const juce::ScopedLock lock(midiRelayTargetLock);
    midiRelayTarget = targetUser.isNotEmpty() ? targetUser : "*";
}

juce::String NinjamVst3AudioProcessor::getMidiRelayTarget() const
{
    const juce::ScopedLock lock(midiRelayTargetLock);
    return midiRelayTarget.isNotEmpty() ? midiRelayTarget : "*";
}

void NinjamVst3AudioProcessor::setMidiLearnStateJson(const juce::String& json)
{
    const juce::ScopedLock lock(learnStateLock);
    midiLearnStateJson = json;
}

juce::String NinjamVst3AudioProcessor::getMidiLearnStateJson() const
{
    const juce::ScopedLock lock(learnStateLock);
    return midiLearnStateJson;
}

void NinjamVst3AudioProcessor::setOscLearnStateJson(const juce::String& json)
{
    const juce::ScopedLock lock(learnStateLock);
    oscLearnStateJson = json;
}

juce::String NinjamVst3AudioProcessor::getOscLearnStateJson() const
{
    const juce::ScopedLock lock(learnStateLock);
    return oscLearnStateJson;
}

void NinjamVst3AudioProcessor::setMidiLearnInputDeviceId(const juce::String& deviceId)
{
    const juce::ScopedLock lock(learnStateLock);
    midiLearnInputDeviceId = deviceId;
}

juce::String NinjamVst3AudioProcessor::getMidiLearnInputDeviceId() const
{
    const juce::ScopedLock lock(learnStateLock);
    return midiLearnInputDeviceId;
}

void NinjamVst3AudioProcessor::setMidiRelayInputDeviceId(const juce::String& deviceId)
{
    const juce::ScopedLock lock(learnStateLock);
    midiRelayInputDeviceId = deviceId;
}

juce::String NinjamVst3AudioProcessor::getMidiRelayInputDeviceId() const
{
    const juce::ScopedLock lock(learnStateLock);
    return midiRelayInputDeviceId;
}

void NinjamVst3AudioProcessor::setSamplePadsMidiInputDeviceId(const juce::String& deviceId)
{
    const juce::ScopedLock lock(learnStateLock);
    samplePadsMidiInputDeviceId = deviceId;
}

juce::String NinjamVst3AudioProcessor::getSamplePadsMidiInputDeviceId() const
{
    const juce::ScopedLock lock(learnStateLock);
    return samplePadsMidiInputDeviceId;
}

void NinjamVst3AudioProcessor::setSamplePadLooperInput(int inputIndex)
{
    samplePadLooperInput.store(inputIndex, std::memory_order_relaxed);
}

int NinjamVst3AudioProcessor::getSamplePadLooperInput() const
{
    return samplePadLooperInput.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::enqueueExternalMidiControllerEvent(const MidiControllerEvent& event, bool forLearn, bool forRelay)
{
    if (forLearn)
    {
        const juce::SpinLock::ScopedLockType midiQueueLock(midiEventQueueLock);
        pendingMidiControllerEvents.push_back(event);
        if (pendingMidiControllerEvents.size() > 512)
            pendingMidiControllerEvents.erase(pendingMidiControllerEvents.begin(), pendingMidiControllerEvents.begin() + (long long)(pendingMidiControllerEvents.size() - 512));
    }

    if (forRelay)
    {
        const juce::SpinLock::ScopedLockType relayQueueLock(outboundMidiRelayQueueLock);
        pendingOutboundMidiRelayEvents.push_back(event);
        if (pendingOutboundMidiRelayEvents.size() > 512)
            pendingOutboundMidiRelayEvents.erase(pendingOutboundMidiRelayEvents.begin(), pendingOutboundMidiRelayEvents.begin() + (long long)(pendingOutboundMidiRelayEvents.size() - 512));
    }
}

void NinjamVst3AudioProcessor::enqueueOutboundOscRelayEvent(const OscRelayEvent& event)
{
    if (event.address.isEmpty())
        return;
    const juce::SpinLock::ScopedLockType lock(outboundOscRelayQueueLock);
    pendingOutboundOscRelayEvents.push_back(event);
    if (pendingOutboundOscRelayEvents.size() > 512)
        pendingOutboundOscRelayEvents.erase(pendingOutboundOscRelayEvents.begin(), pendingOutboundOscRelayEvents.begin() + (long long)(pendingOutboundOscRelayEvents.size() - 512));
}

void NinjamVst3AudioProcessor::flushOutboundMidiRelayEvents()
{
    std::vector<MidiControllerEvent> events;
    {
        const juce::SpinLock::ScopedLockType lock(outboundMidiRelayQueueLock);
        events.swap(pendingOutboundMidiRelayEvents);
    }

    if (events.empty())
        return;

    const juce::String targetsRaw = getMidiRelayTarget().trim();
    juce::StringArray targets;
    if (targetsRaw.isEmpty() || targetsRaw == "*")
    {
        targets.add("*");
    }
    else
    {
        targets.addTokens(targetsRaw, ",", "");
        targets.trim();
        targets.removeEmptyStrings();
        targets.removeDuplicates(true);
        if (targets.isEmpty())
            targets.add("*");
    }

    const juce::String userId = currentUser;
    for (const auto& event : events)
    {
        juce::DynamicObject::Ptr obj = new juce::DynamicObject();
        obj->setProperty("userId", userId);
        obj->setProperty("isController", event.isController);
        obj->setProperty("midiChannel", event.midiChannel);
        obj->setProperty("number", event.number);
        obj->setProperty("value", event.value);
        obj->setProperty("normalized", event.normalized);
        obj->setProperty("isNoteOn", event.isNoteOn);
        const juce::String payload = juce::JSON::toString(juce::var(obj.get()));
        for (const auto& target : targets)
            sendSideSignal(target, "midiRelay", payload);
    }
}

void NinjamVst3AudioProcessor::flushOutboundOscRelayEvents()
{
    std::vector<OscRelayEvent> events;
    {
        const juce::SpinLock::ScopedLockType lock(outboundOscRelayQueueLock);
        events.swap(pendingOutboundOscRelayEvents);
    }

    if (events.empty())
        return;

    const juce::String targetsRaw = getMidiRelayTarget().trim();
    juce::StringArray targets;
    if (targetsRaw.isEmpty() || targetsRaw == "*")
    {
        targets.add("*");
    }
    else
    {
        targets.addTokens(targetsRaw, ",", "");
        targets.trim();
        targets.removeEmptyStrings();
        targets.removeDuplicates(true);
        if (targets.isEmpty())
            targets.add("*");
    }

    const juce::String userId = currentUser;
    for (const auto& event : events)
    {
        juce::DynamicObject::Ptr obj = new juce::DynamicObject();
        obj->setProperty("userId", userId);
        obj->setProperty("address", event.address);
        obj->setProperty("normalized", (double)event.normalized);
        obj->setProperty("binaryOn", event.binaryOn);
        const juce::String payload = juce::JSON::toString(juce::var(obj.get()));
        for (const auto& target : targets)
            sendSideSignal(target, "oscRelay", payload);
    }
}

void NinjamVst3AudioProcessor::injectInboundMidiRelayEvents(juce::MidiBuffer& midiMessages)
{
    std::vector<MidiControllerEvent> events;
    {
        const juce::SpinLock::ScopedLockType lock(inboundMidiRelayQueueLock);
        events.swap(pendingInboundMidiRelayEvents);
    }

    const bool relayFeedsSamplePads = isSamplePadsFeatureEnabled()
        && getSamplePadsMidiInputDeviceId() == samplePadsMidiInputRelayId;
    for (const auto& event : events)
    {
        if (relayFeedsSamplePads && !event.isController)
            handleSamplePadMidiNote(event.number, event.isNoteOn);

        if (event.isController)
            midiMessages.addEvent(juce::MidiMessage::controllerEvent(event.midiChannel, event.number, event.value), 0);
        else if (event.isNoteOn)
            midiMessages.addEvent(juce::MidiMessage::noteOn(event.midiChannel, event.number, (juce::uint8)event.value), 0);
        else
            midiMessages.addEvent(juce::MidiMessage::noteOff(event.midiChannel, event.number), 0);
    }
}

bool NinjamVst3AudioProcessor::isStandaloneWrapper() const
{
    return wrapperType == juce::AudioProcessor::wrapperType_Standalone;
}

int NinjamVst3AudioProcessor::getDisplayIntervalIndex() const
{
    const int absolute = intervalIndex.load();
    if (!isTransportSyncEnabled())
        return absolute;
    if (!hostWasPlaying.load())
        return 0;
    const int base = syncDisplayIntervalOffset.load();
    return juce::jmax(0, absolute - base);
}

void NinjamVst3AudioProcessor::emitMidiTimecode(juce::MidiBuffer& midiMessages, int numSamples, int pos, int length)
{
    const double sampleRate = getSampleRate();
    if (sampleRate <= 1.0 || numSamples <= 0)
        return;

    const bool mtcEnabled = isMtcOutputEnabled();
    const int fpsSetting = getMtcFrameRate();
    const double fps = fpsSetting == 2997 ? 29.97 : (double)fpsSetting;
    const juce::uint8 rateCode = fpsSetting == 24 ? 0x00 : fpsSetting == 25 ? 0x01 : fpsSetting == 2997 ? 0x02 : 0x03;

    const bool waitingForStart = isTransportSyncEnabled() && (!hostWasPlaying.load() || syncWaitForInterval.load());
    const bool shouldRun = (length > 0) && !waitingForStart;

    auto sendLocate = [&midiMessages, rateCode](int sampleOffset, int hours, int minutes, int seconds, int frames)
    {
        const juce::uint8 hr = (juce::uint8)(((rateCode & 0x03u) << 5) | ((juce::uint8)hours & 0x1Fu));
        const juce::uint8 sysex[] = { 0xF0, 0x7F, 0x7F, 0x01, 0x01,
                                      hr,
                                      (juce::uint8)minutes,
                                      (juce::uint8)seconds,
                                      (juce::uint8)frames,
                                      0xF7 };
        midiMessages.addEvent(juce::MidiMessage::createSysExMessage(sysex, (int)sizeof(sysex)), sampleOffset);
    };

    auto getTimecode = [sampleRate, fps](long long timelineSamples)
    {
        if (timelineSamples < 0)
            timelineSamples = 0;
        const double seconds = (double)timelineSamples / sampleRate;
        const long long totalFrames = (long long)std::floor(seconds * fps);
        const int frame = (int)(totalFrames % (long long)std::round(fps));
        const long long totalSeconds = (long long)std::floor((double)totalFrames / fps);
        const int second = (int)(totalSeconds % 60);
        const int minute = (int)((totalSeconds / 60) % 60);
        const int hour = (int)((totalSeconds / 3600) % 24);
        return std::array<int, 4> { hour, minute, second, frame };
    };

    if (!mtcEnabled)
    {
        if (mtcWasRunning)
        {
            midiMessages.addEvent(juce::MidiMessage::midiStop(), 0);
            sendLocate(0, 0, 0, 0, 0);
        }
        mtcWasRunning = false;
        mtcSamplesUntilNextQuarterFrame = 0.0;
        mtcQuarterFramePiece = 0;
        return;
    }

    if (mtcWasRunning && !shouldRun)
    {
        midiMessages.addEvent(juce::MidiMessage::midiStop(), 0);
        sendLocate(0, 0, 0, 0, 0);
        mtcSamplesUntilNextQuarterFrame = 0.0;
        mtcQuarterFramePiece = 0;
    }

    int displayInterval = getDisplayIntervalIndex();
    int timelinePos = 0;
    if (length > 0)
    {
        if (!waitingForStart)
            timelinePos = juce::jlimit(0, juce::jmax(0, length - 1), pos);
    }
    long long blockStartSamples = (long long)displayInterval * (long long)juce::jmax(0, length) + (long long)timelinePos;

    if (!mtcWasRunning && shouldRun)
    {
        const auto tc = getTimecode(blockStartSamples);
        sendLocate(0, tc[0], tc[1], tc[2], tc[3]);
        midiMessages.addEvent(juce::MidiMessage::midiStart(), 0);
        mtcSamplesUntilNextQuarterFrame = 0.0;
        mtcQuarterFramePiece = 0;
    }

    mtcWasRunning = shouldRun;
    if (!shouldRun)
        return;

    const double qfPerSecond = fps * 4.0;
    const double samplesPerQuarterFrame = sampleRate / qfPerSecond;
    double sampleCursor = mtcSamplesUntilNextQuarterFrame;
    if (sampleCursor <= 0.0)
        sampleCursor = samplesPerQuarterFrame;

    while (sampleCursor < (double)numSamples)
    {
        const int eventSample = juce::jlimit(0, numSamples - 1, (int)std::floor(sampleCursor));
        const long long eventTimelineSamples = blockStartSamples + (long long)eventSample;
        const auto tc = getTimecode(eventTimelineSamples);

        const int piece = mtcQuarterFramePiece & 0x07;
        int value = 0;
        switch (piece)
        {
            case 0: value = tc[3] & 0x0F; break;
            case 1: value = (tc[3] >> 4) & 0x01; break;
            case 2: value = tc[2] & 0x0F; break;
            case 3: value = (tc[2] >> 4) & 0x03; break;
            case 4: value = tc[1] & 0x0F; break;
            case 5: value = (tc[1] >> 4) & 0x03; break;
            case 6: value = tc[0] & 0x0F; break;
            case 7: value = ((tc[0] >> 4) & 0x01) | (0x03 << 1); break;
            default: break;
        }

        const juce::uint8 data = (juce::uint8)(((piece & 0x07) << 4) | (value & 0x0F));
        midiMessages.addEvent(juce::MidiMessage(0xF1, data), eventSample);
        mtcQuarterFramePiece = (piece + 1) & 0x07;
        sampleCursor += samplesPerQuarterFrame;
    }

    mtcSamplesUntilNextQuarterFrame = sampleCursor - (double)numSamples;
}

bool NinjamVst3AudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* NinjamVst3AudioProcessor::createEditor()
{
    return new NinjamVst3AudioProcessorEditor (*this);
}

void NinjamVst3AudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree state("NINJAM_STATE");
    state.setProperty("midiRelayTarget", getMidiRelayTarget(), nullptr);
    state.setProperty("midiLearnStateJson", getMidiLearnStateJson(), nullptr);
    state.setProperty("oscLearnStateJson", getOscLearnStateJson(), nullptr);
    state.setProperty("midiLearnInputDeviceId", getMidiLearnInputDeviceId(), nullptr);
    state.setProperty("midiRelayInputDeviceId", getMidiRelayInputDeviceId(), nullptr);
    state.setProperty("samplePadsMidiInputDeviceId", getSamplePadsMidiInputDeviceId(), nullptr);
    state.setProperty("samplePadsFeatureEnabled", isSamplePadsFeatureEnabled(), nullptr);
    state.setProperty("samplePadLooperInput", getSamplePadLooperInput(), nullptr);
    state.setProperty("autoTranslate", isAutoTranslateEnabled(), nullptr);
    state.setProperty("translateSourceLang", getTranslateSourceLang(), nullptr);
    state.setProperty("translateTargetLang", getTranslateTargetLang(), nullptr);
    state.setProperty("fxReverbEnabled", isFxReverbEnabled(), nullptr);
    state.setProperty("fxReverbWetDryMix", (double)getFxReverbWetDryMix(), nullptr);
    state.setProperty("fxDelayEnabled", isFxDelayEnabled(), nullptr);
    state.setProperty("fxDelayMode", (int)getFxDelayMode(), nullptr);
    state.setProperty("fxDelayTimeMs", (double)getFxDelayTimeMs(), nullptr);
    state.setProperty("fxDelaySyncToHost", isFxDelaySyncToHost(), nullptr);
    state.setProperty("fxDelayDivision", getFxDelayDivision(), nullptr);
    state.setProperty("fxDelayPingPong", isFxDelayPingPong(), nullptr);
    state.setProperty("fxDelayWetDryMix", (double)getFxDelayWetDryMix(), nullptr);
    state.setProperty("fxDelayFeedback", (double)getFxDelayFeedback(), nullptr);
    state.setProperty("syncMode", (int)getSyncMode(), nullptr);
    state.setProperty("syncStartCompensationMs", (double)getSyncStartCompensationMs(), nullptr);
    state.setProperty("linkAudioEnabled", isLinkAudioEnabled(), nullptr);
    state.setProperty("linkAudioSendEnabled", isLinkAudioSendEnabled(), nullptr);
    state.setProperty("linkAudioReceiveEnabled", isLinkAudioReceiveEnabled(), nullptr);
    state.setProperty("linkAudioReceiveSelection", getLinkAudioReceiveSelection(), nullptr);
    state.setProperty("metronomeMuted", isMetronomeMuted(), nullptr);
    state.setProperty("metronomeVolume", (double)getStoredMetronomeVolume(), nullptr);
    state.setProperty("metronomeSoundKey", getMetronomeSoundKey(), nullptr);
    state.setProperty("metronomeOutputChannel", getMetronomeOutputChannel(), nullptr);
    state.setProperty("transmitLocal", isTransmittingLocal(), nullptr);
    state.setProperty("mobileHotspotMode", isMobileHotspotModeEnabled(), nullptr);
    state.setProperty("dpiScaleSetting", getDpiScaleSetting(), nullptr);
    state.setProperty("sshTunnelEnabled", isSshTunnelEnabled(), nullptr);
    state.setProperty("sshTunnelHost", getSshTunnelHost(), nullptr);
    state.setProperty("sshTunnelPort", getSshTunnelPort(), nullptr);
    state.setProperty("sshTunnelUser", getSshTunnelUser(), nullptr);
    state.setProperty("sshTunnelKeyFile", getSshTunnelKeyFile(), nullptr);
    state.setProperty("autoReconnectEnabled", isAutoReconnectEnabled(), nullptr);
    state.setProperty("chordDetectionEnabled", isChordDetectionEnabled(), nullptr);
    state.setProperty("samplePadsVolume", (double)getSamplePadVolume(), nullptr);
    state.setProperty("samplePadsLimiter", isSamplePadLimiterEnabled(), nullptr);
    state.setProperty("samplePadsDuck", isSamplePadDuckEnabled(), nullptr);
    state.setProperty("samplePadsDuckShape", (int)getSamplePadDuckShape(), nullptr);
    state.setProperty("samplePadsDuckLength", (int)getSamplePadDuckLength(), nullptr);
    state.setProperty("samplePadsUseDefaultFx", getSamplePadsUseDefaultFx(), nullptr);
    for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
    {
        state.setProperty("samplePadFxType" + juce::String(slot), (int)getSamplePadFxSlotType(slot), nullptr);
        state.setProperty("samplePadFxAmount" + juce::String(slot), (double)getSamplePadFxSlotAmount(slot), nullptr);
        for (int targetSlot = 0; targetSlot < numSamplePadFxSlots; ++targetSlot)
        {
            state.setProperty("samplePadFxSlotChainRoute" + juce::String(slot) + "_" + juce::String(targetSlot),
                              isSamplePadFxSlotToSlotRouteEnabled(slot, targetSlot),
                              nullptr);
        }
    }
    {
        const juce::ScopedLock lock(samplePadsLock);
        for (int pad = 0; pad < numSamplePads; ++pad)
        {
            const auto& samplePad = samplePads[(size_t)pad];
            state.setProperty("samplePadFile" + juce::String(pad), samplePad.file.getFullPathName(), nullptr);
            state.setProperty("samplePadLoop" + juce::String(pad), samplePad.loop.load(std::memory_order_relaxed), nullptr);
            state.setProperty("samplePadReverse" + juce::String(pad), samplePad.reverse.load(std::memory_order_relaxed), nullptr);
            state.setProperty("samplePadMatchBpi" + juce::String(pad), samplePad.matchBpi.load(std::memory_order_relaxed), nullptr);
            state.setProperty("samplePadBpmSync" + juce::String(pad), samplePad.bpmSyncEnabled.load(std::memory_order_relaxed), nullptr);
            state.setProperty("samplePadPlaybackSpeed" + juce::String(pad), samplePad.playbackSpeed.load(std::memory_order_relaxed), nullptr);
            state.setProperty("samplePadDuckRoute" + juce::String(pad), samplePad.duckRoute.load(std::memory_order_relaxed), nullptr);
            state.setProperty("samplePadVolume" + juce::String(pad), (double)samplePad.volume.load(std::memory_order_relaxed), nullptr);
            for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
            {
                state.setProperty("samplePadFxSlotRoute" + juce::String(pad) + "_" + juce::String(slot),
                                  samplePad.fxSlotRoutes[(size_t)slot].load(std::memory_order_relaxed),
                                  nullptr);
            }
            state.setProperty("samplePadName" + juce::String(pad), samplePad.name, nullptr);
            state.setProperty("samplePadNameCustom" + juce::String(pad), samplePad.nameIsCustom, nullptr);
        }
    }
    for (int channel = 0; channel < maxLocalChannels; ++channel)
        state.setProperty("localInput" + juce::String(channel), getLocalChannelInput(channel), nullptr);
    state.setProperty("voiceInput", getVoiceChannelInput(), nullptr);
    state.setProperty("voiceGain", (double)getVoiceChannelGain(), nullptr);
    state.setProperty("autoTuneEnabled", getAutoTuneEnabled(), nullptr);
    state.setProperty("autoTuneQuality", getAutoTuneQuality(), nullptr);
    state.setProperty("autoTuneScale", getAutoTuneScale(), nullptr);
    state.setProperty("autoTuneKey", getAutoTuneKey(), nullptr);
    state.setProperty("autoTuneSpeed", (double)getAutoTuneSpeed(), nullptr);
    state.setProperty("localBitrate", getLocalBitrate(), nullptr);
    {
        const juce::ScopedLock lock(localChannelNamesLock);
        for (int channel = 0; channel < maxLocalChannels; ++channel)
            state.setProperty("localChannelName" + juce::String(channel), localChannelNames[channel], nullptr);
    }

    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void NinjamVst3AudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    if (xmlState == nullptr)
        return;

    const juce::ValueTree state = juce::ValueTree::fromXml(*xmlState);
    if (!state.isValid())
        return;

    setMidiRelayTarget(state.getProperty("midiRelayTarget", "*").toString());
    setMidiLearnStateJson(state.getProperty("midiLearnStateJson", "").toString());
    setOscLearnStateJson(state.getProperty("oscLearnStateJson", "").toString());
    setMidiLearnInputDeviceId(state.getProperty("midiLearnInputDeviceId", "").toString());
    setMidiRelayInputDeviceId(state.getProperty("midiRelayInputDeviceId", "").toString());
    setSamplePadsMidiInputDeviceId(state.getProperty("samplePadsMidiInputDeviceId", "").toString());
    setSamplePadLooperInput((int)state.getProperty("samplePadLooperInput", looperInputLocalChannel));
    setSamplePadsFeatureEnabled((bool)state.getProperty("samplePadsFeatureEnabled", true));
    setAutoTranslateEnabled((bool) state.getProperty("autoTranslate", false));
    setTranslateSourceLang(state.getProperty("translateSourceLang", "en").toString());
    setTranslateTargetLang(state.getProperty("translateTargetLang", "system").toString());
    setFxReverbEnabled((bool)state.getProperty("fxReverbEnabled", true));
    setFxReverbWetDryMix((float)(double)state.getProperty("fxReverbWetDryMix", 1.0));
    setFxDelayEnabled((bool)state.getProperty("fxDelayEnabled", true));
    setFxDelayMode((int)state.getProperty("fxDelayMode", (int)FxDelayMode::standard) == (int)FxDelayMode::frippertronics
        ? FxDelayMode::frippertronics
        : FxDelayMode::standard);
    setFxDelayTimeMs((float)(double)state.getProperty("fxDelayTimeMs", 320.0));
    setFxDelaySyncToHost((bool)state.getProperty("fxDelaySyncToHost", true));
    setFxDelayDivision((int)state.getProperty("fxDelayDivision", 8));
    setFxDelayPingPong((bool)state.getProperty("fxDelayPingPong", false));
    setFxDelayWetDryMix((float)(double)state.getProperty("fxDelayWetDryMix", 1.0));
    setFxDelayFeedback((float)(double)state.getProperty("fxDelayFeedback", 0.38));
    setSyncMode((SyncMode) (int) state.getProperty("syncMode", (int) SyncMode::off));
    setSyncStartCompensationMs((float)(double)state.getProperty("syncStartCompensationMs", 0.0));
    setLinkAudioEnabled((bool)state.getProperty("linkAudioEnabled", false));
    setLinkAudioSendEnabled((bool)state.getProperty("linkAudioSendEnabled", true));
    setLinkAudioReceiveEnabled((bool)state.getProperty("linkAudioReceiveEnabled", false));
    setLinkAudioReceiveSelection(state.getProperty("linkAudioReceiveSelection", "").toString());
    storedMetronomeVolume.store(juce::jlimit(0.0f, 1.0f, (float)(double)state.getProperty("metronomeVolume", 1.0)));
    setMetronomeMuted((bool)state.getProperty("metronomeMuted", false));
    setMetronomeSoundKey(state.getProperty("metronomeSoundKey", getClassicMetronomeSoundKey()).toString());
    setMetronomeOutputChannel((int)state.getProperty("metronomeOutputChannel", 0));
    setTransmitLocal((bool)state.getProperty("transmitLocal", false));
    setMobileHotspotModeEnabled((bool)state.getProperty("mobileHotspotMode", false));
    setDpiScaleSetting((int)state.getProperty("dpiScaleSetting", 0));
    setSshTunnelEnabled((bool)state.getProperty("sshTunnelEnabled", false));
    setSshTunnelHost(state.getProperty("sshTunnelHost", "").toString());
    setSshTunnelPort((int)state.getProperty("sshTunnelPort", 22));
    setSshTunnelUser(state.getProperty("sshTunnelUser", "").toString());
    setSshTunnelKeyFile(state.getProperty("sshTunnelKeyFile", "").toString());
    setAutoReconnectEnabled((bool)state.getProperty("autoReconnectEnabled", true));
    setChordDetectionEnabled((bool)state.getProperty("chordDetectionEnabled", true));
    setSamplePadVolume(juce::jlimit(0.0f, 2.0f, (float)(double)state.getProperty("samplePadsVolume", 1.0)));
    setSamplePadLimiterEnabled((bool)state.getProperty("samplePadsLimiter", false));
    setSamplePadDuckEnabled((bool)state.getProperty("samplePadsDuck", false));
    setSamplePadDuckShape(sanitizeSamplePadDuckShape((int)state.getProperty("samplePadsDuckShape",
                                                                            (int)SamplePadDuckShape::smoothPump)));
    setSamplePadDuckLength(sanitizeSamplePadDuckLength((int)state.getProperty("samplePadsDuckLength",
                                                                              (int)SamplePadDuckLength::quarter)));
    setSamplePadsUseDefaultFx((bool)state.getProperty("samplePadsUseDefaultFx", true));
    for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
    {
        setSamplePadFxSlotType(slot,
                               sanitizeSamplePadFxType((int)state.getProperty("samplePadFxType" + juce::String(slot),
                                                                               (int)getSamplePadFxSlotType(slot))));
        setSamplePadFxSlotAmount(slot,
                                 juce::jlimit(0.0f, 1.0f,
                                              (float)(double)state.getProperty("samplePadFxAmount" + juce::String(slot),
                                                                               getSamplePadFxSlotAmount(slot))));
    }
    for (int sourceSlot = 0; sourceSlot < numSamplePadFxSlots; ++sourceSlot)
    {
        for (int targetSlot = 0; targetSlot < numSamplePadFxSlots; ++targetSlot)
        {
            setSamplePadFxSlotToSlotRouteEnabled(sourceSlot, targetSlot, false);
            if ((bool)state.getProperty("samplePadFxSlotChainRoute" + juce::String(sourceSlot) + "_" + juce::String(targetSlot), false))
                setSamplePadFxSlotToSlotRouteEnabled(sourceSlot, targetSlot, true);
        }
    }
    for (int pad = 0; pad < numSamplePads; ++pad)
    {
        const juce::String filePath = state.getProperty("samplePadFile" + juce::String(pad), "").toString();
        const bool loaded = filePath.isNotEmpty() && loadSamplePad(pad, juce::File(filePath));
        if (!loaded)
            clearSamplePad(pad);

        setSamplePadPlaybackSpeed(pad,
                                  sanitizeSamplePadPlaybackSpeed((int)state.getProperty("samplePadPlaybackSpeed" + juce::String(pad),
                                                                                        (int)SamplePadPlaybackSpeed::normal)));
        setSamplePadBpmSyncEnabled(pad, (bool)state.getProperty("samplePadBpmSync" + juce::String(pad), true));
        setSamplePadLoopEnabled(pad, (bool)state.getProperty("samplePadLoop" + juce::String(pad), false));
        setSamplePadReverseEnabled(pad, (bool)state.getProperty("samplePadReverse" + juce::String(pad), false));
        setSamplePadMatchBpiEnabled(pad, (bool)state.getProperty("samplePadMatchBpi" + juce::String(pad), false));
        setSamplePadDuckRouteEnabled(pad, (bool)state.getProperty("samplePadDuckRoute" + juce::String(pad), false));
        setSamplePadVolume(pad, juce::jlimit(0.0f, 2.0f, (float)(double)state.getProperty("samplePadVolume" + juce::String(pad), 1.0)));
        for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
            setSamplePadFxSlotRouteEnabled(pad,
                                           slot,
                                           (bool)state.getProperty("samplePadFxSlotRoute" + juce::String(pad) + "_" + juce::String(slot),
                                                                   false));
        if ((bool)state.getProperty("samplePadNameCustom" + juce::String(pad), false))
            setSamplePadName(pad, state.getProperty("samplePadName" + juce::String(pad), "").toString());
    }
    for (int channel = 0; channel < maxLocalChannels; ++channel)
        setLocalChannelInput(channel, (int)state.getProperty("localInput" + juce::String(channel), -1));
    setVoiceChannelInput((int)state.getProperty("voiceInput", 0));
    setVoiceChannelGain(juce::jlimit(0.0f, 2.0f, (float)(double)state.getProperty("voiceGain", 1.0)));
    setAutoTuneEnabled((bool)state.getProperty("autoTuneEnabled", false));
    setAutoTuneQuality((int)state.getProperty("autoTuneQuality", 0));
    setAutoTuneScale((int)state.getProperty("autoTuneScale", 0));
    setAutoTuneKey((int)state.getProperty("autoTuneKey", 0));
    setAutoTuneSpeed(juce::jlimit(0.0f, 1.0f, (float)(double)state.getProperty("autoTuneSpeed", 1.0)));
    setLocalBitrate((int)state.getProperty("localBitrate", 64));
    {
        const juce::ScopedLock lock(localChannelNamesLock);
        for (int channel = 0; channel < maxLocalChannels; ++channel)
            localChannelNames[channel] = state.getProperty("localChannelName" + juce::String(channel), "Ch" + juce::String(channel + 1)).toString();
    }
}

void NinjamVst3AudioProcessor::processPendingIntervalSyncMarkers(int localMarkerBeat, long long localMarkerSampleCount, double intervalDurationMs)
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
        return;

    const int safeLocalMarkerBeat = juce::jmax(0, localMarkerBeat);
    const double safeIntervalDurationMs = juce::jmax(1.0, intervalDurationMs);
    const double localMarkerAtMs = juce::Time::getMillisecondCounterHiRes();
    const int localIntervalNow = getDisplayIntervalIndex();

    for (;;)
    {
        juce::String senderKey;
        PendingRemoteIntervalStart pending;
        {
            const juce::ScopedLock lock(intervalSyncAnnouncementLock);
            if (pendingRemoteIntervalStartsByUser.empty())
                break;
            for (auto staleIt = pendingRemoteIntervalStartsByUser.begin(); staleIt != pendingRemoteIntervalStartsByUser.end();)
            {
                if (staleIt->second.receivedSampleCount < 0 && staleIt->second.receivedAtMs <= 0.0)
                    staleIt = pendingRemoteIntervalStartsByUser.erase(staleIt);
                else if (staleIt->second.remoteIntervalAbsolute >= 0
                         && localIntervalNow >= 0
                         && staleIt->second.remoteIntervalAbsolute < localIntervalNow - 2)
                {
                    // Pending entry from an interval that has already passed — remove it
                    // so it doesn't produce a buffer offset by a full interval.
                    staleIt = pendingRemoteIntervalStartsByUser.erase(staleIt);
                }
                else
                    ++staleIt;
            }
            auto chosenIt = pendingRemoteIntervalStartsByUser.end();
            for (auto it = pendingRemoteIntervalStartsByUser.begin(); it != pendingRemoteIntervalStartsByUser.end(); ++it)
            {
                if (it->second.remoteBeat == safeLocalMarkerBeat && it->second.receivedSampleCount <= localMarkerSampleCount)
                {
                    chosenIt = it;
                    break;
                }
            }
            if (chosenIt == pendingRemoteIntervalStartsByUser.end())
                break;
            pending = chosenIt->second;
            senderKey = pending.senderKey.isNotEmpty()
                ? pending.senderKey
                : chosenIt->first.upToFirstOccurrenceOf(":", false, false);
            pendingRemoteIntervalStartsByUser.erase(chosenIt);
        }
        if (senderKey.isEmpty() || (pending.receivedSampleCount < 0 && pending.receivedAtMs <= 0.0))
            continue;
        double elapsedToNextLocalMarkerMs = -1.0;
        if (pending.receivedAtMs > 0.0 && localMarkerAtMs >= pending.receivedAtMs)
        {
            elapsedToNextLocalMarkerMs = localMarkerAtMs - pending.receivedAtMs;
        }
        else if (pending.receivedSampleCount >= 0)
        {
            const long long elapsedSamples = localMarkerSampleCount - pending.receivedSampleCount;
            if (elapsedSamples < 0)
                continue;
            const double sampleRate = juce::jmax(1.0, getSampleRate());
            elapsedToNextLocalMarkerMs = ((double)elapsedSamples / sampleRate) * 1000.0;
        }
        const double outlierLimitMs = safeIntervalDurationMs * 2.0;
        if (!std::isfinite(elapsedToNextLocalMarkerMs) || elapsedToNextLocalMarkerMs < 0.0 || elapsedToNextLocalMarkerMs > outlierLimitMs)
            continue;
        const int elapsedMs = (int)std::llround(juce::jlimit(0.0, safeIntervalDurationMs, elapsedToNextLocalMarkerMs));
        int averageMs = -1;
        int firmAverageMs = -1;
        int correctedDelayMs = -1;
        const int senderServerLatencyMs = pending.remoteServerLatencyMs >= 0 ? juce::jmax(0, pending.remoteServerLatencyMs) : 0;
        const int serverRouteLatencyMs = pending.serverRouteLatencyMs >= 0
            ? juce::jmax(0, pending.serverRouteLatencyMs)
            : 0;
        {
            const juce::ScopedLock lock(intervalSyncAnnouncementLock);
            auto& avgState = remoteLatencyAverageByUser[senderKey];
            avgState.lastMeasurementMs = (double)elapsedMs;
            bool includeInAverage = true;
            if (avgState.sampleCount >= 3)
            {
                const double baselineMs = avgState.firmAverageMs > 0.0 ? avgState.firmAverageMs : avgState.averageMs;
                const double deltaMs = std::abs((double)elapsedMs - baselineMs);
                const double spikeThresholdMs = juce::jlimit(5.0, 20.0, baselineMs * 0.30 + 2.0);
                if (deltaMs > spikeThresholdMs)
                {
                    const int spikeDirection = elapsedMs > baselineMs ? 1 : -1;
                    if (avgState.rejectedSpikeDirection != spikeDirection)
                    {
                        avgState.rejectedSpikeDirection = spikeDirection;
                        avgState.rejectedSpikeCount = 0;
                        avgState.rejectedSpikeSumMs = 0.0;
                    }
                    avgState.rejectedSpikeCount += 1;
                    avgState.rejectedSpikeSumMs += (double)elapsedMs;
                    if (avgState.rejectedSpikeCount >= 3)
                    {
                        avgState.sampleCount = avgState.rejectedSpikeCount;
                        avgState.sumMs = avgState.rejectedSpikeSumMs;
                        avgState.averageMs = avgState.sumMs / (double)juce::jmax(1, avgState.sampleCount);
                        avgState.firmAverageMs = avgState.averageMs;
                        avgState.rejectedSpikeCount = 0;
                        avgState.rejectedSpikeDirection = 0;
                        avgState.rejectedSpikeSumMs = 0.0;
                    }
                    includeInAverage = false;
                }
            }
            if (includeInAverage)
            {
                avgState.rejectedSpikeCount = 0;
                avgState.rejectedSpikeDirection = 0;
                avgState.rejectedSpikeSumMs = 0.0;
                avgState.sampleCount += 1;
                avgState.sumMs += (double)elapsedMs;
                avgState.averageMs = avgState.sumMs / (double)juce::jmax(1, avgState.sampleCount);
                if (avgState.sampleCount == 1)
                    avgState.firmAverageMs = (double)elapsedMs;
                else
                    avgState.firmAverageMs = (avgState.firmAverageMs * 0.88) + ((double)elapsedMs * 0.12);
            }
            if (avgState.sampleCount >= 3)
            {
                averageMs = juce::jmax(0, (int)std::llround(avgState.averageMs));
                firmAverageMs = juce::jmax(0, (int)std::llround(avgState.firmAverageMs));
            }
            else if (avgState.lastMeasurementMs >= 0.0)
            {
                averageMs = juce::jmax(0, (int)std::llround(avgState.lastMeasurementMs));
            }
            if (firmAverageMs >= 0 || averageMs >= 0)
            {
                const double rawDelayMs = (double)(firmAverageMs >= 0 ? firmAverageMs : averageMs);
                // The measured elapsed time already includes the full end-to-end path
                // (remote user → server → us), so we do NOT add serverRouteLatencyMs
                // on top — that would double-count the network latency.
                correctedDelayMs = juce::jmax(0, (int)std::llround(rawDelayMs));
            }
        }
        if (correctedDelayMs >= 0)
        {
            const int sourceInterval = pending.remoteIntervalAbsolute >= 0 ? pending.remoteIntervalAbsolute : pending.remoteInterval;
            const long long sourceMarkerKey = makeIntervalSyncMarkerKey(sourceInterval, pending.remoteBeat);
            const juce::String canonicalSenderKey = canonicalDelayUserKey(senderKey);
            const juce::ScopedLock lock(intervalSyncAnnouncementLock);
            long long priorAppliedMarker = std::numeric_limits<long long>::min();
            auto appliedIt = remoteLatencyLastAppliedIntervalByUser.find(senderKey);
            if (appliedIt != remoteLatencyLastAppliedIntervalByUser.end())
                priorAppliedMarker = appliedIt->second;
            const bool isFirstAppliedDelay = (appliedIt == remoteLatencyLastAppliedIntervalByUser.end());
            bool shouldApply = isFirstAppliedDelay;
            if (!shouldApply)
            {
                const long long markerDelta = sourceMarkerKey - priorAppliedMarker;
                const bool cadenceReached = markerDelta >= remoteLatencyUpdateCadenceIntervals;
                const bool markerSequenceReset = sourceMarkerKey + intervalSyncMarkerKeyBeatStride < priorAppliedMarker;
                shouldApply = cadenceReached || markerSequenceReset;
            }
            if (shouldApply)
            {
                remoteLatencyFirmDelayMsByUser[senderKey] = correctedDelayMs;
                if (canonicalSenderKey.isNotEmpty())
                    remoteLatencyFirmDelayMsByUser[canonicalSenderKey] = correctedDelayMs;
                lastRemoteServerLatencyMsByUser[senderKey] = senderServerLatencyMs;
                if (canonicalSenderKey.isNotEmpty())
                    lastRemoteServerLatencyMsByUser[canonicalSenderKey] = senderServerLatencyMs;
                remoteLatencyLastAppliedIntervalByUser[senderKey] = sourceMarkerKey;
                if (canonicalSenderKey.isNotEmpty())
                    remoteLatencyLastAppliedIntervalByUser[canonicalSenderKey] = sourceMarkerKey;
                if (isFirstAppliedDelay && vdoVideoSyncEnabled.load(std::memory_order_relaxed) && videoHelperRunning.load())
                {
                    const auto refreshId = ++videoBufferRefreshCounter;
                    remoteVideoBufferRefreshIdByUser[senderKey] = refreshId;
                    if (canonicalSenderKey.isNotEmpty())
                        remoteVideoBufferRefreshIdByUser[canonicalSenderKey] = refreshId;
                    intervalHelperPayloadForceWrite.store(true, std::memory_order_release);
                    lastIntervalHelperPayloadWriteMs = 0.0;
                }
            }
        }
    }
}

void NinjamVst3AudioProcessor::timerCallback()
{
    processPendingMediaItems();

    const double timerStartMs = juce::Time::getMillisecondCounterHiRes();
    juce::String intervalPerfDetails;
    auto noteSlowIntervalStep = [&intervalPerfDetails](const char* name, double elapsedMs)
    {
        if (elapsedMs < intervalPerfStepThresholdMs)
            return;

        if (intervalPerfDetails.isNotEmpty())
            intervalPerfDetails << " ";
        intervalPerfDetails << name << "=" << juce::String(elapsedMs, 2) << "ms";
    };
    bool perfIntervalWrapped = false;
    bool perfMarkerChanged = false;
    bool perfHelperWrote = false;
    int perfDisplayInterval = -1;

    int loopCount = 0;
    const bool budgetNinjamRun = vdoVideoSyncEnabled.load(std::memory_order_relaxed);
    const int maxRunIterations = budgetNinjamRun ? ninjamRunMaxIterationsPerTimer
                                                  : ninjamRunDefaultMaxIterationsPerTimer;
    double stepStartMs = juce::Time::getMillisecondCounterHiRes();
    {
        const juce::ScopedLock clientLock(ninjamClientLock);
        bool wantsSleep = false;
        do
        {
            wantsSleep = ninjamClient.Run() != 0;
            ++loopCount;
        }
        while (!wantsSleep
               && loopCount < maxRunIterations
               && (!budgetNinjamRun
                   || (juce::Time::getMillisecondCounterHiRes() - stepStartMs) < ninjamRunBudgetMs));

        int cachePos = 0, cacheLen = 0;
        ninjamClient.GetPosition(&cachePos, &cacheLen);
        cachedNinjamTransportPos.store(cachePos, std::memory_order_relaxed);
        cachedNinjamTransportLen.store(cacheLen, std::memory_order_relaxed);
        cachedNinjamBpi.store(juce::jmax(1, ninjamClient.GetBPI()), std::memory_order_relaxed);
        cachedNinjamBpm.store(juce::jmax(1.0f, (float)ninjamClient.GetActualBPM()), std::memory_order_relaxed);
        cachedNinjamTransportSampleCounter.store(intervalSyncSampleCounter.load(std::memory_order_relaxed),
                                                 std::memory_order_release);
    }
    noteSlowIntervalStep("clientRun", juce::Time::getMillisecondCounterHiRes() - stepStartMs);
    updateMasterChordTimeline();

    // Check for deferred playback speed changes that were applied at a loop boundary
    // and trigger the resync from here (safe to use samplePadsLock).
    if (isSamplePadsFeatureEnabled())
    {
        const double currentBpm = (double)getBPM();
        if (currentBpm > 1.0)
        {
            for (int pad = 0; pad < numSamplePads; ++pad)
            {
                if (samplePadPendingSpeedResync[(size_t)pad].exchange(false, std::memory_order_relaxed))
                    resyncSamplePadToBpm(pad, currentBpm, true);
            }
        }
    }

    if (disconnectAfterLicenseRejected.exchange(false, std::memory_order_acq_rel))
    {
        disconnectFromServer();
        return;
    }

    int status = NJClient::NJC_STATUS_DISCONNECTED;
    bool serverSupportsZapVideo = false;
    bool serverSupportsSideSignal = false;
    {
        const juce::ScopedLock clientLock(ninjamClientLock);
        status = ninjamClient.GetStatus();
        serverSupportsZapVideo = status == NJClient::NJC_STATUS_OK && ninjamClient.GetServerVideoSupported();
    }
    ninjamZapServerVideoSupported.store(serverSupportsZapVideo, std::memory_order_relaxed);
    serverSupportsSideSignal = status == NJClient::NJC_STATUS_OK
        && (serverSupportsZapVideo || ninjamSideSignalServerSupported.load(std::memory_order_relaxed));
    if (serverSupportsSideSignal)
        ninjamSideSignalServerSupported.store(true, std::memory_order_relaxed);
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    if (status == NJClient::NJC_STATUS_PRECONNECT
        && autoReconnectAttemptStartedMs.load(std::memory_order_relaxed) > 0.0
        && (nowMs - autoReconnectAttemptStartedMs.load(std::memory_order_relaxed)) >= 20000.0)
    {
        {
            const juce::ScopedLock lifecycleLock(ninjamAudioLifecycleLock);
            const juce::ScopedLock clientLock(ninjamClientLock);
            ninjamClient.Disconnect();
        }
        autoReconnectAttemptStartedMs.store(0.0, std::memory_order_relaxed);
        status = NJClient::NJC_STATUS_DISCONNECTED;
        scheduleAutoReconnect(nowMs, "connection attempt timed out");
    }
    if (status == NJClient::NJC_STATUS_OK
        && mobileHotspotModeEnabled.load(std::memory_order_relaxed)
        && (lastMobileHotspotHeartbeatSendMs <= 0.0 || (nowMs - lastMobileHotspotHeartbeatSendMs) >= 500.0))
    {
        sendIntervalSignal("mobileHotspotKeepalive", "x", "*");
        lastMobileHotspotHeartbeatSendMs = nowMs;
    }
    const bool vdoSyncActive = vdoVideoSyncEnabled.load(std::memory_order_relaxed)
        && !ninjamZapVideoEnabled.load(std::memory_order_relaxed);
    const bool wantsSideSignalVideoCap = vdoSyncActive
        || ninjamZapVideoEnabled.load(std::memory_order_relaxed)
        || ninjamZapCameraSendEnabled.load(std::memory_order_relaxed)
        || ninjamZapBrowserCameraSendEnabled.load(std::memory_order_relaxed);
    if (serverSupportsSideSignal
        && wantsSideSignalVideoCap
        && (lastNinjamVideoCapSendMs <= 0.0 || (nowMs - lastNinjamVideoCapSendMs) >= 10000.0))
    {
        const juce::ScopedLock clientLock(ninjamClientLock);
        if (ninjamClient.GetStatus() == NJClient::NJC_STATUS_OK)
        {
            ninjamClient.ChatMessage_Send("VIDEO_CAP", "1", nullptr, nullptr, nullptr);
            lastNinjamVideoCapSendMs = nowMs;
        }
    }
    if (status == NJClient::NJC_STATUS_OK && vdoSyncActive)
    {
        juce::String vdoRoomServerKey;
        juce::String vdoRoomName;
        {
            const juce::ScopedLock lock(vdoRoomLock);
            if (announcedVdoRoomOwnedLocally
                && announcedVdoRoomServerKey.isNotEmpty()
                && announcedVdoRoomName.isNotEmpty()
                && (lastVdoRoomAnnouncementMs <= 0.0 || (nowMs - lastVdoRoomAnnouncementMs) >= 10000.0))
            {
                vdoRoomServerKey = announcedVdoRoomServerKey;
                vdoRoomName = announcedVdoRoomName;
                lastVdoRoomAnnouncementMs = nowMs;
            }
        }
        if (vdoRoomServerKey.isNotEmpty() && vdoRoomName.isNotEmpty())
            announceVdoRoomName(vdoRoomServerKey, vdoRoomName);
    }
    if (status == NJClient::NJC_STATUS_OK && (nowMs - lastRemoteSyncUserPruneMs) >= 350.0)
    {
        pruneDisconnectedRemoteSyncState();
        lastRemoteSyncUserPruneMs = nowMs;
    }

    if (status != lastStatus)
    {
        const bool lostConnectedSession = lastStatus == NJClient::NJC_STATUS_OK
            && (status == NJClient::NJC_STATUS_DISCONNECTED
                || status == NJClient::NJC_STATUS_CANTCONNECT
                || status == NJClient::NJC_STATUS_INVALIDAUTH);
        if (lostConnectedSession)
        {
            stopNinjamZapVideoTransportForDisconnect();
            ninjamZapServerVideoSupported.store(false, std::memory_order_relaxed);
            ninjamSideSignalServerSupported.store(false, std::memory_order_relaxed);
            lastNinjamVideoCapSendMs = 0.0;
            lastRemoteVideoRoomActivityMs.store(0.0, std::memory_order_relaxed);
            opusSyncServerSupported.store(false);
            {
                const juce::ScopedLock lock(opusSyncPeerLock);
                opusSyncPeers.clear();
            }
            invalidateIntervalSyncLatencyState(false);
            opusSyncAvailable.store(false);
            opusSyncHasLegacyClients.store(false);
            serverMaxLocalChannelsCached.store(32, std::memory_order_relaxed);
            lastIntervalSyncFallbackSubscriptionMs = 0.0;
            lastNinjamPlusControlSubscriptionMs = 0.0;
            setIntervalSyncStatusText({});
            lastBroadcastIntervalTag.store(-1);
            resetIntervalSyncTimingCache();
            applyCodecPreference();
        }

        if (status == NJClient::NJC_STATUS_CANTCONNECT || status == NJClient::NJC_STATUS_INVALIDAUTH)
        {
            autoReconnectAttemptStartedMs.store(0.0, std::memory_order_relaxed);
            juce::String err = juce::String::fromUTF8(ninjamClient.GetErrorStr());
            juce::Logger::writeToLog("NINJAM Error (" + juce::String(status) + "): " + err);

            if (status == NJClient::NJC_STATUS_INVALIDAUTH
                && duplicateNameRetryEnabled
                && looksLikeDuplicateNameError(err))
            {
                if (pendingConnectNameAttempt < 3)
                {
                    const int nextAttempt = ++pendingConnectNameAttempt;
                    const juce::String nextUser = buildNumberedUserName(pendingConnectOriginalUser, nextAttempt);
                    addSystemChatLine("Username is already in use; retrying as " + stripAnonymousPrefix(nextUser) + ".");
                    {
                        const juce::ScopedLock lifecycleLock(ninjamAudioLifecycleLock);
                        const juce::ScopedLock clientLock(ninjamClientLock);
                        applyCodecPreference();
                        ninjamClient.Connect(pendingConnectHost.toRawUTF8(),
                                             nextUser.toRawUTF8(),
                                             pendingConnectPass.toRawUTF8());
                    }
                    currentServer = pendingConnectHost;
                    currentUser = nextUser;
                    refreshAbletonLinkActivation();
                    lastStatus = NJClient::NJC_STATUS_PRECONNECT;
                    return;
                }

                duplicateNameRetryEnabled = false;
                pendingConnectNameAttempt = 0;
                cancelAutoReconnect(true);
                addSystemChatLine("Username retry failed after 3 attempts; disconnected.");
                {
                    const juce::ScopedLock lifecycleLock(ninjamAudioLifecycleLock);
                    const juce::ScopedLock clientLock(ninjamClientLock);
                    ninjamClient.Disconnect();
                }
                currentServer = {};
                currentUser = {};
                refreshAbletonLinkActivation();
                status = NJClient::NJC_STATUS_DISCONNECTED;
            }

            if (status == NJClient::NJC_STATUS_INVALIDAUTH)
                cancelAutoReconnect(true);
        }
        else if (status == NJClient::NJC_STATUS_OK)
        {
            const bool reconnected = autoReconnectAttemptCount.load(std::memory_order_relaxed) > 0;
            autoReconnectNextAttemptMs.store(0.0, std::memory_order_relaxed);
            autoReconnectAttemptStartedMs.store(0.0, std::memory_order_relaxed);
            autoReconnectConnectedSinceMs.store(nowMs, std::memory_order_relaxed);
            autoReconnectSuppressed.store(false, std::memory_order_relaxed);
            if (reconnected)
                addSystemChatLine("NINJAM connection restored.");
            duplicateNameRetryEnabled = false;
            const int serverMaxLocalChannels = juce::jmax(1, ninjamClient.GetMaxLocalChannels());
            serverMaxLocalChannelsCached.store(serverMaxLocalChannels, std::memory_order_relaxed);
            njplus_debug_log("CONNECT serverMaxLocalChannels=%d user='%s'", serverMaxLocalChannels, currentUser.toRawUTF8());
            juce::String serverChannelMessage = "Server allows " + juce::String(serverMaxLocalChannels)
                + " local channel" + (serverMaxLocalChannels == 1 ? "" : "s")
                + ". NINJAMplus indices: audio 0, hidden control 1, Opus 2, voice 3.";
            if (serverMaxLocalChannels <= kNinjamPlusControlChannelIndex)
                serverChannelMessage << " Hidden control and Opus cannot fit on this server; voice can only replace audio index 0 while voice mode is enabled.";
            else if (serverMaxLocalChannels <= kVoiceChatChannelIndex)
                serverChannelMessage << " Dedicated voice may not fit; voice falls back to audio index 0 while voice mode is enabled.";
            addSystemChatLine(serverChannelMessage);
            pendingConnectNameAttempt = 0;
            opusSyncServerSupported.store(false);
            broadcastChatStyle();
            {
                const juce::ScopedLock lock(opusSyncPeerLock);
                opusSyncPeers.clear();
            }
            {
                invalidateIntervalSyncLatencyState(false);
            }
            opusSyncAvailable.store(false);
            opusSyncHasLegacyClients.store(false);
            lastOpusSupportBroadcastMs = 0.0;
            if (serverSupportsZapVideo && ninjamZapCameraSendEnabled.load(std::memory_order_relaxed))
            {
                ninjamZapVideoEnabled.store(true, std::memory_order_relaxed);
                configureNinjamZapVideoLocalChannel();
            }
            else
            {
                if (!serverSupportsZapVideo && ninjamZapCameraSendEnabled.load(std::memory_order_relaxed))
                    stopNinjamZapCameraSend();
                ninjamZapVideoEnabled.store(false, std::memory_order_relaxed);
                ninjamZapVideoReceivedNotice.store(false, std::memory_order_relaxed);
                {
                    const juce::ScopedLock lock(ninjamZapVideoChunkLock);
                    ninjamZapVideoChunkReassemblers.clear();
                    ninjamZapVideoAudioGuidByReassemblyKey.clear();
                    ninjamZapVideoMarkerIntervalByReassemblyKey.clear();
                    ninjamZapVideoMarkerSeenByReassemblyKey.clear();
                }
            }
            lastNinjamZapVideoSubscriptionSyncMs = 0.0;
            lastIntervalSyncFallbackSubscriptionMs = 0.0;
            lastNinjamPlusControlSubscriptionMs = 0.0;
            resetIntervalSyncTimingCache();
            if (!isTransportSyncEnabled())
            {
                syncWaitForInterval.store(false);
                syncTargetInterval.store(-1);
                intervalIndex.store(0);
                lastIntervalPos.store(0);
            }
            lastBroadcastIntervalTag.store(-1);
            setIntervalSyncStatusText({});
            syncLocalIntervalChannelConfig();
        }
        else if (status == NJClient::NJC_STATUS_DISCONNECTED)
        {
            autoReconnectAttemptStartedMs.store(0.0, std::memory_order_relaxed);
        }
        lastStatus = status;
    }

    if (status == NJClient::NJC_STATUS_OK
        && autoReconnectConnectedSinceMs.load(std::memory_order_relaxed) > 0.0
        && (nowMs - autoReconnectConnectedSinceMs.load(std::memory_order_relaxed)) >= 30000.0)
    {
        autoReconnectAttemptCount.store(0, std::memory_order_relaxed);
        autoReconnectConnectedSinceMs.store(0.0, std::memory_order_relaxed);
    }
    else if (status == NJClient::NJC_STATUS_DISCONNECTED || status == NJClient::NJC_STATUS_CANTCONNECT)
    {
        scheduleAutoReconnect(nowMs, status == NJClient::NJC_STATUS_CANTCONNECT ? "connection failed" : "connection lost");
        if (attemptAutoReconnect(nowMs, status))
            return;
    }

    if (isLinkAudioEnabled() && isLinkAudioReceiveEnabled())
    {
        const juce::String selectedReceiveKey = getLinkAudioReceiveSelection();
        if (selectedReceiveKey.isNotEmpty())
        {
            bool hasReceiveSource = false;
            std::optional<ableton::ChannelId> currentSourceId;
            {
                const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
                hasReceiveSource = abletonLinkSource != nullptr;
                if (abletonLinkSource != nullptr)
                    currentSourceId = abletonLinkSource->id();
            }

            bool selectedChannelAvailable = false;
            bool sourceMatchesSelectedChannel = false;
            if (abletonLink != nullptr)
            {
                for (const auto& channel : abletonLink->channels())
                {
                    const juce::String peerName = juce::String::fromUTF8(channel.peerName.c_str()).trim();
                    const juce::String channelName = juce::String::fromUTF8(channel.name.c_str()).trim();
                    if (buildLinkAudioChannelKey(peerName, channelName) != selectedReceiveKey)
                        continue;

                    selectedChannelAvailable = true;
                    sourceMatchesSelectedChannel = currentSourceId.has_value() && *currentSourceId == channel.id;
                    break;
                }
            }

            if (selectedChannelAvailable)
                linkAudioReceiveSelectedMissingSinceMs = 0.0;
            else if (linkAudioReceiveSelectedMissingSinceMs <= 0.0)
                linkAudioReceiveSelectedMissingSinceMs = nowMs;

            const bool selectedMissingLongEnough = linkAudioReceiveSelectedMissingSinceMs > 0.0
                && (nowMs - linkAudioReceiveSelectedMissingSinceMs) >= 1500.0;
            const bool shouldCreateReceiveEndpoint = !hasReceiveSource && selectedChannelAvailable;
            const bool shouldSwitchReceiveEndpoint = hasReceiveSource && selectedChannelAvailable && !sourceMatchesSelectedChannel;
            const bool shouldClearReceiveEndpoint = hasReceiveSource && !selectedChannelAvailable && selectedMissingLongEnough;
            const bool shouldRefreshReceiveEndpoint = shouldCreateReceiveEndpoint
                || shouldSwitchReceiveEndpoint
                || shouldClearReceiveEndpoint;

            if (shouldRefreshReceiveEndpoint && (nowMs - lastLinkAudioEndpointRefreshMs) >= 250.0)
            {
                rebuildLinkAudioEndpoints();
                lastLinkAudioEndpointRefreshMs = nowMs;
            }
        }
        else
        {
            linkAudioReceiveSelectedMissingSinceMs = 0.0;
        }
    }

    if (status == NJClient::NJC_STATUS_OK)
    {
        // Rebuild peer capability state only after Run() finishes dispatching inbound callbacks.
        stepStartMs = juce::Time::getMillisecondCounterHiRes();
        refreshOpusSyncAvailabilityFromUsers();
        noteSlowIntervalStep("refreshOpus", juce::Time::getMillisecondCounterHiRes() - stepStartMs);
        if ((nowMs - lastNinjamPlusControlSubscriptionMs) >= 1000.0)
        {
            stepStartMs = juce::Time::getMillisecondCounterHiRes();
            ensureRawIntervalSyncFallbackSubscriptions();
            noteSlowIntervalStep("controlSub", juce::Time::getMillisecondCounterHiRes() - stepStartMs);
            lastNinjamPlusControlSubscriptionMs = nowMs;
        }
        if (vdoSyncActive && !serverSupportsSideSignal && (nowMs - lastIntervalSyncFallbackSubscriptionMs) >= 1000.0)
        {
            stepStartMs = juce::Time::getMillisecondCounterHiRes();
            const int changedFallbackSubs = ensureRawIntervalSyncFallbackSubscriptions();
            if (changedFallbackSubs > 0)
                intervalHelperPayloadForceWrite.store(true, std::memory_order_release);
            noteSlowIntervalStep("rawSyncSub", juce::Time::getMillisecondCounterHiRes() - stepStartMs);
            lastIntervalSyncFallbackSubscriptionMs = nowMs;
        }
        else if (serverSupportsSideSignal)
        {
            if (vdoSyncActive && (nowMs - lastIntervalSyncFallbackSubscriptionMs) >= 1000.0)
            {
                stepStartMs = juce::Time::getMillisecondCounterHiRes();
                if (!vdoCarrierChannelConfigured.exchange(true, std::memory_order_acq_rel))
                    syncLocalIntervalChannelConfig();
                const int changedZapCarrierSubs = syncNinjamZapVideoSubscriptions(true);
                if (changedZapCarrierSubs > 0)
                    intervalHelperPayloadForceWrite.store(true, std::memory_order_release);
                noteSlowIntervalStep("zapVdoCarrierSub", juce::Time::getMillisecondCounterHiRes() - stepStartMs);
                lastIntervalSyncFallbackSubscriptionMs = nowMs;
            }
            else if (!vdoSyncActive)
            {
                lastIntervalSyncFallbackSubscriptionMs = nowMs;
            }
        }
        if (serverSupportsZapVideo && ninjamZapVideoEnabled.load(std::memory_order_relaxed))
        {
            if ((nowMs - lastNinjamZapVideoSubscriptionSyncMs) >= 750.0)
            {
                stepStartMs = juce::Time::getMillisecondCounterHiRes();
                syncNinjamZapVideoSubscriptions(true);
                noteSlowIntervalStep("zapSubSync", juce::Time::getMillisecondCounterHiRes() - stepStartMs);
                lastNinjamZapVideoSubscriptionSyncMs = nowMs;
            }
            stepStartMs = juce::Time::getMillisecondCounterHiRes();
            processPendingNinjamZapVideoPlaybackSwap();
            noteSlowIntervalStep("zapPlaybackSwap", juce::Time::getMillisecondCounterHiRes() - stepStartMs);
            stepStartMs = juce::Time::getMillisecondCounterHiRes();
            processPendingNinjamZapVideoIntervalRotate();
            noteSlowIntervalStep("zapRotate", juce::Time::getMillisecondCounterHiRes() - stepStartMs);
            stepStartMs = juce::Time::getMillisecondCounterHiRes();
            flushPendingNinjamZapCameraVideo();
            noteSlowIntervalStep("zapFlush", juce::Time::getMillisecondCounterHiRes() - stepStartMs);
        }
        else
        {
            pendingNinjamZapVideoPlaybackSwap.store(false, std::memory_order_release);
            pendingNinjamZapIntervalRotate.store(false, std::memory_order_release);
        }
        {
            const juce::ScopedLock lock(intervalSyncAnnouncementLock);
            const int displayInterval = getDisplayIntervalIndex();
            if (localIntervalStartMsByInterval.find(displayInterval) == localIntervalStartMsByInterval.end())
                localIntervalStartMsByInterval[displayInterval] = nowMs;
        }
        if (nowMs - lastOpusSupportBroadcastMs >= 1500.0)
        {
            stepStartMs = juce::Time::getMillisecondCounterHiRes();
            broadcastOpusSyncSupport();
            noteSlowIntervalStep("opusBroadcast", juce::Time::getMillisecondCounterHiRes() - stepStartMs);
            lastOpusSupportBroadcastMs = nowMs;
        }
        if (vdoSyncActive && nowMs - lastServerLatencyProbeAttemptMs >= 5000.0)
        {
            stepStartMs = juce::Time::getMillisecondCounterHiRes();
            broadcastTransportProbe();
            noteSlowIntervalStep("transportProbe", juce::Time::getMillisecondCounterHiRes() - stepStartMs);
            lastServerLatencyProbeAttemptMs = nowMs;
        }

        stepStartMs = juce::Time::getMillisecondCounterHiRes();
        flushOutboundMidiRelayEvents();
        noteSlowIntervalStep("midiRelayFlush", juce::Time::getMillisecondCounterHiRes() - stepStartMs);
        stepStartMs = juce::Time::getMillisecondCounterHiRes();
        flushOutboundOscRelayEvents();
        noteSlowIntervalStep("oscRelayFlush", juce::Time::getMillisecondCounterHiRes() - stepStartMs);
    }

    int pos = 0;
    int length = 0;
    stepStartMs = juce::Time::getMillisecondCounterHiRes();
    {
        const juce::ScopedLock clientLock(ninjamClientLock);
        ninjamClient.GetPosition(&pos, &length);
    }
    noteSlowIntervalStep("getPosition", juce::Time::getMillisecondCounterHiRes() - stepStartMs);
    if (length > 0)
    {
        bool forceIntervalHelperPayloadWrite = false;
        const int localBpi = juce::jmax(1, getBPI());
        const double localBpm = juce::jmax(1.0, (double)getBPM());
        const bool hadPreviousTiming = lastLatencyTimingBpi > 0 && lastLatencyTimingBpm > 0.0;
        const bool bpmChanged = hadPreviousTiming && std::abs(lastLatencyTimingBpm - localBpm) > 0.05;
        const bool timingChanged = lastLatencyTimingBpi != localBpi
            || lastLatencyTimingLength != length
            || bpmChanged;

        if (std::abs(lastSamplePadBpmSyncBpm - localBpm) > 0.05)
        {
            lastSamplePadBpmSyncBpm = localBpm;
            requestLoopedSamplePadsResync(localBpm);
        }

        if (timingChanged)
        {
            stepStartMs = juce::Time::getMillisecondCounterHiRes();
            int timingDelayDeltaMs = 0;
            if (hadPreviousTiming)
            {
                const double previousIntervalDurationMs = (60.0 / lastLatencyTimingBpm) * (double)lastLatencyTimingBpi * 1000.0;
                const double newIntervalDurationMs = (60.0 / localBpm) * (double)localBpi * 1000.0;
                if (std::isfinite(previousIntervalDurationMs) && std::isfinite(newIntervalDurationMs)
                    && previousIntervalDurationMs > 0.0 && newIntervalDurationMs > 0.0)
                    timingDelayDeltaMs = (int)std::llround(newIntervalDurationMs - previousIntervalDurationMs);
            }
            if (hadPreviousTiming && status == NJClient::NJC_STATUS_OK)
            {
                const juce::ScopedLock lock(intervalSyncAnnouncementLock);
                if (!remoteLatencyFirmDelayMsByUser.empty())
                {
                    const bool shouldRefreshVideoBuffers = vdoSyncActive && videoHelperRunning.load();
                    const auto refreshId = shouldRefreshVideoBuffers ? ++videoBufferRefreshCounter : 0;
                    for (auto& userDelay : remoteLatencyFirmDelayMsByUser)
                    {
                        if (timingDelayDeltaMs != 0)
                            userDelay.second = juce::jmax(0, userDelay.second + timingDelayDeltaMs);
                        if (shouldRefreshVideoBuffers)
                            remoteVideoBufferRefreshIdByUser[userDelay.first] = refreshId;
                    }
                }
            }
            if (vdoSyncActive && hadPreviousTiming && status == NJClient::NJC_STATUS_OK)
                broadcastVideoTimingChange(lastLatencyTimingBpm, localBpm, localBpi, length, timingDelayDeltaMs);
            invalidateIntervalSyncLatencyState(true);
            lastLatencyTimingBpi = localBpi;
            lastLatencyTimingLength = length;
            lastLatencyTimingBpm = localBpm;
            lastBroadcastIntervalTag.store(-1);
            setIntervalSyncStatusText("Interval sync timing changed, recalculating delay...");
            forceIntervalHelperPayloadWrite = true;
            noteSlowIntervalStep("timingChanged", juce::Time::getMillisecondCounterHiRes() - stepStartMs);
        }

        int last = lastIntervalPos.load();
        if (pos < last)
        {
            stepStartMs = juce::Time::getMillisecondCounterHiRes();
            perfIntervalWrapped = true;
            intervalIndex.fetch_add(1);
            forceIntervalHelperPayloadWrite = true;
            const int localDisplayInterval = getDisplayIntervalIndex();
            perfDisplayInterval = localDisplayInterval;
            const double localIntervalStartMs = juce::Time::getMillisecondCounterHiRes();
            {
                const juce::ScopedLock lock(intervalSyncAnnouncementLock);
                localIntervalStartMsByInterval[localDisplayInterval] = localIntervalStartMs;
                const int minIntervalToKeep = localDisplayInterval - 64;
                for (auto it = localIntervalStartMsByInterval.begin(); it != localIntervalStartMsByInterval.end();)
                {
                    if (it->first < minIntervalToKeep)
                        it = localIntervalStartMsByInterval.erase(it);
                    else
                        ++it;
                }
            }
            noteSlowIntervalStep("intervalWrap", juce::Time::getMillisecondCounterHiRes() - stepStartMs);
        }
        const int localDisplayInterval = getDisplayIntervalIndex();
        perfDisplayInterval = localDisplayInterval;
        const int currentBeatIndex = getIntervalBeatIndexForPosition(pos, length, localBpi);
        const int localMarkerBeat = getIntervalSyncMarkerBeatForBeat(currentBeatIndex, localBpi);
        const long long localMarkerKey = makeIntervalSyncMarkerKey(localDisplayInterval, localMarkerBeat);
        const bool markerChanged = lastProcessedIntervalMarkerKey.load() != localMarkerKey;
        if (markerChanged)
        {
            stepStartMs = juce::Time::getMillisecondCounterHiRes();
            perfMarkerChanged = true;
            lastProcessedIntervalMarkerKey.store(localMarkerKey);
            if (vdoSyncActive && status == NJClient::NJC_STATUS_OK && currentBeatIndex == localMarkerBeat)
            {
                forceIntervalHelperPayloadWrite = true;
                const long long localMarkerSampleCount = intervalSyncSampleCounter.load(std::memory_order_relaxed);
                const double intervalDurationMs = (60.0 / localBpm) * (double)localBpi * 1000.0;
                processPendingIntervalSyncMarkers(localMarkerBeat, localMarkerSampleCount, intervalDurationMs);
                if (lastBroadcastIntervalTag.load() != localMarkerKey)
                {
                    broadcastIntervalSyncTag("*", localMarkerBeat);
                    lastBroadcastIntervalTag.store(localMarkerKey);
                }
            }
            noteSlowIntervalStep("markerWork", juce::Time::getMillisecondCounterHiRes() - stepStartMs);
        }
        lastIntervalPos.store(pos);
        if (vdoSyncActive && status == NJClient::NJC_STATUS_OK && videoHelperRunning.load())
        {
            forceIntervalHelperPayloadWrite = intervalHelperPayloadForceWrite.exchange(false, std::memory_order_acq_rel)
                || forceIntervalHelperPayloadWrite;
            const double elapsedSinceHelperWriteMs = nowMs - lastIntervalHelperPayloadWriteMs;
            if (forceIntervalHelperPayloadWrite
                || lastIntervalHelperPayloadWriteMs <= 0.0
                || elapsedSinceHelperWriteMs >= intervalHelperPayloadMinWriteMs)
            {
                stepStartMs = juce::Time::getMillisecondCounterHiRes();
                lastIntervalHelperPayloadWriteMs = nowMs;
                writeIntervalHelperJson(pos, length);
                perfHelperWrote = true;
                noteSlowIntervalStep("helperJson", juce::Time::getMillisecondCounterHiRes() - stepStartMs);
            }
        }
    }

    const double timerTotalMs = juce::Time::getMillisecondCounterHiRes() - timerStartMs;
    if (timerTotalMs >= intervalPerfTotalThresholdMs || intervalPerfDetails.isNotEmpty())
    {
        int pendingZapChunks = -1;
        if (ninjamZapCameraSendEnabled.load(std::memory_order_relaxed))
        {
            const juce::SpinLock::ScopedLockType lock(ninjamZapCameraChunkQueueLock);
            pendingZapChunks = (int)pendingNinjamZapCameraChunks.size();
        }

        juce::String line;
        line << "timer total=" << juce::String(timerTotalMs, 2) << "ms"
             << " status=" << status
             << " loops=" << loopCount
             << " pos=" << pos << "/" << length
             << " interval=" << perfDisplayInterval
             << " intervalStart=" << (perfIntervalWrapped ? 1 : 0)
             << " markerChanged=" << (perfMarkerChanged ? 1 : 0)
             << " helperWrite=" << (perfHelperWrote ? 1 : 0)
             << " vdoSync=" << (vdoSyncActive ? 1 : 0)
             << " zapServer=" << (serverSupportsZapVideo ? 1 : 0)
             << " zapEnabled=" << (ninjamZapVideoEnabled.load(std::memory_order_relaxed) ? 1 : 0)
             << " zapCam=" << (ninjamZapCameraSendEnabled.load(std::memory_order_relaxed) ? 1 : 0)
             << " zapPending=" << pendingZapChunks;
        if (intervalPerfDetails.isNotEmpty())
            line << " slow=[" << intervalPerfDetails << "]";
        logIntervalPerf(line);
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NinjamVst3AudioProcessor();
}
