// Probe on SoundVoice::CreateDsObject: tags the DSOUND object that the call creates with the voice's volume
// category, 3D/stream flags, id and sound name (m5_audio.md 6.1). The DSOUND API itself does not know categories;
// the engine routes each voice to the submix of its category (stems for capture_stems, per-category levels) and
// the event list names the sounds. Objects created outside this probe (XMV streams) stay untagged = "movie".
// M5 DSOUND core agent.
#include "audio/dsound_hle.h"

#include "core/guest.h"
#include "core/guest_call.h"
#include "core/hooks.h"

#include <cstring>

namespace snd {
namespace {

std::uint8_t ReadU8(std::uint32_t address) noexcept {
    return guest::IsReadable(address, 1) ? guest::Read<std::uint8_t>(address) : 0;
}
std::uint32_t ReadU32(std::uint32_t address) noexcept {
    return guest::IsReadable(address, 4) ? guest::Read<std::uint32_t>(address) : 0;
}

void ReadName(std::uint32_t address, char (&out)[64]) noexcept {
    out[0] = '\0';
    if (address == 0) {
        return;
    }
    std::size_t n = 0;
    while (n + 1 < sizeof(out) && guest::IsReadable(address + static_cast<std::uint32_t>(n), 1)) {
        const char c = guest::Read<char>(address + static_cast<std::uint32_t>(n));
        if (c == '\0') {
            break;
        }
        out[n++] = (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    }
    out[n] = '\0';
}

}  // namespace

// SoundVoice::CreateDsObject FUN_000CC150 - thiscall (ecx = SoundVoice), no stack args, plain ret (0xCC352,
// 0xCC383); only caller the voice update 0xCC621 (capstone sweep: no other call, no branch into 0xCC151..0xCC155).
// Creates a stream (SoundData+0x27 set: DirectSoundCreateStream 0xCC1C3) or a buffer (DirectSoundCreateBuffer
// 0xCC296) and stores it at voice+0x74 / +0xA4 without releasing a previous one (double Play, m5_audio.md 3.3 item
// 6: reported here when the old object is still live). Declared __fastcall with an unused edx (same layout and
// purge as thiscall without stack args), like probe:Game::Init.
std::uint32_t __fastcall CreateDsObjectProbe(std::uint32_t self, std::uint32_t /*edx*/) noexcept {
    VoiceTag tag;
    const std::uint32_t data = ReadU32(self + guest::kSoundVoiceDataOffset);
    const std::uint32_t category = ReadU32(self + guest::kSoundVoiceCategoryOffset);
    tag.category = category <= 2 ? static_cast<Category>(category) : Category::Effects;
    // 3D only for mono samples (the stream flag 0x60010 needs SoundData+0x25 && voice+0x1E, 0xCC195..0xCC1A8; a 3D
    // buffer on a stereo sample is created and released without Play, 0xCC631).
    tag.is3D = ReadU8(self + guest::kSoundVoice3DOffset) != 0;
    tag.stream = data != 0 && ReadU8(data + guest::kSoundDataStreamOffset) != 0;
    tag.voiceId = ReadU32(self + guest::kSoundVoiceIdOffset);
    tag.voice = self;
    if (data != 0) {
        ReadName(ReadU32(data + guest::kSoundDataNameOffset), tag.name);
    }
    const std::uint32_t previous =
        ReadU32(self + (tag.stream ? guest::kSoundVoiceStreamObjOffset : guest::kSoundVoiceBufferObjOffset));
    if (previous != 0) {
        NoteOrphanedObject(guest::Ptr<const void>(previous), tag.voiceId);
    }
    SetPendingVoiceTag(tag);
    const std::uint32_t eax =
        core::CallGuestThiscall(core::HookTrampoline(guest::kSoundVoiceCreateDsObject), self, {});
    ClearPendingVoiceTag();
    return eax;
}

}  // namespace snd

FUT_PROBE(guest::kSoundVoiceCreateDsObject, &snd::CreateDsObjectProbe, Thiscall, "probe:Game::SoundVoiceCreateDsObject",
          "64 A1 00 00 00 00", 6);
