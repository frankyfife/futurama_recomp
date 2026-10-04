// PE/XBE polyglot header at 0x10000 (design 1.4 step 5, E2). Owned by the core agent.
#pragma once

namespace core {

// Stage "BuildPolyglotHeader": save the launcher's NT headers + section table, copy the 0x968-byte
// XBE header to 0x10000, then WORD [0x10000] = 'MZ', DWORD [0x1003C] = 0x970 and the saved NT
// headers at 0x10970 (guest::kPolyglot*). RtlImageNtHeader(GetModuleHandle(NULL)) must stay valid.
bool StageBuildPolyglotHeader();

}  // namespace core
