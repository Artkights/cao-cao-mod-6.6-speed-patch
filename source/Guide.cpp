#include "Guide.h"
#include "ScenarioScript.h"
#include "BattleRuntime.h"
#include "EngineProfile.h"
#include "ResourceIds.h"

#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace {

constexpr UINT kGuideReadComplete = WM_APP + 0x670;
constexpr UINT kGuideScenario = 0x6703;
constexpr UINT kGuideExport = 0x6704;
constexpr UINT kGuideFontSize = 0x6705;
constexpr UINT kGuideLineSpacing = 0x6706;

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

struct GuideActionOrigin {
    std::wstring source;
    size_t scene = 0;
    size_t event = 0;
    size_t block = 0;
    size_t ordinal = SIZE_MAX;
    size_t offset = 0;
    size_t narrativeGroup = SIZE_MAX;
    std::vector<ScenarioCondition> conditions;
};

struct GuideAction {
    std::wstring text;
    GuideActionOrigin origin;
};

struct GuideNarrativeGroup {
    const ScenarioCommand* first = nullptr;
    std::vector<std::wstring> speakers;
    std::vector<const ScenarioCommand*> duels;
    bool hasDialog = false;
};

struct GuideEntry {
    GuideFilter filter = GuideFilter::All;
    std::wstring text;
    std::wstring conditionKey;
    std::wstring prefix;
    std::vector<std::wstring> actions;
    std::vector<GuideAction> sourceActions;
    size_t narrativeGroup = SIZE_MAX;
    bool keepWithoutResult = false;
    bool hideInAll = false;
    bool preserveText = false;
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
    bool authored = false;
    std::wstring authoredBody;
    bool parsed = false;
    std::wstring diagnostics;
    BattleResourceSnapshot resources;
    bool hasResources = false;
    std::wstring readStatus;
};

struct GuideBook {
    bool readable = false;
    std::unordered_map<int, std::wstring> sections;
};

struct GuideTaskState {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> completed{false};
    HWND window = nullptr;
    unsigned request = 0;
    GuideResult result;
    bool exporting = false;
    size_t exportSuccess = 0;
    std::wstring exportText;
    std::vector<int> exportSkipped;
    std::wstring exportError;
};

using GuideCondition = ScenarioCondition;
using GuideCommand = ScenarioCommand;

struct GuideContext {
    std::vector<GuideCommand> commands;
    std::vector<GuideUnit> units;
    std::unordered_map<int, std::wstring> people;
    std::unordered_map<int, std::wstring> items;
    bool namesReady = false;
    BattleResourceSnapshot resources;
    bool conditionalExtension = false;
    std::wstring sourcePath;
    std::wstring* diagnostics = nullptr;
    mutable std::unordered_map<int, bool> reportedMissingPeople;
    mutable std::unordered_map<int, bool> reportedMissingItems;
    mutable std::unordered_map<std::wstring, bool> reportedPointerTokens;
    mutable bool narrativesReady = false;
    mutable std::vector<GuideNarrativeGroup> narratives;
    mutable std::unordered_map<size_t, size_t> narrativeOwners;
};

enum class GuideAbilityOperation {
    Increase,
    Decrease,
    Set,
    Unknown
};

struct GuideAbilityChange {
    int personId = -1;
    int attribute = -1;
    GuideAbilityOperation operation = GuideAbilityOperation::Unknown;
    int32_t amount = 0;
    int rawOperation = -1;
    size_t ordinal = 0;
    size_t block = 0;
    size_t event = 0;
};

const wchar_t* GuideAbilityName(int attribute);

HWND gGuideWindow = nullptr;
HWND gGuideLevelLabel = nullptr;
HWND gGuideScenarioBox = nullptr;
HWND gGuideExport = nullptr;
HWND gGuideFontLabel = nullptr;
HWND gGuideFontSize = nullptr;
HWND gGuideSpacingLabel = nullptr;
HWND gGuideLineSpacing = nullptr;
HWND gGuideText = nullptr;
std::wstring gGuideRoot;
std::vector<GuideFile> gGuideFiles;
std::shared_ptr<GuideTaskState> gGuideTask;
GuideResult gGuideResult;
unsigned gGuideRequest = 0;
HFONT gGuideFont = nullptr;
bool gGuideFontOwned = false;
HFONT gGuideBodyFont = nullptr;
HMODULE gGuideRichEditModule = nullptr;
bool gGuideUsesRichEdit = false;
SRWLOCK gObservedScenarioLock = SRWLOCK_INIT;
std::wstring gObservedScenarioName;
std::mutex gGuideDocumentLock;
std::atomic<bool> gGuideConditionalExtension{false};

// RichEdit stores both values in twips (1/20 point).  They intentionally stay
// as native values in the settings UI so users can tune the renderer without
// a hidden unit conversion.
struct GuideDisplaySettings {
    int fontSize = 220;
    int lineSpacing = 45;
};

GuideDisplaySettings gGuideDisplaySettings;
bool gGuideDisplaySettingsReady = false;

constexpr int kGuideFontSizeMinimum = 80;
constexpr int kGuideFontSizeMaximum = 640;
constexpr int kGuideLineSpacingMinimum = 0;
constexpr int kGuideLineSpacingMaximum = 400;

constexpr size_t kGuideTextCharacters = 1024u * 1024u;
constexpr size_t kGuideBookBytes = 64u * 1024u * 1024u;

bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>& bytes,
    size_t maximum = 0x7fffffffu) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    const bool sized = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0 &&
        static_cast<uint64_t>(size.QuadPart) <= maximum;
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

bool DecodeGuideDocument(const std::vector<unsigned char>& bytes, std::wstring& text,
    const std::atomic<bool>* cancelled = nullptr) {
    text.clear();
    if (cancelled && cancelled->load()) return false;
    if (bytes.size() > kGuideBookBytes) return false;
    if (bytes.empty()) return true;
    size_t offset = 0;
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        offset = 2;
        if ((bytes.size() - offset) % 2 != 0) return false;
        const size_t count = (bytes.size() - offset) / 2;
        text.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            if ((index & 4095u) == 0 && cancelled && cancelled->load()) return false;
            const size_t at = offset + index * 2;
            text.push_back(static_cast<wchar_t>(bytes[at] |
                (static_cast<uint16_t>(bytes[at + 1]) << 8)));
        }
    } else {
        const bool utf8Marked = bytes.size() >= 3 && bytes[0] == 0xEF &&
            bytes[1] == 0xBB && bytes[2] == 0xBF;
        offset = utf8Marked ? 3 : 0;
        const unsigned char* data = bytes.data() + offset;
        const size_t length = bytes.size() - offset;
        if (!length) return true;
        auto decode = [&](UINT codePage, DWORD flags, std::wstring& output) {
            if (length > static_cast<size_t>(INT_MAX)) return false;
            const int required = MultiByteToWideChar(codePage, flags,
                reinterpret_cast<const char*>(data), static_cast<int>(length), nullptr, 0);
            if (required <= 0) return false;
            output.assign(static_cast<size_t>(required), L'\0');
            return MultiByteToWideChar(codePage, flags, reinterpret_cast<const char*>(data),
                static_cast<int>(length), output.data(), required) == required;
        };
        if (!decode(CP_UTF8, MB_ERR_INVALID_CHARS, text)) {
            if (utf8Marked || !decode(936, MB_ERR_INVALID_CHARS, text)) {
                text.clear();
                return false;
            }
        }
    }
    for (size_t index = 0; index < text.size(); ++index) {
        if ((index & 4095u) == 0 && cancelled && cancelled->load()) return false;
        const wchar_t ch = text[index];
        if (ch == 0 || (ch >= 0xDC00 && ch <= 0xDFFF)) { text.clear(); return false; }
        if (ch >= 0xD800 && ch <= 0xDBFF) {
            if (++index >= text.size() || text[index] < 0xDC00 || text[index] > 0xDFFF) {
                text.clear();
                return false;
            }
        }
    }
    std::wstring normalized;
    normalized.reserve(text.size() + 16);
    for (size_t index = 0; index < text.size(); ++index) {
        if ((index & 4095u) == 0 && cancelled && cancelled->load()) return false;
        const wchar_t ch = text[index];
        if (ch == L'\r') {
            normalized += L"\r\n";
            if (index + 1 < text.size() && text[index + 1] == L'\n') ++index;
        } else if (ch == L'\n') {
            normalized += L"\r\n";
        } else {
            normalized.push_back(ch);
        }
    }
    text.swap(normalized);
    return true;
}

bool GuideTextBlank(const std::wstring& text) {
    return std::all_of(text.begin(), text.end(), [](wchar_t ch) {
        return iswspace(ch) != 0;
    });
}

bool ParseGuideHeading(const std::wstring& line, int& number) {
    size_t cursor = 0;
    while (cursor < line.size() && iswspace(line[cursor])) ++cursor;
    if (cursor >= line.size() || line[cursor++] != L'第') return false;
    while (cursor < line.size() && iswspace(line[cursor])) ++cursor;
    const size_t digitsStart = cursor;
    while (cursor < line.size() && line[cursor] >= L'0' && line[cursor] <= L'9') ++cursor;
    if (cursor == digitsStart || cursor - digitsStart > 3) return false;
    int parsed = 0;
    for (size_t at = digitsStart; at < cursor; ++at) parsed = parsed * 10 + (line[at] - L'0');
    while (cursor < line.size() && iswspace(line[cursor])) ++cursor;
    static const std::wstring suffix = L"关简易攻略";
    if (line.compare(cursor, suffix.size(), suffix) != 0) return false;
    cursor += suffix.size();
    while (cursor < line.size() && iswspace(line[cursor])) ++cursor;
    if (cursor != line.size() || parsed < 0 || parsed > 999) return false;
    number = parsed;
    return true;
}

void FlushGuideSection(int number, const std::wstring& body, GuideBook& book) {
    if (number < 0) return;
    book.sections.erase(number);
    if (!GuideTextBlank(body)) book.sections[number] = body;
}

bool ParseGuideDocument(const std::wstring& text, GuideBook& book,
    const std::atomic<bool>* cancelled = nullptr) {
    book.sections.clear();
    book.readable = true;
    int current = -1;
    std::wstring body;
    bool hasBodyLine = false;
    size_t cursor = 0;
    while (cursor <= text.size()) {
        if (cancelled && cancelled->load()) return false;
        const size_t lineEnd = text.find(L"\r\n", cursor);
        const size_t end = lineEnd == std::wstring::npos ? text.size() : lineEnd;
        const std::wstring line = text.substr(cursor, end - cursor);
        int heading = -1;
        if (ParseGuideHeading(line, heading)) {
            FlushGuideSection(current, body, book);
            current = heading;
            body.clear();
            hasBodyLine = false;
        } else if (current >= 0) {
            if (hasBodyLine) body += L"\r\n";
            body += line;
            hasBodyLine = true;
        }
        if (lineEnd == std::wstring::npos) break;
        cursor = lineEnd + 2;
    }
    FlushGuideSection(current, body, book);
    return true;
}

bool ReadGuideBook(const std::wstring& root, GuideBook& book,
    const std::atomic<bool>* cancelled = nullptr) {
    book = GuideBook();
    // Scenario changes can cancel detached reads. Serialize large document
    // decoding so cancelled readers cannot exhaust the x86 game's memory.
    std::lock_guard<std::mutex> lock(gGuideDocumentLock);
    if (cancelled && cancelled->load()) return false;
    std::vector<unsigned char> bytes;
    if (!ReadFileBytes(root + L"\\简易攻略.txt", bytes, kGuideBookBytes)) return false;
    std::wstring text;
    if (!DecodeGuideDocument(bytes, text, cancelled)) return false;
    return ParseGuideDocument(text, book, cancelled);
}

bool GetAuthoredGuideBody(const GuideBook& book, int number, std::wstring& body) {
    body.clear();
    const auto found = book.sections.find(number);
    if (found == book.sections.end() || GuideTextBlank(found->second)) return false;
    body = found->second;
    return true;
}

bool HasPlayerText(const std::wstring& text) {
    if (text.empty() || text.size() > 240) return false;
    return std::any_of(text.begin(), text.end(), [](wchar_t ch) {
        return ch >= 0x4E00 && ch <= 0x9FFF;
    });
}

bool ParseEex(const std::vector<unsigned char>& bytes, std::vector<GuideCommand>& output,
    const std::atomic<bool>* cancelled = nullptr) {
    return ParseScenarioScript(bytes, output, cancelled);
}

std::wstring TrimNull(const unsigned char* data, size_t length) {
    size_t used = 0;
    while (used < length && data[used]) ++used;
    return DecodeGbk(data, used);
}

