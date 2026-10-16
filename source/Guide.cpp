#include "Guide.h"
#include "ResourceIds.h"

#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace {

constexpr UINT kGuideReadComplete = WM_APP + 0x670;
constexpr UINT kGuideReload = 0x6701;
constexpr UINT kGuideFilter = 0x6702;
constexpr UINT kGuideScenario = 0x6703;

enum class GuideFilter : int { All, Dialog, Ambush, Ai, Rewards };

struct GuideFile {
    std::wstring path;
    std::wstring name;
    std::wstring battlefieldPath;
    std::wstring title;
    int number = 0;
    int battlefieldNumber = 0;
    bool isBattlefield = true;
};

struct GuideEntry {
    GuideFilter filter = GuideFilter::All;
    std::wstring text;
    std::wstring conditionKey;
    std::wstring prefix;
    std::vector<std::wstring> actions;
    bool hideInAll = false;
};

struct GuideUnit {
    enum class Camp { Player, Friend, Enemy };
    int id = -1;
    int battlefieldNumber = -1;
    int x = -1;
    int y = -1;
    int policy = -1;
    bool hidden = false;
    bool reinforcement = false;
    Camp camp = Camp::Enemy;
};

struct GuideResult {
    std::wstring title;
    std::vector<GuideEntry> entries;
};

struct GuideTaskState {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> completed{false};
    HWND window = nullptr;
    unsigned request = 0;
    GuideResult result;
};

struct GuideCondition {
    unsigned id = 0;
    std::vector<int32_t> values;
};

struct GuideCommand {
    unsigned id = 0;
    std::vector<int32_t> values;
    std::vector<std::wstring> texts;
    std::vector<GuideCondition> conditions;
    size_t ordinal = 0;
    size_t block = 0;
    size_t event = 0;
};

struct GuideContext {
    std::vector<GuideCommand> commands;
    std::vector<GuideUnit> units;
    std::unordered_map<int, std::wstring> people;
    std::unordered_map<int, std::wstring> items;
    bool namesReady = false;
};

HWND gGuideWindow = nullptr;
HWND gGuideLevelLabel = nullptr;
HWND gGuideScenarioBox = nullptr;
HWND gGuideFilterBox = nullptr;
HWND gGuideReload = nullptr;
HWND gGuideText = nullptr;
std::wstring gGuideRoot;
std::vector<GuideFile> gGuideFiles;
std::shared_ptr<GuideTaskState> gGuideTask;
GuideResult gGuideResult;
GuideFilter gGuideCurrentFilter = GuideFilter::All;
unsigned gGuideRequest = 0;
HFONT gGuideFont = nullptr;
bool gGuideFontOwned = false;
HMODULE gGuideRichEditModule = nullptr;
bool gGuideUsesRichEdit = false;
SRWLOCK gObservedScenarioLock = SRWLOCK_INIT;
std::wstring gObservedScenarioName;

constexpr size_t kGuideTextCharacters = 1024u * 1024u;

bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    const bool sized = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0 && size.QuadPart <= 0x7fffffff;
    if (!sized) { CloseHandle(file); return false; }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool okay = bytes.empty() || ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    if (!okay || read != bytes.size()) { bytes.clear(); return false; }
    return true;
}

std::wstring DecodeGbk(const unsigned char* data, size_t length) {
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
    for (wchar_t& ch : result) {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
    }
    while (!result.empty() && result.front() == L' ') result.erase(result.begin());
    while (!result.empty() && result.back() == L' ') result.pop_back();
    return result;
}

bool HasPlayerText(const std::wstring& text) {
    if (text.empty() || text.size() > 240) return false;
    return std::any_of(text.begin(), text.end(), [](wchar_t ch) {
        return ch >= 0x4E00 && ch <= 0x9FFF;
    });
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
    GuideCommand& result) {
    uint16_t rawId = 0;
    if (!ReadU16(bytes, cursor, rawId)) return false;
    cursor += 2;
    if (rawId >= _countof(kParamCounts)) return false;
    result = GuideCommand();
    result.id = rawId;
    unsigned count = kParamCounts[rawId];
    if (rawId == 0x46) count = 11u * 20u;
    if (rawId == 0x47) count = 12u * 80u;
    const unsigned baseCount = kParamCounts[rawId] ? kParamCounts[rawId] : 1;
    for (unsigned index = 0; index < count; ++index) {
        const unsigned slot = rawId == 0x46 ? index % baseCount : rawId == 0x47 ? index % baseCount : index;
        uint16_t tag = 0;
        if (!ReadU16(bytes, cursor, tag)) return false;
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
            if (!ReadU16(bytes, cursor, arrayCount) || arrayCount > 4096 ||
                cursor + 2u + static_cast<size_t>(arrayCount) * 2u > end) return false;
            cursor += 2;
            result.values.push_back(arrayCount);
            for (unsigned item = 0; item < arrayCount; ++item) {
                uint16_t value = 0;
                if (!ReadU16(bytes, cursor, value)) return false;
                cursor += 2;
                result.values.push_back(static_cast<int16_t>(value));
            }
        } else if (textMask & (1u << slot)) {
            const size_t start = cursor;
            while (cursor < end && bytes[cursor] != 0) ++cursor;
            if (cursor >= end) return false;
            std::wstring text = DecodeGbk(bytes.data() + start, cursor - start);
            ++cursor;
            if (!text.empty()) result.texts.push_back(std::move(text));
        } else if ((dwordMask & (1u << slot)) != 0) {
            int32_t value = 0;
            if (!ReadI32(bytes, cursor, value)) return false;
            cursor += 4;
            result.values.push_back(value);
        } else {
            uint16_t value = 0;
            if (!ReadU16(bytes, cursor, value)) return false;
            cursor += 2;
            result.values.push_back(static_cast<int16_t>(value));
        }
        (void)tag;
    }
    return true;
}

bool IsInternalEventMarker(const GuideCommand& command) {
    if (command.id == 0x02) return true;
    return command.id == 0x05 && command.values.size() == 3 &&
        command.values[0] == 0 && command.values[1] == 1 && command.values[2] >= 0;
}

bool ShouldInheritCondition(const GuideCommand& command) {
    return command.id != 0x00 && command.id != 0x01 && !IsInternalEventMarker(command);
}

bool ParseBlock(const std::vector<unsigned char>& bytes, size_t start, size_t length,
    bool root, const std::vector<GuideCondition>& inherited,
    size_t& nextBlock, std::vector<GuideCommand>& output, const std::atomic<bool>* cancelled) {
    if ((cancelled && cancelled->load()) || start > bytes.size() || length > bytes.size() - start)
        return false;
    struct BlockFrame {
        size_t cursor = 0;
        size_t end = 0;
        size_t block = 0;
        size_t event = 0;
        bool head = false;
        bool pendingSubEvent = false;
        std::vector<GuideCondition> conditions;
        std::vector<GuideCondition> headerConditions;
    };
    std::vector<BlockFrame> stack;
    const size_t rootBlock = nextBlock++;
    stack.push_back({start, start + length, rootBlock, rootBlock, root, false, inherited, {}});
    while (!stack.empty()) {
        if (cancelled && cancelled->load()) return false;
        BlockFrame& frame = stack.back();
        if (frame.cursor == frame.end) {
            stack.pop_back();
            continue;
        }
        if (frame.cursor > frame.end) return false;
        GuideCommand command;
        if (!ParseCommand(bytes, frame.cursor, frame.end, command)) return false;
        command.ordinal = output.size();
        command.block = frame.block;
        command.event = frame.event;
        command.conditions = frame.conditions;
        output.push_back(command);
        if (frame.head && command.id == 0) {
            uint16_t childLength = 0;
            if (!ReadU16(bytes, frame.cursor, childLength)) return false;
            const size_t childStart = frame.cursor + 2;
            if (childStart > frame.end || childLength > frame.end - childStart || stack.size() >= 512)
                return false;
            frame.cursor = childStart + childLength;
            frame.head = false;
            frame.pendingSubEvent = false;
            std::vector<GuideCondition> childConditions = frame.conditions;
            childConditions.insert(childConditions.end(), frame.headerConditions.begin(),
                frame.headerConditions.end());
            stack.push_back({childStart, childStart + childLength, nextBlock++, frame.event,
                false, false, std::move(childConditions), {}});
        } else if (frame.pendingSubEvent && command.id < _countof(kCanOpenSubEvent) &&
            kCanOpenSubEvent[command.id]) {
            uint16_t childLength = 0;
            if (!ReadU16(bytes, frame.cursor, childLength)) return false;
            const size_t childStart = frame.cursor + 2;
            if (childStart > frame.end || childLength > frame.end - childStart || stack.size() >= 512)
                return false;
            std::vector<GuideCondition> conditions = frame.conditions;
            if (frame.head) {
                if (ShouldInheritCondition(command))
                    frame.headerConditions.push_back({command.id, command.values});
                conditions.insert(conditions.end(), frame.headerConditions.begin(),
                    frame.headerConditions.end());
            } else if (ShouldInheritCondition(command)) {
                conditions.push_back({command.id, command.values});
            }
            frame.cursor = childStart + childLength;
            frame.pendingSubEvent = false;
            stack.push_back({childStart, childStart + childLength, nextBlock++, frame.event,
                false, false, std::move(conditions), {}});
        } else if (frame.head) {
            if (ShouldInheritCondition(command))
                frame.headerConditions.push_back({command.id, command.values});
        } else if (command.id != 1) {
            frame.pendingSubEvent = false;
        }
        if (command.id == 1) frame.pendingSubEvent = true;
    }
    return true;
}

bool ParseEex(const std::vector<unsigned char>& bytes, std::vector<GuideCommand>& output,
    const std::atomic<bool>* cancelled = nullptr) {
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
            const std::vector<GuideCondition> none;
            if (!ParseBlock(bytes, cursor, sectionLength, true, none, nextBlock, output,
                cancelled)) return false;
            cursor += sectionLength;
        }
    }
    return !output.empty();
}

