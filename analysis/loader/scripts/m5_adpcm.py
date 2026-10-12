"""Reference Xbox ADPCM (WAVE_FORMAT 0x0069) codec in numpy-free Python (+ optional numpy speed-up).

Block layout (per block, nBlockAlign = 36 * channels):
  header per channel (4 bytes): int16 predictor, uint8 step index, uint8 reserved
  data: 8 rounds; in each round 4 bytes per channel (8 nibbles, low nibble first)
Two variants are offered because sources disagree about the sample count per block:
  variant 64: the header predictor is output sample 0, the first 63 nibbles give samples 1..63 and the
              64th nibble is decoded but not output (64 samples per block = wSamplesPerBlock 64)
  variant 65: header sample + all 64 nibbles (65 samples per block)
Result of m5_adpcm_check.py (scripts/out/m5_adpcm_check.out): variant 64 with EXACT = True is bit-exact
with ffmpeg adpcm_ima_xbox on 5 game files (mono/stereo, 22.05/44.1/48 kHz); the classic bitwise IMA
step formula differs by up to 28 LSB.

Also: encode() (IMA, for synthetic unit-test signals) so the host's tests can round-trip without game
data. Nothing here touches the disc.
"""
import struct

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
        80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
        544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
        2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
        11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


EXACT = True  # ffmpeg adpcm_ima_xbox formula diff = ((2*(n&7)+1)*step) >> 3 (bit-exact, m5_adpcm_check.out); False = classic bitwise IMA


def _nibble(state, n):
    pred, idx = state
    step = STEP[idx]
    if EXACT:
        diff = ((2 * (n & 7) + 1) * step) >> 3
    else:
        diff = step >> 3
        if n & 4:
            diff += step
        if n & 2:
            diff += step >> 1
        if n & 1:
            diff += step >> 2
    if n & 8:
        pred -= diff
    else:
        pred += diff
    pred = max(-32768, min(32767, pred))
    idx = max(0, min(88, idx + INDEX[n]))
    state[0], state[1] = pred, idx
    return pred


def decode(data, channels, variant=64):
    """Decode Xbox ADPCM bytes -> list of per-channel int lists."""
    ba = 36 * channels
    out = [[] for _ in range(channels)]
    nblocks = len(data) // ba
    for b in range(nblocks):
        blk = data[b * ba:(b + 1) * ba]
        st = []
        for c in range(channels):
            p, i, _ = struct.unpack_from("<hBB", blk, 4 * c)
            st.append([p, min(i, 88)])
            out[c].append(p)
        dec = [[] for _ in range(channels)]
        off = 4 * channels
        for _round in range(8):
            for c in range(channels):
                for k in range(4):
                    byte = blk[off + 4 * c + k]
                    dec[c].append(_nibble(st[c], byte & 0xF))
                    dec[c].append(_nibble(st[c], byte >> 4))
            off += 4 * channels
        for c in range(channels):
            out[c].extend(dec[c][:63] if variant == 64 else dec[c])
    return out


def encode(samples, channels):
    """IMA encoder producing Xbox ADPCM blocks (variant 64 layout): samples = per-channel int lists of
    equal length (multiple of 64). Greedy nibble choice; good enough for synthetic test signals."""
    n = len(samples[0])
    assert n % 64 == 0
    out = bytearray()
    state = [[samples[c][0], 0] for c in range(channels)]
    for b in range(n // 64):
        base = b * 64
        hdr = bytearray()
        for c in range(channels):
            state[c][0] = samples[c][base]
            hdr += struct.pack("<hBB", samples[c][base], state[c][1], 0)
        nibs = [[] for _ in range(channels)]
        for c in range(channels):
            for k in range(64):
                target = samples[c][base + 1 + k] if k < 63 else samples[c][base + 63]
                pred, idx = state[c]
                step = STEP[idx]
                diff = target - pred
                code = 0
                if diff < 0:
                    code = 8
                    diff = -diff
                if diff >= step:
                    code |= 4
                    diff -= step
                if diff >= step >> 1:
                    code |= 2
                    diff -= step >> 1
                if diff >= step >> 2:
                    code |= 1
                _nibble(state[c], code)
                nibs[c].append(code)
        body = bytearray()
        for r in range(8):
            for c in range(channels):
                for k in range(4):
                    i = r * 8 + 2 * k
                    body.append(nibs[c][i] | (nibs[c][i + 1] << 4))
        out += hdr + body
    return bytes(out)
