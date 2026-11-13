# Writes the default button sounds (Project Settings > User Interface) as 16-bit mono 44.1 kHz WAVs:
#   SFX_UI_Hover.wav  a soft 2400 Hz tick, 25 ms (exponential decay)
#   SFX_UI_Click.wav  a 1300 -> 800 Hz blip, 60 ms
# The same formula as ui_sound_wav in editor/templates.cpp (templates write them into Content/Templates/UI).
# Usage: python tools/ui_sounds.py <folder>
import math
import struct
import sys
from pathlib import Path


def ui_sound_wav(slide: bool) -> bytes:
    rate = 44100
    samples = rate * (25 if slide else 60) // 1000
    data = bytearray()
    for i in range(samples):
        t = i / rate
        decay = math.exp(-t * (180.0 if slide else 60.0))
        pitch = 2400.0 if slide else 1300.0 - 500.0 * t / 0.06
        v = math.sin(2.0 * math.pi * pitch * t) * decay * (0.25 if slide else 0.45)
        data += struct.pack("<h", int(v * 32767.0))
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
    return header + b"data" + struct.pack("<I", len(data)) + bytes(data)


if __name__ == "__main__":
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "Content/UI/Sounds")
    out.mkdir(parents=True, exist_ok=True)
    (out / "SFX_UI_Hover.wav").write_bytes(ui_sound_wav(True))
    (out / "SFX_UI_Click.wav").write_bytes(ui_sound_wav(False))
    print("wrote", out / "SFX_UI_Hover.wav", "and", out / "SFX_UI_Click.wav")