std::wstring TrimNull(const unsigned char* data, size_t length) {
    size_t used = 0;
    while (used < length && data[used]) ++used;
    return DecodeGbk(data, used);
}

bool LoadTableNames(const std::wstring& path, size_t offset, size_t count, size_t rowSize,
    size_t nameBytes, int firstId, std::unordered_map<int, std::wstring>& result) {
    std::vector<unsigned char> bytes;
    if (!ReadFileBytes(path, bytes) || offset > bytes.size() || count > (bytes.size() - offset) / rowSize) return false;
    std::unordered_map<int, std::wstring> candidate;
    for (size_t index = 0; index < count; ++index) {
        const std::wstring name = TrimNull(bytes.data() + offset + index * rowSize, nameBytes);
        if (HasPlayerText(name)) candidate[firstId + static_cast<int>(index)] = name;
    }
    if (candidate.size() < count / 5) return false;
    result = std::move(candidate);
    return true;
}

void LoadNames(const std::wstring& root, GuideContext& context) {
    std::wstring data = root + L"\\Data.e5";
    std::wstring star = root + L"\\Star.e5";
    std::unordered_map<int, std::wstring> people;
    std::unordered_map<int, std::wstring> lowItems;
    std::unordered_map<int, std::wstring> highItems;
    const bool peopleOkay = LoadTableNames(data, 396, 1024, 32, 13, 0, people);
    const bool lowOkay = LoadTableNames(data, 33164, 104, 25, 17, 0, lowItems);
    const bool highOkay = LoadTableNames(star, 0, 151, 25, 17, 104, highItems);
    if (peopleOkay) context.people = std::move(people);
    if (lowOkay) context.items = std::move(lowItems);
    if (highOkay) context.items.insert(highItems.begin(), highItems.end());
    context.namesReady = peopleOkay && (!context.items.empty());
}

std::wstring PersonName(const GuideContext& context, int id) {
    const auto found = context.people.find(id);
    return found == context.people.end() ? std::wstring() : found->second;
}

std::wstring ConditionPersonName(const GuideContext& context, int id);

const GuideUnit* UnitByBattlefieldNumber(const GuideContext& context, int number) {
    for (const GuideUnit& unit : context.units)
        if (unit.battlefieldNumber == number) return &unit;
    return nullptr;
}

enum class PersonSide { Unknown, Friendly, Enemy };

PersonSide SideForPerson(const GuideContext& context, int id) {
    bool friendly = false;
    bool enemy = false;
    for (const GuideUnit& unit : context.units) {
        if (unit.id != id) continue;
        if (unit.camp == GuideUnit::Camp::Enemy) enemy = true;
        else friendly = true;
    }
    if (enemy && !friendly) return PersonSide::Enemy;
    if (friendly && !enemy) return PersonSide::Friendly;
    return PersonSide::Unknown;
}

std::wstring RetreatConditionText(const GuideContext& context, int id) {
    const std::wstring person = ConditionPersonName(context, id);
    if (person.empty()) return {};
    if (id == 1026 || SideForPerson(context, id) == PersonSide::Enemy) return L"击退" + person;
    if (id == 1024 || id == 1025 || id == 1027 ||
        SideForPerson(context, id) == PersonSide::Friendly) return person + L"撤退";
    // Most HP=0 event tests with no deployment evidence refer to an enemy.
    return L"击退" + person;
}

std::wstring ConditionPersonName(const GuideContext& context, int id) {
    const std::wstring person = PersonName(context, id);
    if (!person.empty()) return person;
    if (id == 1024) return L"任意部队";
    if (id == 1025) return L"我方部队";
    if (id == 1026) return L"敌军部队";
    if (id == 1027) return L"当前行动人物";
    return {};
}

std::wstring ItemName(const GuideContext& context, int id) {
    const auto found = context.items.find(id);
    return found == context.items.end() ? std::wstring() : found->second;
}

void AddUnique(std::vector<std::wstring>& values, const std::wstring& value) {
    if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end()) values.push_back(value);
}

bool IsKnownPersonName(const GuideContext& context, const std::wstring& name) {
    if (name.empty()) return false;
    for (const auto& person : context.people) if (person.second == name) return true;
    return false;
}

std::vector<std::wstring> ExtractSpeakers(const GuideContext& context, const GuideCommand& command) {
    std::vector<std::wstring> result;
    for (const std::wstring& text : command.texts) {
        for (size_t at = text.find(L'&'); at != std::wstring::npos; at = text.find(L'&', at + 1)) {
            size_t start = at + 1;
            while (start < text.size() && text[start] == L' ') ++start;
            size_t end = start;
            while (end < text.size() && end - start < 12) {
                const wchar_t ch = text[end];
                if (ch == L' ' || ch == L'\r' || ch == L'\n' || ch == L'@' ||
                    ch == L'：' || ch == L':' || ch == L'，' || ch == L'。') break;
                ++end;
            }
            if (end > start) {
                const std::wstring name = text.substr(start, end - start);
                if (IsKnownPersonName(context, name)) AddUnique(result, name);
            }
            at = end;
        }
    }
    return result;
}

std::wstring JoinNames(const std::vector<std::wstring>& values) {
    if (values.empty()) return {};
    if (values.size() == 1) return values[0];
    if (values.size() == 2) return values[0] + L"与" + values[1];
    return values[0] + L"、" + values[1] + L"等人";
}

std::wstring JoinItems(const std::vector<std::wstring>& values) {
    std::wstring result;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) result += L"、";
        result += values[i];
    }
    return result;
}

bool EndsWith(const std::wstring& value, const wchar_t* suffix) {
    const size_t length = wcslen(suffix);
    return value.size() >= length && value.compare(value.size() - length, length, suffix) == 0;
}

std::wstring WhenLead(const std::wstring& conditions) {
    return conditions + (EndsWith(conditions, L"前") || EndsWith(conditions, L"开始") ? L"，" : L"时，");
}

std::wstring AfterLead(const std::wstring& conditions) {
    return conditions + (EndsWith(conditions, L"前") || EndsWith(conditions, L"开始") ||
        EndsWith(conditions, L"回合") ? L"，" : L"后，");
}

std::wstring PolicyName(int value) {
    static const wchar_t* names[] = {
        L"被动出击", L"主动出击", L"坚守原地", L"攻击指定武将",
        L"前往指定地点", L"跟随指定武将", L"撤往指定地点"
    };
    return value >= 0 && value < static_cast<int>(_countof(names)) ? names[value] : L"";
}

bool RenderConditions(const GuideContext& context, const std::vector<GuideCondition>& conditions,
    std::wstring& text, bool& simpleAdjacent, std::pair<int, int>& adjacentPeople) {
    if (conditions.empty()) return false;
    std::vector<std::wstring> parts;
    simpleAdjacent = conditions.size() == 1 && conditions[0].id == 0x2E;
    adjacentPeople = {-1, -1};
    int minimumTurn = -1;
    int beforeTurn = -1;
    int exactTurn = -1;
    for (const GuideCondition& condition : conditions) {
        const std::vector<int32_t>& values = condition.values;
        if (condition.id == 0x2E && values.size() >= 2) {
            const std::wstring first = PersonName(context, values[0]);
            const std::wstring second = PersonName(context, values[1]);
            if (first.empty() || second.empty()) return false;
            parts.push_back(first + L"与" + second + L"相邻");
            adjacentPeople = {values[0], values[1]};
        } else if (condition.id == 0x25 && values.size() >= 3) {
            const std::wstring person = ConditionPersonName(context, values[0]);
            if (person.empty() || values[1] < 0 || values[2] < 0) return false;
            parts.push_back(person + L"到达（" + std::to_wstring(values[1]) + L"，" +
                std::to_wstring(values[2]) + L"）");
        } else if (condition.id == 0x26 && values.size() >= 5) {
            const std::wstring person = ConditionPersonName(context, values[0]);
            if (person.empty()) return false;
            for (size_t i = 1; i < 5; ++i) if (values[i] < 0) return false;
            if (values[1] == 0 && values[2] == 0 && values[3] == 99 && values[4] == 99)
                parts.push_back(person + L"在场");
            else
                parts.push_back(person + L"进入（" + std::to_wstring(values[1]) + L"，" +
                    std::to_wstring(values[2]) + L"）至（" + std::to_wstring(values[3]) + L"，" +
                    std::to_wstring(values[4]) + L"）区域");
        } else if (condition.id == 0x36 && values.size() >= 4) {
            static const wchar_t* attributes[] = {
                L"攻击力", L"防御力", L"精神力", L"爆发力", L"士气", L"HP", L"MP",
                L"当前 HP", L"当前 MP", L"等级", L"武力", L"统率", L"智力", L"敏捷",
                L"运气", L"头像"
            };
            const std::wstring person = ConditionPersonName(context, values[0]);
            const int attribute = values[1], amount = values[2], comparison = values[3];
            if (person.empty() || attribute < 0 || attribute >= static_cast<int>(_countof(attributes)) ||
                comparison < 0 || comparison > 2) return false;
            if (attribute == 7 && amount == 0 && comparison == 2) {
                const std::wstring retreat = RetreatConditionText(context, values[0]);
                if (retreat.empty()) return false;
                parts.push_back(retreat);
            } else {
                static const wchar_t* comparisons[] = {L"不低于", L"低于", L"等于"};
                parts.push_back(person + attributes[attribute] + L" " + comparisons[comparison] + L" " +
                    std::to_wstring(amount));
            }
        } else if (condition.id == 0x3F && values.size() >= 2) {
            if (values[0] < 1 || values[0] > 999) return false;
            if (values[1] == 0) minimumTurn = std::max(minimumTurn, values[0]);
            else if (values[1] == 1) beforeTurn = beforeTurn < 0 ? values[0] : std::min(beforeTurn, values[0]);
            else if (values[1] == 2) {
                if (exactTurn >= 0 && exactTurn != values[0]) return false;
                exactTurn = values[0];
            }
            else return false;
        } else if (condition.id == 0x40 && !values.empty()) {
            if (values[0] == 0) parts.push_back(L"我军阶段开始");
            else if (values[0] == 1) parts.push_back(L"友军阶段开始");
            else if (values[0] == 2) parts.push_back(L"敌军阶段开始");
            else return false;
        } else if (condition.id == 0x41 && values.size() >= 4) {
            static const wchar_t* camps[] = {
                L"我军", L"友军", L"敌军", L"援军", L"我军及友军", L"敌军及援军", L"全部部队"
            };
            const int camp = values[0], count = values[1], comparison = values[2], scope = values[3];
            if (camp < 0 || camp >= static_cast<int>(_countof(camps)) || count < 0 ||
                comparison < 0 || comparison > 2 || (scope != 0 && scope != 1)) return false;
            if (scope == 0 && (camp == 2 || camp == 5) && count == 0 && comparison == 2) {
                parts.push_back(L"全灭敌军");
            } else {
                static const wchar_t* comparisons[] = {L"不少于", L"少于", L"等于"};
                std::wstring area = L"战场上的";
                if (scope == 1) {
                    if (values.size() < 8) return false;
                    for (size_t i = 4; i < 8; ++i) if (values[i] < 0) return false;
                    area = L"（" + std::to_wstring(values[4]) + L"，" + std::to_wstring(values[5]) +
                        L"）至（" + std::to_wstring(values[6]) + L"，" + std::to_wstring(values[7]) +
                        L"）区域内的";
                }
                parts.push_back(area + camps[camp] + L"数量" + comparisons[comparison] +
                    std::to_wstring(count));
            }
        } else if (condition.id == 0x42) {
            parts.push_back(L"完成本关");
        } else {
            return false;
        }
    }
    if (exactTurn >= 0) {
        if ((minimumTurn >= 0 && exactTurn < minimumTurn) || (beforeTurn >= 0 && exactTurn >= beforeTurn))
            return false;
        parts.push_back(L"第 " + std::to_wstring(exactTurn) + L" 回合");
    } else {
        if (minimumTurn >= 0 && beforeTurn >= 0 && minimumTurn >= beforeTurn) return false;
        if (minimumTurn >= 0) parts.push_back(L"第 " + std::to_wstring(minimumTurn) + L" 回合开始");
        if (beforeTurn >= 0) parts.push_back(L"第 " + std::to_wstring(beforeTurn) + L" 回合前");
    }
    if (parts.empty()) return false;
    text = parts[0];
    for (size_t i = 1; i < parts.size(); ++i) text += L"且" + parts[i];
    return true;
}

