// Use only with --reference-public: matches the native test client's wall-clock
// tone every five seconds. Generates a camera; never reads real camera/mic input.
(() => {
    const canvas = document.createElement('canvas');
    canvas.width = 1280; canvas.height = 720;
    const context = canvas.getContext('2d');
    function draw() {
        const now = Date.now(), event = Math.floor(now / 5000), phase = now % 5000;
        context.fillStyle = phase < 100 ? 'white' : '#112638';
        context.fillRect(0, 0, 1280, 720);
        context.fillStyle = phase < 100 ? 'black' : 'white';
        context.font = '48px monospace';
        context.fillText('STEVE REFERENCE — NINJAM AUDIO + VDO VIDEO', 30, 100);
        context.font = '80px monospace';
        context.fillText('EVENT ' + event, 40, 250);
        context.fillText(new Date(now).toISOString().slice(11, 23), 40, 380);
        context.font = '36px monospace';
        context.fillText('WHITE FLASH SHOULD MATCH THE SHORT NINJAM BEEP', 30, 520);
        context.fillText('Compare this feed directly, not the returned screen share', 30, 600);
    }
    draw();
    const timer = setInterval(draw, 16);
    const stream = canvas.captureStream(30);
    navigator.mediaDevices.enumerateDevices = async () => [{deviceId:'reference',groupId:'reference',kind:'videoinput',label:'Generated paired reference'}];
    navigator.mediaDevices.getUserMedia = async constraints => {
        const output = new MediaStream();
        if (constraints.video) output.addTrack(stream.getVideoTracks()[0].clone());
        return output;
    };
    window.__njReference = {canvas, stream, timer};
})();
