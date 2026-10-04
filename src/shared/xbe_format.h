// XBE file format structures (guest-visible: the header is mapped at 0x10000 and read by the guest,
// see design 1.4 step 5). Layout verified against orig/retail_eu/default.xbe (SizeOfImageHeader
// 0x180, certificate size 0x1EC, 10 sections of 0x38 bytes) and against the guest's absolute header
// reads (design 1.4 step 5: 0x10108, 0x10118, 0x1011C, 0x10120, 0x10124, 0x10130, 0x10134, 0x10138).
#pragma once

#include <cstddef>
#include <cstdint>

namespace xbe {

inline constexpr std::uint32_t kMagic = 0x48454258;  // "XBEH"
inline constexpr std::uint32_t kEntryXorRetail = 0xA8FC57AB;
inline constexpr std::uint32_t kEntryXorDebug = 0x94859D4B;
inline constexpr std::uint32_t kThunkXorRetail = 0x5B6D40B6;
inline constexpr std::uint32_t kThunkXorDebug = 0xEFB1F152;

struct ImageHeader {
    std::uint32_t Magic;                        // 0x000 "XBEH"
    std::uint8_t Signature[256];                // 0x004 RSA signature (never read by the guest)
    std::uint32_t BaseAddress;                  // 0x104 0x10000
    std::uint32_t SizeOfHeaders;                // 0x108 0x968
    std::uint32_t SizeOfImage;                  // 0x10C 0x30A300
    std::uint32_t SizeOfImageHeader;            // 0x110 0x180
    std::uint32_t TimeDateStamp;                // 0x114
    std::uint32_t CertificateAddress;           // 0x118 0x10180
    std::uint32_t NumberOfSections;             // 0x11C 10
    std::uint32_t SectionHeadersAddress;        // 0x120 0x1036C
    std::uint32_t InitFlags;                    // 0x124 5 (read by XapiInitProcess)
    std::uint32_t EntryPoint;                   // 0x128 XOR kEntryXorRetail = 0xF84DD
    std::uint32_t TlsAddress;                   // 0x12C 0x2A5D2C
    std::uint32_t PeStackCommit;                // 0x130 0x10000 (CreateThread/CreateFiber default stack)
    std::uint32_t PeHeapReserve;                // 0x134 0x100000 (XAPI heap)
    std::uint32_t PeHeapCommit;                 // 0x138 0x1000
    std::uint32_t PeBaseAddress;                // 0x13C
    std::uint32_t PeSizeOfImage;                // 0x140
    std::uint32_t PeChecksum;                   // 0x144
    std::uint32_t PeTimeDateStamp;              // 0x148
    std::uint32_t DebugPathNameAddress;         // 0x14C
    std::uint32_t DebugFileNameAddress;         // 0x150
    std::uint32_t DebugUnicodeFileNameAddress;  // 0x154
    std::uint32_t KernelImageThunkAddress;      // 0x158 XOR kThunkXorRetail = 0x293560
    std::uint32_t NonKernelImportDirectoryAddress;  // 0x15C
    std::uint32_t NumberOfLibraryVersions;      // 0x160
    std::uint32_t LibraryVersionsAddress;       // 0x164
    std::uint32_t KernelLibraryVersionAddress;  // 0x168
    std::uint32_t XapiLibraryVersionAddress;    // 0x16C
    std::uint32_t LogoBitmapAddress;            // 0x170
    std::uint32_t LogoBitmapSize;               // 0x174
    // ASSUMPTION: names of the two fields of the 0x180-byte header variant (both 0 in this XBE).
    std::uint32_t LibraryFeaturesAddress;       // 0x178
    std::uint32_t NumberOfLibraryFeatures;      // 0x17C
};
static_assert(sizeof(ImageHeader) == 0x180);
static_assert(offsetof(ImageHeader, BaseAddress) == 0x104);
static_assert(offsetof(ImageHeader, SizeOfHeaders) == 0x108);
static_assert(offsetof(ImageHeader, CertificateAddress) == 0x118);
static_assert(offsetof(ImageHeader, NumberOfSections) == 0x11C);
static_assert(offsetof(ImageHeader, SectionHeadersAddress) == 0x120);
static_assert(offsetof(ImageHeader, InitFlags) == 0x124);
static_assert(offsetof(ImageHeader, EntryPoint) == 0x128);
static_assert(offsetof(ImageHeader, TlsAddress) == 0x12C);
static_assert(offsetof(ImageHeader, PeStackCommit) == 0x130);
static_assert(offsetof(ImageHeader, PeHeapReserve) == 0x134);
static_assert(offsetof(ImageHeader, PeHeapCommit) == 0x138);
static_assert(offsetof(ImageHeader, KernelImageThunkAddress) == 0x158);
static_assert(offsetof(ImageHeader, LogoBitmapSize) == 0x174);

// Section flags
inline constexpr std::uint32_t kSectionWritable = 0x01;
inline constexpr std::uint32_t kSectionPreload = 0x02;
inline constexpr std::uint32_t kSectionExecutable = 0x04;
inline constexpr std::uint32_t kSectionInsertedFile = 0x08;
inline constexpr std::uint32_t kSectionHeadPageReadOnly = 0x10;
inline constexpr std::uint32_t kSectionTailPageReadOnly = 0x20;

struct SectionHeader {
    std::uint32_t Flags;                          // 0x00
    std::uint32_t VirtualAddress;                 // 0x04
    std::uint32_t VirtualSize;                    // 0x08
    std::uint32_t RawAddress;                     // 0x0C file offset
    std::uint32_t RawSize;                        // 0x10
    std::uint32_t SectionNameAddress;             // 0x14
    std::uint32_t SectionReferenceCount;          // 0x18 (XeLoadSection/XeUnloadSection count here)
    std::uint32_t HeadSharedPageRefCountAddress;  // 0x1C
    std::uint32_t TailSharedPageRefCountAddress;  // 0x20
    std::uint8_t SectionDigest[20];               // 0x24
};
static_assert(sizeof(SectionHeader) == 0x38);
static_assert(offsetof(SectionHeader, SectionReferenceCount) == 0x18);
static_assert(offsetof(SectionHeader, SectionDigest) == 0x24);

struct Certificate {
    std::uint32_t SizeOfCertificate;           // 0x000 0x1EC
    std::uint32_t TimeDateStamp;               // 0x004
    std::uint32_t TitleId;                     // 0x008 0x46490002
    wchar_t TitleName[40];                     // 0x00C
    std::uint32_t AlternateTitleIds[16];       // 0x05C
    std::uint32_t AllowedMediaTypes;           // 0x09C 2
    std::uint32_t GameRegion;                  // 0x0A0
    std::uint32_t GameRatings;                 // 0x0A4 0 (parental control compare, startup.md 2.3 step 9)
    std::uint32_t DiskNumber;                  // 0x0A8
    std::uint32_t Version;                     // 0x0AC
    std::uint8_t LanKey[16];                   // 0x0B0
    std::uint8_t SignatureKey[16];             // 0x0C0
    std::uint8_t AlternateSignatureKeys[16][16];  // 0x0D0
    // ASSUMPTION: extended certificate fields (size 0x1EC), names after xboxdevwiki.
    std::uint32_t OriginalCertificateSize;     // 0x1D0
    std::uint32_t OnlineServiceId;             // 0x1D4
    std::uint32_t SecurityFlags;               // 0x1D8
    std::uint8_t CodeEncKey[16];               // 0x1DC
};
static_assert(sizeof(wchar_t) == 2);
static_assert(sizeof(Certificate) == 0x1EC);
static_assert(offsetof(Certificate, TitleName) == 0x0C);
static_assert(offsetof(Certificate, AlternateTitleIds) == 0x5C);
static_assert(offsetof(Certificate, AllowedMediaTypes) == 0x9C);
static_assert(offsetof(Certificate, GameRatings) == 0xA4);
static_assert(offsetof(Certificate, SignatureKey) == 0xC0);
static_assert(offsetof(Certificate, AlternateSignatureKeys) == 0xD0);

struct TlsDirectory {
    std::uint32_t RawDataStart;        // 0x00
    std::uint32_t RawDataEnd;          // 0x04
    std::uint32_t AddressOfIndex;      // 0x08 0x2F3D18 (_tls_index)
    std::uint32_t AddressOfCallbacks;  // 0x0C
    std::uint32_t SizeOfZeroFill;      // 0x10 0x20
    std::uint32_t Characteristics;     // 0x14
};
static_assert(sizeof(TlsDirectory) == 0x18);

struct LibraryVersion {
    char Name[8];
    std::uint16_t MajorVersion;
    std::uint16_t MinorVersion;
    std::uint16_t BuildVersion;
    std::uint16_t Flags;
};
static_assert(sizeof(LibraryVersion) == 0x10);

}  // namespace xbe
