"""Serve PCM from one preloaded Piper model over private process pipes."""

import json
import struct
import sys

from piper import PiperVoice, SynthesisConfig


def run(model_path):
    voice = PiperVoice.load(model_path)
    output = sys.stdout.buffer
    output.write(struct.pack("<I", 1))
    output.flush()
    for line in sys.stdin.buffer:
        try:
            request = json.loads(line)
            settings = SynthesisConfig(length_scale=request["length_scale"])
            for chunk in voice.synthesize(request["text"], syn_config=settings):
                if (chunk.sample_rate, chunk.sample_width, chunk.sample_channels) != (22050, 2, 1):
                    raise ValueError("The SAPI engine requires mono 22.05 kHz 16-bit PCM.")
                pcm = chunk.audio_int16_bytes
                if pcm:
                    output.write(struct.pack("<I", len(pcm)))
                    output.write(pcm)
            output.write(struct.pack("<I", 0))
        except (ValueError, KeyError, RuntimeError):
            output.write(struct.pack("<I", 0xFFFFFFFF))
        output.flush()


if __name__ == "__main__":
    run(sys.argv[1])