std::wstring PositionSummary(const std::vector<const GuideUnit*>& units) {
    if (units.empty()) return {};
    std::vector<std::pair<int, int>> positions;
    for (const GuideUnit* unit : units) {
        const std::pair<int, int> point(unit->x, unit->y);
        if (std::find(positions.begin(), positions.end(), point) == positions.end()) positions.push_back(point);
    }
    if (positions.size() <= 3) {
        std::wstring text;
        for (size_t i = 0; i < positions.size(); ++i) {
            if (i) text += L"、";
            text += L"（" + std::to_wstring(positions[i].first) + L"，" + std::to_wstring(positions[i].second) + L"）";
        }
        return text;
    }
    int minX = positions[0].first, maxX = minX, minY = positions[0].second, maxY = minY;
    for (const auto& point : positions) {
        minX = std::min(minX, point.first); maxX = std::max(maxX, point.first);
        minY = std::min(minY, point.second); maxY = std::max(maxY, point.second);
    }
    return L"（" + std::to_wstring(minX) + L"，" + std::to_wstring(minY) + L"）至（" +
        std::to_wstring(maxX) + L"，" + std::to_wstring(maxY) + L"）范围";
}

void AddEntry(GuideResult& result, GuideFilter filter, const std::wstring& text) {
    if (text.empty()) return;
    for (const GuideEntry& entry : result.entries) if (entry.filter == filter && entry.text == text) return;
    GuideEntry entry;
    entry.filter = filter;
    entry.text = text;
    result.entries.push_back(std::move(entry));
}

std::wstring ConditionKey(size_t event, const std::vector<GuideCondition>& conditions) {
    std::wstring key = L"#event:" + std::to_wstring(event) + L"|";
    if (conditions.empty()) return key + L"empty";
    for (const GuideCondition& condition : conditions) {
        key += L"[" + std::to_wstring(condition.id) + L":";
        for (int32_t value : condition.values) key += std::to_wstring(value) + L",";
        key += L"]";
    }
    return key;
}

std::wstring JoinGuideActions(const std::vector<std::wstring>& actions) {
    if (actions.empty()) return {};
    std::wstring text = actions[0];
    for (size_t i = 1; i < actions.size(); ++i) {
        if (actions.size() == 2) text += L"并且";
        else if (i + 1 == actions.size()) text += L"，并且";
        else text += L"；";
        text += actions[i];
    }
    return text;
}

void AddActionEntry(GuideResult& result, GuideFilter filter, const std::wstring& conditionKey,
    const std::wstring& prefix, const std::wstring& action) {
    if (conditionKey.empty() || prefix.empty() || action.empty()) return;
    for (GuideEntry& entry : result.entries) {
        if (entry.filter != filter || entry.conditionKey != conditionKey || entry.prefix != prefix) continue;
        if (std::find(entry.actions.begin(), entry.actions.end(), action) == entry.actions.end())
            entry.actions.push_back(action);
        entry.text = prefix + JoinGuideActions(entry.actions) + L"。";
        return;
    }
    GuideEntry entry;
    entry.filter = filter;
    entry.conditionKey = conditionKey;
    entry.prefix = prefix;
    entry.actions.push_back(action);
    entry.text = prefix + action + L"。";
    result.entries.push_back(std::move(entry));
}

void CollectDeployments(const GuideContext& context, std::vector<GuideUnit>& units) {
    int playerSlot = 0;
    for (const GuideCommand& command : context.commands) {
        if (command.id == 0x46 && command.values.size() == 11u * 20u) {
            for (size_t i = 0; i < 20; ++i) {
                const size_t base = i * 11;
                const int id = command.values[base];
                const int x = command.values[base + 2];
                const int y = command.values[base + 3];
                const int policy = command.values[base + 7];
                if (id < 0 || id >= 1024 || x < 0 || y < 0 || x >= 256 || y >= 256 ||
                    policy < 0 || policy > 6) continue;
                units.push_back({id, 20 + static_cast<int>(i), x, y, policy,
                    command.values[base + 1] != 0, false, GuideUnit::Camp::Friend});
            }
        } else if (command.id == 0x47 && command.values.size() == 12u * 80u) {
            for (size_t i = 0; i < 80; ++i) {
                const size_t base = i * 12;
                const int id = command.values[base];
                const int x = command.values[base + 3];
                const int y = command.values[base + 4];
                const int policy = command.values[base + 8];
                if (id < 0 || id >= 1024 || x < 0 || y < 0 || x >= 256 || y >= 256 ||
                    policy < 0 || policy > 6) continue;
                units.push_back({id, 60 + static_cast<int>(i), x, y, policy,
                    command.values[base + 2] != 0, command.values[base + 1] != 0,
                    GuideUnit::Camp::Enemy});
            }
        } else if (command.id == 0x4B && command.values.size() >= 5) {
            const int id = command.values[0];
            const int x = command.values[1];
            const int y = command.values[2];
            if (id >= 0 && id < 1024 && x >= 0 && y >= 0 && x < 256 && y < 256)
                units.push_back({id, playerSlot, x, y, -1, command.values[4] != 0,
                    false, GuideUnit::Camp::Player});
            ++playerSlot;
        }
    }
}

std::vector<const GuideCommand*> CommandsInBlock(const GuideContext& context, size_t block) {
    std::vector<const GuideCommand*> result;
    for (const GuideCommand& command : context.commands) if (command.block == block) result.push_back(&command);
    return result;
}

const GuideCommand* DuelInBlock(const GuideContext& context, size_t block, size_t beforeOrdinal = SIZE_MAX) {
    const GuideCommand* result = nullptr;
    for (const GuideCommand& command : context.commands)
        if (command.block == block && command.ordinal < beforeOrdinal && command.id == 0x68 &&
            command.values.size() >= 2) result = &command;
    return result;
}

void AddDialogEntries(const GuideContext& context, GuideResult& result) {
    std::vector<size_t> visited;
    for (const GuideCommand& seed : context.commands) {
        if (std::find(visited.begin(), visited.end(), seed.block) != visited.end()) continue;
        visited.push_back(seed.block);
        const std::vector<const GuideCommand*> commands = CommandsInBlock(context, seed.block);
        const GuideCommand* duel = DuelInBlock(context, seed.block);
        std::vector<std::wstring> speakers;
        const GuideCommand* representative = nullptr;
        for (const GuideCommand* command : commands) {
            if (command->id != 0x14 && command->id != 0x15 && command->id != 0x16 &&
                command->id != 0x63 && command->id != 0x69) continue;
            if (!representative) representative = command;
            if (command->id == 0x15 && command->values.size() >= 2) {
                AddUnique(speakers, PersonName(context, command->values[0]));
                AddUnique(speakers, PersonName(context, command->values[1]));
            }
            for (const std::wstring& name : ExtractSpeakers(context, *command)) AddUnique(speakers, name);
        }
        if (representative && !speakers.empty()) {
            std::wstring conditions;
            bool simpleAdjacent = false;
            std::pair<int, int> adjacent;
            if (RenderConditions(context, representative->conditions, conditions, simpleAdjacent, adjacent)) {
                const std::wstring prefix = simpleAdjacent ? conditions + L"后，" : WhenLead(conditions);
                const std::wstring action = simpleAdjacent ? L"触发对话" : JoinNames(speakers) + L"触发对话";
                AddActionEntry(result, GuideFilter::Dialog,
                    ConditionKey(representative->event, representative->conditions), prefix, action);
            }
        }
        if (!duel || duel->values.size() < 2) continue;
        const std::wstring first = PersonName(context, duel->values[0]);
        const std::wstring second = PersonName(context, duel->values[1]);
        if (first.empty() || second.empty()) continue;
        std::wstring conditions;
        bool simpleAdjacent = false;
        std::pair<int, int> adjacent;
        if (!RenderConditions(context, duel->conditions, conditions, simpleAdjacent, adjacent)) continue;
        const bool matchingAdjacent = simpleAdjacent &&
            ((adjacent.first == duel->values[0] && adjacent.second == duel->values[1]) ||
                (adjacent.first == duel->values[1] && adjacent.second == duel->values[0]));
        const std::wstring prefix = matchingAdjacent ? conditions + L"后，" : WhenLead(conditions);
        const std::wstring action = matchingAdjacent ? L"触发单挑" : first + L"与" + second + L"单挑";
        AddActionEntry(result, GuideFilter::Dialog,
            ConditionKey(duel->event, duel->conditions), prefix, action);
    }
}

