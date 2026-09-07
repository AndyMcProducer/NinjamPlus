// Track the age of the local preview as well as the receiving side. A source
// capture stall must not be misclassified as a receiver buffer or network fault.
(() => {
    if (window.__njSourceObserver) return;
    const timestampNow = Date.now.bind(Date);
    const state = window.__njSourceObserver = { samples: [] };
    const canvas = document.createElement('canvas');
    canvas.width = 1280; canvas.height = 720;
    const ctx = canvas.getContext('2d', { willReadFrequently: true });
    state.timer = setInterval(() => {
        const video = document.getElementById('videosource');
        const row = { at: timestampNow(), visibility: document.visibilityState };
        if (video && video.videoWidth) {
            row.currentTime = video.currentTime;
            row.paused = video.paused;
            try {
                ctx.drawImage(video, 0, 0, 1280, 720);
                let header = 0, timestamp = 0;
                for (let bit = 0; bit < 40; bit++) {
                    const value = ctx.getImageData(bit * 32 + 16, 50, 1, 1).data[0] > 128 ? 1 : 0;
                    if (bit < 8) header = (header << 1) | value;
                    else timestamp = ((timestamp << 1) | value) >>> 0;
                }
                if (header === 0xa5) row.captureAgeMs = ((row.at >>> 0) - timestamp) >>> 0;
            } catch (error) { row.error = String(error); }
        }
        state.samples.push(row);
        if (state.samples.length > 6000) state.samples.splice(0, 1000);
    }, 200);
})();