// Person/item name tables in Data.e5 and Star.e5 are always GBK. A GBK byte
// pair can accidentally form a valid UTF-8 sequence, so the general
// UTF-8-first heuristic would silently drop names such as 陆逊 (C2 BD D1 B7).
std::wstring TrimNullGbk(const unsigned char* data, size_t length) {
    size_t used = 0;
    while (used < length && data[used]) ++used;
    if (!used) return {};
    const int required = MultiByteToWideChar(936, 0,
        reinterpret_cast<const char*>(data), static_cast<int>(used), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(936, 0, reinterpret_cast<const char*>(data),
        static_cast<int>(used), result.data(), required);
    return result;
}

bool LoadTableNames(const std::wstring& path, size_t offset, size_t count, size_t rowSize,
    size_t nameBytes, int firstId, std::unordered_map<int, std::wstring>& result) {
    std::vector<unsigned char> bytes;
    if (!ReadFileBytes(path, bytes) || offset > bytes.size() || count > (bytes.size() - offset) / rowSize) return false;
    std::unordered_map<int, std::wstring> candidate;
    for (size_t index = 0; index < count; ++index) {
        const std::wstring name = TrimNullGbk(bytes.data() + offset + index * rowSize, nameBytes);
        if (HasPlayerText(name)) candidate[firstId + static_cast<int>(index)] = name;
    }
    if (candidate.size() < count / 5) return false;
    result = std::move(candidate);
    return true;
}

BattleResourceSnapshot CaptureGuideResources(const std::wstring& root,
    const std::atomic<bool>* cancelled = nullptr) {
    std::wstring data = root + L"\\Data.e5";
    std::wstring star = root + L"\\Star.e5";
    std::unordered_map<int, std::wstring> people;
    std::unordered_map<int, std::wstring> lowItems;
    std::unordered_map<int, std::wstring> highItems;
    const bool peopleOkay = LoadTableNames(data, 396, 1024, 32, 13, 0, people);
    const bool lowOkay = LoadTableNames(data, 33164, 104, 25, 17, 0, lowItems);
    const bool highOkay = LoadTableNames(star, 0, 151, 25, 17, 104, highItems);
    BattleResourceSnapshot snapshot;
    ReadBattleResourceSnapshot(snapshot);
    // Each table has its own readiness state. In particular an early item
    // table must not stop retrying the not-yet-published person pointer.
    for (unsigned attempt = 0; BattleRuntimeResourceNamesEnabled() &&
            (!snapshot.peopleReady || !snapshot.itemsReady) && attempt < 60; ++attempt) {
        if (cancelled && cancelled->load()) return snapshot;
        Sleep(500);
        if (cancelled && cancelled->load()) return snapshot;
        ReadBattleResourceSnapshot(snapshot);
    }
    const auto merge = [](std::vector<std::wstring>& names, bool runtimeReady,
        const std::unordered_map<int, std::wstring>& disk, BattleResourceOrigin& origin) {
        bool fromDisk = false;
        for (const auto& item : disk) {
            if (item.first < 0 || item.second.empty()) continue;
            const size_t id = static_cast<size_t>(item.first);
            if (names.size() <= id) names.resize(id + 1);
            if (names[id].empty()) { names[id] = item.second; fromDisk = true; }
        }
        origin = runtimeReady ? (fromDisk ? BattleResourceOrigin::RuntimeWithDiskFallback : BattleResourceOrigin::Runtime) :
            fromDisk ? BattleResourceOrigin::Disk : BattleResourceOrigin::Unavailable;
        return runtimeReady || fromDisk;
    };
    if (!peopleOkay) people.clear();
    if (!lowOkay) lowItems.clear();
    if (highOkay) lowItems.insert(highItems.begin(), highItems.end());
    snapshot.peopleReady = merge(snapshot.people, snapshot.peopleReady, people, snapshot.peopleOrigin);
    snapshot.itemsReady = merge(snapshot.items, snapshot.itemsReady, lowItems, snapshot.itemOrigin);
    return snapshot;
}

void LoadNames(const std::wstring& root, GuideContext& context,
    const std::atomic<bool>* cancelled = nullptr, const BattleResourceSnapshot* shared = nullptr) {
    context.conditionalExtension = gGuideConditionalExtension.load(std::memory_order_acquire);
    context.resources = shared ? *shared : CaptureGuideResources(root, cancelled);
    for (size_t id = 0; id < context.resources.people.size(); ++id)
        if (!context.resources.people[id].empty()) context.people[static_cast<int>(id)] = context.resources.people[id];
    for (size_t id = 0; id < context.resources.items.size(); ++id)
        if (!context.resources.items[id].empty()) context.items[static_cast<int>(id)] = context.resources.items[id];
    if (context.diagnostics) {
        *context.diagnostics += context.sourcePath + L": resource-snapshot process=" +
            std::to_wstring(context.resources.processGeneration) + L" generation=" +
            std::to_wstring(context.resources.resourceGeneration) + L" people-origin=" +
            std::to_wstring(static_cast<int>(context.resources.peopleOrigin)) + L" items-origin=" +
            std::to_wstring(static_cast<int>(context.resources.itemOrigin)) + L"\n";
        if (!context.resources.peopleReady || !context.resources.itemsReady)
            *context.diagnostics += context.sourcePath + L": runtime-resource-names-unavailable (using ID fallback)\n";
    }
    context.namesReady = !context.people.empty() || !context.items.empty();
}

std::wstring PersonName(const GuideContext& context, int id) {
    const auto found = context.people.find(id);
    return found == context.people.end() ? std::wstring() : found->second;
}

// A script text/parameter may reference a character by pointer notation
// "*.xx", where xx is the person data number in Data.e5. Resolve only a
// numeric data number and never guess an unknown pointer encoding.
std::wstring ResolvePersonToken(const GuideContext& context, const std::wstring& token) {
    if (token.size() < 3 || token[0] != L'*' || token[1] != L'.') return {};
    int id = 0;
    for (size_t i = 2; i < token.size(); ++i) {
        if (!iswdigit(token[i])) return {};
        id = id * 10 + (token[i] - L'0');
        if (id > 1023) return {};
    }
    const std::wstring name = PersonName(context, id);
    return name == L"空" ? std::wstring() : name;
}

std::wstring ResolvePersonTokens(const GuideContext& context, const std::wstring& text) {
    std::wstring result;
    for (size_t i = 0; i < text.size();) {
        if (i + 1 < text.size() && text[i] == L'*' && text[i + 1] == L'.') {
            size_t end = i + 2;
            while (end < text.size() && iswdigit(text[end])) ++end;
            if (end > i + 2) {
                const std::wstring token = text.substr(i, end - i);
                const std::wstring name = ResolvePersonToken(context, token);
                if (!name.empty()) {
                    result += name;
                    i = end;
                    continue;
                }
                if (context.diagnostics &&
                    context.reportedPointerTokens.emplace(token, true).second)
                    *context.diagnostics += context.sourcePath +
                        L": person-pointer-unresolved: token=" + token + L"\n";
            }
        }
        result += text[i++];
    }
    return result;
}

std::wstring FirstPersonTokenName(const GuideContext& context, const std::wstring& text) {
    for (size_t i = 0; i + 2 < text.size(); ++i) {
        if (text[i] != L'*' || text[i + 1] != L'.') continue;
        size_t end = i + 2;
        while (end < text.size() && iswdigit(text[end])) ++end;
        if (end == i + 2) continue;
        const std::wstring name = ResolvePersonToken(context, text.substr(i, end - i));
        if (!name.empty()) return name;
    }
    return {};
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
    if (id >= 0 && id < 1024) {
        if (context.diagnostics && context.reportedMissingPeople.emplace(id, true).second)
            *context.diagnostics += context.sourcePath + L": person-name-missing: id=" +
                std::to_wstring(id) + L" (using ID fallback)\n";
        return L"武将 " + std::to_wstring(id);
    }
    return {};
}

std::wstring PersonReferenceName(const GuideContext& context,
    const GuideCommand& command, size_t valueIndex, int32_t value) {
    (void)valueIndex;
    if (value >= 0 && value < 1024) {
        const std::wstring name = PersonName(context, static_cast<int>(value));
        if (!name.empty() && name != L"空") return name;
    }
    for (const std::wstring& text : command.texts) {
        const std::wstring name = FirstPersonTokenName(context, text);
        if (!name.empty()) return name;
    }
    return ConditionPersonName(context, static_cast<int>(value));
}

std::wstring PersonReferenceName(const GuideContext& context,
    const GuideCondition& condition, size_t valueIndex, int32_t value) {
    (void)valueIndex;
    if (value >= 0 && value < 1024) {
        const std::wstring name = PersonName(context, static_cast<int>(value));
        if (!name.empty() && name != L"空") return name;
    }
    for (const std::wstring& text : condition.texts) {
        const std::wstring name = FirstPersonTokenName(context, text);
        if (!name.empty()) return name;
    }
    return ConditionPersonName(context, static_cast<int>(value));
}

std::wstring ItemName(const GuideContext& context, int id) {
    const auto found = context.items.find(id);
    return found == context.items.end() ? std::wstring() : found->second;
}

std::wstring ItemDisplayName(const GuideContext& context, int id) {
    const std::wstring item = ItemName(context, id);
    if (!item.empty()) return item;
    if (id >= 0 && id < 256) {
        if (context.diagnostics && context.reportedMissingItems.emplace(id, true).second)
            *context.diagnostics += context.sourcePath + L": item-name-missing: id=" +
                std::to_wstring(id) + L" (using ID fallback)\n";
        return L"道具 " + std::to_wstring(id);
    }
    return {};
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
                const std::wstring raw = text.substr(start, end - start);
                const std::wstring name = ResolvePersonTokens(context, raw);
                // Dialogue text carries its speaker label independently from
                // the person table. Keep it when the protected runtime table
                // is not ready; dropping the whole dialogue would make a
                // transient name lookup failure look like an empty event.
                if (IsKnownPersonName(context, name) || !name.empty()) AddUnique(result, name);
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
        L"前往指定地点", L"跟随指定武将", L"撤往指定地点", L"玩家可控"
    };
    return value >= 0 && value < static_cast<int>(_countof(names)) ? names[value] : L"";
}

bool ParseConditionalFields(const std::wstring& text, size_t count,
    std::vector<unsigned>& values) {
    values.clear();
    size_t cursor = 0;
    while (values.size() < count) {
        const size_t start = cursor;
        unsigned value = 0;
        while (cursor < text.size() && text[cursor] >= L'0' && text[cursor] <= L'9') {
            const unsigned digit = static_cast<unsigned>(text[cursor] - L'0');
            if (value > (65535u - digit) / 10u) return false;
            value = value * 10u + digit;
            ++cursor;
        }
        if (cursor == start) return false;
        values.push_back(value);
        if (values.size() == count) return cursor == text.size();
        if (cursor >= text.size() || text[cursor++] != L',') return false;
    }
    return cursor == text.size();
}

std::wstring ConditionalUnitName(const GuideContext& context, unsigned mode, unsigned id) {
    if (mode == 0 && id <= 1023) return ConditionPersonName(context, static_cast<int>(id));
    // Mode 1 names a live tactical slot.  Its occupant can change after a
    // reinforcement, so expose the slot without inventing a static person ID.
    if (mode == 1 && id <= 249) return L"战场槽位 " + std::to_wstring(id) + L" 的部队";
    return {};
}