void AddAmbushEntries(const GuideContext& context, GuideResult& result) {
    struct AmbushGroup {
        std::wstring conditionKey;
        std::wstring conditions;
        std::vector<const GuideUnit*> units;
    };
    std::vector<AmbushGroup> groups;
    for (const GuideCommand& command : context.commands) {
        if (command.id != 0x4C || command.values.size() < 3) continue;
        std::wstring conditions;
        bool simpleAdjacent = false;
        std::pair<int, int> adjacent;
        if (!RenderConditions(context, command.conditions, conditions, simpleAdjacent, adjacent)) continue;
        std::vector<const GuideUnit*> matches;
        for (const GuideUnit& unit : context.units) {
            if (unit.camp != GuideUnit::Camp::Enemy || !unit.hidden) continue;
            if ((command.values[0] == 0 && unit.id == command.values[1]) ||
                (command.values[0] == 1 && unit.battlefieldNumber == command.values[2])) matches.push_back(&unit);
        }
        if (matches.empty()) continue;
        const std::wstring conditionKey = ConditionKey(command.event, command.conditions);
        auto group = std::find_if(groups.begin(), groups.end(), [&](const AmbushGroup& item) {
            return item.conditionKey == conditionKey && item.conditions == conditions;
        });
        if (group == groups.end()) {
            groups.push_back({conditionKey, conditions, {}});
            group = groups.end() - 1;
        }
        for (const GuideUnit* unit : matches)
            if (std::find(group->units.begin(), group->units.end(), unit) == group->units.end())
                group->units.push_back(unit);
    }
    for (const AmbushGroup& group : groups) {
        const std::wstring positions = PositionSummary(group.units);
        if (positions.empty()) continue;
        const std::wstring count = std::to_wstring(group.units.size()) + L" 支";
        AddEntry(result, GuideFilter::Ambush,
            WhenLead(group.conditions) + count + L"隐藏敌军在" + positions + L"出现。");
    }
}

void AddAiEntries(const GuideContext& context, GuideResult& result) {
    for (const GuideCommand& command : context.commands) {
        if (command.id != 0x4E || command.values.size() < 8) continue;
        std::wstring conditions;
        bool simpleAdjacent = false;
        std::pair<int, int> adjacent;
        if (!RenderConditions(context, command.conditions, conditions, simpleAdjacent, adjacent)) continue;
        const int newPolicy = command.values[7];
        const std::wstring newName = PolicyName(newPolicy);
        if (newName.empty()) continue;
        std::vector<const GuideUnit*> targets;
        if (command.values[0] == 0) {
            for (const GuideUnit& unit : context.units)
                if (unit.camp == GuideUnit::Camp::Enemy && unit.id == command.values[1]) targets.push_back(&unit);
        } else if (command.values[0] == 1) {
            const int camp = command.values[6];
            if (camp != 2 && camp != 5 && camp != 6) continue;
            const int x1 = std::min(command.values[2], command.values[4]);
            const int x2 = std::max(command.values[2], command.values[4]);
            const int y1 = std::min(command.values[3], command.values[5]);
            const int y2 = std::max(command.values[3], command.values[5]);
            for (const GuideUnit& unit : context.units)
                if (unit.camp == GuideUnit::Camp::Enemy && unit.x >= x1 && unit.x <= x2 &&
                    unit.y >= y1 && unit.y <= y2) targets.push_back(&unit);
        } else continue;
        for (int oldPolicy = 0; oldPolicy <= 6; ++oldPolicy) {
            std::vector<const GuideUnit*> group;
            for (const GuideUnit* unit : targets) if (unit->policy == oldPolicy && oldPolicy != newPolicy) group.push_back(unit);
            const std::wstring positions = PositionSummary(group);
            const std::wstring oldName = PolicyName(oldPolicy);
            if (positions.empty() || oldName.empty()) continue;
            AddActionEntry(result, GuideFilter::Ai,
                ConditionKey(command.event, command.conditions), WhenLead(conditions),
                L"初始部署于" + positions + L"的敌军由" + oldName + L"改为" + newName);
        }
    }
}

std::wstring RewardPrefix(const GuideContext& context, const GuideCommand& command) {
    const GuideCommand* duel = DuelInBlock(context, command.block, command.ordinal);
    if (duel && duel->values.size() >= 2) {
        const std::wstring first = PersonName(context, duel->values[0]);
        const std::wstring second = PersonName(context, duel->values[1]);
        if (!first.empty() && !second.empty()) return L"完成" + first + L"与" + second + L"的单挑后，";
    }
    std::wstring conditions;
    bool simpleAdjacent = false;
    std::pair<int, int> adjacent;
    if (!RenderConditions(context, command.conditions, conditions, simpleAdjacent, adjacent)) return {};
    return AfterLead(conditions);
}

std::wstring JoinedAbnormalNames(unsigned mask) {
    static const struct { unsigned bit; const wchar_t* name; } names[] = {
        {2u, L"麻痹"}, {4u, L"封咒"}, {8u, L"混乱"}, {16u, L"中毒"}
    };
    std::wstring result;
    for (const auto& item : names) {
        if ((mask & item.bit) == 0) continue;
        if (!result.empty()) result += L"和";
        result += item.name;
    }
    return result;
}

std::wstring AttributeChangeText(unsigned mask, int change) {
    if (!mask || change < 0 || change > 2) return {};
    const wchar_t* suffix = change == 0 ? L"下降" : change == 1 ? L"恢复正常" : L"上升";
    std::wstring result;
    const unsigned abilities = mask & 0x1Fu;
    if (abilities == 0x1Fu) result = L"全能力";
    else {
        static const wchar_t* shortNames[] = {L"攻", L"防", L"精", L"爆", L"士"};
        static const wchar_t* fullNames[] = {L"攻击", L"防御", L"精神", L"爆发", L"士气"};
        if (abilities && (abilities & (abilities - 1)) == 0) {
            for (size_t i = 0; i < _countof(fullNames); ++i)
                if (abilities & (1u << i)) result = fullNames[i];
        } else {
            for (size_t i = 0; i < _countof(shortNames); ++i)
                if (abilities & (1u << i)) result += shortNames[i];
        }
    }
    if (mask & (1u << 5)) {
        if (!result.empty()) result += L"和";
        result += L"移动力";
    }
    return result.empty() ? std::wstring() : result + suffix;
}

std::wstring JoinStateEffects(const std::vector<std::wstring>& effects) {
    std::wstring result;
    for (size_t i = 0; i < effects.size(); ++i) {
        if (i) result += i + 1 == effects.size() ? L"并" : L"、";
        result += effects[i];
    }
    return result;
}

std::wstring FormatStateSummary(unsigned down, unsigned normal, unsigned up,
    unsigned abnormalApply, unsigned abnormalClear, int hpLoss, int mpLoss) {
    std::vector<std::wstring> effects;
    for (const auto& change : {std::pair<unsigned, int>(down, 0), {normal, 1}, {up, 2}}) {
        const std::wstring text = AttributeChangeText(change.first, change.second);
        if (!text.empty()) effects.push_back(text);
    }
    const unsigned knownAbnormal = 2u | 4u | 8u | 16u;
    if ((abnormalClear & knownAbnormal) == knownAbnormal) effects.push_back(L"解除异常");
    else {
        const std::wstring names = JoinedAbnormalNames(abnormalClear);
        if (!names.empty()) effects.push_back(L"解除" + names);
    }
    if ((abnormalApply & knownAbnormal) == knownAbnormal) effects.push_back(L"陷入异常");
    else {
        const std::wstring names = JoinedAbnormalNames(abnormalApply);
        if (!names.empty()) effects.push_back(L"陷入" + names);
    }
    if (hpLoss > 0) effects.push_back(L"HP减少 " + std::to_wstring(hpLoss));
    if (mpLoss > 0) effects.push_back(L"MP减少 " + std::to_wstring(mpLoss));
    return JoinStateEffects(effects);
}

std::wstring CampTargetName(int camp) {
    static const wchar_t* names[] = {
        L"我军", L"友军", L"敌军", L"援军", L"我军及友军", L"敌军及援军", L"全部部队"
    };
    return camp >= 0 && camp < static_cast<int>(_countof(names)) ? names[camp] : std::wstring();
}

bool StateTargetText(const GuideContext& context, const GuideCommand& command, std::wstring& text) {
    if (command.values.size() < 13) return false;
    if (command.values[0] == 0) {
        text = ConditionPersonName(context, command.values[1]);
        return !text.empty();
    }
    if (command.values[0] == 1) {
        const GuideUnit* unit = UnitByBattlefieldNumber(context, command.values[2]);
        text = unit ? PersonName(context, unit->id) : std::wstring();
        if (text.empty()) text = L"战场编号 " + std::to_wstring(command.values[2]) + L" 的部队";
        return true;
    }
    if (command.values[0] != 2) return false;
    const std::wstring camp = CampTargetName(command.values[7]);
    if (camp.empty()) return false;
    for (size_t i = 3; i <= 6; ++i)
        if (command.values[i] < 0 || command.values[i] >= 256) return false;
    const int x1 = std::min(command.values[3], command.values[5]);
    const int y1 = std::min(command.values[4], command.values[6]);
    const int x2 = std::max(command.values[3], command.values[5]);
    const int y2 = std::max(command.values[4], command.values[6]);
    text = L"（" + std::to_wstring(x1) + L"，" + std::to_wstring(y1) + L"）至（" +
        std::to_wstring(x2) + L"，" + std::to_wstring(y2) + L"）区域内的" + camp;
    return true;
}

