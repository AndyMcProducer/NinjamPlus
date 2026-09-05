// Run in each real VDO iframe's MAIN execution context before selecting a camera.
// This replaces only media acquisition; signaling, encoding, transport, buffering,
// and rendering are the unmodified VDO player. Closing the test tab removes it.
(() => {
    const canvas = document.createElement("canvas");
    canvas.width = 1280;
    canvas.height = 720;
    const ctx = canvas.getContext("2d");
    const label = new URLSearchParams(location.search).get("label");
    function draw() {
        const now = Date.now();
        ctx.fillStyle = label === "alpha" ? "#234e85" : "#853423";
        ctx.fillRect(0, 0, 1280, 720);
        ctx.fillStyle = "white";
        ctx.font = "50px monospace";
        ctx.fillText(label + " " + now, 50, 220);
        for (let bit = 0; bit < 40; bit++) {
            const value = bit < 8 ? ((0xa5 >>> (7 - bit)) & 1) : ((now >>> (39 - bit)) & 1);
            ctx.fillStyle = value ? "white" : "black";
            ctx.fillRect(bit * 32, 0, 32, 100);
        }
    }
    draw();
    const timer = setInterval(draw, 33);
    const stream = canvas.captureStream(30);
    const originalGet = navigator.mediaDevices.getUserMedia.bind(navigator.mediaDevices);
    const originalEnum = navigator.mediaDevices.enumerateDevices.bind(navigator.mediaDevices);
    navigator.mediaDevices.enumerateDevices = async () => [
        { deviceId: "sync-probe", groupId: "sync-probe", kind: "videoinput", label: "Generated sync clock" },
        { deviceId: "sync-silent", groupId: "sync-probe", kind: "audioinput", label: "Generated silence" },
    ];
    navigator.mediaDevices.getUserMedia = async (constraints) => {
        const result = new MediaStream();
        if (constraints.video) result.addTrack(stream.getVideoTracks()[0].clone());
        if (constraints.audio) {
            const ac = new AudioContext();
            const dest = ac.createMediaStreamDestination();
            const oscillator = ac.createOscillator();
            const gain = ac.createGain();
            gain.gain.value = 0;
            oscillator.connect(gain).connect(dest);
            oscillator.start();
            result.addTrack(dest.stream.getAudioTracks()[0]);
        }
        return result;
    };
    const probe = window.__njLiveProbe = { canvas, stream, timer, originalGet, originalEnum, started: Date.now(), samples: [] };
    const scratch = document.createElement("canvas");
    scratch.width = 1280;
    scratch.height = 720;
    const reader = scratch.getContext("2d", { willReadFrequently: true });
    probe.sampleTimer = setInterval(() => {
        for (const el of document.querySelectorAll("video,canvas")) {
            if (!el.id.startsWith("videosource_") || !(el.videoWidth || el.width)) continue;
            try {
                reader.drawImage(el, 0, 0, 1280, 720);
                let header = 0, time = 0;
                for (let bit = 0; bit < 40; bit++) {
                    const value = reader.getImageData(bit * 32 + 16, 50, 1, 1).data[0] > 128 ? 1 : 0;
                    if (bit < 8) header = (header << 1) | value;
                    else time = ((time << 1) | value) >>> 0;
                }
                if (header === 0xa5) {
                    const now = Date.now();
                    probe.samples.push({ now, id: el.id, tag: el.tagName, delay: ((now >>> 0) - time) >>> 0 });
                }
            } catch (_) { /* No decoded frame yet. */ }
        }
    }, 200);
})();