bool RenderConditionalExtension(const GuideContext& context, const GuideCondition& condition,
    std::wstring& text) {
    text.clear();
    if (!context.conditionalExtension || condition.id != 0x7B ||
        condition.values.size() != 1 || condition.texts.size() != 1) return false;
    const int branch = condition.values[0];
    std::vector<unsigned> fields;
    const auto parse = [&](size_t count) {
        return ParseConditionalFields(condition.texts[0], count, fields);
    };
    if (branch == 1) {
        if (!parse(3) || fields[0] > 255) return false;
        if (fields[2] == 0) {
            const std::wstring item = ItemDisplayName(context, static_cast<int>(fields[0]));
            text = item + L"在行囊与已加入我军人物装备中的总数不少于 " + std::to_wstring(fields[1]);
        } else if (fields[2] == 1) {
            const int extendedId = static_cast<int>(fields[0] + 256);
            const std::wstring item = ItemName(context, extendedId);
            if (item.empty() && context.diagnostics &&
                context.reportedMissingItems.emplace(extendedId, true).second)
                *context.diagnostics += context.sourcePath + L": item-name-missing: id=" +
                    std::to_wstring(extendedId) + L" (using ID fallback)\n";
            text = (item.empty() ? L"扩展消耗品 " + std::to_wstring(fields[0]) : item) +
                L"的持有数量不少于 " + std::to_wstring(fields[1]);
        } else return false;
    } else if (branch == 2) {
        if (!parse(2) || fields[0] > 1023 || fields[1] > 1) return false;
        text = PersonReferenceName(context, condition, 0, static_cast<int32_t>(fields[0])) +
            (fields[1] ? L"已加入我军" : L"尚未加入我军");
    } else if (branch == 3) {
        if (!parse(3) || fields[0] > 1 || fields[2] >= 255) return false;
        const std::wstring person = ConditionalUnitName(context, fields[0], fields[1]);
        if (person.empty()) return false;
        text = person + L"已在场且所在位置的地形编号为 " + std::to_wstring(fields[2]);
    } else if (branch == 4) {
        if (!parse(1) || fields[0] > 4) return false;
        text = L"当前天气编号为 " + std::to_wstring(fields[0]);
    } else if (branch == 5) {
        if (!parse(4) || fields[0] > 1 || fields[1] > 1023 ||
            fields[2] < 1 || fields[2] > 4 || fields[3] > 1) return false;
        const std::wstring person = ConditionalUnitName(context, fields[0], fields[1]);
        if (person.empty()) return false;
        static const wchar_t* statuses[] = {L"麻痹", L"封咒", L"混乱", L"中毒"};
        text = person + (fields[3] ? L"处于" : L"未处于") + statuses[fields[2] - 1] + L"状态";
    } else return false;
    return !text.empty();
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
        if (condition.id == 0x7B) {
            std::wstring extended;
            if (!RenderConditionalExtension(context, condition, extended)) {
                extended = L"满足尚未解析的扩展条件（0x7B）";
            }
            parts.push_back(std::move(extended));
        } else if (condition.id == 0x2E && values.size() >= 2) {
            const std::wstring first = PersonReferenceName(context, condition, 0, values[0]);
            const std::wstring second = PersonReferenceName(context, condition, 1, values[1]);
            if (first.empty() || second.empty()) return false;
            parts.push_back(first + L"与" + second + L"相邻");
            adjacentPeople = {values[0], values[1]};
        } else if (condition.id == 0x25 && values.size() >= 3) {
            const std::wstring person = PersonReferenceName(context, condition, 0, values[0]);
            if (person.empty() || values[1] < 0 || values[2] < 0) return false;
            parts.push_back(person + L"到达（" + std::to_wstring(values[1]) + L"，" +
                std::to_wstring(values[2]) + L"）");
        } else if (condition.id == 0x26 && values.size() >= 5) {
            const std::wstring person = PersonReferenceName(context, condition, 0, values[0]);
            if (person.empty()) return false;
            for (size_t i = 1; i < 5; ++i) if (values[i] < 0) return false;
            if (values[1] == 0 && values[2] == 0 && values[3] == 99 && values[4] == 99)
                parts.push_back(person + L"在场");
            else
                parts.push_back(person + L"进入（" + std::to_wstring(values[1]) + L"，" +
                    std::to_wstring(values[2]) + L"）至（" + std::to_wstring(values[3]) + L"，" +
                    std::to_wstring(values[4]) + L"）区域");
        } else if (condition.id == 0x36 && values.size() >= 4) {
            const std::wstring person = PersonReferenceName(context, condition, 0, values[0]);
            const int attribute = values[1], amount = values[2], comparison = values[3];
            const wchar_t* attributeName = GuideAbilityName(attribute);
            if (person.empty() || !attributeName ||
                comparison < 0 || comparison > 2) return false;
            if (attribute == 7 && amount == 0 && comparison == 2) {
                const std::wstring retreat = RetreatConditionText(context, values[0]);
                if (retreat.empty()) return false;
                parts.push_back(retreat);
            } else {
                static const wchar_t* comparisons[] = {L"不低于", L"低于", L"等于"};
                parts.push_back(person + attributeName + comparisons[comparison] + L" " +
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
        for (const std::wstring& text : condition.texts)
            key += L"t" + std::to_wstring(text.size()) + L":" + text;
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

GuideActionOrigin GuideOrigin(const GuideContext& context, const GuideCommand& command) {
    const auto owner = context.narrativeOwners.find(command.ordinal);
    return {context.sourcePath, command.scene, command.event, command.block,
        command.ordinal, command.offset,
        owner == context.narrativeOwners.end() ? SIZE_MAX : owner->second, command.conditions};
}

void AddActionEntry(GuideResult& result, GuideFilter filter, const std::wstring& conditionKey,
    const std::wstring& prefix, const std::wstring& action,
    const GuideContext* context = nullptr, const GuideCommand* command = nullptr) {
    if (conditionKey.empty() || prefix.empty() || action.empty()) return;
    // Source actions stay separate until dialogue ownership is known. Equal
    // condition text does not establish execution order or branch identity.
    for (GuideEntry& entry : result.entries) {
        if (command || !entry.sourceActions.empty()) continue;
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
    if (context && command) entry.sourceActions.push_back({action, GuideOrigin(*context, *command)});
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
                if (id < 0 || id >= 1024 || x < 0 || y < 0 || x >= 256 || y >= 256) continue;
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
                if (id < 0 || id >= 1024 || x < 0 || y < 0 || x >= 256 || y >= 256) continue;
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

void GuideDiagnostic(const GuideContext& context, const GuideCommand& command,
    const std::wstring& detail);
std::wstring RewardPrefix(const GuideContext& context, const GuideCommand& command,
    bool allowBestEffort);

bool IsGuideDialogue(unsigned id) {
    return id == 0x14 || id == 0x15 || id == 0x16 || id == 0x63 || id == 0x69;
}

bool IsGuideNarrativeEffect(const GuideCommand& command) {
    switch (command.id) {
    case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3D:
    case 0x4D: case 0x4E: case 0x53: case 0x59: return true;
    case 0x78: return command.values.size() == 4 && command.values[1] == 1 &&
        command.values[3] == 32;
    default: return false;
    }
}

bool IsGuideNarrativeBoundary(unsigned id) {
    return id == 0x0C || id == 0x0D || id == 0x0E || id == 0x11 || id == 0x12 || id == 0x13 ||
        id == 0x49 || id == 0x6A || id == 0x72 || id == 0x76;
}

void EnsureGuideNarratives(const GuideContext& context) {
    if (context.narrativesReady) return;
    context.narrativesReady = true;
    struct Frame {
        size_t parent = SIZE_MAX;
        size_t group = SIZE_MAX;
        bool local = false;
        bool afterEffect = false;
    };
    std::unordered_map<size_t, Frame> frames;
    for (const GuideCommand& command : context.commands) {
        auto found = frames.find(command.block);
        if (found == frames.end()) {
            Frame frame;
            frame.parent = command.parentBlock;
            const auto parent = frames.find(command.parentBlock);
            if (parent != frames.end()) {
                frame.group = parent->second.group;
                frame.afterEffect = parent->second.afterEffect;
            }
            // An unpaired else/selection is not proof that the parent dialogue
            // dominates this branch. Its own dialogue may establish a new group.
            if (command.blockOwnerOrdinal < context.commands.size()) {
                const unsigned opener = context.commands[command.blockOwnerOrdinal].id;
                if (opener == 0x03 || opener == 0x12 || opener == 0x13)
                    frame.group = SIZE_MAX;
            }
            found = frames.emplace(command.block, frame).first;
        }
        Frame& frame = found->second;
        const bool dialogue = IsGuideDialogue(command.id);
        const bool duel = command.id == 0x68 && command.values.size() >= 2;
        if (dialogue || duel) {
            const bool inheritedDuel = duel && frame.group != SIZE_MAX && !frame.afterEffect &&
                ConditionKey(command.event, command.conditions) ==
                ConditionKey(context.narratives[frame.group].first->event,
                    context.narratives[frame.group].first->conditions);
            // A duel performance in an unconditional child belongs to the
            // preceding parent dialogue. A child dialogue still starts its
            // own group and never flows back into the parent or a sibling.
            if ((!frame.local && !inheritedDuel) || frame.group == SIZE_MAX || frame.afterEffect) {
                frame.group = context.narratives.size();
                GuideNarrativeGroup group;
                group.first = &command;
                context.narratives.push_back(std::move(group));
                frame.local = true;
                frame.afterEffect = false;
            }
            GuideNarrativeGroup& group = context.narratives[frame.group];
            if (dialogue) {
                group.hasDialog = true;
                if (command.id == 0x15 && command.values.size() >= 2) {
                    AddUnique(group.speakers, PersonReferenceName(context, command, 0, command.values[0]));
                    AddUnique(group.speakers, PersonReferenceName(context, command, 1, command.values[1]));
                }
                for (const auto& name : ExtractSpeakers(context, command))
                    AddUnique(group.speakers, name);
            }
            if (duel) group.duels.push_back(&command);
        }
        if (frame.group != SIZE_MAX) context.narrativeOwners[command.ordinal] = frame.group;
        if (IsGuideNarrativeEffect(command)) {
            size_t block = command.block;
            while (block != SIZE_MAX) {
                const auto ancestor = frames.find(block);
                if (ancestor == frames.end()) break;
                ancestor->second.afterEffect = true;
                block = ancestor->second.parent;
            }
        }
        if (IsGuideNarrativeBoundary(command.id)) {
            size_t block = command.block;
            while (block != SIZE_MAX) {
                const auto ancestor = frames.find(block);
                if (ancestor == frames.end()) break;
                ancestor->second.group = SIZE_MAX;
                ancestor->second.local = false;
                block = ancestor->second.parent;
            }
        }
    }
}

void AddDialogEntries(const GuideContext& context, GuideResult& result) {
    EnsureGuideNarratives(context);
    for (size_t index = 0; index < context.narratives.size(); ++index) {
        const GuideNarrativeGroup& group = context.narratives[index];
        if (!group.first) continue;
        const GuideCommand& first = *group.first;
        std::wstring conditions;
        bool simpleAdjacent = false;
        std::pair<int, int> adjacent;
        const bool rendered = RenderConditions(context, first.conditions, conditions, simpleAdjacent, adjacent);
        GuideEntry entry;
        entry.filter = GuideFilter::Dialog;
        entry.narrativeGroup = index;
        entry.keepWithoutResult = !group.duels.empty();
        entry.conditionKey = ConditionKey(first.event, first.conditions);
        entry.prefix = rendered ? (simpleAdjacent ? conditions + L"后，" : WhenLead(conditions)) :
            RewardPrefix(context, first, true);
        if (group.hasDialog) entry.actions.push_back(simpleAdjacent ? L"触发对话" :
            (group.speakers.empty() ? L"触发对话" : JoinNames(group.speakers) + L"触发对话"));
        for (const GuideCommand* duel : group.duels) {
            const std::wstring left = PersonReferenceName(context, *duel, 0, duel->values[0]);
            const std::wstring right = PersonReferenceName(context, *duel, 1, duel->values[1]);
            if (left.empty() || right.empty()) continue;
            const bool matching = simpleAdjacent &&
                ((adjacent.first == duel->values[0] && adjacent.second == duel->values[1]) ||
                 (adjacent.first == duel->values[1] && adjacent.second == duel->values[0]));
            entry.actions.push_back(matching ? L"触发单挑" : left + L"与" + right + L"单挑");
        }
        if (entry.actions.empty()) continue;
        entry.sourceActions.push_back({entry.actions.front(), GuideOrigin(context, first)});
        entry.text = entry.prefix + JoinGuideActions(entry.actions) + L"。";
        result.entries.push_back(std::move(entry));
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

struct GuideAiChange {
    const GuideCommand* command = nullptr;
    std::wstring action;
    bool targetResolved = false;
};

std::wstring GuideAiRawCommand(const GuideCommand& command) {
    std::wstring result = L"原始指令 " + std::to_wstring(command.id) + L" 参数[";
    for (size_t i = 0; i < command.values.size(); ++i) {
        if (i) result += L",";
        result += std::to_wstring(command.values[i]);
    }
    result += L"] 标签[";
    for (size_t i = 0; i < command.parameterTags.size(); ++i) {
        if (i) result += L",";
        result += std::to_wstring(command.parameterTags[i]);
    }
    return result + L"]";
}

std::wstring GuideAiPerson(const GuideContext& context, const GuideCommand& command,
    size_t valueIndex, int32_t value, bool& resolved) {
    resolved = false;
    if (value >= 0 && value < 1024) {
        resolved = true;
        const std::wstring name = PersonReferenceName(context, command, valueIndex, value);
        std::wstring side;
        bool ambiguous = false;
        // 4B describes a deployment slot, not a literal person. Only explicit
        // 46/47 person fields establish an initial side for this description.
        for (const auto& deployment : context.commands) {
            const size_t stride = deployment.id == 0x46 ? 11u : deployment.id == 0x47 ? 12u : 0u;
            if (!stride || deployment.values.size() % stride != 0) continue;
            for (size_t base = 0; base < deployment.values.size(); base += stride) {
                if (deployment.values[base] != value || base >= deployment.parameterTags.size() ||
                    deployment.parameterTags[base] != 2) continue;
                const std::wstring candidate = deployment.id == 0x46 ? L"友军" :
                    deployment.values[base + 1] != 0 ? L"援军" : L"敌军";
                if (!side.empty() && side != candidate) ambiguous = true;
                side = candidate;
            }
        }
        return (ambiguous ? std::wstring() : side) + name;
    }
    if (value >= 1024 && value <= 1027)
        return L"特殊人物选择器 " + std::to_wstring(value) + L" 指定的部队";
    if (value >= 1028 && value <= 1277) {
        resolved = true;
        return L"战场编号 " + std::to_wstring(value - 1028) + L" 的部队";
    }
    if (value <= -2)
        return L"由脚本变量 " + std::to_wstring(-static_cast<int64_t>(value) - 2) + L" 指定的部队";
    return L"人物参数 " + std::to_wstring(value) + L" 指定的部队";
}

std::wstring GuideAiCoordinate(int32_t value, bool& resolved) {
    if (value < 0) {
        resolved = false;
        // Type 04 dereferences a numeric variable for negative parameters;
        // its encoding is not the tag-02 (-2-N) person encoding.
        return L"数值变量引用 " + std::to_wstring(value);
    }
    return std::to_wstring(value);
}

GuideAiChange DecodeGuidePolicyChange(const GuideContext& context, const GuideCommand& command) {
    GuideAiChange result;
    result.command = &command;
    static const std::vector<uint16_t> tags = {0x2C,2,4,4,4,4,3,7,2,4,4};
    if (command.values.size() != 11 || command.parameterTags != tags) {
        result.action = L"部队 AI／控制状态变更（" + GuideAiRawCommand(command) + L"）";
        GuideDiagnostic(context, command, L"ai-command-layout-unresolved: " + GuideAiRawCommand(command));
        return result;
    }
    const auto& values = command.values;
    std::wstring target;
    result.targetResolved = true;
    // Native 4E normalizes the WORD mode FFFF to zero before dispatching
    // its single-person branch (6.4/6.5/6.6: 00414726..00414744).
    if (values[0] == 0 || values[0] == -1) {
        target = GuideAiPerson(context, command, 1, values[1], result.targetResolved);
    } else if (values[0] == 1 && values[6] >= 0 && values[6] <= 6) {
        static const wchar_t* camps[] = {L"我军", L"友军", L"敌军", L"援军",
            L"我军及友军", L"敌军及援军", L"全部部队"};
        int32_t x1 = values[2], y1 = values[3], x2 = values[4], y2 = values[5];
        if (x1 >= 0 && x2 >= 0 && y1 >= 0 && y2 >= 0) {
            if (x1 > x2) std::swap(x1, x2);
            if (y1 > y2) std::swap(y1, y2);
        }
        target = L"（" + GuideAiCoordinate(x1, result.targetResolved) + L"，" +
            GuideAiCoordinate(y1, result.targetResolved) + L"）至（" +
            GuideAiCoordinate(x2, result.targetResolved) + L"，" +
            GuideAiCoordinate(y2, result.targetResolved) + L"）区域内的" + camps[values[6]];
    } else {
        result.targetResolved = false;
        result.action = L"部队 AI／控制状态变更（" + GuideAiRawCommand(command) + L"）";
        GuideDiagnostic(context, command, L"ai-target-unresolved: " + GuideAiRawCommand(command));
        return result;
    }
    const int policy = values[7];
    if (policy == 7) result.action = target + L"可由玩家控制";
    else {
        const std::wstring policyName = PolicyName(policy);
        result.action = target + L"的行动方针设为" +
            (policyName.empty() ? L"未知方针码 " + std::to_wstring(policy) : policyName);
        if (policy == 3 || policy == 5) {
            bool referenceResolved = false;
            const std::wstring reference = GuideAiPerson(context, command, 8, values[8], referenceResolved);
            result.targetResolved = result.targetResolved && referenceResolved;
            result.action += L"（目标：" + reference + L"）";
        } else if (policy == 4 || policy == 6) {
            result.action += L"（目标地点：" + GuideAiCoordinate(values[9], result.targetResolved) +
                L"，" + GuideAiCoordinate(values[10], result.targetResolved) + L"）";
        }
        if (policyName.empty())
            GuideDiagnostic(context, command, L"ai-policy-unresolved: " + GuideAiRawCommand(command));
    }
    return result;
}

bool GuideAiKeepsIntegerConstants(unsigned id) {
    // These commands have no integer-variable write side effect. Everything
    // else ends the small constant-propagation window, including script calls,
    // jumps, arbitrary memory operations, and unaudited extension commands.
    switch (id) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05:
    case 0x09: case 0x0B:
    case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A:
    case 0x25: case 0x26: case 0x2D: case 0x2E: case 0x36: case 0x3C:
    case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x63: case 0x68: case 0x69: case 0x79: case 0x7B:
        return true;
    default:
        return false;
    }
}

std::vector<GuideAiChange> CollectGuideAiChanges(const GuideContext& context) {
    using Constants = std::unordered_map<int32_t, int32_t>;
    struct Block {
        size_t id = 0;
        size_t parent = SIZE_MAX;
        size_t owner = SIZE_MAX;
        std::vector<const GuideCommand*> commands;
    };
    std::vector<Block> blocks;
    std::unordered_map<size_t, size_t> blockIndex;
    std::unordered_map<size_t, std::vector<size_t>> children;
    for (const auto& command : context.commands) {
        auto inserted = blockIndex.emplace(command.block, blocks.size());
        if (inserted.second) {
            blocks.push_back({command.block, command.parentBlock, command.blockOwnerOrdinal, {}});
            if (command.parentBlock != SIZE_MAX) children[command.blockOwnerOrdinal].push_back(inserted.first->second);
        }
        blocks[inserted.first->second].commands.push_back(&command);
    }
    std::vector<GuideAiChange> changes;
    auto intersect = [](Constants& entry, const Constants& exit) {
        for (auto it = entry.begin(); it != entry.end();) {
            const auto found = exit.find(it->first);
            if (found == exit.end() || found->second != it->second) it = entry.erase(it);
            else ++it;
        }
    };
    auto walk = [&](auto&& self, size_t index, Constants state, size_t depth) -> Constants {
        if (depth > 512) { state.clear(); return state; }
        const Block& block = blocks[index];
        for (const GuideCommand* pointer : block.commands) {
            const GuideCommand& command = *pointer;
            const auto& values = command.values;
            if (command.id == 0x4E) {
                changes.push_back(DecodeGuidePolicyChange(context, command));
                if (values.size() != 11 || command.parameterTags !=
                    std::vector<uint16_t>({0x2C,2,4,4,4,4,3,7,2,4,4})) state.clear();
            } else if (command.id == 0x77) {
                const bool standard = values.size() == 5 && command.parameterTags ==
                    std::vector<uint16_t>({0x51,4,0x53,0x52,4});
                if (!standard || values[0] != 2 || values[1] < 0 || values[1] >= 4096) {
                    state.clear();
                } else if (values[2] == 2 && values[3] == 0) {
                    state[values[1]] = values[4];
                } else {
                    state.erase(values[1]);
                }
            } else if (command.id == 0x78) {
                const bool standard = values.size() == 4 && command.parameterTags ==
                    std::vector<uint16_t>({4,0x54,2,0x55});
                if (!standard) {
                    // A nonstandard 78 layout cannot be ruled out as a control
                    // write. Retain its raw action rather than silently drop it.
                    changes.push_back({&command, L"行动方针／控制状态变更（" +
                        GuideAiRawCommand(command) + L"）", false});
                    GuideDiagnostic(context, command, L"ai-variable-layout-unresolved: " + GuideAiRawCommand(command));
                    state.clear();
                } else if (values[1] == 0) {
                    if (values[0] >= 0) state.erase(values[0]);
                    else state.clear();
                } else if (values[1] == 1) {
                    if (values[3] == 32) {
                        bool resolved = false;
                        const std::wstring target = GuideAiPerson(context, command, 2, values[2], resolved);
                        const auto known = state.find(values[0]);
                        std::wstring action;
                        if (values[0] >= 0 && known != state.end()) {
                            const unsigned byte = static_cast<unsigned char>(static_cast<uint32_t>(known->second));
                            if (byte == 7) action = target + L"可由玩家控制";
                            else if (byte == 8) action = target + L"设为不可控";
                            else action = target + L"的行动方针／控制状态原始字节设为 " + std::to_wstring(byte);
                            GuideDiagnostic(context, command, L"ai-variable-constant: variable=" +
                                std::to_wstring(values[0]) + L" raw=" + std::to_wstring(known->second) +
                                L" byte=" + std::to_wstring(byte));
                        } else {
                            action = target + L"的行动方针／控制状态由脚本变量 " +
                                std::to_wstring(values[0]) + L" 决定";
                            GuideDiagnostic(context, command, L"ai-variable-unresolved: " + GuideAiRawCommand(command));
                        }
                        changes.push_back({&command, std::move(action), resolved});
                    }
                } else {
                    state.clear();
                    if (values[3] == 32) {
                        changes.push_back({&command, L"行动方针／控制状态变更（" +
                            GuideAiRawCommand(command) + L"）", false});
                        GuideDiagnostic(context, command, L"ai-variable-direction-unresolved: " + GuideAiRawCommand(command));
                    }
                }
            } else if (!GuideAiKeepsIntegerConstants(command.id)) {
                state.clear();
            }
            const auto found = children.find(command.ordinal);
            if (found == children.end()) continue;
            for (size_t childIndex : found->second) {
                if (blocks[childIndex].parent != block.id) continue;
                const Constants exit = self(self, childIndex, state, depth + 1);
                // A section body is unconditional. Other subevents may not
                // execute; keep only facts true both on entry and on exit.
                if (command.id == 0 && block.parent == SIZE_MAX) state = exit;
                else intersect(state, exit);
            }
        }
        return state;
    };
    for (size_t index = 0; index < blocks.size(); ++index)
        if (blocks[index].parent == SIZE_MAX) walk(walk, index, Constants{}, 0);
    std::stable_sort(changes.begin(), changes.end(), [](const GuideAiChange& left, const GuideAiChange& right) {
        return left.command->ordinal < right.command->ordinal;
    });
    return changes;
}
void AddAiEntries(const GuideContext& context, GuideResult& result) {
    EnsureGuideNarratives(context);
    for (const GuideAiChange& change : CollectGuideAiChanges(context)) {
        const GuideCommand& command = *change.command;
        if (context.narrativeOwners.find(command.ordinal) == context.narrativeOwners.end()) {
            const bool priorDialogue = std::any_of(context.narratives.begin(), context.narratives.end(),
                [&](const GuideNarrativeGroup& group) {
                    return group.first && group.first->event == command.event &&
                        group.first->ordinal < command.ordinal;
                });
            if (priorDialogue) GuideDiagnostic(context, command, L"ai-narrative-order-or-path-unproven");
        }
        AddActionEntry(result, GuideFilter::Ai, ConditionKey(command.event, command.conditions),
            RewardPrefix(context, command, true), change.action, &context, &command);
    }
}

const wchar_t* GuideAbilityName(int attribute) {
    static const wchar_t* names[] = {
        L"攻击", L"防御", L"精神", L"爆发", L"士气", L"HP", L"MP",
        L"当前HP", L"当前MP", L"等级", L"武力", L"统帅", L"智力", L"敏捷", L"运气", L"头像"
    };
    return attribute >= 0 && attribute < static_cast<int>(_countof(names)) ?
        names[attribute] : nullptr;
}

void GuideDiagnostic(const GuideContext& context, const GuideCommand& command,
    const std::wstring& message) {
    const std::wstring entry = context.sourcePath + L" section=" + std::to_wstring(command.section) +
        L" offset=" + std::to_wstring(command.offset) + L" block=" + std::to_wstring(command.block) +
        L" event=" + std::to_wstring(command.event) + L" ordinal=" + std::to_wstring(command.ordinal) +
        L" command=" + std::to_wstring(command.id) +
        L": " + message + L"\n";
    if (context.diagnostics) *context.diagnostics += entry;
    OutputDebugStringW(entry.c_str());
}

void RecordUnknownGuideConditions(const GuideContext& context) {
    for (const auto& command : context.commands) for (const auto& condition : command.conditions) {
        if (condition.id != 0x7B) continue;
        std::wstring rendered;
        if (RenderConditionalExtension(context, condition, rendered)) continue;
        std::wstring detail = L"condition-unresolved: id=123 (action retained); values=";
        for (int32_t value : condition.values) detail += std::to_wstring(value) + L",";
        for (const auto& text : condition.texts) detail += L" text=" + text.substr(0, 256);
        GuideDiagnostic(context, command, detail);
    }
}

bool DecodeAbilityChangeCommand(const GuideCommand& command, GuideAbilityChange& result) {
    result = GuideAbilityChange();
    result.ordinal = command.ordinal;
    result.block = command.block;
    result.event = command.event;
    // 0x38 is a fixed four-argument command: person, ability, operation,
    // amount.  Refuse malformed records instead of silently treating a
    // truncated or extended command as a valid ability change.  The parser
    // already records the command location, so callers can continue scanning
    // the rest of the event stream when this one is malformed.
    if (command.id != 0x38 || command.values.size() != 4) return false;
    result.personId = command.values[0];
    result.attribute = command.values[1];
    result.rawOperation = command.values[2];
    result.amount = command.values[3];
    if (result.personId < 0 || result.personId >= 0xFFFF) return false;
    if (result.rawOperation == 1) result.operation = GuideAbilityOperation::Increase;
    else if (result.rawOperation == 2) result.operation = GuideAbilityOperation::Decrease;
    else if (result.rawOperation == 0) result.operation = GuideAbilityOperation::Set;
    else result.operation = GuideAbilityOperation::Unknown;
    return true;
}

bool IsBestEffortRewardCommand(int commandId) {
    switch (commandId) {
    case 0x38: // 武将能力设定
    case 0x39: // 等级变化
    case 0x3A: // 金钱变化
    case 0x3B: // 武将加入
    case 0x3D: // 道具奖励
    case 0x53: // 撤退/死亡
        return true;
    default:
        return false;
    }
}

std::wstring FormatAbilityChange(const GuideContext& context,
    const GuideAbilityChange& change, const GuideCommand* command = nullptr) {
    std::wstring person = command ?
        PersonReferenceName(context, *command, 0, change.personId) :
        PersonName(context, change.personId);
    if (person.empty()) person = L"武将 " + std::to_wstring(change.personId);
    const wchar_t* attribute = change.attribute >= 0 && change.attribute <= 14 ?
        GuideAbilityName(change.attribute) : nullptr;
    if (!attribute) {
        return person + L"能力码 " + std::to_wstring(change.attribute) +
            L" 操作码 " + std::to_wstring(change.rawOperation) +
            L" 变化 " + std::to_wstring(change.amount);
    }
    std::wstring action;
    switch (change.operation) {
    case GuideAbilityOperation::Increase: action = L"提升 "; break;
    case GuideAbilityOperation::Decrease: action = L"下降 "; break;
    case GuideAbilityOperation::Set: action = L"设为 "; break;
    default:
        return person + attribute + L"操作码 " + std::to_wstring(change.rawOperation) +
            L" 变化 " + std::to_wstring(change.amount);
    }
    return person + attribute + action + std::to_wstring(change.amount) + L" 点";
}

std::wstring RewardPrefix(const GuideContext& context, const GuideCommand& command,
    bool allowBestEffort = false) {
    EnsureGuideNarratives(context);
    const GuideCommand* duel = nullptr;
    const auto owner = context.narrativeOwners.find(command.ordinal);
    if (owner != context.narrativeOwners.end()) {
        for (const GuideCommand* candidate : context.narratives[owner->second].duels)
            if (candidate->ordinal < command.ordinal) duel = candidate;
    }
    // A later child may add conditions after the duel. Its own trigger must
    // still be rendered, including unresolved-condition diagnostics.
    if (duel && duel->values.size() >= 2 &&
        ConditionKey(command.event, command.conditions) == ConditionKey(duel->event, duel->conditions)) {
        const std::wstring first = PersonReferenceName(context, *duel, 0, duel->values[0]);
        const std::wstring second = PersonReferenceName(context, *duel, 1, duel->values[1]);
        if (!first.empty() && !second.empty()) return L"完成" + first + L"与" + second + L"的单挑后，";
    }
    std::wstring conditions;
    bool simpleAdjacent = false;
    std::pair<int, int> adjacent;
    if (command.conditions.empty())
        return allowBestEffort ? L"事件执行后，" : std::wstring();
    if (RenderConditions(context, command.conditions, conditions, simpleAdjacent, adjacent))
        return AfterLead(conditions);
    if (!allowBestEffort) return {};
    std::vector<std::wstring> known;
    for (const auto& condition : command.conditions) {
        std::wstring part;
        if (RenderConditions(context, {condition}, part, simpleAdjacent, adjacent))
            AddUnique(known, part);
        else
            GuideDiagnostic(context, command, L"condition-unresolved: id=" + std::to_wstring(condition.id));
    }
    if (known.empty()) return L"事件执行后，";
    conditions.clear();
    for (const auto& part : known) {
        if (!conditions.empty()) conditions += L"且";
        conditions += part;
    }
    return conditions + L"且满足其他事件条件后，";
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
        text = PersonReferenceName(context, command, 1, command.values[1]);
        return !text.empty();
    }
    if (command.values[0] == 1) {
        const GuideUnit* unit = UnitByBattlefieldNumber(context, command.values[2]);
        text = unit ? PersonReferenceName(context, command, 2, unit->id) : std::wstring();
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
        const GuideCommand* first = nullptr;
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
        if (!found->first) found->first = &command;
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
                group.target + summary, &context, group.first);
    }
}

bool SingleRetreatTarget(const GuideContext& context, const GuideCommand& command,
    int& id, PersonSide& side, std::wstring& person) {
    id = -1;
    side = PersonSide::Unknown;
    if (command.values.size() < 3) return false;
    if (command.values[0] == 0) {
        id = command.values[1];
        person = PersonReferenceName(context, command, 1, id);
        side = SideForPerson(context, id);
        if (id == 1025) side = PersonSide::Friendly;
        if (id == 1026) side = PersonSide::Enemy;
        return !person.empty();
    }
    if (command.values[0] != 1) return false;
    const GuideUnit* unit = UnitByBattlefieldNumber(context, command.values[2]);
    if (!unit) return false;
    id = unit->id;
    person = ConditionPersonName(context, id);
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
    EnsureGuideNarratives(context);
    const auto owner = context.narrativeOwners.find(command.ordinal);
    return owner != context.narrativeOwners.end() &&
        context.narratives[owner->second].first->ordinal < command.ordinal;
}

void AddRewardEntries(const GuideContext& context, GuideResult& result) {
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
            const std::wstring item = ItemDisplayName(context, itemId);
            if (item.empty()) unresolvedBattleReward = true;
            else {
                if (ItemName(context, itemId).empty())
                    GuideDiagnostic(context, command, L"item-name-missing: id=" + std::to_wstring(itemId));
                AddUnique(items, item);
            }
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
        const std::wstring prefix = RewardPrefix(context, command,
            IsBestEffortRewardCommand(command.id));
        if (prefix.empty()) continue;
        const std::wstring conditionKey = ConditionKey(command.event, command.conditions);
        if (command.id == 0x3D && !command.values.empty()) {
            const std::wstring item = ItemDisplayName(context, command.values[0]);
            if (!item.empty()) {
                if (ItemName(context, command.values[0]).empty())
                    GuideDiagnostic(context, command, L"item-name-missing: id=" +
                        std::to_wstring(command.values[0]));
                AddActionEntry(result, GuideFilter::Rewards, conditionKey, prefix,
                    L"获得" + item, &context, &command);
            }
        } else if (command.id == 0x38) {
            GuideAbilityChange change;
            if (!DecodeAbilityChangeCommand(command, change)) {
                GuideDiagnostic(context, command, L"ability-command-invalid");
                continue;
            }
            if (change.attribute == 15) {
                GuideDiagnostic(context, command, L"ability-code-excluded: id=15");
                continue;
            }
            if (PersonName(context, change.personId).empty())
                GuideDiagnostic(context, command, L"person-name-missing: id=" + std::to_wstring(change.personId));
            if (change.attribute < 0 || change.attribute > 14)
                GuideDiagnostic(context, command, L"ability-code-unknown: id=" + std::to_wstring(change.attribute));
            if (change.operation == GuideAbilityOperation::Unknown)
                GuideDiagnostic(context, command, L"ability-operation-unknown: id=" + std::to_wstring(change.rawOperation));
            AddActionEntry(result, GuideFilter::Rewards, conditionKey, prefix,
                FormatAbilityChange(context, change, &command), &context, &command);
        } else if (command.id == 0x3A && command.values.size() >= 3 && command.values[0] == 0 &&
            (command.values[1] == 1 || command.values[1] == 2) && command.values[2] > 0) {
            AddActionEntry(result, GuideFilter::Rewards, conditionKey, prefix,
                (command.values[1] == 1 ? L"获得 " : L"失去 ") +
                std::to_wstring(command.values[2]) + L" 金钱", &context, &command);
        } else if (command.id == 0x3B && command.values.size() == 3 && command.values[1] == 0) {
            if (command.parameterTags != std::vector<uint16_t>({0x02, 0x0E, 0x3E}) ||
                command.values[0] < 0 || command.values[0] >= 1024) {
                GuideDiagnostic(context, command, L"join-dynamic-or-unknown-parameters");
                continue;
            }
            std::wstring person = PersonReferenceName(context, command, 0, command.values[0]);
            if (person.empty()) person = L"武将 " + std::to_wstring(command.values[0]);
            AddActionEntry(result, GuideFilter::Rewards, conditionKey, prefix, person + L"加入队伍", &context, &command);
        } else if (command.id == 0x39 && command.values.size() >= 2 && command.values[1] > 0) {
            const std::wstring person = PersonReferenceName(context, command, 0, command.values[0]);
            if (!person.empty()) AddActionEntry(result, GuideFilter::Rewards, conditionKey, prefix,
                person + L"提升 " + std::to_wstring(command.values[1]) + L" 级", &context, &command);
        } else if (command.id == 0x53 && command.values.size() >= 8) {
            int personId = -1;
            PersonSide side = PersonSide::Unknown;
            std::wstring person;
            if (!SingleRetreatTarget(context, command, personId, side, person)) continue;
            if (HasMatchingRetreatCondition(command, personId) ||
                !HasNarrativeTriggerBefore(context, command)) continue;
            if (command.values[7] != 0)
                AddActionEntry(result, GuideFilter::Rewards, conditionKey, prefix, person + L"死亡", &context, &command);
            else
                AddActionEntry(result, GuideFilter::Rewards, conditionKey, prefix, person + L"撤退", &context, &command);
        }
    }
}

std::wstring EntryConsolidationKey(const GuideEntry& entry) {
    if (entry.preserveText || entry.prefix.empty() || entry.actions.empty() ||
        entry.sourceActions.empty() || entry.hideInAll) return {};
    const GuideActionOrigin& origin = entry.sourceActions.front().origin;
    std::wstring key = std::to_wstring(static_cast<int>(entry.filter)) + L"|" +
        origin.source + L"|" + std::to_wstring(origin.scene) + L"|" +
        ConditionKey(origin.event, origin.conditions);
    // Dialogue branches stay separate; effect summaries may combine equal
    // event/condition entries even when they originated in sibling blocks.
    if (entry.filter == GuideFilter::Dialog)
        key += L"|block:" + std::to_wstring(origin.block);
    return key;
}

void ConsolidateEquivalentEntries(GuideResult& result) {
    std::vector<GuideEntry> entries;
    std::unordered_map<std::wstring, size_t> positions;
    for (GuideEntry& entry : result.entries) {
        const std::wstring key = EntryConsolidationKey(entry);
        if (key.empty()) {
            entries.push_back(std::move(entry));
            continue;
        }
        auto found = positions.find(key);
        if (found == positions.end()) {
            positions.emplace(key, entries.size());
            entries.push_back(std::move(entry));
            continue;
        }
        GuideEntry& target = entries[found->second];
        for (const std::wstring& action : entry.actions)
            if (std::find(target.actions.begin(), target.actions.end(), action) == target.actions.end())
                target.actions.push_back(action);
        target.sourceActions.insert(target.sourceActions.end(),
            entry.sourceActions.begin(), entry.sourceActions.end());
        target.text = target.prefix + JoinGuideActions(target.actions) + L"。";
    }
    result.entries = std::move(entries);
}

void MergeAndPruneNarrativeEntries(GuideResult& result) {
    std::vector<GuideEntry*> effects;
    for (auto& entry : result.entries)
        if ((entry.filter == GuideFilter::Rewards || entry.filter == GuideFilter::Ai) &&
            !entry.sourceActions.empty()) effects.push_back(&entry);
    std::stable_sort(effects.begin(), effects.end(), [](const GuideEntry* left, const GuideEntry* right) {
        const auto& a = left->sourceActions.front().origin;
        const auto& b = right->sourceActions.front().origin;
        return a.source == b.source ? a.ordinal < b.ordinal : a.source < b.source;
    });
    std::vector<GuideEntry> dialogues;
    for (const GuideEntry& original : result.entries) {
        if (original.filter != GuideFilter::Dialog || original.sourceActions.empty()) continue;
        const GuideActionOrigin& trigger = original.sourceActions.front().origin;
        struct Branch {
            GuideEntry entry;
            std::vector<std::wstring> outcomes;
            std::wstring outcomePrefix;
            bool conditional = false;
        };
        std::vector<Branch> variants;
        for (GuideEntry* pointer : effects) {
            GuideEntry& effect = *pointer;
            const GuideActionOrigin& origin = effect.sourceActions.front().origin;
            if (origin.source != trigger.source || origin.scene != trigger.scene ||
                origin.event != trigger.event || origin.narrativeGroup != original.narrativeGroup ||
                origin.ordinal <= trigger.ordinal) continue;
            // Separate conditional outcomes; equal text in sibling blocks is
            // insufficient to combine mutually exclusive effects.
            const std::wstring branchKey = effect.conditionKey + L"#block:" + std::to_wstring(origin.block);
            auto found = std::find_if(variants.begin(), variants.end(), [&](const Branch& branch) {
                return branch.entry.conditionKey == branchKey;
            });
            if (found == variants.end()) {
                Branch branch;
                branch.entry = original;
                branch.entry.conditionKey = branchKey;
                branch.outcomePrefix = effect.prefix;
                branch.conditional = effect.conditionKey != original.conditionKey;
                variants.push_back(std::move(branch));
                found = variants.end() - 1;
            }
            for (const auto& action : effect.actions)
                if (found->outcomes.empty() || found->outcomes.back() != action) found->outcomes.push_back(action);
            // AI remains visible in its own chapter, by user preference.
            if (effect.filter == GuideFilter::Rewards) effect.hideInAll = true;
        }
        if (variants.empty() && original.keepWithoutResult) dialogues.push_back(original);
        for (Branch& branch : variants) {
            GuideEntry& entry = branch.entry;
            if (branch.conditional) {
                // The parent dialogue runs before this child test. Never move
                // the child's condition to the dialogue's own trigger.
                entry.preserveText = true;
                entry.text = original.prefix + JoinGuideActions(original.actions) +
                    L"；随后，" + branch.outcomePrefix + JoinGuideActions(branch.outcomes) + L"。";
            } else {
                entry.actions.insert(entry.actions.end(), branch.outcomes.begin(), branch.outcomes.end());
                entry.text = entry.prefix + JoinGuideActions(entry.actions) + L"。";
            }
            dialogues.push_back(std::move(entry));
        }
    }
    std::vector<GuideEntry> entries;
    for (auto& entry : dialogues) entries.push_back(std::move(entry));
    for (GuideEntry& entry : result.entries) {
        if (entry.filter == GuideFilter::Dialog) continue;
        // Aggregate only after ownership is fixed, and only within the same
        // trigger, path and block. Each source action remains available.
        auto found = entries.end();
        if (!entry.sourceActions.empty()) {
            const auto& origin = entry.sourceActions.front().origin;
            found = std::find_if(entries.begin(), entries.end(), [&](const GuideEntry& previous) {
                if (previous.filter != entry.filter || previous.prefix != entry.prefix ||
                    previous.conditionKey != entry.conditionKey || previous.hideInAll != entry.hideInAll ||
                    previous.sourceActions.empty()) return false;
                const auto& earlier = previous.sourceActions.back().origin;
                return earlier.source == origin.source && earlier.scene == origin.scene &&
                    earlier.event == origin.event && earlier.block == origin.block &&
                    earlier.narrativeGroup == origin.narrativeGroup && earlier.ordinal < origin.ordinal;
            });
        }
        if (found == entries.end()) entries.push_back(std::move(entry));
        else {
            // Preserve successive assignments, including a return to an
            // earlier policy; remove only adjacent identical observations.
            for (const auto& action : entry.actions)
                if (found->actions.empty() || found->actions.back() != action) found->actions.push_back(action);
            found->sourceActions.insert(found->sourceActions.end(), entry.sourceActions.begin(), entry.sourceActions.end());
            found->text = found->prefix + JoinGuideActions(found->actions) + L"。";
        }
    }
    result.entries = std::move(entries);
    ConsolidateEquivalentEntries(result);
}

std::wstring FileTitle(const std::wstring& path, int number);

void PersistGuideDiagnostics(const std::wstring& root, const std::wstring& diagnostics) {
    if (diagnostics.empty()) return;
    static std::mutex lock;
    std::lock_guard<std::mutex> guard(lock);
    const std::wstring patch = root + L"\\patch";
    const std::wstring privateRoot = PatchPrivatePath(root, CompiledEngineProfile());
    const std::wstring directory = privateRoot + L"\\diagnostics";
    CreateDirectoryW(patch.c_str(), nullptr);
    CreateDirectoryW(privateRoot.c_str(), nullptr);
    CreateDirectoryW(directory.c_str(), nullptr);
    const std::wstring path = directory + L"\\guide.log";
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(file, &size) && size.QuadPart < 1024 * 1024 && diagnostics.size() < 64 * 1024) {
        DWORD written = 0;
        if (!size.QuadPart) { const WORD bom = 0xFEFF; WriteFile(file, &bom, sizeof(bom), &written, nullptr); }
        WriteFile(file, diagnostics.data(), static_cast<DWORD>(diagnostics.size() * sizeof(wchar_t)), &written, nullptr);
    }
    CloseHandle(file);
}

GuideResult AnalyzeScenario(const GuideFile& file, const std::wstring& root, const std::atomic<bool>& cancelled,
    bool persistDiagnostics = true, const BattleResourceSnapshot* shared = nullptr) {
    GuideResult result;
    result.title = FileTitle(file.battlefieldPath, file.battlefieldNumber) + L"简易攻略";
    if (cancelled) return result;
    std::vector<unsigned char> bytes;
    GuideContext context;
    context.sourcePath = file.battlefieldPath;
    context.diagnostics = &result.diagnostics;
    const bool readable = ReadFileBytes(file.battlefieldPath, bytes);
    if (readable && ParseScenarioScript(bytes, context.commands, &cancelled, &result.diagnostics)) {
        result.parsed = true;
        if (cancelled) return result;
        LoadNames(root, context, &cancelled, shared);
        result.resources = context.resources;
        result.hasResources = true;
        RecordUnknownGuideConditions(context);
        CollectDeployments(context, context.units);
        AddDialogEntries(context, result);
        AddAmbushEntries(context, result);
        AddAiEntries(context, result);
        AddRewardEntries(context, result);
        for (auto& entry : result.entries) entry.conditionKey = file.battlefieldPath + L"|" + entry.conditionKey;
    }
    // The R rewards are useful even when there is no valid S jump target.
    // Keep file identity in the key so equal event ordinals in R and S cannot
    // attach campaign rewards to an unrelated battlefield dialogue.
    if (!file.isBattlefield && _wcsicmp(file.path.c_str(), file.battlefieldPath.c_str()) != 0) {
        std::vector<unsigned char> rbytes;
        GuideContext rcontext;
        rcontext.sourcePath = file.path;
        rcontext.diagnostics = &result.diagnostics;
        std::wstring parseDiagnostics;
        if (!cancelled && ReadFileBytes(file.path, rbytes) &&
            ParseScenarioScript(rbytes, rcontext.commands, &cancelled, &parseDiagnostics) && !cancelled) {
            result.parsed = true;
            LoadNames(root, rcontext, &cancelled, result.hasResources ? &result.resources : shared);
            result.resources = rcontext.resources;
            result.hasResources = true;
            RecordUnknownGuideConditions(rcontext);
            const size_t first = result.entries.size();
            AddRewardEntries(rcontext, result);
            for (size_t i = first; i < result.entries.size(); ++i)
                result.entries[i].conditionKey = file.path + L"|" + result.entries[i].conditionKey;
        }
        result.diagnostics += parseDiagnostics;
    }
    if (persistDiagnostics) PersistGuideDiagnostics(root, result.diagnostics);
    if (!result.parsed) result.readStatus = readable ? L"剧本解析失败，请检查剧本格式与攻略诊断。" : L"攻略剧本文件不可读。";
    else if (result.hasResources && (!result.resources.peopleReady || !result.resources.itemsReady))
        result.readStatus = L"名称表尚未完整就绪，缺失名称使用明确的 ID 回退；进入战场或读档后可重新打开攻略。";
    else if (result.diagnostics.find(L"name-missing:") != std::wstring::npos)
        result.readStatus = L"部分名称缺失，相应事件保留并使用 ID 回退。";
    MergeAndPruneNarrativeEntries(result);
    return result;
}

GuideResult ReadGuideDisplay(const GuideFile& file, const std::wstring& root,
    const std::atomic<bool>& cancelled) {
    GuideBook book;
    std::wstring body;
    if (ReadGuideBook(root, book, &cancelled) && !cancelled.load() &&
        GetAuthoredGuideBody(book, file.battlefieldNumber, body)) {
        GuideResult result;
        result.title = FileTitle(file.battlefieldPath, file.battlefieldNumber) + L"简易攻略";
        result.authored = true;
        result.authoredBody = std::move(body);
        return result;
    }
    return AnalyzeScenario(file, root, cancelled);
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

std::wstring RenderGuideBody(const GuideResult& result);

struct GuideExportPiece {
    int number = 0;
    std::wstring body;
};

std::wstring TrimGuideLineBreaks(std::wstring value) {
    while (value.size() >= 2 && value[value.size() - 2] == L'\r' && value.back() == L'\n')
        value.resize(value.size() - 2);
    return value;
}

std::wstring BuildGuideExportText(const std::vector<GuideExportPiece>& pieces) {
    std::wstring output;
    for (const GuideExportPiece& piece : pieces) {
        if (!output.empty()) output += L"\r\n\r\n";
        output += FileTitle(L"", piece.number) + L"简易攻略\r\n";
        output += TrimGuideLineBreaks(piece.body);
    }
    return output;
}

std::wstring GuideSystemError(const wchar_t* action, DWORD code) {
    wchar_t* description = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
        reinterpret_cast<wchar_t*>(&description), 0, nullptr);
    std::wstring error = action;
    if (description) { error += L"\n"; error += description; LocalFree(description); }
    error += L"\n错误代码：" + std::to_wstring(code);
    return error;
}

bool WriteGuideBookFile(const std::wstring& root, const std::wstring& text, std::wstring& error,
    const std::atomic<bool>* cancelled = nullptr, const BattleResourceSnapshot* resources = nullptr) {
    error.clear();
    if (text.empty() || text.size() > static_cast<size_t>(INT_MAX)) {
        error = L"攻略内容为空或过长，未修改原文件。";
        return false;
    }
    const int bytesRequired = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.c_str(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (bytesRequired <= 0 || static_cast<size_t>(bytesRequired) > kGuideBookBytes - 3) {
        error = L"无法将攻略转换为 UTF-8。";
        return false;
    }
    std::string utf8(static_cast<size_t>(bytesRequired), '\0');
    if (bytesRequired > 0 && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.c_str(),
        static_cast<int>(text.size()), utf8.data(), bytesRequired, nullptr, nullptr) != bytesRequired) {
        error = L"无法将攻略转换为 UTF-8。";
        return false;
    }
    std::vector<unsigned char> bytes;
    bytes.reserve(3 + utf8.size());
    bytes.push_back(0xEF);
    bytes.push_back(0xBB);
    bytes.push_back(0xBF);
    bytes.insert(bytes.end(), utf8.begin(), utf8.end());

    const std::wstring target = root + L"\\简易攻略.txt";
    std::wstring temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 8 && file == INVALID_HANDLE_VALUE; ++attempt) {
        temporary = root + L"\\简易攻略.txt.tmp-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt);
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (file == INVALID_HANDLE_VALUE) {
        error = GuideSystemError(L"无法创建攻略临时文件。", GetLastError());
        return false;
    }
    DWORD written = 0;
    const bool wrote = bytes.empty() || (bytes.size() <= 0xffffffffu &&
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE &&
        written == bytes.size());
    const bool flushed = wrote && FlushFileBuffers(file) != FALSE;
    const DWORD writeError = flushed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (cancelled && cancelled->load()) {
        DeleteFileW(temporary.c_str());
        error = L"导出已取消。";
        return false;
    }
    if (resources && !IsBattleResourceSnapshotCurrent(*resources)) {
        DeleteFileW(temporary.c_str());
        error = L"运行时名称已变化，本次导出未替换原文件。";
        return false;
    }
    if (!flushed || MoveFileExW(temporary.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        const DWORD code = flushed ? GetLastError() : writeError;
        DeleteFileW(temporary.c_str());
        error = GuideSystemError(L"无法保存简易攻略.txt，原文件未修改。", code);
        return false;
    }
    return true;
}

bool EnumerateExportGuideFiles(const std::wstring& root, std::vector<GuideFile>& files,
    std::vector<int>& skipped, std::wstring& error, const std::atomic<bool>* cancelled) {
    files.clear();
    skipped.clear();
    const std::wstring pattern = root + L"\\RS\\S_*.eex";
    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
        error = GuideSystemError(L"无法扫描 RS 文件夹。", code);
        return false;
    }
    std::vector<GuideFile> candidates;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring path = root + L"\\RS\\" + data.cFileName;
        GuideFile file;
        if (!BuildGuideFile(path, root, file) || !file.isBattlefield) continue;
        // Empty placeholders are outside the requested export range.
        if (data.nFileSizeHigh == 0 && data.nFileSizeLow == 0) continue;
        candidates.push_back(std::move(file));
    } while (FindNextFileW(find, &data));
    const DWORD scanError = GetLastError();
    FindClose(find);
    if (scanError != ERROR_NO_MORE_FILES) {
        error = GuideSystemError(L"扫描 RS 文件夹时失败。", scanError);
        return false;
    }
    std::sort(candidates.begin(), candidates.end(), [](const GuideFile& left, const GuideFile& right) {
        if (left.number != right.number) return left.number < right.number;
        const std::wstring canonical = L"S_" + std::wstring(left.number < 10 ? L"0" : L"") +
            std::to_wstring(left.number) + L".eex";
        const bool leftCanonical = !_wcsicmp(left.name.c_str(), canonical.c_str());
        const bool rightCanonical = !_wcsicmp(right.name.c_str(), canonical.c_str());
        if (leftCanonical != rightCanonical) return leftCanonical;
        return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
    });
    for (size_t start = 0; start < candidates.size();) {
        size_t end = start + 1;
        while (end < candidates.size() && candidates[end].number == candidates[start].number) ++end;
        bool found = false;
        for (size_t index = start; index < end && !found; ++index) {
            if (cancelled && cancelled->load()) return false;
            std::vector<unsigned char> bytes;
            std::vector<GuideCommand> commands;
            if (ReadFileBytes(candidates[index].path, bytes) && !bytes.empty() &&
                ParseEex(bytes, commands, cancelled)) {
                files.push_back(std::move(candidates[index]));
                found = true;
            }
        }
        if (!found) skipped.push_back(candidates[start].number);
        start = end;
    }
    return true;
}

bool BuildGuideExportForRoot(const std::wstring& root, std::wstring& text,
    std::vector<int>& skipped, size_t& success, std::wstring& error,
    const std::atomic<bool>* cancelled = nullptr, BattleResourceSnapshot* usedResources = nullptr,
    unsigned retry = 0) {
    text.clear();
    error.clear();
    success = 0;
    std::vector<GuideFile> files;
    if (!EnumerateExportGuideFiles(root, files, skipped, error, cancelled)) {
        return false;
    }
    if (files.empty()) {
        error = L"没有找到可导出的有效 S 剧本。";
        return false;
    }
    std::vector<GuideExportPiece> pieces;
    pieces.reserve(files.size());
    size_t characters = 0;
    const BattleResourceSnapshot resources = CaptureGuideResources(root, cancelled);
    for (const GuideFile& file : files) {
        if (cancelled && cancelled->load()) return false;
        if (!IsBattleResourceSnapshotCurrent(resources)) {
            if (retry < 2) return BuildGuideExportForRoot(root, text, skipped, success, error,
                cancelled, usedResources, retry + 1);
            error = L"运行时名称在导出期间持续变化，请待读档或切关完成后重试。";
            return false;
        }
        const std::atomic<bool> neverCancelled(false);
        GuideResult result = AnalyzeScenario(file, root, cancelled ? *cancelled : neverCancelled, true, &resources);
        if (!result.parsed) {
            skipped.push_back(file.number);
            continue;
        }
        std::wstring body = RenderGuideBody(result);
        characters += body.size() + result.title.size() + 4;
        if (characters > kGuideBookBytes / sizeof(wchar_t)) {
            error = L"攻略内容超过 64 MiB，未修改原文件。";
            return false;
        }
        pieces.push_back({file.number, std::move(body)});
    }
    if (pieces.empty()) {
        error = L"没有成功生成任何攻略内容。";
        return false;
    }
    if (!IsBattleResourceSnapshotCurrent(resources)) {
        if (retry < 2) return BuildGuideExportForRoot(root, text, skipped, success, error,
            cancelled, usedResources, retry + 1);
        error = L"运行时名称在导出期间变化，本次结果未提交。";
        return false;
    }
    if (usedResources) *usedResources = resources;
    text = BuildGuideExportText(pieces);
    std::sort(skipped.begin(), skipped.end());
    skipped.erase(std::unique(skipped.begin(), skipped.end()), skipped.end());
    success = pieces.size();
    return !text.empty();
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

std::wstring GuideDisplaySettingsPath() {
    return gGuideRoot.empty() ? std::wstring() :
        PatchPrivatePath(gGuideRoot, CompiledEngineProfile()) + L"\\guide-ui.ini";
}

void NormalizeGuideDisplaySettings(GuideDisplaySettings& settings) {
    settings.fontSize = std::clamp(settings.fontSize,
        kGuideFontSizeMinimum, kGuideFontSizeMaximum);
    settings.lineSpacing = std::clamp(settings.lineSpacing,
        kGuideLineSpacingMinimum, kGuideLineSpacingMaximum);
}

void LoadGuideDisplaySettings() {
    gGuideDisplaySettings = GuideDisplaySettings();
    const std::wstring path = GuideDisplaySettingsPath();
    if (!path.empty()) {
        gGuideDisplaySettings.fontSize = GetPrivateProfileIntW(
            L"Guide", L"FontHeightTwips", gGuideDisplaySettings.fontSize, path.c_str());
        gGuideDisplaySettings.lineSpacing = GetPrivateProfileIntW(
            L"Guide", L"LineSpacingTwips", gGuideDisplaySettings.lineSpacing, path.c_str());
    }
    NormalizeGuideDisplaySettings(gGuideDisplaySettings);
}

bool SaveGuideDisplaySettings() {
    const std::wstring path = GuideDisplaySettingsPath();
    if (path.empty()) return false;
    CreateDirectoryW((gGuideRoot + L"\\patch").c_str(), nullptr);
    CreateDirectoryW(PatchPrivatePath(gGuideRoot, CompiledEngineProfile()).c_str(), nullptr);
    const std::wstring contents = L"\xfeff[Guide]\r\nFontHeightTwips=" +
        std::to_wstring(gGuideDisplaySettings.fontSize) + L"\r\nLineSpacingTwips=" +
        std::to_wstring(gGuideDisplaySettings.lineSpacing) + L"\r\n";
    std::wstring temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 8 && file == INVALID_HANDLE_VALUE; ++attempt) {
        temporary = path + L".tmp-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt);
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (file == INVALID_HANDLE_VALUE) return false;
    const DWORD bytes = static_cast<DWORD>(contents.size() * sizeof(wchar_t));
    DWORD written = 0;
    const bool saved = WriteFile(file, contents.data(), bytes, &written, nullptr) != FALSE &&
        written == bytes && FlushFileBuffers(file) != FALSE;
    const bool closed = CloseHandle(file) != FALSE;
    if (!saved || !closed || MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    // Flush the profile API cache so a later window reads the replaced file.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    return true;
}

bool ReadGuideDisplayValue(HWND control, int minimum, int maximum, int& value) {
    if (!control) return false;
    wchar_t buffer[64] = {};
    const int length = GetWindowTextW(control, buffer, static_cast<int>(_countof(buffer)));
    if (length <= 0 || length >= static_cast<int>(_countof(buffer))) return false;
    wchar_t* end = nullptr;
    const long parsed = wcstol(buffer, &end, 10);
    if (end == buffer || *end != L'\0' || parsed < minimum || parsed > maximum) return false;
    value = static_cast<int>(parsed);
    return true;
}

void WriteGuideDisplayControls() {
    if (gGuideFontSize)
        SetDlgItemInt(gGuideWindow, kGuideFontSize,
            static_cast<UINT>(gGuideDisplaySettings.fontSize), FALSE);
    if (gGuideLineSpacing)
        SetDlgItemInt(gGuideWindow, kGuideLineSpacing,
            static_cast<UINT>(gGuideDisplaySettings.lineSpacing), FALSE);
}

void ApplyGuideTextFormat(bool preservePosition = false) {
    if (!gGuideText) return;
    if (!gGuideUsesRichEdit) {
        int dpi = 96;
        HDC dc = GetDC(gGuideText);
        if (dc) { dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(gGuideText, dc); }
        HFONT font = CreateFontW(-MulDiv(gGuideDisplaySettings.fontSize, dpi, 1440),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        if (font) {
            SendMessageW(gGuideText, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            if (gGuideBodyFont) DeleteObject(gGuideBodyFont);
            gGuideBodyFont = font;
        }
        return;
    }
    CHARRANGE selection = {};
    POINT scroll = {};
    if (preservePosition) {
        SendMessageW(gGuideText, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
        SendMessageW(gGuideText, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
    }
    CHARFORMAT2W character = {};
    character.cbSize = sizeof(character);
    character.dwMask = CFM_FACE | CFM_SIZE;
    character.yHeight = gGuideDisplaySettings.fontSize;
    wcscpy_s(character.szFaceName, L"Microsoft YaHei UI");
    PARAFORMAT2 format = {};
    format.cbSize = sizeof(format);
    format.dwMask = PFM_SPACEAFTER;
    format.bLineSpacingRule = 1;
    format.dyLineSpacing = 0;
    format.dySpaceAfter = gGuideDisplaySettings.lineSpacing;
    SendMessageW(gGuideText, EM_SETSEL, 0, -1);
    SendMessageW(gGuideText, EM_SETCHARFORMAT, SCF_SELECTION,
        reinterpret_cast<LPARAM>(&character));
    SendMessageW(gGuideText, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&format));
    if (preservePosition) {
        SendMessageW(gGuideText, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
        SendMessageW(gGuideText, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
    } else {
        SendMessageW(gGuideText, EM_SETSEL, 0, 0);
        SendMessageW(gGuideText, EM_SCROLLCARET, 0, 0);
    }
}

void ApplyGuideDisplaySettingsFromControls(bool persist, bool normalizeInvalid) {
    if (!gGuideDisplaySettingsReady) return;
    int fontSize = 0;
    int lineSpacing = 0;
    const bool fontValid = ReadGuideDisplayValue(gGuideFontSize,
        kGuideFontSizeMinimum, kGuideFontSizeMaximum, fontSize);
    const bool spacingValid = !gGuideUsesRichEdit || ReadGuideDisplayValue(gGuideLineSpacing,
        kGuideLineSpacingMinimum, kGuideLineSpacingMaximum, lineSpacing);
    if (!gGuideUsesRichEdit) lineSpacing = gGuideDisplaySettings.lineSpacing;
    if (!fontValid || !spacingValid) {
        if (normalizeInvalid) WriteGuideDisplayControls();
        return;
    }
    const bool changed = fontSize != gGuideDisplaySettings.fontSize ||
        lineSpacing != gGuideDisplaySettings.lineSpacing;
    const GuideDisplaySettings previous = gGuideDisplaySettings;
    gGuideDisplaySettings.fontSize = fontSize;
    gGuideDisplaySettings.lineSpacing = lineSpacing;
    if (changed && persist && !SaveGuideDisplaySettings()) {
        gGuideDisplaySettings = previous;
        gGuideDisplaySettingsReady = false;
        WriteGuideDisplayControls();
        gGuideDisplaySettingsReady = true;
        return;
    }
    if (changed) ApplyGuideTextFormat(true);
}

void SetGuideText(const std::wstring& text) {
    if (!gGuideText) return;
    SendMessageW(gGuideText, EM_SETLIMITTEXT,
        static_cast<WPARAM>(std::max(kGuideTextCharacters, text.size())), 0);
    SetWindowTextW(gGuideText, text.c_str());
    ApplyGuideTextFormat();
}

std::wstring RenderGuideText(const GuideResult& result, GuideFilter filter,
    size_t maximum = kGuideTextCharacters) {
    if (result.authored) return result.title + L"\r\n" + result.authoredBody;
    std::wstring output = result.title + L"\r\n";
    if (!result.readStatus.empty()) output += result.readStatus + L"\r\n";
    const GuideFilter filters[] = {GuideFilter::Dialog, GuideFilter::Ambush, GuideFilter::Ai, GuideFilter::Rewards};
    const wchar_t* headings[] = {L"一、人物对话与单挑", L"二、伏兵", L"三、部队 AI 与控制变化", L"四、状态与奖惩"};
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
            if (output.size() + section.size() + rendered.size() + 2 > maximum) break;
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

std::wstring RenderGuideBody(const GuideResult& result) {
    if (result.authored) return result.authoredBody;
    const std::wstring rendered = RenderGuideText(result, GuideFilter::All, kGuideBookBytes);
    const std::wstring prefix = result.title + L"\r\n";
    if (rendered.compare(0, prefix.size(), prefix) == 0) return rendered.substr(prefix.size());
    return rendered;
}

void RenderGuide() {
    SetGuideText(RenderGuideText(gGuideResult, GuideFilter::All));
}

void SetGuideBusy(bool busy) {
    EnableWindow(gGuideScenarioBox, busy ? FALSE : TRUE);
    EnableWindow(gGuideExport, busy ? FALSE : TRUE);
    EnableWindow(gGuideFontSize, busy ? FALSE : TRUE);
    EnableWindow(gGuideLineSpacing, busy ? FALSE : TRUE);
    SetWindowTextW(gGuideExport, busy ? L"导出中……" : L"导出攻略");
}

void StartRead() {
    if (!gGuideWindow || gGuideFiles.empty() || !gGuideScenarioBox ||
        (gGuideTask && gGuideTask->exporting)) return;
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
    EnableWindow(gGuideExport, FALSE);
    gGuideResult = GuideResult();
    gGuideResult.title = FileTitle(file.battlefieldPath, file.battlefieldNumber) + L"简易攻略";
    SetGuideText(L"正在整理本关攻略……");
    try {
        std::thread([state, request, file, root]() {
            try {
                state->result = ReadGuideDisplay(file, root, state->cancelled);
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
        EnableWindow(gGuideExport, TRUE);
        RenderGuide();
    }
}

void StartExport() {
    if (!gGuideWindow || gGuideRoot.empty() || gGuideTask) return;
    const HWND window = gGuideWindow;
    const std::wstring root = gGuideRoot;
    const unsigned beforeConfirmation = gGuideRequest;
    const std::wstring target = root + L"\\简易攻略.txt";
    WIN32_FILE_ATTRIBUTE_DATA existing = {};
    const bool exists = GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &existing) != FALSE;
    const DWORD attributesError = exists ? ERROR_SUCCESS : GetLastError();
    if (!exists && attributesError != ERROR_FILE_NOT_FOUND && attributesError != ERROR_PATH_NOT_FOUND) {
        const std::wstring error = GuideSystemError(L"无法检查现有简易攻略.txt。", attributesError);
        MessageBoxW(gGuideWindow, error.c_str(), L"导出攻略失败", MB_OK | MB_ICONERROR);
        return;
    }
    if (exists && (existing.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        MessageBoxW(gGuideWindow, L"简易攻略.txt 是一个文件夹，无法导出。", L"导出攻略失败", MB_OK | MB_ICONERROR);
        return;
    }
    if (exists && (existing.nFileSizeHigh || existing.nFileSizeLow)) {
        const int response = MessageBoxW(gGuideWindow,
            L"当前目录已有简易攻略.txt，导出将覆盖作者内容。\n是否继续？",
            L"导出攻略", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
        if (response != IDOK) return;
    }
    // MessageBox runs a nested loop. A confirmation belongs only to the
    // window and request for which it was shown.
    if (gGuideWindow != window || !IsWindow(window) || gGuideRoot != root ||
        gGuideRequest != beforeConfirmation || gGuideTask) return;
    auto state = std::make_shared<GuideTaskState>();
    state->window = gGuideWindow;
    state->request = ++gGuideRequest;
    state->exporting = true;
    const unsigned request = state->request;
    gGuideTask = state;
    SetGuideBusy(true);
    try {
        std::thread([state, request, root]() {
            try {
                if (!BuildGuideExportForRoot(root, state->exportText, state->exportSkipped,
                    state->exportSuccess, state->exportError, &state->cancelled, &state->result.resources) && state->exportError.empty())
                    state->exportError = L"导出已取消。";
                if (state->exportError.empty() && !state->cancelled.load()) {
                    std::wstring writeError;
                    if (!IsBattleResourceSnapshotCurrent(state->result.resources))
                        state->exportError = L"运行时名称已变化，请重新导出。";
                    else if (!WriteGuideBookFile(root, state->exportText, writeError, &state->cancelled,
                        &state->result.resources))
                        state->exportError = writeError;
                }
            } catch (...) {
                state->exportError = L"导出攻略时发生未知错误。";
            }
            if (state->cancelled.load()) return;
            state->completed.store(true, std::memory_order_release);
            if (IsWindow(state->window)) PostMessageW(state->window, kGuideReadComplete, request, 0);
        }).detach();
    } catch (...) {
        state->cancelled.store(true);
        gGuideTask.reset();
        SetGuideBusy(false);
        RenderGuide();
        MessageBoxW(gGuideWindow, L"无法启动导出任务。", L"导出攻略失败", MB_OK | MB_ICONERROR);
    }
}

int GuideScale(HWND window, int value) {
    int dpi = 96;
    HDC dc = GetDC(window);
    if (dc) {
        const int detected = GetDeviceCaps(dc, LOGPIXELSY);
        if (detected > 0) dpi = detected;
        ReleaseDC(window, dc);
    }
    return MulDiv(value, dpi, 96);
}

void LayoutGuide(HWND window) {
    RECT client = {};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int margin = GuideScale(window, 10);
    const int exportWidth = GuideScale(window, 78);
    const int valueWidth = GuideScale(window, 60);
    const int labelWidth = GuideScale(window, 76);
    const int gap = GuideScale(window, 6);
    const int controlHeight = GuideScale(window, 24);
    const int labelTop = margin + GuideScale(window, 3);
    const int exportLeft = width - margin - exportWidth;
    const int scenarioLeft = margin + GuideScale(window, 52);
    // Keep the scenario selector compact while leaving enough room for the
    // longest localized title at 96 DPI.  It no longer expands with the
    // window; the remaining toolbar space is reserved for the two native
    // display parameters and export action.
    const int scenarioWidth = GuideScale(window, 240);
    const int fontLabelLeft = scenarioLeft + scenarioWidth + gap;
    const int fontValueLeft = fontLabelLeft + labelWidth;
    const int spacingLabelLeft = fontValueLeft + valueWidth + gap;
    const int spacingValueLeft = spacingLabelLeft + labelWidth;
    MoveWindow(gGuideLevelLabel, margin, labelTop, GuideScale(window, 52), GuideScale(window, 20), TRUE);
    MoveWindow(gGuideScenarioBox, scenarioLeft, margin, scenarioWidth, controlHeight, TRUE);
    MoveWindow(gGuideExport, exportLeft, margin, exportWidth, controlHeight, TRUE);
    MoveWindow(gGuideFontLabel, fontLabelLeft, labelTop, labelWidth, GuideScale(window, 20), TRUE);
    MoveWindow(gGuideFontSize, fontValueLeft, margin, valueWidth, controlHeight, TRUE);
    MoveWindow(gGuideSpacingLabel, spacingLabelLeft, labelTop, labelWidth, GuideScale(window, 20), TRUE);
    MoveWindow(gGuideLineSpacing, spacingValueLeft, margin, valueWidth, controlHeight, TRUE);
    const int bodyTop = GuideScale(window, 44);
    MoveWindow(gGuideText, margin, bodyTop, std::max(100, width - 2 * margin),
        std::max(80, height - bodyTop - margin), TRUE);
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
            if (gGuideTask->exporting) {
                const std::wstring error = gGuideTask->exportError;
                const size_t success = gGuideTask->exportSuccess;
                const std::vector<int> skipped = gGuideTask->exportSkipped;
                gGuideTask.reset();
                SetGuideBusy(false);
                if (!error.empty()) {
                    RenderGuide();
                    MessageBoxW(window, error.c_str(), L"导出攻略失败", MB_OK | MB_ICONERROR);
                } else {
                    std::wstring messageText = L"已导出到：\n" + gGuideRoot +
                        L"\\简易攻略.txt\n成功生成 " + std::to_wstring(success) +
                        L" 关，跳过 " + std::to_wstring(skipped.size()) + L" 关。";
                    if (!skipped.empty()) {
                        messageText += L"，跳过关卡：";
                        for (size_t index = 0; index < skipped.size(); ++index) {
                            if (index) messageText += L"、";
                            messageText += std::to_wstring(skipped[index]);
                        }
                    }
                    StartRead();
                    MessageBoxW(window, messageText.c_str(), L"导出攻略", MB_OK | MB_ICONINFORMATION);
                }
                return 0;
            }
            if (gGuideTask->result.hasResources &&
                !IsBattleResourceSnapshotCurrent(gGuideTask->result.resources)) {
                gGuideTask.reset();
                StartRead();
                return 0;
            }
            gGuideResult = std::move(gGuideTask->result);
            gGuideTask.reset();
            EnableWindow(gGuideExport, TRUE);
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
        gGuideFontLabel = CreateWindowExW(0, WC_STATICW, L"字体大小：",
            WS_CHILD | WS_VISIBLE, 0, 0, 42, 20, window, nullptr, gInstance, nullptr);
        gGuideFontSize = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL,
            0, 0, 42, 24, window, reinterpret_cast<HMENU>(kGuideFontSize), gInstance, nullptr);
        gGuideSpacingLabel = CreateWindowExW(0, WC_STATICW, L"行间距：",
            WS_CHILD | WS_VISIBLE, 0, 0, 42, 20, window, nullptr, gInstance, nullptr);
        gGuideLineSpacing = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL,
            0, 0, 42, 24, window, reinterpret_cast<HMENU>(kGuideLineSpacing), gInstance, nullptr);
        gGuideExport = CreateWindowExW(0, WC_BUTTONW, L"导出攻略",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 64, 24, window,
            reinterpret_cast<HMENU>(kGuideExport), gInstance, nullptr);
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
        SendMessageW(gGuideFontLabel, WM_SETFONT, reinterpret_cast<WPARAM>(gGuideFont), TRUE);
        SendMessageW(gGuideFontSize, WM_SETFONT, reinterpret_cast<WPARAM>(gGuideFont), TRUE);
        SendMessageW(gGuideSpacingLabel, WM_SETFONT, reinterpret_cast<WPARAM>(gGuideFont), TRUE);
        SendMessageW(gGuideLineSpacing, WM_SETFONT, reinterpret_cast<WPARAM>(gGuideFont), TRUE);
        SendMessageW(gGuideFontSize, EM_SETLIMITTEXT, 6, 0);
        SendMessageW(gGuideLineSpacing, EM_SETLIMITTEXT, 6, 0);
        SendMessageW(gGuideExport, WM_SETFONT, reinterpret_cast<WPARAM>(gGuideFont), TRUE);
        LoadGuideDisplaySettings();
        WriteGuideDisplayControls();
        if (!gGuideUsesRichEdit) {
            EnableWindow(gGuideLineSpacing, FALSE);
            HWND tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
                CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr, gInstance, nullptr);
            if (tooltip) {
                TOOLINFOW tool = {sizeof(tool)};
                tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
                tool.hwnd = window;
                tool.uId = reinterpret_cast<UINT_PTR>(gGuideSpacingLabel);
                tool.lpszText = const_cast<wchar_t*>(
                    L"当前系统使用兼容文本控件，不支持行间距调整；字体大小仍可调整。");
                SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
            }
        }
        gGuideDisplaySettingsReady = true;
        ScanFiles();
        for (const GuideFile& file : gGuideFiles)
            SendMessageW(gGuideScenarioBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(file.title.c_str()));
        if (!gGuideFiles.empty()) {
            SendMessageW(gGuideScenarioBox, CB_SETCURSEL, DefaultScenarioIndex(), 0);
            StartRead();
        } else SetGuideText(L"本关暂无可整理的攻略信息。");
        SetGuideBusy(false);
        LayoutGuide(window);
        return 0;
    }
    case WM_SIZE:
        LayoutGuide(window);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = GuideScale(window, 700);
        info->ptMinTrackSize.y = GuideScale(window, 240);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == kGuideExport && HIWORD(wParam) == BN_CLICKED) {
            StartExport();
            return 0;
        }
        if (LOWORD(wParam) == kGuideScenario && HIWORD(wParam) == CBN_SELCHANGE) {
            if (gGuideTask && gGuideTask->exporting) return 0;
            StartRead();
            return 0;
        }
        if ((LOWORD(wParam) == kGuideFontSize || LOWORD(wParam) == kGuideLineSpacing) &&
            (HIWORD(wParam) == EN_CHANGE || HIWORD(wParam) == EN_KILLFOCUS)) {
            ApplyGuideDisplaySettingsFromControls(true, HIWORD(wParam) == EN_KILLFOCUS);
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
        gGuideExport = nullptr;
        gGuideFontLabel = nullptr;
        gGuideFontSize = nullptr;
        gGuideSpacingLabel = nullptr;
        gGuideLineSpacing = nullptr;
        gGuideText = nullptr;
        gGuideDisplaySettingsReady = false;
        if (gGuideFontOwned && gGuideFont) DeleteObject(gGuideFont);
        gGuideFont = nullptr;
        gGuideFontOwned = false;
        if (gGuideBodyFont) DeleteObject(gGuideBodyFont);
        gGuideBodyFont = nullptr;
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

void ConfigureGuideConditionalExtension(bool enabled) noexcept {
    gGuideConditionalExtension.store(enabled, std::memory_order_release);
}

bool OpenGuide(HWND owner, const wchar_t* gameRoot) {
    if (gGuideWindow && IsWindow(gGuideWindow)) {
        ShowWindow(gGuideWindow, SW_RESTORE);
        SetForegroundWindow(gGuideWindow);
        return true;
    }
    if (!gameRoot || !*gameRoot || !RegisterGuideClass()) return false;
    gGuideRoot = gameRoot;
    gGuideResult = GuideResult();
    RECT ownerRect = {};
    if (!GetWindowRect(owner, &ownerRect)) SetRect(&ownerRect, 100, 100, 900, 700);
    const int width = GuideScale(owner, 760);
    const int height = GuideScale(owner, 480);
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
    GuideResult result = AnalyzeScenario(file, gameRoot, cancelled, false);
    if (result.title.empty()) return false;
    text = RenderGuideText(result, GuideFilter::All);
    return true;
}

std::wstring NormalizeGuideTestText(const std::wstring& input) {
    std::wstring normalized;
    normalized.reserve(input.size() + 16);
    for (size_t index = 0; index < input.size(); ++index) {
        if (input[index] == L'\r') {
            normalized += L"\r\n";
            if (index + 1 < input.size() && input[index + 1] == L'\n') ++index;
        } else if (input[index] == L'\n') {
            normalized += L"\r\n";
        } else {
            normalized.push_back(input[index]);
        }
    }
    return normalized;
}

bool ParseGuideAuthorTextForTest(const wchar_t* source,
    std::vector<GuideAuthorSectionTest>& sections) {
    sections.clear();
    if (!source) return false;
    GuideBook book;
    if (!ParseGuideDocument(NormalizeGuideTestText(source), book)) return false;
    for (const auto& item : book.sections) sections.push_back({item.first, item.second});
    std::sort(sections.begin(), sections.end(), [](const GuideAuthorSectionTest& left,
        const GuideAuthorSectionTest& right) { return left.number < right.number; });
    return true;
}

bool SelectGuideAuthorTextForTest(const wchar_t* source, int battlefieldNumber,
    std::wstring& text) {
    text.clear();
    if (!source || battlefieldNumber < 0 || battlefieldNumber > 999) return false;
    GuideBook book;
    if (!ParseGuideDocument(NormalizeGuideTestText(source), book)) return false;
    return GetAuthoredGuideBody(book, battlefieldNumber, text);
}

bool BuildGuideExportTextForTest(const std::vector<GuideExportSectionTest>& sections,
    std::wstring& text) {
    text.clear();
    std::unordered_map<int, GuideExportPiece> unique;
    for (const GuideExportSectionTest& section : sections) {
        if (section.number < 0 || section.number > 999) return false;
        unique[section.number] = {section.number, NormalizeGuideTestText(section.body)};
    }
    std::vector<GuideExportPiece> pieces;
    pieces.reserve(unique.size());
    for (auto& item : unique) pieces.push_back(std::move(item.second));
    std::sort(pieces.begin(), pieces.end(), [](const GuideExportPiece& left,
        const GuideExportPiece& right) { return left.number < right.number; });
    text = BuildGuideExportText(pieces);
    return !text.empty();
}

bool BuildGuideDisplayTextForTest(const wchar_t* scenarioPath, const wchar_t* gameRoot,
    std::wstring& text, bool& authored) {
    text.clear();
    authored = false;
    if (!scenarioPath || !gameRoot || !*scenarioPath || !*gameRoot) return false;
    GuideFile file;
    if (!BuildGuideFile(scenarioPath, gameRoot, file)) return false;
    const std::atomic<bool> cancelled(false);
    const GuideResult result = ReadGuideDisplay(file, gameRoot, cancelled);
    authored = result.authored;
    text = RenderGuideText(result, GuideFilter::All);
    return !text.empty();
}

bool ExportGuideBookForTest(const wchar_t* root, size_t& success,
    std::vector<int>& skipped, std::wstring& error) {
    success = 0;
    skipped.clear();
    error.clear();
    if (!root || !*root) return false;
    std::wstring text;
    BattleResourceSnapshot resources;
    return BuildGuideExportForRoot(root, text, skipped, success, error, nullptr, &resources) &&
        WriteGuideBookFile(root, text, error, nullptr, &resources);
}

bool DecodeGuideDocumentForTest(const std::vector<unsigned char>& bytes, std::wstring& text) {
    return DecodeGuideDocument(bytes, text);
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
    }
    for (const GuideAiChange& change : CollectGuideAiChanges(context)) {
        const GuideCommand& command = *change.command;
        ++stats.aiCommands;
        for (const GuideCondition& condition : command.conditions)
            if (condition.id < _countof(stats.aiConditions)) ++stats.aiConditions[condition.id];
        if (!command.conditions.empty()) ++stats.conditionedAiCommands;
        if (change.targetResolved) ++stats.matchedAiCommands;
        ++stats.changingAiCommands;
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


bool BuildGuideScriptTextForTest(const std::vector<unsigned char>& bytes,
    const std::vector<std::wstring>& people, std::wstring& text, std::wstring& diagnostics) {
    text.clear();
    diagnostics.clear();
    GuideContext context;
    context.sourcePath = L"synthetic.eex";
    context.diagnostics = &diagnostics;
    context.conditionalExtension = gGuideConditionalExtension.load(std::memory_order_acquire);
    if (!ParseScenarioScript(bytes, context.commands, nullptr, &diagnostics)) return false;
    for (size_t i = 0; i < people.size(); ++i) if (!people[i].empty())
        context.people[static_cast<int>(i)] = people[i];
    RecordUnknownGuideConditions(context);
    GuideResult result;
    result.title = L"简易攻略";
    CollectDeployments(context, context.units);
    AddDialogEntries(context, result);
    AddAmbushEntries(context, result);
    AddAiEntries(context, result);
    AddRewardEntries(context, result);
    MergeAndPruneNarrativeEntries(result);
    text = RenderGuideText(result, GuideFilter::All);
    return true;
}

#ifdef XA66_TEST
bool CaptureGuideResourcesForTest(const wchar_t* root, BattleResourceSnapshot& snapshot) {
    if (!root) return false;
    snapshot = CaptureGuideResources(root);
    return snapshot.peopleReady || snapshot.itemsReady;
}
#endif