void AddStateChangeEntries(const GuideContext& context, GuideResult& result) {
    struct Group {
        size_t block = 0;
        std::wstring conditionKey;
        std::wstring prefix;
        std::wstring target;
        unsigned changes[3] = {};
        unsigned abnormalApply = 0;
        unsigned abnormalClear = 0;
        int hpLoss = 0;
        int mpLoss = 0;
        size_t lastOrdinal = 0;
    };
    std::vector<Group> groups;
    for (const GuideCommand& command : context.commands) {
        if (command.id != 0x4D || command.values.size() < 13) continue;
        const std::wstring prefix = RewardPrefix(context, command);
        const std::wstring conditionKey = ConditionKey(command.event, command.conditions);
        std::wstring target;
        if (prefix.empty() || !StateTargetText(context, command, target)) continue;
        auto found = groups.end();
        if (!groups.empty() && groups.back().block == command.block &&
            groups.back().conditionKey == conditionKey && groups.back().prefix == prefix &&
            groups.back().target == target &&
            groups.back().lastOrdinal + 1 == command.ordinal) found = groups.end() - 1;
        if (found == groups.end()) {
            groups.push_back({command.block, conditionKey, prefix, target});
            found = groups.end() - 1;
        }
        found->lastOrdinal = command.ordinal;
        const int attribute = command.values[8];
        const int change = command.values[9];
        if (attribute >= 0 && attribute <= 5 && change >= 0 && change <= 2)
            found->changes[change] |= 1u << attribute;
        const int abnormal = command.values[10];
        if (abnormal > 0) {
            const unsigned bits = static_cast<unsigned>(abnormal) & 0x7Eu;
            if (abnormal & 0x80) found->abnormalClear |= bits;
            else found->abnormalApply |= bits;
        }
        found->hpLoss = std::max(found->hpLoss, command.values[11]);
        found->mpLoss = std::max(found->mpLoss, command.values[12]);
    }
    for (const Group& group : groups) {
        const std::wstring summary = FormatStateSummary(group.changes[0], group.changes[1],
            group.changes[2], group.abnormalApply, group.abnormalClear, group.hpLoss, group.mpLoss);
        if (!summary.empty())
            AddActionEntry(result, GuideFilter::Rewards, group.conditionKey, group.prefix,
                group.target + summary);
    }
}

bool SingleRetreatTarget(const GuideContext& context, const GuideCommand& command,
    int& id, PersonSide& side, std::wstring& person) {
    id = -1;
    side = PersonSide::Unknown;
    if (command.values.size() < 3) return false;
    if (command.values[0] == 0) {
        id = command.values[1];
        person = ConditionPersonName(context, id);
        side = SideForPerson(context, id);
        if (id == 1025) side = PersonSide::Friendly;
        if (id == 1026) side = PersonSide::Enemy;
        return !person.empty();
    }
    if (command.values[0] != 1) return false;
    const GuideUnit* unit = UnitByBattlefieldNumber(context, command.values[2]);
    if (!unit) return false;
    id = unit->id;
    person = PersonName(context, id);
    side = unit->camp == GuideUnit::Camp::Enemy ? PersonSide::Enemy : PersonSide::Friendly;
    return !person.empty();
}

bool HasMatchingRetreatCondition(const GuideCommand& command, int personId) {
    for (const GuideCondition& condition : command.conditions) {
        if (condition.id == 0x36 && condition.values.size() >= 4 &&
            condition.values[0] == personId && condition.values[1] == 7 &&
            condition.values[2] == 0 && condition.values[3] == 2) return true;
    }
    return false;
}

bool HasNarrativeTriggerBefore(const GuideContext& context, const GuideCommand& command) {
    for (const GuideCommand& candidate : context.commands) {
        if (candidate.event != command.event || candidate.ordinal >= command.ordinal) continue;
        if (candidate.id == 0x14 || candidate.id == 0x15 || candidate.id == 0x16 ||
            candidate.id == 0x63 || candidate.id == 0x68 || candidate.id == 0x69) return true;
    }
    return false;
}

void AddRewardEntries(const GuideContext& context, GuideResult& result) {
    static const wchar_t* attributes[] = {
        L"攻击力", L"防御力", L"精神力", L"爆发力", L"士气", L"HP", L"MP",
        L"当前 HP", L"当前 MP", L"等级", L"武力", L"统率", L"智力", L"敏捷", L"运气"
    };
    AddStateChangeEntries(context, result);
    std::vector<std::vector<std::wstring>> battleRewards;
    std::vector<int> battleMoney;
    bool unresolvedBattleReward = false;
    for (const GuideCommand& command : context.commands) {
        if (command.id != 0x59 || command.values.size() < 8) continue;
        battleMoney.push_back(command.values[0]);
        std::vector<std::wstring> items;
        for (size_t slot : {size_t(1), size_t(3), size_t(5)}) {
            const int itemId = command.values[slot];
            if (itemId < 0 || itemId == 255) continue;
            const std::wstring item = ItemName(context, itemId);
            if (item.empty()) unresolvedBattleReward = true;
            else AddUnique(items, item);
        }
        battleRewards.push_back(std::move(items));
    }
    if (!unresolvedBattleReward && !battleRewards.empty()) {
        std::vector<std::wstring> common = battleRewards.front();
        for (size_t i = 1; i < battleRewards.size(); ++i) {
            common.erase(std::remove_if(common.begin(), common.end(), [&](const std::wstring& item) {
                return std::find(battleRewards[i].begin(), battleRewards[i].end(), item) == battleRewards[i].end();
            }), common.end());
        }
        if (!common.empty())
            AddActionEntry(result, GuideFilter::Rewards, L"#battle-complete", L"完成本关后，",
                L"获得" + JoinItems(common));
    }
    if (!battleMoney.empty() && battleMoney.front() > 0 &&
        std::all_of(battleMoney.begin() + 1, battleMoney.end(), [&](int value) {
            return value == battleMoney.front();
        })) {
        AddActionEntry(result, GuideFilter::Rewards, L"#battle-complete", L"完成本关后，",
            L"获得 " + std::to_wstring(battleMoney.front()) + L" 金钱");
    }
    for (const GuideCommand& command : context.commands) {
        if (command.id == 0x59) continue;
        const std::wstring prefix = RewardPrefix(context, command);
        if (prefix.empty()) continue;
        const std::wstring conditionKey = ConditionKey(command.event, command.conditions);
        if (command.id == 0x3D && !command.values.empty()) {
            const std::wstring item = ItemName(context, command.values[0]);
            if (!item.empty()) AddActionEntry(result, GuideFilter::Rewards, conditionKey, prefix,
                L"获得" + item);
        } else if (command.id == 0x38 && command.values.size() >= 4) {
            const std::wstring person = PersonName(context, command.values[0]);
            const int attribute = command.values[1], operation = command.values[2], amount = command.values[3];
            if (!person.empty() && attribute >= 0 && attribute < static_cast<int>(_countof(attributes)) &&
                (operation == 1 || operation == 2) && amount > 0)
                AddActionEntry(result, GuideFilter::Rewards, conditionKey, prefix,
                    person + attributes[attribute] + (operation == 1 ? L"提升 " : L"下降 ") +
                    std::to_wstring(amount) + L" 点");
        } else if (command.id == 0x3A && command.values.size() >= 3 && command.values[0] == 0 &&
            (command.values[1] == 1 || command.values[1] == 2) && command.values[2] > 0) {
            AddActionEntry(result, GuideFilter::Rewards, conditionKey, prefix,
                (command.values[1] == 1 ? L"获得 " : L"失去 ") +
                std::to_wstring(command.values[2]) + L" 金钱");
        } else if (command.id == 0x3B && command.values.size() >= 2 && command.values[1] <= 1) {
            const std::wstring person = PersonName(context, command.values[0]);
            if (!person.empty()) AddActionEntry(result, GuideFilter::Rewards, conditionKey, prefix,
                person + L"加入队伍");
        } else if (command.id == 0x39 && command.values.size() >= 2 && command.values[1] > 0) {
            const std::wstring person = PersonName(context, command.values[0]);
            if (!person.empty()) AddActionEntry(result, GuideFilter::Rewards, conditionKey, prefix,
                person + L"提升 " + std::to_wstring(command.values[1]) + L" 级");
        } else if (command.id == 0x53 && command.values.size() >= 8) {
            int personId = -1;
            PersonSide side = PersonSide::Unknown;
            std::wstring person;
            if (!SingleRetreatTarget(context, command, personId, side, person)) continue;
            if (HasMatchingRetreatCondition(command, personId) ||
                !HasNarrativeTriggerBefore(context, command)) continue;
            if (command.values[7] != 0)
                AddActionEntry(result, GuideFilter::Rewards, conditionKey, prefix, person + L"死亡");
            else
                AddActionEntry(result, GuideFilter::Rewards, conditionKey, prefix, person + L"撤退");
        }
    }
}

void MergeAndPruneNarrativeEntries(GuideResult& result) {
    std::vector<bool> keep(result.entries.size(), true);
    for (size_t dialogIndex = 0; dialogIndex < result.entries.size(); ++dialogIndex) {
        GuideEntry& dialog = result.entries[dialogIndex];
        if (dialog.filter != GuideFilter::Dialog) continue;
        bool hasUsefulResult = false;
        for (GuideEntry& reward : result.entries) {
            if (reward.filter != GuideFilter::Rewards ||
                reward.conditionKey != dialog.conditionKey) continue;
            for (const std::wstring& action : reward.actions) {
                if (std::find(dialog.actions.begin(), dialog.actions.end(), action) == dialog.actions.end())
                    dialog.actions.push_back(action);
            }
            reward.hideInAll = true;
            hasUsefulResult = true;
        }
        if (hasUsefulResult) dialog.text = dialog.prefix + JoinGuideActions(dialog.actions) + L"。";
        else keep[dialogIndex] = false;
    }
    size_t index = 0;
    result.entries.erase(std::remove_if(result.entries.begin(), result.entries.end(),
        [&](const GuideEntry&) { return !keep[index++]; }), result.entries.end());
}

