"""verify_d3d_r2_dis.py - disassembly evidence for review pass 2.
usage: python verify_d3d_r2_dis.py [addr len]...   (hex)
without arguments prints the fixed evidence list below."""
import sys
from verify_d3d_r2_common import *

EVID = [
    (0x234440, 0x20, "D3DDevice_AddRef"),
    (0x234460, 0x40, "D3DDevice_Release"),
    (0x2392b0, 0x10, "D3DDevice_MakeSpace"),
    (0x240390, 0x10, "FUN_00240390"),
    (0x240480, 0x20, "D3D_SetPushBufferSize"),
    (0x2404a0, 0xa0, "Direct3D_CreateDevice"),
    (0x240fc0, 0x40, "D3DDevice_Swap head"),
    (0x2417b0, 0x8, "MmFreeContiguousMemory thunk in D3D"),
    (0x241790, 0x20, "D3D_AllocContiguousMemory"),
    (0x23a520, 0x30, "FUN_0023a520"),
    (0x23a4b0, 0x50, "D3DVertexBuffer_Lock2"),
    (0x2374d0, 0x30, "D3DResource_Register"),
    (0x235190, 0x30, "D3DDevice_SetIndices"),
    (0x237c90, 0x40, "D3DDevice_SetStreamSource"),
    (0x23b080, 0x40, "CMiniport_GetDisplayCapabilities"),
    (0x23c890, 0x10, "push ISR/DPC"),
    (0x23c920, 0x10, "push ISR"),
    (0x235800, 0x60, "SetRenderStateNotInline"),
    (0x2344c0, 0x40, "BlockUntilVerticalBlank"),
    (0x2354f0, 0x40, "SetScreenSpaceOffset"),
]
if len(sys.argv) > 1:
    a = sys.argv[1:]
    for i in range(0, len(a), 2):
        print("== %s" % a[i]); pdis(int(a[i], 16), int(a[i + 1], 16))
else:
    for a, n, t in EVID:
        print("== %08x %s" % (a, t)); pdis(a, n)
