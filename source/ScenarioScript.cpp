#include "ScenarioScript.h"
#include <windows.h>
#include <algorithm>
#include <climits>
#include <utility>

namespace {
std::wstring DecodeGbk(const unsigned char* data, size_t length, bool normalize = true) {
    if (!data || !length) return {};
    while (length && data[length - 1] == 0) --length;
    if (!length) return {};
    bool utf8Marked = length >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF;
    if (utf8Marked) {
        data += 3;
        length -= 3;
    }
    auto decode = [&](UINT codePage, DWORD flags, std::wstring& output) {
        if (length > static_cast<size_t>(INT_MAX)) return false;
        const int required = MultiByteToWideChar(codePage, flags,
            reinterpret_cast<const char*>(data), static_cast<int>(length), nullptr, 0);
        if (required <= 0) return false;
        output.assign(static_cast<size_t>(required), L'\0');
        return MultiByteToWideChar(codePage, flags, reinterpret_cast<const char*>(data),
            static_cast<int>(length), output.data(), required) == required;
    };
    std::wstring result;
    bool hasHighByte = std::any_of(data, data + length,
        [](unsigned char value) { return value >= 0x80; });
    if ((utf8Marked || hasHighByte) && decode(CP_UTF8, MB_ERR_INVALID_CHARS, result)) {
        // Explicit UTF-8 data is uncommon, but accepting it keeps mixed 6.6x
        // scenario exports readable without depending on the system locale.
    } else if (!decode(936, MB_ERR_INVALID_CHARS, result) && !decode(936, 0, result)) {
        return {};
    }
    if (!normalize) return result;
    for (wchar_t& ch : result) {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
    }
    while (!result.empty() && result.front() == L' ') result.erase(result.begin());
    while (!result.empty() && result.back() == L' ') result.pop_back();
    return result;
}

bool ReadU16(const std::vector<unsigned char>& bytes, size_t offset, uint16_t& value) {
    if (offset + 2 > bytes.size()) return false;
    value = static_cast<uint16_t>(bytes[offset] | (static_cast<uint16_t>(bytes[offset + 1]) << 8));
    return true;
}

bool ReadI32(const std::vector<unsigned char>& bytes, size_t offset, int32_t& value) {
    if (offset + 4 > bytes.size()) return false;
    value = static_cast<int32_t>(static_cast<uint32_t>(bytes[offset]) |
        (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
        (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
        (static_cast<uint32_t>(bytes[offset + 3]) << 24));
    return true;
}

// Parameter counts copied from the local scenario command layout.  A compact
// count/mask representation keeps the proxy self-contained while preserving
// the EEX framing rules used by the editor.
const unsigned char kParamCounts[124] = {
    0,0,1,0,1,2,12,0,1,1,0,2,0,0,0,1,0,1,2,1,1,3,1,1,1,1,1,2,0,0,0,2,
    1,5,1,2,1,3,5,5,1,3,3,1,4,1,3,0,5,7,6,3,2,2,4,3,4,2,3,3,3,4,6,2,
    1,8,0,0,0,10,11,12,6,0,11,5,3,13,11,6,4,0,2,8,0,6,1,1,7,8,0,5,
    1,2,2,0,3,0,1,3,2,3,3,2,3,1,0,4,2,8,1,1,3,1,2,8,1,2,1,5,4,5,1,2
};

const uint16_t kDwordMasks[124] = {
    0,0,0,0,0,0,2,0,0,1,0,1,0,0,0,0,
    0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,3,
    0,3,0,2,0,6,30,0,0,6,6,0,0,0,0,0,
    6,60,28,0,0,0,4,2,8,2,4,0,0,0,0,1,
    0,242,0,0,0,4,1548,3096,0,0,1,7,4,6268,1596,0,
    0,0,0,60,0,28,0,0,24,1,0,15,0,2,0,0,
    0,0,0,0,0,0,0,1,0,0,0,3,0,52,1,0,
    4,1,1,2,0,0,1,18,1,18,0,1
};

const uint16_t kTextMasks[124] = {
    0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,1,0,1,4,1,1,1,1,1,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    2,0,0,2,0,0,0,2,0,1,0,0,0,0,0,0,
    0,0,2,0,0,0,0,0,0,0,1,2
};

const unsigned char kCanOpenSubEvent[124] = {
    0,0,2,1,2,2,0,2,0,0,0,0,0,0,0,0,
    0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,2,2,0,0,0,0,0,0,2,2,0,
    0,0,0,0,0,0,2,2,0,0,0,0,2,0,0,2,
    2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,
    0,0,0,2,0,0,0,0,0,2,0,2
};

bool ParseCommand(const std::vector<unsigned char>& bytes, size_t& cursor, size_t end,
    ScenarioCommand& result) {
    uint16_t rawId = 0;
    if (!(cursor <= end && end - cursor >= 2 && ReadU16(bytes, cursor, rawId))) return false;
    cursor += 2;
    if (rawId >= _countof(kParamCounts)) return false;
    result = ScenarioCommand();
    result.id = rawId;
    unsigned count = kParamCounts[rawId];
    if (rawId == 0x46) count = 11u * 20u;
    if (rawId == 0x47) count = 12u * 80u;
    const unsigned baseCount = kParamCounts[rawId] ? kParamCounts[rawId] : 1;
    for (unsigned index = 0; index < count; ++index) {
        const unsigned slot = rawId == 0x46 ? index % baseCount : rawId == 0x47 ? index % baseCount : index;
        uint16_t tag = 0;
        if (!(cursor <= end && end - cursor >= 2 && ReadU16(bytes, cursor, tag))) return false;
        cursor += 2;
        uint16_t textMask = kTextMasks[rawId];
        uint16_t dwordMask = kDwordMasks[rawId];
        if (rawId == 0x14 || rawId == 0x16 || rawId == 0x69) textMask = 1;
        if (rawId == 0x15) textMask = 1u << 2;
        if (rawId == 0x63) textMask = 1u << 1;
        if (rawId == 0x4C) dwordMask = 1u << 2;
        if (rawId == 0x4E) dwordMask = (1u << 2) | (1u << 3) | (1u << 4) |
            (1u << 5) | (1u << 9) | (1u << 10);
        if (rawId == 0x46) dwordMask = (1u << 2) | (1u << 3) | (1u << 9) | (1u << 10);
        if (rawId == 0x47) dwordMask = (1u << 3) | (1u << 4) | (1u << 10) | (1u << 11);
        if (rawId == 0x05 && slot < 2) {
            uint16_t arrayCount = 0;
            if (!(cursor <= end && end - cursor >= 2 && ReadU16(bytes, cursor, arrayCount)) || arrayCount > 4096 ||
                cursor + 2u + static_cast<size_t>(arrayCount) * 2u > end) return false;
            cursor += 2;
            result.values.push_back(arrayCount);
            for (unsigned item = 0; item < arrayCount; ++item) {
                uint16_t value = 0;
                if (!(cursor <= end && end - cursor >= 2 && ReadU16(bytes, cursor, value))) return false;
                cursor += 2;
                result.values.push_back(static_cast<int16_t>(value));
            }
        } else if (textMask & (1u << slot)) {
            const size_t start = cursor;
            while (cursor < end && bytes[cursor] != 0) ++cursor;
            if (cursor >= end) return false;
            // 0x7B is the audited 6.5/Protected220 conditional extension.
            // Its CSV expression is data, so retain whitespace byte-for-byte
            // after decoding; ordinary dialogue text keeps the legacy trim.
            std::wstring text = DecodeGbk(bytes.data() + start, cursor - start,
                rawId != 0x7B);
            ++cursor;
            if (!text.empty()) result.texts.push_back(std::move(text));
        } else if ((dwordMask & (1u << slot)) != 0) {
            int32_t value = 0;
            if (!(cursor <= end && end - cursor >= 4 && ReadI32(bytes, cursor, value))) return false;
            cursor += 4;
            result.values.push_back(value);
        } else {
            uint16_t value = 0;
            if (!(cursor <= end && end - cursor >= 2 && ReadU16(bytes, cursor, value))) return false;
            cursor += 2;
            result.values.push_back(static_cast<int16_t>(value));
        }
        result.parameterTags.push_back(tag);
    }
    return cursor <= end;
}

bool IsInternalEventMarker(const ScenarioCommand& command) {
    if (command.id == 0x02) return true;
    return command.id == 0x05 && command.values.size() == 3 &&
        command.values[0] == 0 && command.values[1] == 1 && command.values[2] >= 0;
}

bool ShouldInheritCondition(const ScenarioCommand& command) {
    return command.id != 0x00 && command.id != 0x01 && !IsInternalEventMarker(command);
}

bool ParseBlock(const std::vector<unsigned char>& bytes, size_t start, size_t length,
    bool root, const std::vector<ScenarioCondition>& inherited,
    size_t& nextBlock, std::vector<ScenarioCommand>& output, const std::atomic<bool>* cancelled) {
    if ((cancelled && cancelled->load()) || start > bytes.size() || length > bytes.size() - start)
        return false;
    struct BlockFrame {
        size_t cursor = 0;
        size_t end = 0;
        size_t block = 0;
        size_t event = 0;
        size_t parentBlock = SIZE_MAX;
        size_t blockOwnerOrdinal = SIZE_MAX;
        bool head = false;
        bool pendingSubEvent = false;
        std::vector<ScenarioCondition> conditions;
        std::vector<ScenarioCondition> headerConditions;
    };
    std::vector<BlockFrame> stack;
    const size_t rootBlock = nextBlock++;
    stack.push_back({start, start + length, rootBlock, rootBlock, SIZE_MAX, SIZE_MAX,
        root, false, inherited, {}});
    while (!stack.empty()) {
        if (cancelled && cancelled->load()) return false;
        BlockFrame& frame = stack.back();
        if (frame.cursor == frame.end) {
            stack.pop_back();
            continue;
        }
        if (frame.cursor > frame.end) return false;
        ScenarioCommand command;
        const size_t commandOffset = frame.cursor;
        if (!ParseCommand(bytes, frame.cursor, frame.end, command)) return false;
        command.offset = commandOffset;
        command.ordinal = output.size();
        command.block = frame.block;
        command.parentBlock = frame.parentBlock;
        command.blockOwnerOrdinal = frame.blockOwnerOrdinal;
        command.event = frame.event;
        command.conditions = frame.conditions;
        output.push_back(command);
        if (frame.head && command.id == 0) {
            uint16_t childLength = 0;
            if (frame.cursor > frame.end || frame.end - frame.cursor < 2 ||
                !ReadU16(bytes, frame.cursor, childLength)) return false;
            const size_t childStart = frame.cursor + 2;
            if (childStart > frame.end || childLength > frame.end - childStart || stack.size() >= 512)
                return false;
            frame.cursor = childStart + childLength;
            frame.head = false;
            frame.pendingSubEvent = false;
            std::vector<ScenarioCondition> childConditions = frame.conditions;
            childConditions.insert(childConditions.end(), frame.headerConditions.begin(),
                frame.headerConditions.end());
            stack.push_back({childStart, childStart + childLength, nextBlock++, frame.event,
                frame.block, command.ordinal, false, false, std::move(childConditions), {}});
        } else if (frame.pendingSubEvent && command.id < _countof(kCanOpenSubEvent) &&
            kCanOpenSubEvent[command.id]) {
            uint16_t childLength = 0;
            if (frame.cursor > frame.end || frame.end - frame.cursor < 2 ||
                !ReadU16(bytes, frame.cursor, childLength)) return false;
            const size_t childStart = frame.cursor + 2;
            if (childStart > frame.end || childLength > frame.end - childStart || stack.size() >= 512)
                return false;
            std::vector<ScenarioCondition> conditions = frame.conditions;
            if (frame.head) {
                if (ShouldInheritCondition(command))
                    frame.headerConditions.push_back({command.id, command.values, command.texts});
                conditions.insert(conditions.end(), frame.headerConditions.begin(),
                    frame.headerConditions.end());
            } else if (ShouldInheritCondition(command)) {
                conditions.push_back({command.id, command.values, command.texts});
            }
            frame.cursor = childStart + childLength;
            frame.pendingSubEvent = false;
            stack.push_back({childStart, childStart + childLength, nextBlock++, frame.event,
                frame.block, command.ordinal, false, false, std::move(conditions), {}});
        } else if (frame.head) {
            if (ShouldInheritCondition(command))
                frame.headerConditions.push_back({command.id, command.values, command.texts});
        } else if (command.id != 1) {
            frame.pendingSubEvent = false;
        }
        if (command.id == 1) frame.pendingSubEvent = true;
    }
    return true;
}

} // namespace

bool ParseScenarioScript(const std::vector<unsigned char>& bytes, std::vector<ScenarioCommand>& output,
    const std::atomic<bool>* cancelled, std::wstring* diagnostics) {
    output.clear();
    if (diagnostics) diagnostics->clear();
    if (cancelled && cancelled->load()) return false;
    if (bytes.size() < 14 || bytes[0] != 'E' || bytes[1] != 'E' || bytes[2] != 'X' || bytes[3] != 0) return false;
    int32_t tableEnd = 0;
    if (!ReadI32(bytes, 10, tableEnd) || tableEnd < 14 || tableEnd > static_cast<int32_t>(bytes.size()) ||
        (tableEnd - 10) % 4 != 0) return false;
    std::vector<uint32_t> scenes;
    for (int32_t offset = 10; offset < tableEnd; offset += 4) {
        int32_t scene = 0;
        if (!ReadI32(bytes, static_cast<size_t>(offset), scene) || scene < tableEnd ||
            scene >= static_cast<int32_t>(bytes.size())) return false;
        scenes.push_back(static_cast<uint32_t>(scene));
    }
    size_t nextBlock = 1;
    for (uint32_t sceneOffset : scenes) {
        if (cancelled && cancelled->load()) return false;
        uint16_t sectionCount = 0;
        if (!ReadU16(bytes, sceneOffset, sectionCount)) return false;
        size_t cursor = static_cast<size_t>(sceneOffset) + 2;
        for (unsigned section = 0; section < sectionCount; ++section) {
            uint16_t sectionLength = 0;
            if (!ReadU16(bytes, cursor, sectionLength)) return false;
            cursor += 2;
            const std::vector<ScenarioCondition> none;
            const size_t first = output.size();
            if (!ParseBlock(bytes, cursor, sectionLength, true, none, nextBlock, output,
                cancelled)) {
                if (diagnostics) *diagnostics += L"malformed-section: section=" +
                    std::to_wstring(section) + L" offset=" + std::to_wstring(cursor) + L"\n";
                output.resize(first);
            }
            for (size_t i = first; i < output.size(); ++i) {
                output[i].section = section;
                output[i].scene = sceneOffset;
            }
            cursor += sectionLength;
        }
    }
    return !output.empty();
}


bool ExtractScenarioJoinCandidates(const std::wstring& path,
    std::vector<int>& personIds, std::wstring& diagnostics) {
    personIds.clear();
    diagnostics.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { diagnostics = L"scenario-open-failed: " + path; return false; }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 14 || size.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(file); diagnostics = L"scenario-size-invalid: " + path; return false;
    }
    std::vector<unsigned char> bytes(static_cast<size_t>(size.QuadPart));
    DWORD count = 0;
    const bool read = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) != FALSE && count == bytes.size();
    CloseHandle(file);
    std::vector<ScenarioCommand> commands;
    if (!read || !ParseScenarioScript(bytes, commands, nullptr, &diagnostics)) return false;
    for (const auto& command : commands) {
        if (command.id != 0x3B || command.values.size() != 3) continue;
        // The audited 3B schema uses direct person/camp/join-mode tags
        // 02/0E/3E. Never reinterpret a variable or unknown parameter encoding
        // as a literal person number during lossy legacy reconstruction.
        if (command.parameterTags != std::vector<uint16_t>({0x02, 0x0E, 0x3E})) {
            diagnostics += L"join-dynamic-or-unknown-tags: offset=" + std::to_wstring(command.offset) + L"\n";
            continue;
        }
        const int id = command.values[0], camp = command.values[1];
        if (id < 0 || id >= 1024 || camp != 0) continue;
        if (std::find(personIds.begin(), personIds.end(), id) == personIds.end()) personIds.push_back(id);
    }
    std::sort(personIds.begin(), personIds.end());
    return true;
}