std::wstring FileTitle(const std::wstring& path, int number);

GuideResult AnalyzeScenario(const GuideFile& file, const std::wstring& root, const std::atomic<bool>& cancelled) {
    GuideResult result;
    result.title = FileTitle(file.battlefieldPath, file.battlefieldNumber) + L"简易攻略";
    std::vector<unsigned char> bytes;
    if (cancelled || !ReadFileBytes(file.battlefieldPath, bytes)) return result;
    GuideContext context;
    if (cancelled || !ParseEex(bytes, context.commands, &cancelled)) return result;
    if (cancelled) return result;
    LoadNames(root, context);
    if (cancelled) return result;
    CollectDeployments(context, context.units);
    AddDialogEntries(context, result);
    AddAmbushEntries(context, result);
    AddAiEntries(context, result);
    AddRewardEntries(context, result);
    MergeAndPruneNarrativeEntries(result);
    return result;
}

std::wstring FileTitle(const std::wstring& path, int number) {
    (void)path;
    return std::wstring(L"第 ") + (number < 10 ? L"0" : L"") + std::to_wstring(number) + L" 关";
}

bool ParseScenarioName(const std::wstring& path, wchar_t& kind, int& number, std::wstring* name = nullptr) {
    const wchar_t* slash = wcsrchr(path.c_str(), L'\\');
    const wchar_t* forward = wcsrchr(path.c_str(), L'/');
    const wchar_t* fileName = slash && (!forward || slash > forward) ? slash + 1 : forward ? forward + 1 : path.c_str();
    if ((fileName[0] != L'R' && fileName[0] != L'r' && fileName[0] != L'S' && fileName[0] != L's') ||
        fileName[1] != L'_') return false;
    wchar_t* end = nullptr;
    const long parsed = wcstol(fileName + 2, &end, 10);
    if (end == fileName + 2 || parsed < 0 || parsed > 999 || _wcsicmp(end, L".eex") != 0) return false;
    kind = static_cast<wchar_t>(towupper(fileName[0]));
    number = static_cast<int>(parsed);
    if (name) *name = fileName;
    return true;
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring ScenarioPath(const std::wstring& root, wchar_t kind, int number) {
    wchar_t name[32] = {};
    swprintf_s(name, L"%c_%02d.eex", kind, number);
    return root + L"\\RS\\" + name;
}

bool ScenarioJumpName(int rawValue, std::wstring& target) {
    const int64_t adjusted = static_cast<int64_t>(rawValue) + 1;
    if (adjusted <= 0) return false;
    const wchar_t kind = (adjusted & 1) == 0 ? L'R' : L'S';
    const int64_t number = adjusted / 2;
    if (number < 0 || number > 999) return false;
    wchar_t name[32] = {};
    swprintf_s(name, L"%c_%02d.eex", kind, static_cast<int>(number));
    target = name;
    return true;
}

bool ReadScenarioJumpNames(const std::wstring& scenarioPath, std::vector<std::wstring>& targets) {
    targets.clear();
    std::vector<unsigned char> bytes;
    std::vector<GuideCommand> commands;
    if (!ReadFileBytes(scenarioPath, bytes) || !ParseEex(bytes, commands)) return false;
    for (const GuideCommand& command : commands) {
        if (command.id != 0x11 || command.values.empty()) continue;
        std::wstring target;
        if (ScenarioJumpName(command.values[0], target) &&
            std::find(targets.begin(), targets.end(), target) == targets.end()) targets.push_back(target);
    }
    return true;
}

bool ResolveBattlefieldPath(const std::wstring& scenarioPath, const std::wstring& root,
    std::wstring& battlefieldPath, int& battlefieldNumber) {
    wchar_t kind = 0;
    int number = 0;
    if (!ParseScenarioName(scenarioPath, kind, number)) return false;
    if (kind == L'S') {
        battlefieldPath = scenarioPath;
        battlefieldNumber = number;
        return true;
    }
    std::vector<std::wstring> targets;
    if (ReadScenarioJumpNames(scenarioPath, targets)) {
        for (const std::wstring& targetName : targets) {
            wchar_t targetKind = 0;
            int target = 0;
            if (!ParseScenarioName(targetName, targetKind, target) || targetKind != L'S') continue;
            const std::wstring candidate = ScenarioPath(root, targetKind, target);
            if (!FileExists(candidate)) continue;
            battlefieldPath = candidate;
            battlefieldNumber = target;
            return true;
        }
    }
    battlefieldPath = ScenarioPath(root, L'S', number);
    battlefieldNumber = number;
    return true;
}

bool BuildGuideFile(const std::wstring& scenarioPath, const std::wstring& root, GuideFile& file) {
    wchar_t kind = 0;
    int number = 0;
    std::wstring name;
    if (!ParseScenarioName(scenarioPath, kind, number, &name)) return false;
    file = GuideFile();
    file.path = scenarioPath;
    file.name = name;
    file.number = number;
    file.isBattlefield = kind == L'S';
    if (!ResolveBattlefieldPath(scenarioPath, root, file.battlefieldPath, file.battlefieldNumber)) return false;
    const std::wstring source = std::wstring(1, kind) + L"_" + (number < 10 ? L"0" : L"") +
        std::to_wstring(number);
    const std::wstring target = L"S_" + std::wstring(file.battlefieldNumber < 10 ? L"0" : L"") +
        std::to_wstring(file.battlefieldNumber);
    file.title = source;
    if (!file.isBattlefield) file.title += L" → " + target;
    file.title += L" / " + FileTitle(file.battlefieldPath, file.battlefieldNumber);
    return true;
}

void ScanPattern(const wchar_t* patternName) {
    const std::wstring pattern = gGuideRoot + L"\\RS\\" + patternName;
    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        GuideFile file;
        if (BuildGuideFile(gGuideRoot + L"\\RS\\" + data.cFileName, gGuideRoot, file))
            gGuideFiles.push_back(std::move(file));
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

void ScanFiles() {
    gGuideFiles.clear();
    ScanPattern(L"R_*.eex");
    ScanPattern(L"S_*.eex");
    std::sort(gGuideFiles.begin(), gGuideFiles.end(), [](const GuideFile& left, const GuideFile& right) {
        if (left.number != right.number) return left.number < right.number;
        return left.isBattlefield < right.isBattlefield;
    });
}

std::wstring ObservedScenarioName() {
    AcquireSRWLockShared(&gObservedScenarioLock);
    const std::wstring result = gObservedScenarioName;
    ReleaseSRWLockShared(&gObservedScenarioLock);
    return result;
}

int DefaultScenarioIndex() {
    const std::wstring observed = ObservedScenarioName();
    if (!observed.empty()) {
        for (size_t i = 0; i < gGuideFiles.size(); ++i)
            if (!_wcsicmp(gGuideFiles[i].name.c_str(), observed.c_str())) return static_cast<int>(i);
    }
    for (size_t i = 0; i < gGuideFiles.size(); ++i)
        if (gGuideFiles[i].isBattlefield && gGuideFiles[i].number == 0) return static_cast<int>(i);
    return gGuideFiles.empty() ? -1 : 0;
}

void SetGuideText(const std::wstring& text) {
    if (!gGuideText) return;
    SetWindowTextW(gGuideText, text.c_str());
    if (gGuideUsesRichEdit) {
        CHARFORMAT2W character = {};
        character.cbSize = sizeof(character);
        character.dwMask = CFM_FACE | CFM_SIZE;
        character.yHeight = 220;
        wcscpy_s(character.szFaceName, L"Microsoft YaHei UI");
        PARAFORMAT2 format = {};
        format.cbSize = sizeof(format);
        format.dwMask = PFM_SPACEAFTER;
        format.bLineSpacingRule = 1;
        format.dyLineSpacing = 0;
        format.dySpaceAfter = 45;
        SendMessageW(gGuideText, EM_SETSEL, 0, -1);
        SendMessageW(gGuideText, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&character));
        SendMessageW(gGuideText, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&format));
        SendMessageW(gGuideText, EM_SETSEL, 0, 0);
        SendMessageW(gGuideText, EM_SCROLLCARET, 0, 0);
    }
}

std::wstring RenderGuideText(const GuideResult& result, GuideFilter filter) {
    std::wstring output = result.title + L"\r\n";
    const GuideFilter filters[] = {GuideFilter::Dialog, GuideFilter::Ambush, GuideFilter::Ai, GuideFilter::Rewards};
    const wchar_t* headings[] = {L"一、人物对话与单挑", L"二、伏兵", L"三、敌军 AI", L"四、状态与奖惩"};
    bool any = false;
    for (size_t group = 0; group < _countof(filters); ++group) {
        if (filter != GuideFilter::All && filter != filters[group]) continue;
        std::vector<std::wstring> lines;
        for (const GuideEntry& entry : result.entries) {
            if (entry.filter == filters[group] &&
                !(filter == GuideFilter::All && entry.hideInAll)) lines.push_back(entry.text);
        }
        if (lines.empty()) continue;
        std::wstring section = std::wstring(headings[group]) + L"\r\n";
        bool sectionHasEntry = false;
        for (const std::wstring& line : lines) {
            const std::wstring rendered = L"  " + line + L"\r\n";
            if (output.size() + section.size() + rendered.size() + 2 > kGuideTextCharacters) break;
            section += rendered;
            sectionHasEntry = true;
        }
        if (!sectionHasEntry) continue;
        any = true;
        output += section;
    }
    if (!any) output += L"本关暂无可整理的攻略信息。";
    return output;
}

