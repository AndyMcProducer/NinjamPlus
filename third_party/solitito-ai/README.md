---
license: mit
tags:
- audio
- audio-classification
- chord-recognition
- guitar
- onnx
- rust
library_name: ort
---

# Solitito Guitar Chord Model (v0.1.0)

This is a lightweight, real-time guitar chord recognition model optimized for CPU inference. It serves as the core engine for **Solitito** - an open-source Guitar Chord Trainer & Arpeggio Practice tool written in Rust.

- **Repository:** [https://github.com/greblus/solitito](https://github.com/greblus/solitito)
- **Format:** ONNX
- **License:** MIT

## Model Description

The model is a Transformer/CNN hybrid trained to recognize jazz guitar chords from a raw audio signal converted into a specific feature set. It is designed to run efficiently on consumer hardware (laptops) without needing a dedicated GPU.

### Classes
The model recognizes:
- **Roots:** C, C#, D, D#, E, F, F#, G, G#, A, A#, B
- **Qualities:** Major, Minor, 7, Maj7, m7, dim7, m7b5, 9, 13 (and "Note" for single notes).

## Usage (Preprocessing Required!)

**Important:** This model does **not** accept raw audio waveforms. You must preprocess the audio exactly as defined in `dsp_weights.json`.

### Input Specification
- **Input Shape:** `[Batch, 32, 156]`
  - `32`: Context frames (approx. 0.5s of audio history).
  - `156`: Feature vector size per frame.

### DSP Pipeline (Rust/Python)
1.  **Sample Rate:** Resample audio to **16,000 Hz**.
2.  **FFT:** Size 8192, Hop Length 256.
3.  **CQT (Constant-Q Transform):** 144 bins (6 octaves, 24 bins/octave).
4.  **Chroma:** 12 bins derived from CQT.
5.  **Normalization:** Logarithmic (dB) normalization + scaling to 0.0-1.0 range.
6.  **Stack:** The input vector is `[CQT (144) + Chroma (12)]` = 156 float32 values.

*Note: The `dsp_weights.json` file included in this repository contains the pre-calculated complex weights for the CQT matrix multiplication, allowing for fast Rust implementation.*

## Training Data

The model was trained on a combination of custom datasets and the **GuitarSet** dataset.
*   *Xi, Q., Bittner, R. M., Ye, X., & Bello, J. P. (2018). GuitarSet: A Dataset for Guitar Transcription. In ISMIR.*

## How to use in Rust

See [https://github.com/greblus/solitito](https://github.com/greblus/solitito) for a full implementation.