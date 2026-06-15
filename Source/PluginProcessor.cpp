#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ZapVideoCodec.h"
#include "Chromagram.h"
#include "ChordDetector.h"
#include "signalsmith-stretch/signalsmith-stretch.h"
#include <juce_video/juce_video.h>

#ifndef NINJAMPLUS_HAS_AUBIO
 #define NINJAMPLUS_HAS_AUBIO 0
#endif

#if NINJAMPLUS_HAS_AUBIO
 #if __has_include(<aubio/aubio.h>)
  #include <aubio/aubio.h>
 #else
  #include <aubio.h>
 #endif
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
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <vector>

static void writeDiagnosticLogLine(const char* fileName, const juce::String& msg)
{
    static juce::CriticalSection logLock;
    const juce::ScopedLock lock(logLock);
    juce::File f = juce::File::getSpecialLocation(juce::File::tempDirectory)
                       .getChildFile(fileName);
    const juce::String line = juce::Time::getCurrentTime().toString(true, true, true, true)
        + "  " + msg + "\n";
    f.appendText(line, false, false);
}

static void logIntervalPerf(const juce::String& msg)
{
    writeDiagnosticLogLine("ninjam_interval_perf.txt", msg);
}

static constexpr double intervalPerfStepThresholdMs = 3.0;
static constexpr double intervalPerfTotalThresholdMs = 8.0;

static constexpr int advancedVideoHelperBasePort = 8000;
static constexpr int advancedVideoHelperMaxPort = 8199;

class LocalVideoHttpServer final : private juce::Thread
{
public:
    LocalVideoHttpServer(const juce::File& helperRootDir,
                         std::function<juce::String()> intervalPayloadProviderIn,
                         std::function<juce::String()> zapFrameListProviderIn,
                         std::function<bool(const juce::String&, int, juce::MemoryBlock&)> zapFrameProviderIn,
                         std::function<juce::String(const juce::String&)> zapBrowserCameraEnableIn,
                         std::function<juce::String()> zapBrowserCameraStateIn,
                         std::function<juce::String()> zapBrowserCameraDisableIn,
                         std::function<bool(const juce::MemoryBlock&, const juce::String&, const juce::String&, bool, double, double, int, int)> zapBrowserFrameConsumerIn)
        : juce::Thread("NINJAMVideoHelperServer"),
          helperRoot(helperRootDir),
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
        if (!helperRoot.isDirectory() || helperIndexHtml.isEmpty() || helperAppHtml.isEmpty())
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

    juce::File helperRoot;
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
    juce::MemoryBlock helperCloudMaskPng;
    juce::ThreadPool clientThreadPool { 4 };

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