void RenderGuide() {
    SetGuideText(RenderGuideText(gGuideResult, gGuideCurrentFilter));
}

void StartRead() {
    if (!gGuideWindow || gGuideFiles.empty() || !gGuideScenarioBox) return;
    const int index = static_cast<int>(SendMessageW(gGuideScenarioBox, CB_GETCURSEL, 0, 0));
    if (index < 0 || static_cast<size_t>(index) >= gGuideFiles.size()) return;
    if (gGuideTask) gGuideTask->cancelled.store(true);
    auto state = std::make_shared<GuideTaskState>();
    state->window = gGuideWindow;
    state->request = ++gGuideRequest;
    const unsigned request = state->request;
    const GuideFile file = gGuideFiles[static_cast<size_t>(index)];
    const std::wstring root = gGuideRoot;
    gGuideTask = state;
    gGuideResult = GuideResult();
    gGuideResult.title = FileTitle(file.battlefieldPath, file.battlefieldNumber) + L"简易攻略";
    SetGuideText(L"正在整理本关攻略……");
    try {
        std::thread([state, request, file, root]() {
            try {
                state->result = AnalyzeScenario(file, root, state->cancelled);
            } catch (...) {
                state->result = GuideResult();
                state->result.title = FileTitle(file.battlefieldPath, file.battlefieldNumber) + L"简易攻略";
            }
            if (state->cancelled.load()) return;
            state->completed.store(true, std::memory_order_release);
            if (IsWindow(state->window)) PostMessageW(state->window, kGuideReadComplete, request, 0);
        }).detach();
    } catch (...) {
        state->cancelled.store(true);
        gGuideTask.reset();
        RenderGuide();
    }
}

void LayoutGuide(HWND window) {
    RECT client = {};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int margin = 10;
    MoveWindow(gGuideLevelLabel, margin, margin + 3, 52, 20, TRUE);
    MoveWindow(gGuideScenarioBox, margin + 52, margin, std::max(100, width - 262), 24, TRUE);
    MoveWindow(gGuideFilterBox, width - 190, margin, 100, 24, TRUE);
    MoveWindow(gGuideReload, width - 80, margin, 70, 24, TRUE);
    MoveWindow(gGuideText, margin, 42, std::max(100, width - 2 * margin), std::max(80, height - 52), TRUE);
    if (gGuideText && gGuideUsesRichEdit) {
        RECT textRect = {};
        GetClientRect(gGuideText, &textRect);
        textRect.left += 10;
        textRect.right = std::max(textRect.left + 1, textRect.right - 10);
        SendMessageW(gGuideText, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&textRect));
    }
}

void SetGuideIcon(HWND window) {
    HICON large = static_cast<HICON>(LoadImageW(gInstance, MAKEINTRESOURCEW(IDI_DORO_WHITE), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR | LR_SHARED));
    HICON smallIcon = static_cast<HICON>(LoadImageW(gInstance, MAKEINTRESOURCEW(IDI_DORO_WHITE), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED));
    if (large) SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large));
    if (smallIcon) SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
}

HFONT CreateGuideFont(HWND window) {
    int dpi = 96;
    HDC dc = GetDC(window);
    if (dc) {
        const int detected = GetDeviceCaps(dc, LOGPIXELSY);
        if (detected > 0) dpi = detected;
        ReleaseDC(window, dc);
    }
    return CreateFontW(-MulDiv(11, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

LRESULT CALLBACK GuideProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kGuideReadComplete) {
        const unsigned request = static_cast<unsigned>(wParam);
        if (request == gGuideRequest && gGuideTask && gGuideTask->request == request &&
            gGuideTask->completed.load(std::memory_order_acquire)) {
            gGuideResult = std::move(gGuideTask->result);
            gGuideTask.reset();
            RenderGuide();
        }
        return 0;
    }
    switch (message) {
    case WM_CREATE: {
        gGuideWindow = window;
        SetGuideIcon(window);
        gGuideLevelLabel = CreateWindowExW(0, WC_STATICW, L"剧本：",
            WS_CHILD | WS_VISIBLE, 0, 0, 52, 20, window, nullptr, gInstance, nullptr);
        gGuideScenarioBox = CreateWindowExW(0, WC_COMBOBOXW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 0, 0, 100, 24, window,
            reinterpret_cast<HMENU>(kGuideScenario), gInstance, nullptr);
        gGuideFilterBox = CreateWindowExW(0, WC_COMBOBOXW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 0, 0, 100, 24, window,
            reinterpret_cast<HMENU>(kGuideFilter), gInstance, nullptr);
        gGuideReload = CreateWindowExW(0, WC_BUTTONW, L"重新读取",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 70, 24, window,
            reinterpret_cast<HMENU>(kGuideReload), gInstance, nullptr);
        gGuideRichEditModule = LoadLibraryW(L"Riched20.dll");
        gGuideUsesRichEdit = gGuideRichEditModule != nullptr;
        gGuideText = CreateWindowExW(WS_EX_CLIENTEDGE,
            gGuideUsesRichEdit ? RICHEDIT_CLASSW : WC_EDITW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            0, 0, 100, 100, window, nullptr, gInstance, nullptr);
        if (!gGuideText && gGuideUsesRichEdit) {
            FreeLibrary(gGuideRichEditModule);
            gGuideRichEditModule = nullptr;
            gGuideUsesRichEdit = false;
            gGuideText = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, nullptr,
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                0, 0, 100, 100, window, nullptr, gInstance, nullptr);
        }
        gGuideFont = CreateGuideFont(window);
        gGuideFontOwned = gGuideFont != nullptr;
        if (!gGuideFont) gGuideFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        SendMessageW(gGuideText, EM_SETLIMITTEXT, static_cast<WPARAM>(kGuideTextCharacters), 0);
        if (!gGuideUsesRichEdit)
            SendMessageW(gGuideText, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
        SendMessageW(gGuideLevelLabel, WM_SETFONT, reinterpret_cast<WPARAM>(gGuideFont), TRUE);
        SendMessageW(gGuideText, WM_SETFONT, reinterpret_cast<WPARAM>(gGuideFont), TRUE);
        SendMessageW(gGuideScenarioBox, WM_SETFONT, reinterpret_cast<WPARAM>(gGuideFont), TRUE);
        SendMessageW(gGuideFilterBox, WM_SETFONT, reinterpret_cast<WPARAM>(gGuideFont), TRUE);
        SendMessageW(gGuideReload, WM_SETFONT, reinterpret_cast<WPARAM>(gGuideFont), TRUE);
        SendMessageW(gGuideFilterBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"全部"));
        SendMessageW(gGuideFilterBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"人物对话与单挑"));
        SendMessageW(gGuideFilterBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"伏兵"));
        SendMessageW(gGuideFilterBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"敌军 AI"));
        SendMessageW(gGuideFilterBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"状态与奖惩"));
        SendMessageW(gGuideFilterBox, CB_SETCURSEL, 0, 0);
        ScanFiles();
        for (const GuideFile& file : gGuideFiles)
            SendMessageW(gGuideScenarioBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(file.title.c_str()));
        if (!gGuideFiles.empty()) {
            SendMessageW(gGuideScenarioBox, CB_SETCURSEL, DefaultScenarioIndex(), 0);
            StartRead();
        } else SetGuideText(L"本关暂无可整理的攻略信息。");
        LayoutGuide(window);
        return 0;
    }
    case WM_SIZE:
        LayoutGuide(window);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == kGuideReload && HIWORD(wParam) == BN_CLICKED) {
            if (gGuideTask) gGuideTask->cancelled.store(true);
            ++gGuideRequest;
            gGuideTask.reset();
            ScanFiles();
            SendMessageW(gGuideScenarioBox, CB_RESETCONTENT, 0, 0);
            for (const GuideFile& file : gGuideFiles)
                SendMessageW(gGuideScenarioBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(file.title.c_str()));
            if (!gGuideFiles.empty()) {
                SendMessageW(gGuideScenarioBox, CB_SETCURSEL, DefaultScenarioIndex(), 0);
                StartRead();
            } else SetGuideText(L"本关暂无可整理的攻略信息。");
            return 0;
        }
        if (LOWORD(wParam) == kGuideScenario && HIWORD(wParam) == CBN_SELCHANGE) {
            StartRead();
            return 0;
        }
        if (LOWORD(wParam) == kGuideFilter && HIWORD(wParam) == CBN_SELCHANGE) {
            const LRESULT selection = SendMessageW(gGuideFilterBox, CB_GETCURSEL, 0, 0);
            if (selection < static_cast<LRESULT>(GuideFilter::All) ||
                selection > static_cast<LRESULT>(GuideFilter::Rewards)) return 0;
            gGuideCurrentFilter = static_cast<GuideFilter>(selection);
            RenderGuide();
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_NCDESTROY:
        if (gGuideTask) gGuideTask->cancelled.store(true);
        ++gGuideRequest;
        gGuideTask.reset();
        gGuideWindow = nullptr;
        gGuideLevelLabel = nullptr;
        gGuideScenarioBox = nullptr;
        gGuideFilterBox = nullptr;
        gGuideReload = nullptr;
        gGuideText = nullptr;
        if (gGuideFontOwned && gGuideFont) DeleteObject(gGuideFont);
        gGuideFont = nullptr;
        gGuideFontOwned = false;
        if (gGuideRichEditModule) FreeLibrary(gGuideRichEditModule);
        gGuideRichEditModule = nullptr;
        gGuideUsesRichEdit = false;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterGuideClass() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW klass = {sizeof(klass)};
    klass.hInstance = gInstance;
    klass.lpfnWndProc = GuideProcedure;
    klass.lpszClassName = L"XiaoAn66GuideWindow";
    klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    klass.hIcon = static_cast<HICON>(LoadImageW(gInstance, MAKEINTRESOURCEW(IDI_DORO_WHITE), IMAGE_ICON,
        0, 0, LR_DEFAULTSIZE | LR_SHARED));
    klass.hIconSm = klass.hIcon;
    if (!RegisterClassExW(&klass)) return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    registered = true;
    return true;
}

} // namespace

