#!/usr/bin/env python3
"""Generate NextKDE's original short notification tone (run only in a VM/CI)."""
import math
from pathlib import Path
import struct
import wave

output = Path(__file__).resolve().parents[1] / 'shell/desktop/modules/notifications/sounds/notification.wav'
rate = 48000
samples = bytearray()
for index in range(int(rate * 0.24)):
    time = index / rate
    envelope = min(1.0, time / 0.008) * min(1.0, (0.24 - time) / 0.06) * math.exp(-time * 9)
    tone = (math.sin(2 * math.pi * 880 * time) + 0.35 * math.sin(2 * math.pi * 1320 * time)) / 1.35
    samples.extend(struct.pack('<h', round(32767 * 0.4 * envelope * tone)))
with wave.open(str(output), 'wb') as audio:
    audio.setnchannels(1)
    audio.setsampwidth(2)
    audio.setframerate(rate)
    audio.writeframes(samples)