    static juce::String getZapViewerHtml()
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
</style>
</head>
<body>
<header><h1>NINJAMZap Video</h1><div id="status">Waiting for video frames</div></header>
<section class="camera-panel">
  <label>Camera <select id="camSelect"><option value="">Default camera</option></select></label>
  <label>Codec <select id="camCodec"><option value="h264">H.264</option><option value="mjpeg">MJPEG</option><option value="vp8">VP8</option><option value="vp9">VP9</option></select></label>
  <label>Size <select id="camSize"><option value="320x180">320x180</option><option value="640x360" selected>640x360</option><option value="800x450">800x450</option><option value="1280x720">1280x720</option></select></label>
  <label>FPS <select id="camFps"><option value="10">10</option><option value="15">15</option><option value="24">24</option><option value="30" selected>30</option></select></label>
  <label>Quality <select id="camQuality"><option value="balanced">Balanced</option><option value="high" selected>High</option><option value="low">Low</option></select></label>
  <label>View <select id="camLayout"><option value="tiles" selected>Tiles</option><option value="rows">Rows</option><option value="columns">Columns</option></select></label>
  <label>Mask <select id="camMask"><option value="none" selected>None</option><option value="cloud">Cloud</option><option value="circle">Circle</option><option value="rounded">Rounded</option><option value="square">Square</option><option value="tree">Xmas tree</option><option value="pumpkin">Pumpkin</option><option value="star">Star</option><option value="heart">Heart</option></select></label>
  <label>FX <select id="camFx"><option value="off" selected>Off</option><option value="blur">Blur</option><option value="bg-blur">Blur background</option><option value="green-bg">Green background</option><option value="black-bg">Black background</option><option value="custom-bg">Custom background</option><option value="grayscale">Grayscale</option><option value="sepia">Sepia</option></select></label>
  <label>Strength <input id="camFxStrength" type="range" min="0" max="24" value="10"></label>
  <label>BG <input id="camBgColor" type="color" value="#00ff00"></label>
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
<script>
const grid=document.getElementById('grid');
const statusEl=document.getElementById('status');
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
let refreshInFlight=false;
let browserFxSegmenter=null;
let browserFxSegmenterLoading=null;
let browserFxSegmenterFailed=false;
let browserFxSegmentationInFlight=false;
let browserFxLastSegmentationMs=0;
let browserFxMaskReady=false;
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
function setCamStatus(text){ camStatus.textContent=text; }
function applyGridLayout(){
  const value=String(camLayout.value||'tiles').toLowerCase();
  const layout=value==='rows'||value==='columns'?value:'tiles';
  grid.classList.remove('layout-tiles','layout-rows','layout-columns');
  grid.classList.add('layout-'+layout);
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
  return 'url("data:image/svg+xml;utf8,'+encodeURIComponent('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" preserveAspectRatio="none">'+inner+'</svg>')+'")';
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
  url.searchParams.set('layout',camLayout.value||'tiles');
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
  const width=Math.max(160,Math.min(1280,parseInt(parts[0]||'640',10)||640));
  const height=Math.max(90,Math.min(720,parseInt(parts[1]||'360',10)||360));
  return {width,height};
}
function selectedTransportFps(){
  return Math.max(1,Math.min(30,parseInt(camFps.value||'30',10)||30));
}
function selectedTransportQuality(){
  const value=String(camQuality.value||'high').toLowerCase();
  return value==='low'||value==='balanced'||value==='high'?value:'high';
}
function selectedCameraFx(){
  const value=String(camFx.value||'off').toLowerCase();
  return ['off','blur','bg-blur','green-bg','black-bg','custom-bg','grayscale','sepia'].includes(value)?value:'off';
}
function selectedFxStrength(){
  const value=parseInt(camFxStrength.value||'10',10);
  return Number.isFinite(value)?Math.max(0,Math.min(24,value)):10;
}
function selectedFxBgColor(){
  const value=String(camBgColor.value||'#00ff00');
  return /^#[0-9a-f]{6}$/i.test(value)?value:'#00ff00';
}
function isSegmentationFx(value){
  const fx=value||selectedCameraFx();
  return fx==='bg-blur'||fx==='green-bg'||fx==='black-bg'||fx==='custom-bg';
}
function updateFxControls(){
  const fx=selectedCameraFx();
  camFxStrength.disabled=!(fx==='blur'||fx==='bg-blur');
  camBgColor.disabled=fx!=='custom-bg';
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
  updateBrowserFxSegmentation();
  if(isSegmentationFx(fx)&&browserFxMaskReady){
    if(fx==='bg-blur'){
      drawBrowserVideoTo(ctx,width,height,'blur('+String(selectedFxStrength())+'px)',mirror,1.08);
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
  const res=await fetch('/zap-browser-camera-enable?codec='+encodeURIComponent(codec||'mjpeg'),{method:'POST',cache:'no-store'});
  const payload=await res.json().catch(()=>({ok:false,error:'helper did not return JSON'}));
  if(!res.ok||!payload.ok) throw new Error(payload.error||'Zap camera send could not start');
  browserSendArmed=true;
}
async function disarmBrowserZapSend(){
  browserSendArmed=false;
  try{ await fetch('/zap-browser-camera-stop',{method:'POST',cache:'no-store'}); }catch(e){}
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
  if(browserCameraStateTimer){ clearInterval(browserCameraStateTimer); browserCameraStateTimer=0; }
  if(browserEncoder){ try{browserEncoder.close();}catch(e){} browserEncoder=null; }
  browserEncodeStarts.clear();
  browserLastCodecConfig='';
  browserLastKeyframeMs=0;
  browserForceNextKeyframe=false;
  browserLastKeyframeRequestId='';
  resetBrowserFxMask();
)HTML";
        html << R"HTML(
  if(browserStream){ browserStream.getTracks().forEach(track=>track.stop()); browserStream=null; }
  browserPreview.srcObject=null;
  browserFxPreviewCtx.fillStyle='#050808';
  browserFxPreviewCtx.fillRect(0,0,browserFxPreview.width,browserFxPreview.height);
  browserPosting=false;
  browserEncoderH264Format='avc';
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
  const res=await fetch(url,{method:'POST',headers:{'Content-Type':'image/jpeg'},body:blob,cache:'no-store'});
  if(!res.ok&&res.status!==204) throw new Error('helper rejected frame '+String(res.status));
  setCamStatus('Browser camera sending MJPEG '+String(width)+'x'+String(height)+' @ '+camFps.value+'fps');
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
  const res=await fetch(url,{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:bytes,cache:'no-store'});
  if(!res.ok&&res.status!==204) throw new Error('helper rejected frame '+String(res.status));
    const payloadLabel=bytes&&bytes.length?'frame':'config';
    setCamStatus('Browser camera sending '+codec.toUpperCase()+' '+payloadLabel+' '+String(width)+'x'+String(height)+' @ '+camFps.value+'fps');
}
async function handleEncodedBrowserChunk(chunk,metadata){
  const fallbackSize=selectedTransportSize();
  const info=browserEncodeStarts.get(chunk.timestamp)||{captureStartedMs:performance.now(),width:fallbackSize.width,height:fallbackSize.height};
  browserEncodeStarts.delete(chunk.timestamp);
  const bytes=new Uint8Array(chunk.byteLength);
  chunk.copyTo(bytes);
  const encodeMs=Math.max(0,performance.now()-info.captureStartedMs);
  let configBase64='';
  let frameBytes=bytes;
  let keyFrame=chunk.type==='key';
  if(browserEncoderCodec==='h264'&&browserEncoderH264Format==='annexb'){
    const nals=splitAnnexBNals(bytes);
    const annexConfig=h264ConfigFromNalsBase64(nals);
    if(annexConfig) configBase64=annexConfig;
    keyFrame=keyFrame||nals.some(nal=>nal.length&&((nal[0]&31)===5));
    frameBytes=h264NalsToAvcc(nals);
        if(!frameBytes.length&&configBase64)
            return await postBrowserEncodedBytes(new Uint8Array(),browserEncoderCodec,info.captureStartedMs,encodeMs,info.width,info.height,keyFrame,configBase64);
        if(!frameBytes.length) return;
  }
  if(browserEncoderCodec==='h264'&&metadata&&metadata.decoderConfig&&metadata.decoderConfig.description){
    const avcConfig=avccToZapConfigBase64(new Uint8Array(metadata.decoderConfig.description));
    if(avcConfig) configBase64=avcConfig;
  }
    if(browserEncoderCodec==='h264'&&!configBase64){
        const avccConfig=h264ConfigFromAvccFrameBase64(frameBytes);
        if(avccConfig) configBase64=avccConfig;
    }
    const configChanged=browserEncoderCodec==='h264'&&configBase64&&configBase64!==browserLastCodecConfig;
    if(configBase64) browserLastCodecConfig=configBase64;
  else if(browserEncoderCodec==='h264'&&keyFrame&&browserLastCodecConfig) configBase64=browserLastCodecConfig;
    if(configChanged)
        await postBrowserEncodedBytes(new Uint8Array(),browserEncoderCodec,info.captureStartedMs,encodeMs,info.width,info.height,false,configBase64);
  await postBrowserEncodedBytes(frameBytes,browserEncoderCodec,info.captureStartedMs,encodeMs,info.width,info.height,keyFrame,configBase64);
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
async function startBrowserVideoEncoder(codec,width,height,fps){
  if(codec==='mjpeg') return;
  if(!('VideoEncoder' in window)||!('VideoFrame' in window)) throw new Error('Browser WebCodecs encoder unavailable');
  browserEncoderCodec=codec;
  const config=await getSupportedEncoderConfig(codec,width,height,fps);
  if(!config) throw new Error(codec.toUpperCase()+' browser encoder unsupported');
  browserEncoder=new VideoEncoder({
    output(chunk,metadata){ handleEncodedBrowserChunk(chunk,metadata).catch(e=>setCamStatus(String(e.message||e))); },
    error(err){ setCamStatus('Browser encoder error: '+String(err&&err.message?err.message:err)); }
  });
  browserEncoder.configure(config);
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
    },'image/jpeg',0.72);
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
  const fps=selectedTransportFps();
  const codec=String(camCodec.value||'h264').toLowerCase();
  const {width,height}=selectedTransportSize();
  const video={width:{ideal:width,max:width},height:{ideal:height,max:height},frameRate:{ideal:fps,max:fps}};
  if(camSelect.value) video.deviceId={exact:camSelect.value};
  try{
    browserStream=await navigator.mediaDevices.getUserMedia({audio:false,video});
    browserPreview.srcObject=browserStream;
    await browserPreview.play();
    resetBrowserFxMask();
    browserFxSegmenterFailed=false;
    let activeCodec=codec;
    browserEncoderCodec='mjpeg';
    if(codec!=='mjpeg'){
      try{
        await startBrowserVideoEncoder(codec,width,height,fps);
      }catch(encoderError){
        activeCodec='mjpeg';
        browserEncoderCodec='mjpeg';
        if(browserEncoder){ try{browserEncoder.close();}catch(e){} browserEncoder=null; }
        setCamStatus(codec.toUpperCase()+' encoder unavailable, using MJPEG');
      }
    }
    await armBrowserZapSend(activeCodec);
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
camLayout.addEventListener('change',applyGridLayout);
camMask.addEventListener('change',refreshDisplayMasks);
camFx.addEventListener('change',()=>{
  resetBrowserFxMask();
  browserFxSegmenterFailed=false;
  updateFxControls();
});
camObs.addEventListener('click',()=>window.open('/zap-wall?layout='+encodeURIComponent(camLayout.value||'tiles'),'_blank'));
applyGridLayout();
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
    const allStreams=await res.json();
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
        return html;
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
        helperIndexHtml = helperRoot.getChildFile("index.html").loadFileAsString();
        helperAppHtml = helperRoot.getChildFile("app.html").loadFileAsString();

        helperIconPng.reset();
        helperRoot.getChildFile("icon.png").loadFileAsData(helperIconPng);

        helperCloudMaskPng.reset();
        helperRoot.getChildFile("masks").getChildFile("cloud.png").loadFileAsData(helperCloudMaskPng);
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

class LocalChordAnalyzer final
    : private juce::Thread
{
public:
    LocalChordAnalyzer()
        : juce::Thread("NINJAMLocalChordAnalyzer")
    {
        memoryKb.store(estimateMemoryKb());
    }

    ~LocalChordAnalyzer() override
    {
        stop();
    }

    void prepare(double newSampleRate)
    {
        ready.store(false, std::memory_order_release);
        stopThread(1000);

        sampleRate = newSampleRate > 1.0 ? newSampleRate : 44100.0;
        frame.assign((size_t)frameSize, 0.0);
        averagedChroma.assign(12, 0.0);
        readBuffer.assign((size_t)frameSize * 4, 0.0f);
        const int newRingSize = juce::jmax(frameSize * 16, (int)std::round(sampleRate));
        ringBuffer.assign((size_t)newRingSize, 0.0f);
        audioFifo = std::make_unique<juce::AbstractFifo>(newRingSize);
        frameFill = 0;
        rmsSmoothed = 0.0;
        samplesSinceCpuUpdate = 0;
        analysisMsSinceCpuUpdate = 0.0;
        chromaHistory = {};
        chromaHistoryWrite = 0;
        chromaHistorySize = 0;
        resetChordDecisionState();
        droppedSamples.store(0, std::memory_order_relaxed);
        cpuPercent.store(0.0, std::memory_order_relaxed);

        const int roundedRate = juce::jlimit(8000, 192000, (int)std::round(sampleRate));
        requestedSampleRate.store(roundedRate, std::memory_order_relaxed);
        configureChromagram(roundedRate);
        memoryKb.store(estimateMemoryKb(newRingSize), std::memory_order_relaxed);

        ready.store(true, std::memory_order_release);
        startThread(juce::Thread::Priority::background);
    }

    void processBlock(const float* input, int numSamples, int inputSampleRate = 0)
    {
        processFrames(input, numSamples, 1, inputSampleRate);
    }

    void processInterleavedBlock(const float* input, int numFrames, int numChannels, int inputSampleRate)
    {
        processFrames(input, numFrames, juce::jmax(1, numChannels), inputSampleRate);
    }

    void processFrames(const float* input, int numFrames, int numChannels, int inputSampleRate)
    {
        if (!ready.load(std::memory_order_acquire))
            return;

        if (input == nullptr || numFrames <= 0 || audioFifo == nullptr || ringBuffer.empty())
        {
            markNoInput();
            return;
        }

        if (inputSampleRate > 1000)
            requestedSampleRate.store(juce::jlimit(8000, 192000, inputSampleRate), std::memory_order_relaxed);

        int writableSamples = juce::jmin(numFrames, audioFifo->getFreeSpace());
        if (writableSamples <= 0)
        {
            droppedSamples.fetch_add(numFrames, std::memory_order_relaxed);
            return;
        }

        int inputStartFrame = 0;
        if (writableSamples < numFrames)
        {
            inputStartFrame = numFrames - writableSamples;
            droppedSamples.fetch_add(numFrames - writableSamples, std::memory_order_relaxed);
        }

        int start1 = 0;
        int size1 = 0;
        int start2 = 0;
        int size2 = 0;
        audioFifo->prepareToWrite(writableSamples, start1, size1, start2, size2);

        copyMonoFramesToRing(input, inputStartFrame, numChannels, start1, size1);
        copyMonoFramesToRing(input, inputStartFrame + size1, numChannels, start2, size2);

        audioFifo->finishedWrite(size1 + size2);
        samplesAvailable.signal();
    }

    void markNoInput()
    {
        chordValid.store(false, std::memory_order_relaxed);
        noteValid.store(false, std::memory_order_relaxed);
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
            case ChordDetector::Dominant:    suffix = "7"; break;
            case ChordDetector::Dimished5th: suffix = "dim"; break;
            case ChordDetector::Augmented5th:suffix = "aug"; break;
            default:                         suffix = ""; break;
        }

        return juce::String(getPitchClassName(root)) + suffix;
    }

    juce::String getLabel() const
    {
        if (chordValid.load(std::memory_order_relaxed))
        {
            const int root = chordRoot.load(std::memory_order_relaxed);
            const int quality = chordQuality.load(std::memory_order_relaxed);
            const int intervals = chordIntervals.load(std::memory_order_relaxed);
            if (root >= 0 && root < 12)
                return formatChordLabel(root, quality, intervals);
        }

        if (noteValid.load(std::memory_order_relaxed))
        {
            const int root = noteRoot.load(std::memory_order_relaxed);
            if (root >= 0 && root < 12)
                return getPitchClassName(root);
        }

        return "--";
    }

    double getCpuPercent() const
    {
        return cpuPercent.load(std::memory_order_relaxed);
    }

    int getMemoryKb() const
    {
        return memoryKb.load(std::memory_order_relaxed);
    }

private:
    static int estimateMemoryKb(int fifoSamples = 48000)
    {
        constexpr int bufferSize = 8192;
        constexpr int downsampledFrameSize = 512 / 4;
        const size_t doubleVectors = (size_t)(bufferSize + bufferSize + (bufferSize / 2 + 1)
                                      + 12 + downsampledFrameSize + 512) * sizeof(double);
        const size_t kissFftBuffers = (size_t)bufferSize * 2 * sizeof(float) * 2;
        const size_t fifoBytes = (size_t)juce::jmax(0, fifoSamples) * sizeof(float);
        const size_t bytes = doubleVectors + kissFftBuffers + fifoBytes + sizeof(ChordDetector) + (64 * 1024);
        return (int)((bytes + 1023) / 1024);
    }

    void copyMonoFramesToRing(const float* input, int inputStartFrame, int numChannels, int ringStart, int count)
    {
        if (count <= 0)
            return;

        if (numChannels <= 1)
        {
            std::memcpy(ringBuffer.data() + ringStart,
                        input + inputStartFrame,
                        (size_t)count * sizeof(float));
            return;
        }

        for (int i = 0; i < count; ++i)
        {
            const int source = (inputStartFrame + i) * numChannels;
            ringBuffer[(size_t)(ringStart + i)] = 0.5f * (input[source] + input[source + 1]);
        }
    }

    void configureChromagram(int newSampleRate)
    {
        analyzerSampleRate = juce::jlimit(8000, 192000, newSampleRate);
        sampleRate = (double)analyzerSampleRate;
        chromagram = std::make_unique<Chromagram>(frameSize, analyzerSampleRate);
        chromagram->setChromaCalculationInterval(chromaCalculationIntervalSamples);
        frameFill = 0;
        rmsSmoothed = 0.0;
        chromaHistory = {};
        chromaHistoryWrite = 0;
        chromaHistorySize = 0;
        resetChordDecisionState();
    }

    void run() override
    {
        while (!threadShouldExit())
        {
            if (!readAvailableSamples())
                samplesAvailable.wait(20);
        }
    }

    bool readAvailableSamples()
    {
        if (!ready.load(std::memory_order_acquire))
            return false;

        if (audioFifo == nullptr || ringBuffer.empty() || readBuffer.empty())
            return false;

        const int requestedRate = requestedSampleRate.load(std::memory_order_relaxed);
        if (chromagram == nullptr || requestedRate != analyzerSampleRate)
            configureChromagram(requestedRate);

        const int available = audioFifo->getNumReady();
        if (available <= 0)
            return false;

        const int toRead = juce::jmin(available, (int)readBuffer.size());
        int start1 = 0;
        int size1 = 0;
        int start2 = 0;
        int size2 = 0;
        audioFifo->prepareToRead(toRead, start1, size1, start2, size2);

        if (size1 > 0)
            std::memcpy(readBuffer.data(), ringBuffer.data() + start1, (size_t)size1 * sizeof(float));
        if (size2 > 0)
            std::memcpy(readBuffer.data() + size1, ringBuffer.data() + start2, (size_t)size2 * sizeof(float));

        audioFifo->finishedRead(size1 + size2);

        const double startMs = juce::Time::getMillisecondCounterHiRes();
        const int samplesRead = size1 + size2;
        for (int i = 0; i < samplesRead; ++i)
        {
            frame[(size_t)frameFill++] = (double)readBuffer[(size_t)i];
            if (frameFill >= frameSize)
            {
                processFrame();
                frameFill = 0;
            }
        }

        const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - startMs;
        analysisMsSinceCpuUpdate += juce::jmax(0.0, elapsedMs);
        samplesSinceCpuUpdate += samplesRead;

        const int updateSamples = juce::jmax(1, (int)std::round(sampleRate));
        if (samplesSinceCpuUpdate >= updateSamples)
        {
            const double realTimeMs = ((double)samplesSinceCpuUpdate / sampleRate) * 1000.0;
            const double rawCpu = realTimeMs > 0.0 ? (analysisMsSinceCpuUpdate / realTimeMs) * 100.0 : 0.0;
            const double previous = cpuPercent.load(std::memory_order_relaxed);
            const double smoothed = previous <= 0.0 ? rawCpu : previous * 0.75 + rawCpu * 0.25;
            cpuPercent.store(juce::jlimit(0.0, 200.0, smoothed), std::memory_order_relaxed);
            analysisMsSinceCpuUpdate = 0.0;
            samplesSinceCpuUpdate = 0;
        }

        return true;
    }

    void processFrame()
    {
        double energy = 0.0;
        for (double sample : frame)
            energy += sample * sample;

        const double frameRms = std::sqrt(energy / (double)frame.size());
        rmsSmoothed = frameRms > rmsSmoothed ? frameRms : rmsSmoothed * 0.92 + frameRms * 0.08;

        chromagram->processAudioFrame(frame.data());
        if (!chromagram->isReady())
            return;

        if (rmsSmoothed < silenceRmsThreshold)
        {
            noteInvalidChordFrame();
            return;
        }

        auto chroma = chromagram->getChromagram();
        double chromaTotal = 0.0;
        for (double value : chroma)
            chromaTotal += std::abs(value);

        if (chromaTotal <= 1.0e-9)
        {
            noteInvalidChordFrame();
            return;
        }

        chromaHistory[(size_t)chromaHistoryWrite].fill(0.0);
        for (int i = 0; i < 12 && i < (int)chroma.size(); ++i)
            chromaHistory[(size_t)chromaHistoryWrite][(size_t)i] = chroma[(size_t)i];
        chromaHistoryWrite = (chromaHistoryWrite + 1) % chromaHistoryFrames;
        chromaHistorySize = juce::jmin(chromaHistorySize + 1, chromaHistoryFrames);

        averagedChroma.assign(12, 0.0);
        for (int h = 0; h < chromaHistorySize; ++h)
            for (int i = 0; i < 12; ++i)
                averagedChroma[(size_t)i] += chromaHistory[(size_t)h][(size_t)i];

        const double scale = 1.0 / (double)juce::jmax(1, chromaHistorySize);
        for (double& value : averagedChroma)
            value *= scale;

        int dominantPitchClass = -1;
        if (hasEnoughHarmonicContent(averagedChroma))
        {
            chordDetector.detectChord(averagedChroma);
            if (chordDetector.rootNote < 0 || chordDetector.rootNote >= 12)
            {
                noteInvalidChordFrame();
                return;
            }

            publishChordCandidate(chordDetector.rootNote, chordDetector.quality, chordDetector.intervals);
            return;
        }

        if (!hasDominantPitchClass(averagedChroma, dominantPitchClass))
        {
            noteInvalidChordFrame();
            return;
        }

        publishNoteCandidate(dominantPitchClass);
    }

    static int makeChordKey(int root, int quality, int intervals)
    {
        return root * 100 + quality * 10 + intervals;
    }

    void resetChordDecisionState()
    {
        pendingChordKey = -1;
        pendingChordHits = 0;
        pendingNoteRoot = -1;
        pendingNoteHits = 0;
        displayedChordKey = -1;
        invalidChordFrames = 0;
        chordRoot.store(-1, std::memory_order_relaxed);
        chordQuality.store(ChordDetector::Major, std::memory_order_relaxed);
        chordIntervals.store(0, std::memory_order_relaxed);
        chordValid.store(false, std::memory_order_relaxed);
        noteRoot.store(-1, std::memory_order_relaxed);
        noteValid.store(false, std::memory_order_relaxed);
    }

    void noteInvalidChordFrame()
    {
        pendingChordKey = -1;
        pendingChordHits = 0;
        pendingNoteRoot = -1;
        pendingNoteHits = 0;

        if (++invalidChordFrames >= invalidClearThresholdFrames)
            resetChordDecisionState();
    }

    static bool hasEnoughHarmonicContent(const std::vector<double>& chroma)
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

    static bool hasDominantPitchClass(const std::vector<double>& chroma, int& dominantPitchClass)
    {
        dominantPitchClass = -1;

        double sum = 0.0;
        double max1 = 0.0;
        int strongPitchClasses = 0;

        for (int i = 0; i < 12 && i < (int)chroma.size(); ++i)
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

    void publishNoteCandidate(int root)
    {
        invalidChordFrames = 0;
        pendingChordKey = -1;
        pendingChordHits = 0;

        if (root == pendingNoteRoot)
            pendingNoteHits = juce::jmin(pendingNoteHits + 1, 1000);
        else
        {
            pendingNoteRoot = root;
            pendingNoteHits = 1;
        }

        const int displayedNote = noteRoot.load(std::memory_order_relaxed);
        if (noteValid.load(std::memory_order_relaxed) && root == displayedNote)
            return;

        const int requiredHits = displayedNote >= 0 ? stableNoteChangeHitThreshold : stableNoteHitThreshold;
        if (pendingNoteHits < requiredHits)
            return;

        noteRoot.store(root, std::memory_order_relaxed);
        noteValid.store(true, std::memory_order_relaxed);
        chordValid.store(false, std::memory_order_relaxed);
    }

    void publishChordCandidate(int root, int quality, int intervals)
    {
        const int candidateKey = makeChordKey(root, quality, intervals);
        invalidChordFrames = 0;
        pendingNoteRoot = -1;
        pendingNoteHits = 0;

        if (candidateKey == pendingChordKey)
            pendingChordHits = juce::jmin(pendingChordHits + 1, 1000);
        else
        {
            pendingChordKey = candidateKey;
            pendingChordHits = 1;
        }

        if (candidateKey == displayedChordKey)
        {
            noteRoot.store(-1, std::memory_order_relaxed);
            noteValid.store(false, std::memory_order_relaxed);
            chordValid.store(true, std::memory_order_relaxed);
            return;
        }

        const int requiredHits = displayedChordKey >= 0 ? stableChordChangeHitThreshold : stableCandidateHitThreshold;
        if (pendingChordHits < requiredHits)
            return;

        displayedChordKey = candidateKey;
        chordRoot.store(root, std::memory_order_relaxed);
        chordQuality.store(quality, std::memory_order_relaxed);
        chordIntervals.store(intervals, std::memory_order_relaxed);
        noteRoot.store(-1, std::memory_order_relaxed);
        noteValid.store(false, std::memory_order_relaxed);
        chordValid.store(true, std::memory_order_relaxed);
    }

    static constexpr int frameSize = 512;
    static constexpr int chromaCalculationIntervalSamples = 2048;
    static constexpr int chromaHistoryFrames = 8;
    static constexpr int stableCandidateHitThreshold = 3;
    static constexpr int stableChordChangeHitThreshold = 6;
    static constexpr int stableNoteHitThreshold = 1;
    static constexpr int stableNoteChangeHitThreshold = 4;
    static constexpr int invalidClearThresholdFrames = 8;
    static constexpr double silenceRmsThreshold = 0.001;

    std::unique_ptr<Chromagram> chromagram;
    std::unique_ptr<juce::AbstractFifo> audioFifo;
    ChordDetector chordDetector;
    std::vector<double> frame;
    std::vector<double> averagedChroma;
    std::array<std::array<double, 12>, chromaHistoryFrames> chromaHistory {};
    std::vector<float> ringBuffer;
    std::vector<float> readBuffer;
    juce::WaitableEvent samplesAvailable;
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
    std::atomic<bool> ready { false };
    std::atomic<bool> chordValid { false };
    std::atomic<bool> noteValid { false };
    std::atomic<int> chordRoot { -1 };
    std::atomic<int> noteRoot { -1 };
    std::atomic<int> chordQuality { ChordDetector::Major };
    std::atomic<int> chordIntervals { 0 };
    std::atomic<double> cpuPercent { 0.0 };
    std::atomic<int> memoryKb { 0 };
    std::atomic<int> requestedSampleRate { 44100 };
    std::atomic<long long> droppedSamples { 0 };
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
    constexpr int kSyncSignalChannelIndex = 0;
    constexpr double intervalHelperPayloadMinWriteMs = 100.0;
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

    static float getSamplePadDuckGainForBeat(double beat,
                                             NinjamVst3AudioProcessor::SamplePadDuckShape shape,
                                             NinjamVst3AudioProcessor::SamplePadDuckLength length)
    {
        const double cycleBeats = getSamplePadDuckLengthBeats(length);
        double phase = beat / cycleBeats;
        phase = std::fmod(phase, 1.0);
        if (phase < 0.0)
            phase += 1.0;

        constexpr float duckAmount = 0.82f;
        double shaped = 1.0;

        using DuckShape = NinjamVst3AudioProcessor::SamplePadDuckShape;
        switch (shape)
        {
            case DuckShape::tightPump:
                shaped = phase < 0.30 ? smoothDuckStep(phase / 0.30) : 1.0;
                break;

            case DuckShape::slowPump:
                shaped = phase < 0.86 ? smoothDuckStep(phase / 0.86) : 1.0;
                break;

            case DuckShape::hardGate:
                shaped = phase < 0.38 ? 0.03 : phase < 0.47 ? smoothDuckStep((phase - 0.38) / 0.09) : 1.0;
                break;

            case DuckShape::reverseSwell:
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
                shaped = phase < 0.62 ? smoothDuckStep(phase / 0.62) : 1.0;
                break;
        }

        return 1.0f - duckAmount * (1.0f - (float)shaped);
    }

    inline juce::String getDefaultSamplePadName(int padIndex)
    {
        return "Pad " + juce::String(padIndex + 1);
    }

    static juce::File getNinjamplusSettingsDirectory()
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
        return options.getDefaultFile().getParentDirectory();
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

    static int quantiseSamplePadFreeLoopBeats(double capturedBeats)
    {
        constexpr int beatsPerBar = 4;
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
        constexpr double gridBeats = 4.0;
        const double safeBpi = (double)juce::jmax(1, bpi);
        const double intervalStart = std::floor(currentBeat / safeBpi) * safeBpi;
        const double beatInInterval = juce::jlimit(0.0, safeBpi, currentBeat - intervalStart);
        const double nextGridOffset = std::floor(beatInInterval / gridBeats) * gridBeats + gridBeats;
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

    static double nextSamplePadVisibleIntervalStartBeat(double currentBeat, int bpi)
    {
        constexpr double minimumOrangeLeadBeats = 1.0;
        const double safeBpi = (double)juce::jmax(1, bpi);
        double candidate = nextSamplePadIntervalStartBeat(currentBeat, bpi);
        if (candidate - currentBeat < minimumOrangeLeadBeats)
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

    static double detectSampleBpmWithAubio(const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
#if NINJAMPLUS_HAS_AUBIO
        const int numSamples = buffer.getNumSamples();
        if (numSamples <= 0 || sampleRate <= 1.0)
            return 0.0;

        constexpr uint_t windowSize = 1024;
        constexpr uint_t hopSize = 512;
        auto* tempo = new_aubio_tempo((const char_t*)"default",
                                      windowSize,
                                      hopSize,
                                      (uint_t)juce::jmax(1, (int)std::llround(sampleRate)));
        auto* input = new_fvec(hopSize);
        auto* output = new_fvec(1);
        if (tempo == nullptr || input == nullptr || output == nullptr)
        {
            if (tempo != nullptr) del_aubio_tempo(tempo);
            if (input != nullptr) del_fvec(input);
            if (output != nullptr) del_fvec(output);
            return 0.0;
        }

        const float* left = buffer.getReadPointer(0);
        const float* right = buffer.getNumChannels() > 1 ? buffer.getReadPointer(1) : left;
        int detectedBeats = 0;
        for (int pos = 0; pos < numSamples; pos += (int)hopSize)
        {
            for (uint_t i = 0; i < hopSize; ++i)
            {
                const int index = pos + (int)i;
                const float sample = index < numSamples ? 0.5f * (left[index] + right[index]) : 0.0f;
                fvec_set_sample(input, (smpl_t)sample, i);
            }
            aubio_tempo_do(tempo, input, output);
            if (fvec_get_sample(output, 0) != 0.0f)
                ++detectedBeats;
        }

        const double bpm = (double)aubio_tempo_get_bpm(tempo);
        const double confidence = (double)aubio_tempo_get_confidence(tempo);
        del_aubio_tempo(tempo);
        del_fvec(input);
        del_fvec(output);

        const double normalisedBpm = normaliseDetectedTempoBpm(bpm);
        const double durationSeconds = (double)numSamples / sampleRate;
        if (normalisedBpm <= 1.0
            || !std::isfinite(confidence)
            || confidence < 0.10
            || (durationSeconds >= 3.0 && detectedBeats < 2))
            return 0.0;

        return normalisedBpm;
#else
        juce::ignoreUnused(buffer, sampleRate);
        return 0.0;
#endif
    }

    static juce::AudioBuffer<float> stretchLoopWithSignalsmith(const juce::AudioBuffer<float>& source,
                                                               double sampleRate,
                                                               int targetSamples)
    {
        const int inputSamples = source.getNumSamples();
        if (inputSamples <= 0 || targetSamples <= 0)
            return {};

        juce::AudioBuffer<float> output(2, targetSamples);
        output.clear();

        if (targetSamples == inputSamples)
        {
            output.copyFrom(0, 0, source, 0, 0, inputSamples);
            output.copyFrom(1, 0, source, source.getNumChannels() > 1 ? 1 : 0, 0, inputSamples);
            return output;
        }

        const float* inL = source.getReadPointer(0);
        const float* inR = source.getNumChannels() > 1 ? source.getReadPointer(1) : inL;
        float* outL = output.getWritePointer(0);
        float* outR = output.getWritePointer(1);
        std::array<const float*, 2> inputs { inL, inR };
        std::array<float*, 2> outputs { outL, outR };

        signalsmith::stretch::SignalsmithStretch<float> stretcher;
        stretcher.presetDefault(2, (float)juce::jmax(1.0, sampleRate), false);
        stretcher.reset();
        stretcher.process(inputs, inputSamples, outputs, targetSamples);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
        {
            auto* data = output.getWritePointer(ch);
            for (int i = 0; i < targetSamples; ++i)
            {
                if (!std::isfinite(data[i]))
                    data[i] = 0.0f;
            }
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
        outData.detectedBpm = detectSampleBpmWithAubio(outData.sample, sourceRate);
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

    localChordAnalyzer = std::make_unique<LocalChordAnalyzer>();
    linkTimingState = std::make_unique<LinkTimingState>();
    for (auto& analyzer : remoteChordAnalyzers)
        analyzer = std::make_unique<LocalChordAnalyzer>();

    setLatencySamples(0);
    startTimer(20); // Run NINJAM client loop every 20ms

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

    videoHelperRootDir = resolveVideoHelperRootDir();
    asyncChatTranslationWorker = std::make_unique<AsyncChatTranslationWorker>(*this);
    abletonLink = std::make_unique<ableton::LinkAudio>(120.0, getLinkPeerName().toStdString());
    refreshAbletonLinkActivation();
}

void NinjamVst3AudioProcessor::connectToServer(juce::String host, juce::String user, juce::String pass)
{
    stopNinjamZapVideoTransportForDisconnect();
    stopAdvancedVideoClient();
    ninjamZapServerVideoSupported.store(false, std::memory_order_relaxed);
    ninjamSideSignalServerSupported.store(false, std::memory_order_relaxed);
    ninjamSideSignalVideoCapSent.store(false, std::memory_order_relaxed);

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

    pendingConnectHost = host;
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
    localServerLatencyMs.store(-1);
    lastServerLatencyProbeInterval.store(-1);
    if (serverLatencyProbeFuture.valid())
        serverLatencyProbeFuture.wait();
    serverLatencyProbeInProgress.store(false);

    {
        const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
        remoteLinkAudioOutputPairs.clear();
        remoteLinkAudioSinks.clear();
    }

    {
        const juce::ScopedLock clientLock(ninjamClientLock);
        applyCodecPreference();
        ninjamClient.Connect(host.toRawUTF8(), user.toRawUTF8(), pass.toRawUTF8());
    }
    currentServer = host;
    currentUser = user;
    refreshAbletonLinkActivation();

    // Do NOT reset isTransmitting here — the user may have toggled it before
    // connecting. The NJC_STATUS_OK handler calls syncLocalIntervalChannelConfig()
    // which re-applies the current isTransmitting state to NJClient.
}

void NinjamVst3AudioProcessor::disconnectFromServer()
{
    duplicateNameRetryEnabled = false;
    pendingConnectNameAttempt = 0;
    ninjamZapServerVideoSupported.store(false, std::memory_order_relaxed);
    ninjamSideSignalServerSupported.store(false, std::memory_order_relaxed);
    ninjamSideSignalVideoCapSent.store(false, std::memory_order_relaxed);
    {
        const juce::ScopedLock clientLock(ninjamClientLock);
        stopNinjamZapVideoTransportForDisconnect();
        ninjamClient.Disconnect();
    }
    stopAdvancedVideoClient();
    currentServer = {};
    currentUser = {};
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
    localServerLatencyMs.store(-1);
    lastServerLatencyProbeInterval.store(-1);
    if (serverLatencyProbeFuture.valid())
        serverLatencyProbeFuture.wait();
    serverLatencyProbeInProgress.store(false);
    applyCodecPreference();
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

void NinjamVst3AudioProcessor::setMetronomeVolume(float vol)
{
    vol = juce::jlimit(0.0f, 1.0f, vol);
    if (vol > 0.0f)
    {
        storedMetronomeVolume.store(vol);
        metronomeMuted.store(false);
    }
    ninjamClient.config_metronome = metronomeMuted.load() ? 0.0f : vol;
}

float NinjamVst3AudioProcessor::getMetronomeVolume() const
{
    return ninjamClient.config_metronome;
}

void NinjamVst3AudioProcessor::setMetronomeMuted(bool shouldMute)
{
    metronomeMuted.store(shouldMute);
    if (shouldMute)
    {
        const float current = ninjamClient.config_metronome;
        if (current > 0.0f)
            storedMetronomeVolume.store(current);
        ninjamClient.config_metronome = 0.0f;
    }
    else
    {
        ninjamClient.config_metronome = juce::jlimit(0.0f, 1.0f, storedMetronomeVolume.load());
    }
}

bool NinjamVst3AudioProcessor::isMetronomeMuted() const
{
    return metronomeMuted.load();
}

void NinjamVst3AudioProcessor::setStoredMetronomeVolume(float vol)
{
    storedMetronomeVolume.store(juce::jlimit(0.0f, 1.0f, vol));
}

float NinjamVst3AudioProcessor::getStoredMetronomeVolume() const
{
    return juce::jlimit(0.0f, 1.0f, storedMetronomeVolume.load());
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

    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("type", "intervalSyncTag");
    obj->setProperty("userId", userId.isNotEmpty() ? userId : currentUser);
    obj->setProperty("tag", tag);
    obj->setProperty("intervalIndex", displayInterval);
    obj->setProperty("intervalAbsolute", intervalIndex.load());
    obj->setProperty("bpi", bpi);
    obj->setProperty("beatIndex", beatIndex);
    obj->setProperty("intervalProgress", intervalProgress);
    if (const int serverLatencyMs = localServerLatencyMs.load(); serverLatencyMs >= 0)
        obj->setProperty("serverLatencyMs", serverLatencyMs);
    obj->setProperty("eventId", "intervalTag:" + (userId.isNotEmpty() ? userId : currentUser) + ":" + juce::String(++sideSignalEventCounter));
    const juce::String payload = juce::JSON::toString(juce::var(obj.get()));
    const juce::String safeTarget = target.isNotEmpty() ? target : "*";
    sendIntervalSignal("intervalSyncTag", payload, safeTarget);
    return;
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

void NinjamVst3AudioProcessor::measureServerLatencyAsync()
{
    // Do not open a throwaway TCP socket to estimate RTT here. NINJAM servers
    // log that unauthenticated probe as "Incoming connection" followed by an
    // empty-user disconnect, and Zap/local servers can get spammed every few
    // seconds. Sync continues to use the authenticated interval markers and
    // treats this optional local route estimate as unavailable.
    serverLatencyProbeInProgress.store(false);
    localServerLatencyMs.store(-1);
}

void NinjamVst3AudioProcessor::broadcastOpusSyncSupport(const juce::String& target)
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
        return;

    const juce::String userId = normaliseOpusPeerId(currentUser);
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("type", "opusSyncSupport");
    obj->setProperty("userId", userId.isNotEmpty() ? userId : currentUser);
    obj->setProperty("clientId", opusSyncInstanceId);
    obj->setProperty("appFamily", opusSyncAppFamily);
    obj->setProperty("handshakeVersion", opusSyncHandshakeVersion);
    obj->setProperty("runtimeFormat", getWrapperTypeName(wrapperType));
    obj->setProperty("pluginName", juce::String(JucePlugin_Name));
    obj->setProperty("pluginVersion", getVersionString());
    obj->setProperty("supportsOpus", true);
    obj->setProperty("enabled", numLocalChannels.load() > 1);
    obj->setProperty("numChannels", numLocalChannels.load());
    obj->setProperty("eventId", "opusSupport:" + (userId.isNotEmpty() ? userId : currentUser) + ":" + juce::String(++sideSignalEventCounter));
    const juce::String payload = juce::JSON::toString(juce::var(obj.get()));
    // Use NINJAM interval channel with custom FOURCC — works on any standard server,
    // routed like audio data, other clients silently ignore unknown FOURCCs.
    // Target is ignored here (interval data goes to all subscribers of our channel 0).
    juce::ignoreUnused(target);
    ninjamClient.SendRawIntervalItem(0, kOpusSyncFourcc, payload.toRawUTF8(), (int)payload.getNumBytesAsUTF8());
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
                peerMultiChanByName[snapKey] = { peer.multiChanEnabled, peer.numChannels };
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
    syncLocalIntervalChannelConfig();
}

void NinjamVst3AudioProcessor::syncLocalIntervalChannelConfig()
{
    const bool shouldTransmit = isTransmitting;
    const int bitrate = shouldTransmit ? localBitrate : 24;
    const int flags = voiceChatMode ? 2 : 0;
    const int numCh = juce::jlimit(1, maxLocalChannels, numLocalChannels.load());
    const bool multiChanAuto = numCh > 1 && opusSyncAvailable.load() && shouldTransmit;
    const bool singleStereoLocal = numCh == 1 && getLocalChannelInput(0) < 0;
    const bool cameraVideoEnabled = ninjamZapCameraSendEnabled.load(std::memory_order_relaxed);
    const int videoChannel = cameraVideoEnabled ? getNinjamZapVideoChannelIndex() : -1;

    if (multiChanAuto)
    {
        // NINJAM ch 0: Vorbis mixdown (for all clients including legacy)
        // NINJAM ch 1..N: Opus per-channel (for our VST3 clients only)
        juce::String ch0Name = getLocalChannelName(0);
        if (ch0Name.isEmpty()) ch0Name = "Mix";
        ninjamClient.SetLocalChannelInfo(0, ch0Name.toRawUTF8(),
            true, numCh,          // srcch = mix buffer at inputs[numCh]
            true, bitrate, true, true, false, 0, true, flags);
        // Mute engine local output — monitor block handles local audio routing with proper stereo.
        ninjamClient.SetLocalChannelMonitoring(0, false, 0.f, false, 0.f, true, true, false, false);
        for (int i = 0; i < numCh; ++i)
        {
            juce::String chName = getLocalChannelName(i);
            if (chName.isEmpty()) chName = "Ch " + juce::String(i + 1);
            ninjamClient.SetLocalChannelInfo(i + 1, chName.toRawUTF8(),
                true, i,          // srcch = original buffer slot i
                true, bitrate, true, true, false, 0, true, flags);
            ninjamClient.SetLocalChannelMonitoring(i + 1, false, 0.f, false, 0.f, true, true, false, false);
        }
        configureNinjamZapVideoLocalChannel();
        for (int i = numCh + 1; i <= maxLocalChannels + 1; ++i)
            if (i != videoChannel)
                ninjamClient.DeleteLocalChannel(i);
    }
    else
    {
        // Vorbis only: single channel
        juce::String ch0Name = getLocalChannelName(0);
        if (ch0Name.isEmpty()) ch0Name = "Input";
        const int sourceChannel = shouldTransmit ? (singleStereoLocal ? 1024 : 0) : 1023;
        ninjamClient.SetLocalChannelInfo(0, ch0Name.toRawUTF8(),
            true, sourceChannel, true, bitrate, true, true, false, 0, true, flags);
        // Mute engine local output — monitor block handles local audio routing with proper stereo.
        ninjamClient.SetLocalChannelMonitoring(0, false, 0.f, false, 0.f, true, true, false, false);
        configureNinjamZapVideoLocalChannel();
        for (int i = 1; i <= maxLocalChannels + 1; ++i)
            if (i != videoChannel)
                ninjamClient.DeleteLocalChannel(i);
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
    syncLocalIntervalChannelConfig();
}

bool NinjamVst3AudioProcessor::isVoiceChatMode() const
{
    return voiceChatMode;
}

void NinjamVst3AudioProcessor::applyCodecPreference()
{
    const int numCh = juce::jlimit(1, maxLocalChannels, numLocalChannels.load());
    const bool multiChanAuto = numCh > 1 && opusSyncAvailable.load();
    const int decodeCaps = NJClient::NJCLIENT_CAP_DECODE_VORBIS | NJClient::NJCLIENT_CAP_DECODE_OPUS;

    if (multiChanAuto)
    {
        // ch 0: Vorbis only (mixdown for all clients)
        // ch 1..N: Opus only (per-channel for our VST3 clients)
        unsigned int vorbisMask = 0x1u;
        unsigned int opusMask = 0u;
        for (int i = 0; i < numCh; ++i)
            opusMask |= (1u << (i + 1));
        ninjamClient.SetCodecCapabilities(
            NJClient::NJCLIENT_CAP_ENCODE_VORBIS | NJClient::NJCLIENT_CAP_ENCODE_OPUS, decodeCaps);
        ninjamClient.SetCodecConfig(vorbisMask, opusMask);
    }
    else
    {
        // Single channel or no VST3 peers: Vorbis only
        ninjamClient.SetCodecCapabilities(NJClient::NJCLIENT_CAP_ENCODE_VORBIS, decodeCaps);
        ninjamClient.SetCodecConfig(0x1u, 0u);
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

juce::File NinjamVst3AudioProcessor::resolveVideoHelperRootDir() const
{
    juce::Array<juce::File> candidates;
    juce::Array<juce::File> roots;

    const juce::File exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    roots.add(exeDir);

    const juce::File moduleFile = getThisModuleFile();
    if (moduleFile.existsAsFile())
        roots.addIfNotAlreadyThere(moduleFile.getParentDirectory());

    for (const auto& root : roots)
    {
        juce::File probe = root;
        for (int i = 0; i < 8; ++i)
        {
            candidates.add(probe.getChildFile("Resources").getChildFile("advanced-vdo-client"));
            candidates.add(probe.getParentDirectory().getChildFile("Resources").getChildFile("advanced-vdo-client"));
            candidates.add(probe.getChildFile("advanced-vdo-client"));
            probe = probe.getParentDirectory();
        }
    }

    for (const auto& dir : candidates)
    {
        if (dir.isDirectory()
            && dir.getChildFile("index.html").existsAsFile()
            && dir.getChildFile("app.html").existsAsFile())
            return dir;
    }

    return {};
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
        lastIntervalHelperPayloadWriteMs = 0.0;
        return true;
    }

    const juce::File rootDir = resolveVideoHelperRootDir();
    if (!rootDir.isDirectory())
        return false;

    if (!advancedVideoServer)
    {
        advancedVideoServer = std::make_unique<LocalVideoHttpServer>(
            rootDir,
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

void NinjamVst3AudioProcessor::launchVideoSessionAsync()
{
    bool expected = false;
    if (!videoLaunchInProgress.compare_exchange_strong(expected, true))
        return;

    const juce::ScopedLock launchLock(videoLaunchWorkerLock);
    if (videoLaunchFuture.valid())
        videoLaunchFuture.wait();

    videoLaunchFuture = std::async(std::launch::async, [this]()
    {
        struct VideoLaunchScope
        {
            explicit VideoLaunchScope(std::atomic<bool>& inProgress) : flag(inProgress) {}
            ~VideoLaunchScope() { flag.store(false); }

            std::atomic<bool>& flag;
        } scope(videoLaunchInProgress);

        try
        {
            launchVideoSession();
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

bool NinjamVst3AudioProcessor::isNinjamRemoteChannelVideoOnly(int userIndex, int channelIndex)
{
    int flags = 0;
    const char* chName = ninjamClient.GetUserChannelState(userIndex, channelIndex,
                                                          nullptr, nullptr, nullptr,
                                                          nullptr, nullptr, nullptr,
                                                          &flags);
    return chName != nullptr && ((flags & kNinjamZapVideoOnlyChannelFlag) != 0);
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
    const int numUsers = ninjamClient.GetNumUsers();
    for (int userIndex = 0; userIndex < numUsers; ++userIndex)
    {
        bool isSubscribed = false;
        int flags = 0;
        const char* chName = ninjamClient.GetUserChannelState(userIndex, 0,
                                                              &isSubscribed,
                                                              nullptr, nullptr,
                                                              nullptr, nullptr,
                                                              nullptr,
                                                              &flags);
        if (chName == nullptr || ((flags & kNinjamZapVideoOnlyChannelFlag) != 0))
            continue;

        if (!isSubscribed)
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
    const int numCh = juce::jlimit(1, maxLocalChannels, numLocalChannels.load());
    const bool multiChanAuto = numCh > 1 && opusSyncAvailable.load() && isTransmittingLocal();
    return multiChanAuto ? numCh + 1 : 1;
}

void NinjamVst3AudioProcessor::configureNinjamZapVideoLocalChannel()
{
    if (!ninjamZapCameraSendEnabled.load(std::memory_order_relaxed))
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

        if (prevMatchIt != intervalsByGuid.end())
        {
            promote(std::move(prevMatchIt->second));
            intervalsByGuid.erase(prevMatchIt);
            zapVideoPlaybackByStream[streamKey].holdCount = 0;
        }
        else if (currentMatchIt != intervalsByGuid.end())
        {
            zapVideoDeferredPlaybackByStream[streamKey] = std::move(currentMatchIt->second);
            intervalsByGuid.erase(currentMatchIt);
            zapVideoPlaybackByStream[streamKey].holdCount = 0;
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



void NinjamVst3AudioProcessor::launchVideoSession()
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

    vdoVideoSyncEnabled.store(true, std::memory_order_relaxed);
    intervalHelperPayloadForceWrite.store(true, std::memory_order_release);
    lastIntervalHelperPayloadWriteMs = 0.0;
    resetIntervalSyncTimingCache();
    {
        const juce::ScopedLock clientLock(ninjamClientLock);
        if (ninjamClient.GetStatus() == NJClient::NJC_STATUS_OK
            && ninjamClient.GetServerVideoSupported())
        {
            ninjamZapServerVideoSupported.store(true, std::memory_order_relaxed);
            ninjamSideSignalServerSupported.store(true, std::memory_order_relaxed);
            if (!ninjamSideSignalVideoCapSent.load(std::memory_order_relaxed))
            {
                ninjamClient.ChatMessage_Send("VIDEO_CAP", "1", nullptr, nullptr, nullptr);
                ninjamSideSignalVideoCapSent.store(true, std::memory_order_relaxed);
            }
        }
    }

    juce::String roomSource = currentServer.trim();
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
    const int lastColonPos = hostPart.lastIndexOfChar(':');
    if (lastColonPos > 0 && lastColonPos + 1 < hostPart.length())
    {
        const juce::String candidatePort = hostPart.substring(lastColonPos + 1).trim();
        bool allDigits = candidatePort.isNotEmpty();
        for (int i = 0; i < candidatePort.length() && allDigits; ++i)
            allDigits = juce::CharacterFunctions::isDigit(candidatePort[i]);
        if (allDigits)
        {
            hostPart = hostPart.substring(0, lastColonPos);
            portPart = candidatePort;
        }
    }

    const int firstDotPos = hostPart.indexOfChar('.');
    if (firstDotPos > 0)
        hostPart = hostPart.substring(0, firstDotPos);

    juce::String roomRaw = hostPart;
    if (portPart.isNotEmpty())
        roomRaw << "_" << portPart;

    juce::String room;
    bool lastWasUnderscore = false;
    for (int i = 0; i < roomRaw.length(); ++i)
    {
        const juce_wchar ch = roomRaw[i];
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
    room = room.trimCharactersAtStart("_").trimCharactersAtEnd("_");
    if (room.isEmpty())
        room = "ninjam_room";
    const juce::String cleanUserLabel = normaliseChatTargetNick(currentUser);
    const juce::String label = cleanUserLabel.isNotEmpty() ? cleanUserLabel : "NINJAM";
    static constexpr int minimumVdoBufferMs = 0;
    int viewDelayMs = 0;
    {
        const juce::ScopedLock lock(intervalSyncAnnouncementLock);
        for (const auto& entry : remoteLatencyFirmDelayMsByUser)
            viewDelayMs = juce::jmax(viewDelayMs, juce::jmax(0, entry.second));
    }
    const int launchBufferMs = juce::jmax(minimumVdoBufferMs, viewDelayMs);
    const int chunkMs = juce::jlimit(60, 800, (int)std::llround((double)launchBufferMs * 0.25));

    if (ensureAdvancedVideoClientStarted())
    {
        const int helperPort = advancedVideoHelperPort.load();
        juce::URL helperUrl("http://127.0.0.1:" + juce::String(helperPort) + "/buffer-room");
        helperUrl = helperUrl.withParameter("room", room)
                             .withParameter("label", label)
                             .withParameter("bufferMode", "remote")
                             .withParameter("buffer", juce::String(launchBufferMs))
                             .withParameter("chunked", juce::String(chunkMs))
                             .withParameter("helperVersion", getVersionString())
                             .withParameter("cacheBust", juce::String((juce::int64)juce::Time::getMillisecondCounter()));
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

    juce::URL url("https://vdo.ninja/");
    url = url.withParameter("room", room)
             .withParameter("label", label)
             .withParameter("chunked", juce::String(chunkMs))
             .withParameter("chunkbufferadaptive", "0")
             .withParameter("chunkbufferceil", "180000")
             .withParameter("noaudio", "1")
             .withParameter("buffer2", "0")
             .withParameter("buffer", juce::String(launchBufferMs));
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
        const juce::String senderKey = normaliseOpusPeerId(userName);
        const juce::String canonicalUserKey = canonicalDelayUserKey(userName);
        const juce::String rosterKey = canonicalUserKey.isNotEmpty() ? canonicalUserKey : senderKey;
        if (rosterKey.isNotEmpty())
            activeRoster.add(rosterKey);
        const int localServerLatencyRawMs = localServerLatencyMs.load();
        const int localServerRouteLatencyMs = juce::jmax(0, localServerLatencyRawMs);
        bool remoteSub = false;
        float remoteChVol = 1.0f, remoteChPan = 0.0f;
        bool remoteChMute = false, remoteChSolo = false;
        int remoteOutCh = 0, remoteFlags = 0;
        const char* remoteChannelName = ninjamClient.GetUserChannelState(userIdx, 0,
                                         &remoteSub,
                                         &remoteChVol,
                                         &remoteChPan,
                                         &remoteChMute,
                                         &remoteChSolo,
                                         &remoteOutCh,
                                         &remoteFlags);
        const bool remoteVoiceChatMode = (remoteChannelName != nullptr) && ((remoteFlags & 2) != 0);
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
            const int fallbackRouteLatencyMs = juce::jmax(0, remoteServerLatencyMs) + localServerRouteLatencyMs;
            const int routeLatencyForBufferMs = serverRouteLatencyMs >= 0 ? serverRouteLatencyMs : fallbackRouteLatencyMs;
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
                        bufferMs = juce::jmax(0, (int)std::llround(fallback)) + routeLatencyForBufferMs;
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
                        bufferMs = juce::jmax(0, (int)std::llround(fallback)) + routeLatencyForBufferMs;
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
        userObj->setProperty("ourServerLatencyReady", localServerLatencyRawMs >= 0);
        userObj->setProperty("ourServerLatencyProbeEnabled", false);
        userObj->setProperty("theirServerLatencyReady", remoteServerLatencyMs >= 0);
        userObj->setProperty("serverRouteLatencyReady", serverRouteLatencyMs >= 0);
        if (bufferMs >= 0)
        {
            userObj->setProperty("bufferTotalMs", (double)bufferMs);
            userObj->setProperty("senderBufferMs", 0.0);
            userObj->setProperty("receiverBufferMs", (double)bufferMs);
            userObj->setProperty("measuredAudioDelayMs", (double)bufferMs);
            if (remoteServerLatencyMs >= 0)
                userObj->setProperty("senderServerLatencyMs", (double)remoteServerLatencyMs);
            if (serverRouteLatencyMs >= 0)
                userObj->setProperty("serverRouteLatencyMs", (double)serverRouteLatencyMs);
            if (localServerLatencyRawMs >= 0)
                userObj->setProperty("receiverServerLatencyMs", (double)localServerRouteLatencyMs);
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

    const int maxOutputPairs = 16;
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

            const juce::String userIdentityKey = canonicalDelayUserKey(u.name);
            auto identityIt = remoteUserNameByIndex.find(i);
            if (identityIt == remoteUserNameByIndex.end() || identityIt->second != userIdentityKey)
            {
                resetRemoteUserIndexState(i, u.name);
                remoteUserNameByIndex[i] = userIdentityKey;
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

                if (itAssign != userOutputAssignment.end())
                {
                    desiredPair = itAssign->second;
                }
                else
                {
                    if ((int)reservedPairs.size() < maxOutputPairs)
                    {
                        for (int cand = 0; cand < maxOutputPairs; ++cand)
                        {
                            if (!reservedPairs.count(cand))
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
                        for (int cand = 0; cand < maxOutputPairs; ++cand)
                        {
                            if (!connectedNow.count(cand))
                            {
                                fallback = cand;
                                break;
                            }
                        }
                        if (fallback < 0)
                            fallback = 0;
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

            // Look up multichannel state from the snapshot updated by refreshOpusSyncAvailabilityFromUsers().
            // This map is keyed by normalised username and never holds njclient locks.
            {
                const juce::String normName = canonicalDelayUserKey(u.name);
                const juce::ScopedLock mcLock(peerMultiChanLock);
                auto it = peerMultiChanByName.find(normName);
                if (it != peerMultiChanByName.end())
                {
                    u.isMultiChanPeer = it->second.isMultiChan;
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
                    int channelFlags = 0;
                    const char* chName = ninjamClient.GetUserChannelState(i, ch + 1,
                                                                          nullptr, nullptr, nullptr,
                                                                          nullptr, nullptr, nullptr,
                                                                          &channelFlags); // ch0=mix, ch1..N=individual
                    if (chName != nullptr && *chName != '\0' && ((channelFlags & kNinjamZapVideoOnlyChannelFlag) == 0))
                        u.channelNames.add(juce::String::fromUTF8(chName));
                    else
                        u.channelNames.add("Ch " + juce::String(ch + 1));
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
            }

            if (i >= 0 && i < maxRemoteChordUsers)
            {
                if (remoteChordUserKeys[(size_t)i] != u.name)
                {
                    remoteChordUserKeys[(size_t)i] = u.name;
                    remoteChordDetectionEnabled[(size_t)i].store(true, std::memory_order_relaxed);
                    auto& analyzer = remoteChordAnalyzers[(size_t)i];
                    if (analyzer && analyzer->isPrepared())
                        analyzer->markNoInput();
                }

                auto& analyzer = remoteChordAnalyzers[(size_t)i];
                if (analyzer && !analyzer->isPrepared())
                    analyzer->prepare(processingSampleRate);
            }

            users.push_back(u);
        }
    }

    for (int i = numUsers; i < maxRemoteChordUsers; ++i)
    {
        remoteChordUserKeys[(size_t)i].clear();
        remoteChordDetectionEnabled[(size_t)i].store(true, std::memory_order_relaxed);
        auto& analyzer = remoteChordAnalyzers[(size_t)i];
        if (analyzer && analyzer->isPrepared())
            analyzer->markNoInput();
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
        auto& analyzer = remoteChordAnalyzers[(size_t)userIndex];
        if (analyzer && analyzer->isPrepared())
            analyzer->markNoInput();
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
}

void NinjamVst3AudioProcessor::setUserOutput(int userIndex, int outputChannelIndex)
{
    const int numUsers = ninjamClient.GetNumUsers();
    if (userIndex < 0 || userIndex >= numUsers)
        return;

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

    if (isNinjamRemoteChannelVideoOnly(userIndex, njChanIdx))
        return 0.0f;

    return ninjamClient.GetUserChannelPeak(userIndex, njChanIdx, lrSide);
}

void NinjamVst3AudioProcessor::setUserNjChannelVolume(int userIndex, int njChanIdx, float volume)
{
    const int numUsers = ninjamClient.GetNumUsers();
    if (userIndex < 0 || userIndex >= numUsers || njChanIdx < 0)
        return;

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

juce::String NinjamVst3AudioProcessor::getVersionString() const
{
    return NINJAM_DISPLAY_VERSION;
}

void NinjamVst3AudioProcessor::setSoftLimiterEnabled(bool shouldEnable)
{
    softLimiterEnabled.store(shouldEnable);
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

    return localChordAnalyzer ? localChordAnalyzer->getLabel() : "--";
}

double NinjamVst3AudioProcessor::getLocalChordCpuPercent() const
{
    if (!isChordDetectionEnabled())
        return 0.0;

    return localChordAnalyzer ? localChordAnalyzer->getCpuPercent() : 0.0;
}

int NinjamVst3AudioProcessor::getLocalChordMemoryKb() const
{
    return localChordAnalyzer ? localChordAnalyzer->getMemoryKb() : 0;
}

juce::String NinjamVst3AudioProcessor::getUserChordLabel(int userIndex) const
{
    if (userIndex < 0 || userIndex >= maxRemoteChordUsers)
        return "--";

    if (!isChordDetectionEnabled() || !isUserChordDetectionEnabled(userIndex))
        return "Off";

    const auto& analyzer = remoteChordAnalyzers[(size_t)userIndex];
    return analyzer ? analyzer->getLabel() : "--";
}

double NinjamVst3AudioProcessor::getUserChordCpuPercent(int userIndex) const
{
    if (userIndex < 0 || userIndex >= maxRemoteChordUsers)
        return 0.0;

    if (!isChordDetectionEnabled() || !isUserChordDetectionEnabled(userIndex))
        return 0.0;

    const auto& analyzer = remoteChordAnalyzers[(size_t)userIndex];
    return analyzer ? analyzer->getCpuPercent() : 0.0;
}

void NinjamVst3AudioProcessor::setChordDetectionEnabled(bool enabled)
{
    chordDetectionEnabled.store(enabled, std::memory_order_relaxed);

    if (!enabled)
    {
        if (localChordAnalyzer)
            localChordAnalyzer->markNoInput();

        for (auto& analyzer : remoteChordAnalyzers)
            if (analyzer && analyzer->isPrepared())
                analyzer->markNoInput();
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

    const auto& analyzer = remoteChordAnalyzers[(size_t)userIndex];
    if (analyzer && analyzer->isPrepared())
        analyzer->markNoInput();
}

bool NinjamVst3AudioProcessor::isUserChordDetectionEnabled(int userIndex) const
{
    if (userIndex < 0 || userIndex >= maxRemoteChordUsers)
        return false;

    return remoteChordDetectionEnabled[(size_t)userIndex].load(std::memory_order_relaxed);
}

int NinjamVst3AudioProcessor::getUserChordMemoryKb(int userIndex) const
{
    if (userIndex < 0 || userIndex >= maxRemoteChordUsers)
        return 0;

    const auto& analyzer = remoteChordAnalyzers[(size_t)userIndex];
    return analyzer ? analyzer->getMemoryKb() : 0;
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

    auto& analyzer = self->remoteChordAnalyzers[(size_t)useridx];
    if (analyzer == nullptr)
        return;

    if (!self->isChordDetectionEnabled() || !self->isUserChordDetectionEnabled(useridx))
    {
        analyzer->markNoInput();
        return;
    }

    analyzer->processInterleavedBlock(interleaved, numFrames, numChannels, sampleRate);
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
    {
        ninjamSideSignalServerSupported.store(true, std::memory_order_relaxed);
        const char* tgt = target.isNotEmpty() ? target.toRawUTF8() : "*";
        ninjamClient.ChatMessage_Send("SIDE_SIGNAL", tgt, type.toRawUTF8(), payload.toRawUTF8());
        return;
    }

    // Wrap in {"sig":type, "data":payload} so the receiver knows the type
    juce::DynamicObject::Ptr wrapper = new juce::DynamicObject();
    wrapper->setProperty("sig", type);
    wrapper->setProperty("data", payload);
    const juce::String msg = juce::JSON::toString(juce::var(wrapper.get()));
    // Custom FOURCC interval items stay hidden from normal chat clients.
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

int NinjamVst3AudioProcessor::getCodecMode() const
{
    const bool multiChanAuto = numLocalChannels.load() > 1 && opusSyncAvailable.load();
    if (!multiChanAuto)
        return 0;
    // multiChanAuto: always mixed mode (Vorbis ch0 + Opus ch1..N)
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
    beginStandaloneShutdown();
    localChordAnalyzer.reset();
    for (auto& analyzer : remoteChordAnalyzers)
        analyzer.reset();
    asyncChatTranslationWorker.reset();
    {
        const juce::ScopedLock launchLock(videoLaunchWorkerLock);
        if (videoLaunchFuture.valid())
            videoLaunchFuture.wait();
    }
    abletonLink.reset();
    if (serverLatencyProbeFuture.valid())
        serverLatencyProbeFuture.wait();
    JNL::close_socketlib();
}

void NinjamVst3AudioProcessor::beginStandaloneShutdown()
{
    stopTimer();
    if (samplePadBackgroundAlive)
        samplePadBackgroundAlive->store(false, std::memory_order_release);
    samplePadBackgroundPool.removeAllJobs(true, 4000);
    {
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
    samplePadLastTransportLength = 0;
    samplePadLastTransportBpi = 0;
    samplePadTransportInterval = 0;
    processingSampleRate = sampleRate > 1.0 ? sampleRate : 44100.0;
    if (linkTimingState != nullptr)
        linkTimingState->reset();
    if (localChordAnalyzer)
        localChordAnalyzer->prepare(processingSampleRate);
    for (auto& analyzer : remoteChordAnalyzers)
        if (analyzer && analyzer->isPrepared())
            analyzer->prepare(processingSampleRate);

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
    samplePadsOneShotRenderBuffer.setSize(2, juce::jmax(1, samplesPerBlock), false, true, true);
    samplePadsRenderBuffer.clear();
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

    const double detectedBpm = detectSampleBpmWithAubio(loaded, sourceRate);

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
        pad.lastSyncedTargetBpm = 0.0;
        pad.bpmSyncApplied = false;
        pad.bpmSyncEnabled.store(detectedBpm > 1.0, std::memory_order_relaxed);
        pad.playbackSpeed.store((int)SamplePadPlaybackSpeed::normal, std::memory_order_relaxed);
        pad.recordedLoop = false;
        pad.loopLengthBeats = 0;
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
                        pad.lastSyncedTargetBpm = 0.0;
                        pad.bpmSyncApplied = false;
                        pad.bpmSyncEnabled.store(prepared.detectedBpm > 1.0, std::memory_order_relaxed);
                        pad.playbackSpeed.store((int)SamplePadPlaybackSpeed::normal, std::memory_order_relaxed);
                        pad.recordedLoop = false;
                        pad.loopLengthBeats = 0;
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
    if (oneShotMode)
    {
        const bool reverse = pad.reverse.load(std::memory_order_relaxed);
        pad.playing.store(false, std::memory_order_relaxed);
        pad.playbackScheduled.store(false, std::memory_order_relaxed);
        pad.position.store(reverse ? juce::jmax(0.0, (double)length - 1.0) : 0.0, std::memory_order_relaxed);

        auto& voice = pad.oneShotVoices[(size_t)pad.nextOneShotVoice];
        voice.active = true;
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

    if (pad.recordedLoop && pad.loopLengthBeats > 0)
    {
        const int bpi = juce::jmax(1, getBPI());
        const double currentBeat = (double)intervalIndex.load(std::memory_order_relaxed) * (double)bpi
            + (double)juce::jlimit(0.0f, 1.0f, getIntervalProgress()) * (double)bpi;
        const double loopLengthBeats = (double)juce::jmax(1, pad.loopLengthBeats);
        if (pad.matchBpi.load(std::memory_order_relaxed))
        {
            double interval = std::floor(currentBeat / (double)bpi);
            double candidate = interval * (double)bpi + pad.recordedStartBeatInInterval;
            if (candidate <= currentBeat + 0.0001)
                candidate += (double)bpi;
            pad.scheduledStartBeat = candidate;
        }
        else
        {
            const double anchor = pad.loopAnchorBeat;
            double loops = std::ceil((currentBeat - anchor) / loopLengthBeats);
            if (!std::isfinite(loops) || loops < 0.0)
                loops = 0.0;
            double candidate = anchor + loops * loopLengthBeats;
            if (candidate <= currentBeat + 0.0001)
                candidate += loopLengthBeats;
            pad.scheduledStartBeat = candidate;
        }
        pad.playbackScheduled.store(true, std::memory_order_relaxed);
        return;
    }

    const bool reverse = pad.reverse.load(std::memory_order_relaxed);
    pad.position.store(reverse ? juce::jmax(0.0, (double)length - 1.0) : 0.0, std::memory_order_relaxed);
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
    const double candidate = nextSamplePadVisibleIntervalStartBeat(currentBeat, bpi);

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
    if (shouldEnable && isSamplePadsFeatureEnabled())
        resyncSamplePadToBpm(padIndex, (double)getBPM(), false);
}

bool NinjamVst3AudioProcessor::isSamplePadBpmSyncEnabled(int padIndex) const
{
    return isValidSamplePadIndex(padIndex)
        && samplePads[(size_t)padIndex].bpmSyncEnabled.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setSamplePadPlaybackSpeed(int padIndex, SamplePadPlaybackSpeed speed)
{
    if (!isValidSamplePadIndex(padIndex))
        return;

    speed = sanitizeSamplePadPlaybackSpeed((int)speed);
    {
        const juce::ScopedLock lock(samplePadsLock);
        auto& pad = samplePads[(size_t)padIndex];
        pad.playbackSpeed.store((int)speed, std::memory_order_relaxed);
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

            const double bpm = juce::jmax(1.0, (double)getBPM());
            const double twoBeatHoldMs = 2.0 * 60000.0 / bpm;
            const double heldMs = juce::Time::getMillisecondCounterHiRes() - pad.midiHoldStartMs;
            shouldArmNormalLooper = !pad.midiHoldActionTriggered && heldMs >= twoBeatHoldMs;
            shouldTrigger = !pad.midiHoldActionTriggered && !shouldArmNormalLooper;
            pad.midiPadDown = false;
            pad.midiHoldActive = false;
            pad.midiHoldActionTriggered = false;
            pad.midiHoldStartMs = 0.0;
        }
        if (shouldArmNormalLooper)
            armSamplePadLooper(padIndex, false);
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

    for (int pad = 0; pad < numSamplePads; ++pad)
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
                sourceBpm = detectSampleBpmWithAubio(original, sourceRate);

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

            const double ratio = sourceBpm / effectiveTargetBpm;
            const int targetSamples = juce::jmax(1, (int) std::llround((double) original.getNumSamples() * ratio));
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
        sourceBpm = detectSampleBpmWithAubio(original, sourceRate);
    }

    if (sourceBpm <= 1.0 || !std::isfinite(sourceBpm))
        return;

    if (effectiveTargetBpm <= 1.0 || !std::isfinite(effectiveTargetBpm))
        return;

    if (!isSamplePadsFeatureEnabled())
        return;

    const double ratio = sourceBpm / effectiveTargetBpm;
    const int targetSamples = juce::jmax(1, (int)std::llround((double)original.getNumSamples() * ratio));
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

void NinjamVst3AudioProcessor::updateSamplePadMidiHolds()
{
    if (!isSamplePadsFeatureEnabled())
        return;

    const int bpi = juce::jmax(1, getBPI());
    const double bpm = juce::jmax(1.0, (double)getBPM());
    const double fourBeatHoldMs = 4.0 * 60000.0 / bpm;
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const double currentBeat = (double)intervalIndex.load(std::memory_order_relaxed) * (double)bpi
        + (double)juce::jlimit(0.0f, 1.0f, getIntervalProgress()) * (double)bpi;
    const double candidate = nextSamplePadVisibleIntervalStartBeat(currentBeat, bpi);

    const juce::ScopedLock lock(samplePadsLock);
    for (auto& pad : samplePads)
    {
        if (!pad.midiHoldActive
            || pad.midiHoldActionTriggered
            || nowMs - pad.midiHoldStartMs < fourBeatHoldMs
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
        pad.recordScheduledStopBeat = 0.0;
        pad.recordLoopLengthBeatsOverride = bpi;
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

    const juce::ScopedLock lock(samplePadsLock);
    for (auto& pad : samplePads)
    {
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
                juce::AudioBuffer<float> loopBuffer(2, targetSamples);
                loopBuffer.clear();

                const int samplesToCopy = juce::jmin(pad.recordBuffer.getNumSamples(), targetSamples);
                loopBuffer.copyFrom(0, 0, pad.recordBuffer, 0, 0, samplesToCopy);
                loopBuffer.copyFrom(1, 0, pad.recordBuffer, 1, 0, samplesToCopy);

                pad.sample = std::move(loopBuffer);
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
                    pad.recordScheduledStartBeat = nextSamplePadVisibleIntervalStartBeat(blockStartBeat, safeBpi);
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

    if (samplePadsRenderBuffer.getNumChannels() < 2 || samplePadsRenderBuffer.getNumSamples() < numSamples)
        samplePadsRenderBuffer.setSize(2, numSamples, false, true, true);
    if (samplePadsOneShotRenderBuffer.getNumChannels() < 2 || samplePadsOneShotRenderBuffer.getNumSamples() < numSamples)
        samplePadsOneShotRenderBuffer.setSize(2, numSamples, false, true, true);
    samplePadsRenderBuffer.clear();
    samplePadsOneShotRenderBuffer.clear();
    for (int pad = 0; pad < numSamplePads; ++pad)
    {
        for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
        {
            auto& slotBuffer = samplePadPerPadFxSlotInputBuffers[(size_t)pad][(size_t)slot];
            if (slotBuffer.getNumChannels() < 2 || slotBuffer.getNumSamples() < numSamples)
                slotBuffer.setSize(2, numSamples, false, true, true);
            slotBuffer.clear();
        }
    }

    const double targetRate = processingSampleRate > 1.0 ? processingSampleRate : juce::jmax(1.0, getSampleRate());
    const double safeSamplesPerBeat = juce::jmax(1.0, samplesPerBeat);
    const double blockEndBeat = blockStartBeat + (double)numSamples / safeSamplesPerBeat;
    float* outL = samplePadsRenderBuffer.getWritePointer(0);
    float* outR = samplePadsRenderBuffer.getWritePointer(1);
    float* oneShotOutL = samplePadsOneShotRenderBuffer.getWritePointer(0);
    float* oneShotOutR = samplePadsOneShotRenderBuffer.getWritePointer(1);
    const bool duckActive = samplePadsDuckEnabled.load(std::memory_order_relaxed);
    const float* duckGains = nullptr;
    if (duckActive)
    {
        if (samplePadDuckGainBuffer.size() < (size_t)numSamples)
            samplePadDuckGainBuffer.resize((size_t)numSamples, 1.0f);

        const double bpm = juce::jmax(1.0, (double)getBPM());
        const auto duckShape = getSamplePadDuckShape();
        const auto duckLength = getSamplePadDuckLength();
        samplePadDuckOscillator.setFrequency((float)(bpm / 60.0), false);
        for (int i = 0; i < numSamples; ++i)
        {
            (void)samplePadDuckOscillator.processSample(0.0f);
            samplePadDuckGainBuffer[(size_t)i] =
                getSamplePadDuckGainForBeat(blockStartBeat + (double)i / safeSamplesPerBeat,
                                            duckShape,
                                            duckLength);
        }
        duckGains = samplePadDuckGainBuffer.data();
    }

    {
        const juce::ScopedLock lock(samplePadsLock);
        for (int padIndex = 0; padIndex < numSamplePads; ++padIndex)
        {
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
            const double step = juce::jmax(0.000001, pad.sourceSampleRate / targetRate);
            const bool duckThisPad = duckActive && pad.duckRoute.load(std::memory_order_relaxed);
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
                startSample = juce::jlimit(0, numSamples - 1,
                                           (int)std::llround((pad.scheduledStartBeat - blockStartBeat) * safeSamplesPerBeat));
                pad.playbackScheduled.store(false, std::memory_order_relaxed);
                pad.position.store(reverse ? juce::jmax(0.0, lengthD - 1.0) : 0.0, std::memory_order_relaxed);
                pad.playing.store(true, std::memory_order_relaxed);
                mainVoiceActive = true;
            }

            if (mainVoiceActive)
            {
                double pos = juce::jlimit(0.0, juce::jmax(0.0, lengthD - 1.0), pad.position.load(std::memory_order_relaxed));
                bool stillPlaying = true;

                for (int i = startSample; i < numSamples; ++i)
                {
                    const int index0 = juce::jlimit(0, length - 1, (int)std::floor(pos));
                    const int index1 = juce::jmin(index0 + 1, length - 1);
                    const float frac = (float)(pos - (double)index0);
                    const float gain = duckThisPad ? duckGains[i] : 1.0f;
                    const float sampleL = (srcL[index0] + (srcL[index1] - srcL[index0]) * frac) * gain;
                    const float sampleR = (srcR[index0] + (srcR[index1] - srcR[index0]) * frac) * gain;
                    outL[i] += sampleL;
                    outR[i] += sampleR;
                    if (hasRoutedFxSlot)
                    {
                        for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
                        {
                            if (!routedFxSlots[(size_t)slot])
                                continue;
                            auto& fxBuffer = samplePadPerPadFxSlotInputBuffers[(size_t)padIndex][(size_t)slot];
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

                for (int i = 0; i < numSamples; ++i)
                {
                    const int index0 = juce::jlimit(0, length - 1, (int)std::floor(pos));
                    const int index1 = juce::jmin(index0 + 1, length - 1);
                    const float frac = (float)(pos - (double)index0);
                    const float gain = duckThisPad ? duckGains[i] : 1.0f;
                    const float sampleL = (srcL[index0] + (srcL[index1] - srcL[index0]) * frac) * gain;
                    const float sampleR = (srcR[index0] + (srcR[index1] - srcR[index0]) * frac) * gain;
                    outL[i] += sampleL;
                    outR[i] += sampleR;
                    oneShotOutL[i] += sampleL;
                    oneShotOutR[i] += sampleR;
                    if (hasRoutedFxSlot)
                    {
                        for (int slot = 0; slot < numSamplePadFxSlots; ++slot)
                        {
                            if (!routedFxSlots[(size_t)slot])
                                continue;
                            auto& fxBuffer = samplePadPerPadFxSlotInputBuffers[(size_t)padIndex][(size_t)slot];
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
        for (int i = 0; i < numSamples; ++i)
        {
            data[i] *= volume;
            oneShotData[i] *= volume;
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

            peak = juce::jmax(peak, std::abs(v));
        }
    }

    samplePadsPeak.store(peak, std::memory_order_relaxed);
    return peak > 0.000001f;
}

void NinjamVst3AudioProcessor::applySamplePadInsertFx(int numSamples,
                                                      double,
                                                      double,
                                                      int)
{
    if (numSamples <= 0
        || samplePadsRenderBuffer.getNumChannels() < 2
        || samplePadsRenderBuffer.getNumSamples() < numSamples)
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

    auto addSlotDeltaToOutput = [this, numSamples](const juce::AudioBuffer<float>& slotBuffer)
    {
        float* outL = samplePadsRenderBuffer.getWritePointer(0);
        float* outR = samplePadsRenderBuffer.getWritePointer(1);
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

            auto& slotBuffer = samplePadPerPadFxSlotInputBuffers[(size_t)padIndex][(size_t)slot];
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
                    auto& filter = samplePadPerPadDjFilters[(size_t)padIndex][(size_t)slot];
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
                    auto& filter = samplePadPerPadDjFilters[(size_t)padIndex][(size_t)slot];
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
                    auto& filter = samplePadPerPadDjFilters[(size_t)padIndex][(size_t)slot];
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
                    auto& filter = samplePadPerPadDjBpFilters[(size_t)padIndex][(size_t)slot];
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
                    auto& phaser = samplePadPerPadPhasers[(size_t)padIndex][(size_t)slot];
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
                    auto& reverb = samplePadPerPadReverbs[(size_t)padIndex][(size_t)slot];
                    reverb.setParameters(params);
                    reverb.processMono(mono, numSamples);

                    const float gain = amount * 0.72f;
                    float* outL = samplePadsRenderBuffer.getWritePointer(0);
                    float* outR = samplePadsRenderBuffer.getWritePointer(1);
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
                    auto& delayBuffer = samplePadPerPadDelayBuffers[(size_t)padIndex][(size_t)slot];
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
                    float* outL = samplePadsRenderBuffer.getWritePointer(0);
                    float* outR = samplePadsRenderBuffer.getWritePointer(1);
                    int writePos = samplePadPerPadDelayWritePositions[(size_t)padIndex][(size_t)slot];

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

                    samplePadPerPadDelayWritePositions[(size_t)padIndex][(size_t)slot] = writePos;
                    break;
                }
            }
        }

        for (int targetSlot = 0; targetSlot < numSamplePadFxSlots; ++targetSlot)
        {
            if (!chainRoutes[(size_t)slot][(size_t)targetSlot])
                continue;

            auto& targetBuffer = samplePadPerPadFxSlotInputBuffers[(size_t)padIndex][(size_t)targetSlot];
            if (targetBuffer.getNumChannels() < 2 || targetBuffer.getNumSamples() < numSamples)
                continue;

            targetBuffer.addFrom(0, 0, slotBuffer, 0, 0, numSamples);
            targetBuffer.addFrom(1, 0, slotBuffer, 1, 0, numSamples);
            slotHasInput[(size_t)targetSlot] = true;
        }
    }
    }
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
    if (!mainIn.isDisabled()
        && mainIn != juce::AudioChannelSet::stereo()
        && mainIn != juce::AudioChannelSet::mono())
        return false;

    for (int i = 1; i < layouts.inputBuses.size(); ++i)
    {
        if (!layouts.inputBuses[i].isDisabled() && layouts.inputBuses[i] != juce::AudioChannelSet::stereo())
            return false;
    }

    for (int i = 1; i < layouts.outputBuses.size(); ++i)
    {
        if (!layouts.outputBuses[i].isDisabled() && layouts.outputBuses[i] != juce::AudioChannelSet::stereo())
            return false;
    }

    return true;
}

int NinjamVst3AudioProcessor::LicenseAgreementCallback(void* userData, const char* licensetext)
{
    // Auto-accept license for now (or log it)
    // Ideally, show a dialog to the user
    // Since this is called from Run(), which we call from timerCallback (UI thread),
    // we can show a message box.
    // However, for automation/testing, we might want to auto-accept.

    // Simple auto-accept for this proof of concept:
    return 1;
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
            captureQueueMs = obj->getProperty("captureQueueMs").toString().getDoubleValue();
            encodeMs = obj->getProperty("encodeMs").toString().getDoubleValue();
        }

        if (appFamily.isNotEmpty() && appFamily != opusSyncAppFamily)
            return;

        const juce::String senderName = payloadUserId.isNotEmpty() ? payloadUserId : sender;
        const juce::String localUserKey = normaliseOpusPeerId(currentUser);
        if (senderName.isEmpty()
            || normaliseOpusPeerId(senderName) == localUserKey
            || channelIndex < 0)
            return;

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
        const juce::String canonicalSenderKey = canonicalDelayUserKey(senderKey);
        if (canonicalSenderKey.isNotEmpty())
            remoteServerRouteLatencyMsByUser[canonicalSenderKey] = smoothedRouteMs;
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
            previousBpm = obj->getProperty("previousBpm").toString().getDoubleValue();
            newBpm = obj->getProperty("bpm").toString().getDoubleValue();
            bpi = (int)obj->getProperty("bpi");
            timingDelayDeltaMs = (int)obj->getProperty("timingDelayDeltaMs");
        }

        if (appFamily.isNotEmpty() && appFamily != opusSyncAppFamily)
            return;

        const juce::String senderKey = normaliseOpusPeerId(payloadUserId.isNotEmpty() ? payloadUserId : sender);
        const juce::String localUserKey = normaliseOpusPeerId(currentUser);
        if (senderKey.isEmpty() || senderKey == localUserKey || !consumeVideoTimingChangeEvent(eventId))
            return;

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
                const bool bpiMatches = (remoteBpi <= 0 || remoteBpi == localBpi);
                if (!bpiMatches)
                    return;
                const int remoteMarkerBeat = getIntervalSyncMarkerBeatForBeat(remoteBeat, localBpi);
                const int remoteSourceInterval = remoteIntervalAbsolute >= 0 ? remoteIntervalAbsolute : remoteInterval;
                const long long remoteMarkerKey = makeIntervalSyncMarkerKey(remoteSourceInterval, remoteMarkerBeat);
                bool shouldStorePending = false;
                const juce::String displaySender = sender.isNotEmpty() ? sender : (payloadUserId.isNotEmpty() ? payloadUserId : senderKey);
                const long long receivedSampleCount = intervalSyncSampleCounter.load(std::memory_order_relaxed);
                const double receivedAtMs = juce::Time::getMillisecondCounterHiRes();
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
                        lastAnnouncedRemoteIntervalByUser[senderKey] = remoteMarkerKey;
                        shouldStorePending = true;
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
                    pending.receivedAtMs = receivedAtMs;
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
                    peer.appFamily = appFamily;
                    peer.handshakeVersion = handshakeVersion;
                    peer.runtimeFormat = runtimeFormat;
                    peer.pluginVersion = pluginVersion;
                    peer.lastSeenMs = juce::Time::getMillisecondCounterHiRes();
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
                juce::ScopedLock lock(self->chatLock);
                self->chatHistory.add(recognizedMessage);
                self->chatSenders.add("");
                self->chatRevision.fetch_add(1);
                if (self->chatHistory.size() > 100)
                {
                    self->chatHistory.removeRange(0, self->chatHistory.size() - 100);
                    self->chatSenders.removeRange(0, juce::jmax(0, self->chatSenders.size() - 100));
                }
            }
        }
        return true;
    };
    auto processInboundSideSignal = [self, &processOpusSyncSupport](const juce::String& sender, const juce::String& type, const juce::String& payload, juce::String* outEventId) -> bool
    {
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
            juce::ScopedLock lock(self->chatLock);
            self->chatHistory.add(stored);
            self->chatSenders.add(lineSender);
            self->chatRevision.fetch_add(1);
            if (self->chatHistory.size() > 100)
            {
                self->chatHistory.removeRange(0, self->chatHistory.size() - 100);
                self->chatSenders.removeRange(0, juce::jmax(0, self->chatSenders.size() - 100));
            }
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
    if (samplePadsEnabledAtBlock)
        updateSamplePadMidiHolds();
    else
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

    std::vector<float> blockLinkAudioSamples;
    int blockLinkAudioNumChannels = 0;
    int blockLinkAudioNumFrames = 0;
    if (isLinkAudioEnabled() && isLinkAudioReceiveEnabled())
    {
        blockLinkAudioSamples.assign((size_t)numSamples * 2u, 0.0f);
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
            blockLinkAudioSamples.clear();
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

    int requestedLocal = numLocalChannels.load();
    int actualLocal = juce::jlimit(1, maxLocalChannels, requestedLocal);
    actualLocal = totalAvailableInputChannels > 0 ? juce::jmin(actualLocal, totalAvailableInputChannels) : 0;
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

    bool samplePadsNeedLocalSlot = false;
    if (samplePadsEnabledAtBlock)
    {
        const juce::ScopedLock lock(samplePadsLock);
        for (const auto& pad : samplePads)
        {
            if (pad.sample.getNumSamples() > 0
                || pad.recordArmed.load(std::memory_order_relaxed)
                || pad.recording.load(std::memory_order_relaxed)
                || pad.recordPendingStart.load(std::memory_order_relaxed)
                || pad.playbackScheduled.load(std::memory_order_relaxed)
                || pad.playing.load(std::memory_order_relaxed))
            {
                samplePadsNeedLocalSlot = true;
                break;
            }
        }
    }

    if (samplePadsNeedLocalSlot && actualLocal <= 0)
        actualLocal = 1;

    bool samplePadsActiveThisBlock = false;
    bool fedLocalChordAnalyzer = false;
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

        if (ch == 0 && localChordAnalyzer && isChordDetectionEnabled())
        {
            localChordAnalyzer->processBlock(localChannelBuffer.getReadPointer(ch), numSamples);
            fedLocalChordAnalyzer = true;
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

    if (localChordAnalyzer && !fedLocalChordAnalyzer)
        localChordAnalyzer->markNoInput();

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
    // - multiChanAuto: >1 local channels + VST3 peers → Vorbis mix on ch0, Opus per-ch on ch1..N
    // - otherwise:     Vorbis only, single channel (mix folded into ch0 above)
    const bool multiChanAuto = numLocalChannels.load() > 1 && opusSyncAvailable.load() && isTransmittingLocal();

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
        if (localMixBuffer.getNumSamples() < numSamples)
            localMixBuffer.setSize(1, numSamples, false, true, true);
        float* mix = localMixBuffer.getWritePointer(0);
        const float* src0 = localChannelBuffer.getReadPointer(0);
        for (int s = 0; s < numSamples; ++s)
            mix[s] = src0[s];
        for (int ch = 1; ch < actualLocal; ++ch)
        {
            const float* src = localChannelBuffer.getReadPointer(ch);
            for (int s = 0; s < numSamples; ++s)
                mix[s] += src[s];
        }
        if (fxSendActive)
            localMixBuffer.addFrom(0, 0, fxTransmitBuffer, 0, 0, numSamples);
    }

    float* inputs[32] = {};
    int actualInputChannels;
    if (multiChanAuto)
    {
        // syncLocalIntervalChannelConfig advertises this slot as the legacy Vorbis mixdown.
        const int n = juce::jlimit(1, maxLocalChannels, numLocalChannels.load());
        for (int i = 0; i < n; ++i)
            inputs[i] = localChannelBuffer.getWritePointer(i);
        inputs[n] = localMixBuffer.getWritePointer(0);
        inputs[n + 1] = fxTransmitBuffer.getWritePointer(0);
        actualInputChannels = n + 2;
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

    // Feed local input to engine only for transmit. Monitoring is handled below
    // with explicit per-channel routing so stereo doesn't collapse when transmit toggles.
    const bool allowEngineLocalInput = transmitEnabled;
    float** engineInputs = allowEngineLocalInput ? inputs : nullptr;
    int engineInputChannels = allowEngineLocalInput ? actualInputChannels : 0;
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
    ninjamClient.AudioProc(engineInputs, engineInputChannels, outputs, actualOutputChannels, numSamples, (int)getSampleRate(), runMonitorOnly);

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

    if (monitorEnabled || transmitEnabled)
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

    if ((monitorEnabled || transmitEnabled) && samplePadsActiveThisBlock && numOutputBusesOut > 0)
    {
        auto mainBus = getBusBuffer(buffer, false, 0);
        const int outChans = mainBus.getNumChannels();
        const float sampleMonitorGain = localChannelGains[0].load(std::memory_order_relaxed);
        if (outChans >= 2)
        {
            mainBus.addFrom(0, 0, samplePadsRenderBuffer, 0, 0, numSamples, sampleMonitorGain);
            mainBus.addFrom(1, 0, samplePadsRenderBuffer, 1, 0, numSamples, sampleMonitorGain);
        }
        else if (outChans == 1)
        {
            const float* padL = samplePadsRenderBuffer.getReadPointer(0);
            const float* padR = samplePadsRenderBuffer.getReadPointer(1);
            float* out = mainBus.getWritePointer(0);
            for (int i = 0; i < numSamples; ++i)
                out[i] += sampleMonitorGain * 0.5f * (padL[i] + padR[i]);
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

    const auto codec = getNinjamZapVideoCodec(fourcc);
    const juce::String sender = username != nullptr ? juce::String::fromUTF8(username) : juce::String();
    const juce::String streamKey = sender + ":" + juce::String(chidx);
    const juce::String reassemblyKey = streamKey + ":" + guidToHexString(guid);
    const double receivedMs = juce::Time::getMillisecondCounterHiRes();
    std::vector<juce::MemoryBlock> chunks;

    if (reassemblyKey.isNotEmpty())
    {
        const juce::ScopedLock lock(self->ninjamZapVideoChunkLock);
        auto& reassembler = self->ninjamZapVideoChunkReassemblers[reassemblyKey];
        if (dataLen > 0 && data != nullptr)
            chunks = reassembler.pushBytes(data, static_cast<size_t>(dataLen));

    }

    if (!self->ninjamZapVideoReceivedNotice.exchange(true, std::memory_order_relaxed))
    {
        juce::String message = "Receiving NINJAMZap video transport";
        if (sender.isNotEmpty())
            message << " from " << sender;
        message << " on channel " << (chidx + 1)
                << " as " << ninjamplus::zap::getCodecName(codec)
                << " (" << dataLen << " bytes first fragment";
        if (!chunks.empty())
            message << ", " << (int)chunks.size() << " complete Zap chunk" << (chunks.size() == 1 ? "" : "s");
        message << ").";
        message << " Browser decode path active.";
        self->addSystemChatLine(message);
    }

    if (!chunks.empty())
    {
        for (const auto& chunk : chunks)
        {
            // Sync markers may be sent as zero-length payloads wrapped by the
            // chunk framing; detect and log them but otherwise ignore.
            ninjamplus::zap::SyncMarker marker;
            bool markerAlreadySeen = false;
            {
                const juce::ScopedLock lock(self->ninjamZapVideoChunkLock);
                auto markerSeenIt = self->ninjamZapVideoMarkerSeenByReassemblyKey.find(reassemblyKey);
                markerAlreadySeen = markerSeenIt != self->ninjamZapVideoMarkerSeenByReassemblyKey.end()
                    && markerSeenIt->second;
            }

            if (!markerAlreadySeen
                && ninjamplus::zap::parseSyncMarkerPayload(chunk.getData(), chunk.getSize(), marker))
            {
                const juce::String audioGuidHex = guidToHexString(marker.audioGuid.data());
                {
                    const juce::ScopedLock lock(self->ninjamZapVideoChunkLock);
                    self->ninjamZapVideoAudioGuidByReassemblyKey[reassemblyKey] = audioGuidHex;
                    self->ninjamZapVideoMarkerIntervalByReassemblyKey[reassemblyKey] = (int)marker.intervalCounter;
                    self->ninjamZapVideoMarkerSeenByReassemblyKey[reassemblyKey] = true;
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
                    const juce::ScopedLock lock(self->ninjamZapVideoChunkLock);
                    auto guidIt = self->ninjamZapVideoAudioGuidByReassemblyKey.find(reassemblyKey);
                    if (guidIt != self->ninjamZapVideoAudioGuidByReassemblyKey.end())
                        audioGuidHex = guidIt->second;
                    auto intervalIt = self->ninjamZapVideoMarkerIntervalByReassemblyKey.find(reassemblyKey);
                    if (intervalIt != self->ninjamZapVideoMarkerIntervalByReassemblyKey.end())
                        markerInterval = intervalIt->second;
                }

                NinjamVst3AudioProcessor::ZapVideoDecodeJob job;
                job.streamKey = streamKey;
                job.sender = sender;
                job.audioGuidHex = audioGuidHex;
                job.markerInterval = markerInterval;
                job.channelIndex = chidx;
                job.codec = codec;
                job.payload.append(chunk.getData(), chunk.getSize());
                job.receivedMs = receivedMs;
                job.queuedMs = juce::Time::getMillisecondCounterHiRes();
                {
                    const juce::ScopedLock lock(self->zapVideoFrameLock);
                    auto timingIt = self->zapVideoSenderTimingByStream.find(streamKey);
                    if (timingIt != self->zapVideoSenderTimingByStream.end()
                        && job.queuedMs - timingIt->second.updatedMs <= 5000.0)
                    {
                        job.senderCaptureQueueMs = timingIt->second.captureQueueMs;
                        job.senderEncodeMs = timingIt->second.encodeMs;
                    }
                }
                self->publishBrowserDecodedZapVideoFrame(job);
            }
        }
    }

    if ((flags & 1) != 0 && reassemblyKey.isNotEmpty())
    {
        const juce::ScopedLock lock(self->ninjamZapVideoChunkLock);
        self->ninjamZapVideoChunkReassemblers.erase(reassemblyKey);
        self->ninjamZapVideoAudioGuidByReassemblyKey.erase(reassemblyKey);
        self->ninjamZapVideoMarkerIntervalByReassemblyKey.erase(reassemblyKey);
        self->ninjamZapVideoMarkerSeenByReassemblyKey.erase(reassemblyKey);
    }
}

void NinjamVst3AudioProcessor::IntervalMediaItem_Callback(void* userData, NJClient* /*inst*/,
    const char* username, int /*chidx*/, unsigned int fourcc,
    const unsigned char* /*guid*/, const void* data, int dataLen)
{
    if (!username || !data || dataLen <= 0) return;
    if (isNinjamZapVideoFourcc(fourcc)) return;
    if (fourcc == kSyncSignalFourcc)
    {
        auto* self = static_cast<NinjamVst3AudioProcessor*>(userData);
        const juce::String sender = juce::String::fromUTF8(username);
        const juce::String msg    = juce::String::fromUTF8(static_cast<const char*>(data), dataLen);
        const juce::var parsed    = juce::JSON::parse(msg);
        if (auto* obj = parsed.getDynamicObject())
        {
            const juce::String type    = obj->getProperty("sig").toString();
            const juce::String payload = obj->getProperty("data").toString();
            if (type.isNotEmpty() && payload.isNotEmpty())
                self->processSyncSignal(sender, type, payload);
        }
        return;
    }
    if (fourcc != kOpusSyncFourcc) return;
    auto* self = static_cast<NinjamVst3AudioProcessor*>(userData);
    const juce::String sender = juce::String::fromUTF8(username);
    const juce::String payload = juce::String::fromUTF8(static_cast<const char*>(data), dataLen);

    juce::var parsed = juce::JSON::parse(payload);
    bool supportsOpus = false;
    bool multiChanEnabled = false;
    int peerNumChannels = 1;
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
        juce::String payloadUserId = obj->getProperty("userId").toString();
        if (payloadUserId.isNotEmpty())
            userId = normaliseOpusPeerId(payloadUserId);
        clientId = obj->getProperty("clientId").toString().trim();
        appFamily = obj->getProperty("appFamily").toString().trim();
        handshakeVersion = (int)obj->getProperty("handshakeVersion");
        runtimeFormat = obj->getProperty("runtimeFormat").toString().trim();
        pluginVersion = obj->getProperty("pluginVersion").toString().trim();
    }
    else { return; }

    const bool isLocalClient = clientId.isNotEmpty() ? (clientId == self->opusSyncInstanceId)
                                                      : (userId == normaliseOpusPeerId(self->currentUser));
    const bool sameAppFamily = appFamily.isEmpty() || appFamily == opusSyncAppFamily;
    const bool compatibleHandshake = handshakeVersion <= 0 || handshakeVersion == opusSyncHandshakeVersion;
    const juce::String peerKey = clientId.isNotEmpty() ? clientId : userId;
    if (peerKey.isEmpty() || userId.isEmpty() || isLocalClient) return;

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
            self->opusSyncPeers.erase(peerKey);
    }
    if (recognizedNow)
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
    state.setProperty("transmitLocal", isTransmittingLocal(), nullptr);
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
    setTransmitLocal((bool)state.getProperty("transmitLocal", false));
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
}

void NinjamVst3AudioProcessor::processPendingIntervalSyncMarkers(int localMarkerBeat, long long localMarkerSampleCount, double intervalDurationMs)
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
        return;

    const int safeLocalMarkerBeat = juce::jmax(0, localMarkerBeat);
    const double safeIntervalDurationMs = juce::jmax(1.0, intervalDurationMs);
    const double localMarkerAtMs = juce::Time::getMillisecondCounterHiRes();

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
        const int receiverServerLatencyMs = juce::jmax(0, localServerLatencyMs.load());
        const int serverRouteLatencyMs = pending.serverRouteLatencyMs >= 0
            ? juce::jmax(0, pending.serverRouteLatencyMs)
            : senderServerLatencyMs + receiverServerLatencyMs;
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
                    includeInAverage = false;
            }
            if (includeInAverage)
            {
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
                correctedDelayMs = juce::jmax(0, (int)std::llround(rawDelayMs) + serverRouteLatencyMs);
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
            bool shouldApply = (appliedIt == remoteLatencyLastAppliedIntervalByUser.end());
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
            }
        }
    }
}

void NinjamVst3AudioProcessor::timerCallback()
{
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
    double stepStartMs = juce::Time::getMillisecondCounterHiRes();
    {
        const juce::ScopedLock clientLock(ninjamClientLock);
        while (!ninjamClient.Run() && loopCount < 50)
            loopCount++;
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
    if (serverSupportsSideSignal && !ninjamSideSignalVideoCapSent.load(std::memory_order_relaxed))
    {
        const juce::ScopedLock clientLock(ninjamClientLock);
        if (ninjamClient.GetStatus() == NJClient::NJC_STATUS_OK)
        {
            ninjamClient.ChatMessage_Send("VIDEO_CAP", "1", nullptr, nullptr, nullptr);
            ninjamSideSignalVideoCapSent.store(true, std::memory_order_relaxed);
        }
    }
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const bool vdoSyncActive = vdoVideoSyncEnabled.load(std::memory_order_relaxed)
        && !ninjamZapVideoEnabled.load(std::memory_order_relaxed);
    if (status == NJClient::NJC_STATUS_OK && (nowMs - lastRemoteSyncUserPruneMs) >= 350.0)
    {
        pruneDisconnectedRemoteSyncState();
        lastRemoteSyncUserPruneMs = nowMs;
    }

    if (status != lastStatus)
    {
        if (status == NJClient::NJC_STATUS_CANTCONNECT || status == NJClient::NJC_STATUS_INVALIDAUTH)
        {
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
                addSystemChatLine("Username retry failed after 3 attempts; disconnected.");
                {
                    const juce::ScopedLock clientLock(ninjamClientLock);
                    ninjamClient.Disconnect();
                }
                currentServer = {};
                currentUser = {};
                refreshAbletonLinkActivation();
                status = NJClient::NJC_STATUS_DISCONNECTED;
            }
        }
        else if (status == NJClient::NJC_STATUS_OK)
        {
            duplicateNameRetryEnabled = false;
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
            localServerLatencyMs.store(-1);
            lastServerLatencyProbeInterval.store(-1);
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
        else if (lastStatus == NJClient::NJC_STATUS_OK
                 && (status == NJClient::NJC_STATUS_DISCONNECTED
                     || status == NJClient::NJC_STATUS_CANTCONNECT
                     || status == NJClient::NJC_STATUS_INVALIDAUTH))
        {
            stopNinjamZapVideoTransportForDisconnect();
            ninjamZapServerVideoSupported.store(false, std::memory_order_relaxed);
            ninjamSideSignalServerSupported.store(false, std::memory_order_relaxed);
            ninjamSideSignalVideoCapSent.store(false, std::memory_order_relaxed);
            opusSyncServerSupported.store(false);
            {
                const juce::ScopedLock lock(opusSyncPeerLock);
                opusSyncPeers.clear();
            }
            {
                invalidateIntervalSyncLatencyState(false);
            }
            opusSyncAvailable.store(false);
            opusSyncHasLegacyClients.store(false);
            localServerLatencyMs.store(-1);
            lastServerLatencyProbeInterval.store(-1);
            lastIntervalSyncFallbackSubscriptionMs = 0.0;
            setIntervalSyncStatusText({});
            lastBroadcastIntervalTag.store(-1);
            resetIntervalSyncTimingCache();
            applyCodecPreference();
        }
        lastStatus = status;
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
            lastIntervalSyncFallbackSubscriptionMs = nowMs;
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

        if (status == NJClient::NJC_STATUS_OK)
            lastServerLatencyProbeInterval.store(getDisplayIntervalIndex());
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
