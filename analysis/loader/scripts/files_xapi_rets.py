"""Print the 'ret N' instructions of the XAPI file/save functions (stdcall arg count = N/4) in retail_eu."""
import sys
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
md = Cs(CS_ARCH_X86, CS_MODE_32)
F = [(0xf76e0,395,"CreateFileA (Vermutung)"),(0xf7195,237,"ReadFile"),(0xf7282,214,"WriteFile"),(0xf73dc,250,"SetFilePointer"),
     (0xf74d6,200,"SetFilePointerEx"),(0xf75c5,62,"GetFileSizeEx"),(0xf76a5,59,"GetFileSize"),(0xf7603,81,"ReadFileEx"),
     (0xf7654,81,"WriteFileEx"),(0xf6ec1,101,"GetOverlappedResult"),(0xf7177,30,"CloseHandle"),(0xf7358,132,"SetEndOfFile"),
     (0xf759e,39,"FlushFileBuffers"),(0xf786b,738,"CopyFileExA (Vermutung)"),(0xf7b4d,31,"CopyFileA (Vermutung)"),
     (0xf7b6c,166,"SetFileAttributesA (Vermutung)"),(0xf7c12,75,"GetFileAttributesA (Vermutung)"),
     (0xf7c5d,244,"GetDiskFreeSpaceExA (Vermutung)"),(0xf8081,159,"XGetDiskClusterSizeA (Vermutung)"),
     (0xf8120,155,"XGetDiskSectorSizeA (Vermutung)"),(0xf85aa,108,"CreateDirectoryA (Vermutung)"),
     (0xf8616,142,"RemoveDirectoryA (Vermutung)"),(0xf8961,269,"FindFirstFileA (Vermutung)"),(0xf8a6e,81,"FindNextFileA (Vermutung)"),
     (0xf8886,111,"delete tree (Vermutung)"),(0xf7f7c,261,"XMountUtilityDrive"),(0xf946d,790,"FATX format (Vermutung)"),
     (0xf6987,611,"XCreateSaveGame"),(0xf6bea,208,"XDeleteSaveGame"),(0xf6cba,263,"XFindFirstSaveGame"),
     (0xf6dc1,71,"XFindNextSaveGame"),(0xf6e08,76,"XFindClose"),(0xf706f,125,"XCalculateSignatureBeginEx"),
     (0xf7163,20,"XCalculateSignatureBegin"),(0xf70ec,26,"XCalculateSignatureUpdate"),(0xf7106,93,"XCalculateSignatureEnd"),
     (0xf8c41,820,"XapiMapLetterToDirectory"),(0xf8f75,85,"map T:/U: (Vermutung)"),(0xf90e6,457,"DVD auth (Vermutung)"),
     (0xf6440,58,"XGetLanguage"),(0xf8554,40,"XGetGameRegion"),(0xf82e3,317,"XLaunchNewImageA"),(0x13f1d0,0x46,"NiUDSStream read completion")]
for a, n, nm in F:
    rets = sorted({(i.op_str or "0") for i in md.disasm(X.read(a, n), a) if i.mnemonic == "ret"})
    print(f"{a:08x} {nm:36s} ret {', '.join(rets)}")