bool OpenGuide(HWND owner, const wchar_t* gameRoot) {
    if (gGuideWindow && IsWindow(gGuideWindow)) {
        ShowWindow(gGuideWindow, SW_RESTORE);
        SetForegroundWindow(gGuideWindow);
        return true;
    }
    if (!gameRoot || !*gameRoot || !RegisterGuideClass()) return false;
    gGuideRoot = gameRoot;
    gGuideCurrentFilter = GuideFilter::All;
    gGuideResult = GuideResult();
    RECT ownerRect = {};
    if (!GetWindowRect(owner, &ownerRect)) SetRect(&ownerRect, 100, 100, 900, 700);
    const int width = 620;
    const int height = 480;
    int x = ownerRect.right + 8;
    int y = ownerRect.top;
    HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {sizeof(info)};
    if (GetMonitorInfoW(monitor, &info)) {
        if (x + width > info.rcWork.right) {
            x = ownerRect.left;
            y = ownerRect.bottom + 8;
        }
        if (y + height > info.rcWork.bottom) y = info.rcWork.bottom - height;
        x = Bound(x, info.rcWork.left, info.rcWork.right - width);
        y = Bound(y, info.rcWork.top, info.rcWork.bottom - height);
    }
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, L"XiaoAn66GuideWindow", L"简易攻略",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, width, height, owner, nullptr, gInstance, nullptr);
    if (!window) return false;
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    return true;
}

void CloseGuide() {
    if (gGuideTask) gGuideTask->cancelled.store(true);
    if (gGuideWindow && IsWindow(gGuideWindow)) DestroyWindow(gGuideWindow);
    gGuideWindow = nullptr;
}

bool IsGuideWindow(HWND window) {
    return window && gGuideWindow && (window == gGuideWindow || IsChild(gGuideWindow, window));
}

bool HandleGuideMessage(MSG* message) {
    if (!message || !IsGuideWindow(message->hwnd)) return false;
    return IsDialogMessageW(gGuideWindow, message) != FALSE;
}

void ObserveGuideScenarioPathA(const char* path) {
    if (!path || !*path) return;
    const int length = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
    if (length <= 1 || length > 32768) return;
    std::wstring wide(static_cast<size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_ACP, 0, path, -1, wide.data(), length)) return;
    wide.resize(static_cast<size_t>(length - 1));
    wchar_t kind = 0;
    int number = 0;
    if (!ParseScenarioName(wide, kind, number)) return;
    wchar_t canonical[32] = {};
    swprintf_s(canonical, L"%c_%02d.eex", kind, number);
    AcquireSRWLockExclusive(&gObservedScenarioLock);
    gObservedScenarioName = canonical;
    ReleaseSRWLockExclusive(&gObservedScenarioLock);
}

bool GetObservedGuideScenario(std::wstring& name) {
    name = ObservedScenarioName();
    return !name.empty();
}

bool BuildGuideTextForTest(const wchar_t* scenarioPath, const wchar_t* gameRoot, std::wstring& text) {
    if (!scenarioPath || !gameRoot || !*scenarioPath || !*gameRoot) return false;
    GuideFile file;
    if (!BuildGuideFile(scenarioPath, gameRoot, file)) return false;
    std::atomic<bool> cancelled(false);
    GuideResult result = AnalyzeScenario(file, gameRoot, cancelled);
    if (result.title.empty()) return false;
    text = RenderGuideText(result, GuideFilter::All);
    return true;
}

bool ResolveGuideScenarioForTest(const wchar_t* scenarioPath, const wchar_t* gameRoot,
    std::wstring& battlefieldPath) {
    if (!scenarioPath || !gameRoot || !*scenarioPath || !*gameRoot) return false;
    int number = 0;
    return ResolveBattlefieldPath(scenarioPath, gameRoot, battlefieldPath, number);
}

bool ListGuideScenarioJumpsForTest(const wchar_t* scenarioPath, std::vector<std::wstring>& targets) {
    if (!scenarioPath || !*scenarioPath) return false;
    return ReadScenarioJumpNames(scenarioPath, targets);
}

bool FormatGuideScenarioJumpForTest(int rawValue, std::wstring& target) {
    target.clear();
    return ScenarioJumpName(rawValue, target);
}

bool EnumerateGuideScenariosForTest(const wchar_t* gameRoot, std::vector<std::wstring>& names) {
    names.clear();
    if (!gameRoot || !*gameRoot) return false;
    const std::wstring previousRoot = gGuideRoot;
    std::vector<GuideFile> previousFiles = std::move(gGuideFiles);
    gGuideRoot = gameRoot;
    ScanFiles();
    for (const GuideFile& file : gGuideFiles) names.push_back(file.name);
    gGuideFiles = std::move(previousFiles);
    gGuideRoot = previousRoot;
    return !names.empty();
}

bool SelectObservedGuideScenarioForTest(const wchar_t* gameRoot, std::wstring& selectedName,
    std::wstring& battlefieldPath) {
    selectedName.clear();
    battlefieldPath.clear();
    if (!gameRoot || !*gameRoot) return false;
    const std::wstring previousRoot = gGuideRoot;
    std::vector<GuideFile> previousFiles = std::move(gGuideFiles);
    gGuideRoot = gameRoot;
    ScanFiles();
    const int selected = DefaultScenarioIndex();
    if (selected >= 0 && static_cast<size_t>(selected) < gGuideFiles.size()) {
        selectedName = gGuideFiles[static_cast<size_t>(selected)].name;
        battlefieldPath = gGuideFiles[static_cast<size_t>(selected)].battlefieldPath;
    }
    gGuideFiles = std::move(previousFiles);
    gGuideRoot = previousRoot;
    return !selectedName.empty() && !battlefieldPath.empty();
}

bool FormatGuideStateSummaryForTest(unsigned attributeMask, int change, int abnormalMask,
    int hpLoss, int mpLoss, std::wstring& text) {
    if (change < 0 || change > 2) return false;
    unsigned changes[3] = {};
    changes[change] = attributeMask & 0x3Fu;
    const unsigned abnormal = abnormalMask > 0 ? static_cast<unsigned>(abnormalMask) : 0u;
    const unsigned bits = abnormal & 0x7Eu;
    text = FormatStateSummary(changes[0], changes[1], changes[2],
        abnormal & 0x80 ? 0u : bits, abnormal & 0x80 ? bits : 0u, hpLoss, mpLoss);
    return !text.empty();
}

bool BuildGuideStatsForTest(const wchar_t* scenarioPath, const wchar_t* gameRoot, GuideTestStats& stats) {
    stats = GuideTestStats();
    if (!scenarioPath || !gameRoot || !*scenarioPath || !*gameRoot) return false;
    std::vector<unsigned char> bytes;
    GuideContext context;
    if (!ReadFileBytes(scenarioPath, bytes) || !ParseEex(bytes, context.commands)) return false;
    LoadNames(gameRoot, context);
    CollectDeployments(context, context.units);
    stats.commands = context.commands.size();
    for (const GuideCommand& command : context.commands) {
        if (command.id == 0x4C) {
            ++stats.ambushCommands;
            for (const GuideCondition& condition : command.conditions)
                if (condition.id < _countof(stats.ambushConditions)) ++stats.ambushConditions[condition.id];
            std::wstring conditions;
            bool simpleAdjacent = false;
            std::pair<int, int> adjacent;
            if (RenderConditions(context, command.conditions, conditions, simpleAdjacent, adjacent)) {
                ++stats.conditionedAmbushCommands;
                bool matched = false;
                for (const GuideUnit& unit : context.units) {
                    if (unit.camp != GuideUnit::Camp::Enemy || !unit.hidden) continue;
                    if ((command.values.size() >= 2 && command.values[0] == 0 && unit.id == command.values[1]) ||
                        (command.values.size() >= 3 && command.values[0] == 1 &&
                            unit.battlefieldNumber == command.values[2])) matched = true;
                }
                if (matched) ++stats.matchedAmbushCommands;
            }
        }
        if (command.id == 0x4E) {
            ++stats.aiCommands;
            for (const GuideCondition& condition : command.conditions)
                if (condition.id < _countof(stats.aiConditions)) ++stats.aiConditions[condition.id];
            std::wstring conditions;
            bool simpleAdjacent = false;
            std::pair<int, int> adjacent;
            if (!RenderConditions(context, command.conditions, conditions, simpleAdjacent, adjacent)) continue;
            ++stats.conditionedAiCommands;
            if (command.values.size() < 8) continue;
            bool matched = false;
            bool changing = false;
            for (const GuideUnit& unit : context.units) {
                if (unit.camp != GuideUnit::Camp::Enemy) continue;
                bool target = false;
                if (command.values[0] == 0) target = unit.id == command.values[1];
                else if (command.values[0] == 1) {
                    const int x1 = std::min(command.values[2], command.values[4]);
                    const int x2 = std::max(command.values[2], command.values[4]);
                    const int y1 = std::min(command.values[3], command.values[5]);
                    const int y2 = std::max(command.values[3], command.values[5]);
                    target = unit.x >= x1 && unit.x <= x2 && unit.y >= y1 && unit.y <= y2;
                }
                if (target) {
                    matched = true;
                    if (unit.policy != command.values[7]) changing = true;
                }
            }
            if (matched) ++stats.matchedAiCommands;
            if (changing) ++stats.changingAiCommands;
        }
    }
    GuideResult result;
    AddDialogEntries(context, result);
    AddAmbushEntries(context, result);
    AddAiEntries(context, result);
    AddRewardEntries(context, result);
    MergeAndPruneNarrativeEntries(result);
    for (const GuideEntry& entry : result.entries) {
        if (entry.filter == GuideFilter::Dialog) ++stats.dialogEntries;
        if (entry.filter == GuideFilter::Ambush) ++stats.ambushEntries;
        if (entry.filter == GuideFilter::Ai) ++stats.aiEntries;
        if (entry.filter == GuideFilter::Rewards) ++stats.rewardEntries;
    }
    return true;
}

