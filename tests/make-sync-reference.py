"""Create a 30-second numbered flash/beep clip for REAPER sync diagnosis.

Requires Pillow and an ffmpeg executable (--ffmpeg). Output MOV uses PCM audio
to avoid lossy audio encoder priming; WAV and event JSON are also provided.
"""
import argparse
import json
import math
from pathlib import Path
import struct
import subprocess
import wave
from PIL import Image, ImageDraw, ImageFont

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--ffmpeg', required=True)
parser.add_argument('--output', type=Path, default=Path('test-results/sync-reference'))
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
rate, fps, duration = 48000, 30, 30
events = [2, 7, 13, 20, 28]
pcm = bytearray(rate * duration * 2)
for index, start in enumerate(events):
    frequency = [880, 1100, 1320, 990, 1540][index]
    for sample in range(rate // 10):
        envelope = min(1, sample / 240, (rate // 10 - 1 - sample) / 240)
        value = int(6500 * envelope * math.sin(2 * math.pi * frequency * sample / rate))
        struct.pack_into('<h', pcm, (start * rate + sample) * 2, value)
wav_path = args.output / 'sync-reference.wav'
with wave.open(str(wav_path), 'wb') as wav:
    wav.setnchannels(1)
    wav.setsampwidth(2)
    wav.setframerate(rate)
    wav.writeframes(pcm)
font_path = Path('C:/Windows/Fonts/consolab.ttf')
large = ImageFont.truetype(str(font_path), 96)
small = ImageFont.truetype(str(font_path), 30)
movie_path = args.output / 'sync-reference.mov'
command = [args.ffmpeg, '-hide_banner', '-loglevel', 'error', '-y', '-f', 'rawvideo',
           '-pix_fmt', 'rgb24', '-s', '960x540', '-r', str(fps), '-i', 'pipe:0', '-i', str(wav_path),
           '-c:v', 'libx264', '-preset', 'fast', '-crf', '18', '-pix_fmt', 'yuv420p',
           '-c:a', 'pcm_s16le', '-movflags', '+faststart', str(movie_path)]
process = subprocess.Popen(command, stdin=subprocess.PIPE)
for frame in range(duration * fps):
    active = next((index for index, start in enumerate(events) if start * fps <= frame < start * fps + 3), None)
    canvas = Image.new('RGB', (960, 540), 'white' if active is not None else '#132133')
    draw = ImageDraw.Draw(canvas)
    color = 'black' if active is not None else 'white'
    draw.text((45, 40), 'NINJAM / VDO SYNC REFERENCE', font=small, fill=color)
    draw.text((45, 130), f'{frame // fps:02d}:{frame % fps:02d}', font=large, fill=color)
    draw.text((45, 255), f'FRAME {frame:04d}   30 FPS', font=small, fill=color)
    draw.text((45, 345), f'EVENT {active + 1}' if active is not None else 'WAIT FOR WHITE FLASH + BEEP', font=small, fill=color)
    draw.text((45, 455), 'Events at 2, 7, 13, 20, 28 seconds', font=small, fill=color)
    process.stdin.write(canvas.tobytes())
process.stdin.close()
if process.wait() != 0:
    raise RuntimeError('ffmpeg failed')
(args.output / 'events.json').write_text(json.dumps({'fps': fps, 'sampleRate': rate, 'durationSeconds': duration,
    'events': [{'number': i + 1, 'seconds': t, 'videoFrame': t * fps, 'audioSample': t * rate}
               for i, t in enumerate(events)]}, indent=2))
print(movie_path.resolve())
