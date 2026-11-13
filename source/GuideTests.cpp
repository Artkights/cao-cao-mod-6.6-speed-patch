#include "Guide.h"
#include "ScenarioScript.h"
#include "BattleRuntime.h"
#include "GuideAiFixtures.h"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>
#include <stdio.h>
#include <string>

Settings gSettings;
HMODULE gInstance = nullptr;
uintptr_t gGameBase = 0;
HWND gMainWindow = nullptr;
HWND gDialog = nullptr;

bool EndsWithInsensitive(const std::wstring& value, const wchar_t* suffix) {
    const size_t length = wcslen(suffix);
    return value.size() >= length && !_wcsicmp(value.c_str() + value.size() - length, suffix);
}

bool ValidateStateFormatting() {
    std::wstring text;
    if (!FormatGuideStateSummaryForTest(0x03, 2, -1, 0, 0, text) || text != L"攻防上升") return false;
    if (!FormatGuideStateSummaryForTest(0x1F, 2, 0x80 | 2 | 4 | 8 | 16, 0, 0, text) ||
        text != L"全能力上升并解除异常") return false;
    if (!FormatGuideStateSummaryForTest(0x01, 2, 0x80 | 2 | 16, 0, 0, text) ||
        text != L"攻击上升并解除麻痹和中毒") return false;
    if (!FormatGuideStateSummaryForTest(0x02, 0, 2 | 8, 0, 0, text) ||
        text != L"防御下降并陷入麻痹和混乱") return false;
    if (!FormatGuideScenarioJumpForTest(0, text) || text != L"S_00.eex") return false;
    if (!FormatGuideScenarioJumpForTest(1, text) || text != L"R_01.eex") return false;
    if (!FormatGuideScenarioJumpForTest(2, text) || text != L"S_01.eex") return false;
    if (!FormatGuideScenarioJumpForTest(19, text) || text != L"R_10.eex") return false;
    return true;
}

void ScriptWord(std::vector<unsigned char>& bytes, unsigned value) {
    bytes.push_back(static_cast<unsigned char>(value));
    bytes.push_back(static_cast<unsigned char>(value >> 8));
}

void ScriptDword(std::vector<unsigned char>& bytes, int32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<unsigned char>(static_cast<uint32_t>(value) >> shift));
}

void ScriptAbility(std::vector<unsigned char>& bytes, int person, int attribute,
    int operation, int32_t amount) {
    ScriptWord(bytes, 0x38);
    for (int value : {person, attribute, operation}) { ScriptWord(bytes, 0); ScriptWord(bytes, value); }
    ScriptWord(bytes, 0); ScriptDword(bytes, amount);
}

std::vector<unsigned char> ScriptEex(const std::vector<unsigned char>& actions,
    const std::vector<unsigned char>& conditions = {}) {
    std::vector<unsigned char> section = conditions;
    ScriptWord(section, 0);
    ScriptWord(section, static_cast<unsigned>(actions.size()));
    section.insert(section.end(), actions.begin(), actions.end());
    std::vector<unsigned char> bytes = {'E', 'E', 'X', 0, 0, 0, 0, 0, 0, 0};
    ScriptDword(bytes, 14);
    ScriptWord(bytes, 1);
    ScriptWord(bytes, static_cast<unsigned>(section.size()));
    bytes.insert(bytes.end(), section.begin(), section.end());
    return bytes;
}

bool ValidateAbilityCommands() {
    std::vector<unsigned char> actions;
    ScriptAbility(actions, 0, 0, 1, 50);
    ScriptAbility(actions, 1, 2, 2, 800);
    ScriptAbility(actions, 2, 7, 1, 300);
    ScriptAbility(actions, 3, 8, 2, 20);
    ScriptAbility(actions, 0, 0, 0, 0);
    ScriptAbility(actions, 0, 15, 1, 50);
    ScriptAbility(actions, 0, 0, 3, 50);
    ScriptAbility(actions, 123, 17, 4, 50);
    std::wstring text, diagnostics;
    if (!BuildGuideScriptTextForTest(ScriptEex(actions), {L"曹操", L"吕布", L"张飞", L"诸葛亮"},
        text, diagnostics)) return false;
    for (const wchar_t* expected : {L"曹操攻击力提升 50 点", L"吕布精神力下降 800 点",
        L"张飞HP上限提升 300 点", L"诸葛亮MP上限下降 20 点", L"曹操攻击力设为 0 点",
        L"武将 123能力码 17 操作码 4 变化 50", L"曹操能力码 15 操作码 1 变化 50",
        L"曹操攻击力操作码 3 变化 50"})
        if (text.find(expected) == std::wstring::npos) return false;
    if (text.find(L"当前 HP") != std::wstring::npos || text.find(L"当前 MP") != std::wstring::npos ||
        diagnostics.find(L"person-name-missing: id=123") == std::wstring::npos ||
        diagnostics.find(L"section=0 offset=") == std::wstring::npos) return false;
    std::vector<unsigned char> conditions;
    ScriptWord(conditions, 0x36);
    ScriptWord(conditions, 0); ScriptWord(conditions, 2);
    ScriptWord(conditions, 0); ScriptWord(conditions, 7);
    ScriptWord(conditions, 0); ScriptDword(conditions, 500);
    ScriptWord(conditions, 0); ScriptWord(conditions, 0);
    ScriptWord(conditions, 3); // structurally valid but unknown condition
    if (!BuildGuideScriptTextForTest(ScriptEex(actions, conditions), {L"曹操", L"吕布", L"张飞", L"诸葛亮"},
        text, diagnostics) || text.find(L"张飞HP上限") == std::wstring::npos ||
        text.find(L"满足其他事件条件后") == std::wstring::npos ||
        diagnostics.find(L"condition-unresolved") == std::wstring::npos) return false;
    // A malformed section is skipped without discarding a later valid section.
    auto malformed = ScriptEex(actions);
    std::vector<unsigned char> truncated;
    ScriptAbility(truncated, 0, 0, 1, 50);
    truncated.resize(truncated.size() - 2);
    malformed = ScriptEex(truncated);
    const auto valid = ScriptEex(actions);
    malformed[14] = 2;
    malformed.insert(malformed.end(), valid.begin() + 16, valid.end());
    return BuildGuideScriptTextForTest(malformed, {L"曹操"}, text, diagnostics) &&
        text.find(L"曹操攻击力提升 50 点") != std::wstring::npos &&
        diagnostics.find(L"malformed-section") != std::wstring::npos;
}

void ScriptConditional(std::vector<unsigned char>& bytes, int branch,
    const char* expression) {
    ScriptWord(bytes, 0x7B);
    ScriptWord(bytes, 0);
    ScriptDword(bytes, branch);
    ScriptWord(bytes, 0);
    for (const char* cursor = expression; *cursor; ++cursor)
        bytes.push_back(static_cast<unsigned char>(*cursor));
    bytes.push_back(0);
}

bool ValidateConditionalExtension() {
    std::vector<unsigned char> condition;
    ScriptConditional(condition, 2, "0,1");
    std::vector<unsigned char> action;
    ScriptAbility(action, 0, 0, 1, 25);
    std::wstring text, diagnostics;
    ConfigureGuideConditionalExtension(true);
    const bool parsed = BuildGuideScriptTextForTest(ScriptEex(action, condition),
        {L"曹操"}, text, diagnostics);
    ConfigureGuideConditionalExtension(false);
    if (!parsed || text.find(L"曹操已加入我军") == std::wstring::npos ||
        text.find(L"曹操攻击力提升 25 点") == std::wstring::npos) return false;
    if (!BuildGuideScriptTextForTest(ScriptEex(action, condition), {L"曹操"}, text, diagnostics) ||
        text.find(L"尚未解析的扩展条件（0x7B）") == std::wstring::npos ||
        text.find(L"曹操攻击力提升 25 点") == std::wstring::npos ||
        diagnostics.find(L"condition-unresolved: id=123") == std::wstring::npos) return false;
    ConfigureGuideConditionalExtension(true);
    const int branches[] = {1, 3, 4, 5};
    const char* expressions[] = {"0,1,0", "0,0,2", "1", "0,0,3,1"};
    const wchar_t* expected[] = {L"总数不少于 1", L"地形编号为 2", L"天气编号为 1", L"曹操处于混乱状态"};
    bool okay = true;
    for (size_t i = 0; i < _countof(branches); ++i) {
        condition.clear();
        ScriptConditional(condition, branches[i], expressions[i]);
        okay = BuildGuideScriptTextForTest(ScriptEex(action, condition), {L"曹操"}, text, diagnostics) &&
            text.find(expected[i]) != std::wstring::npos && okay;
    }
    condition.clear();
    ScriptConditional(condition, 5, "0,0,99,1");
    okay = BuildGuideScriptTextForTest(ScriptEex(action, condition), {L"曹操"}, text, diagnostics) &&
        text.find(L"尚未解析的扩展条件（0x7B）") != std::wstring::npos &&
        text.find(L"曹操攻击力提升 25 点") != std::wstring::npos && okay;
    ConfigureGuideConditionalExtension(false);
    return okay;
}

bool ValidateGuideResourceReadiness() {
    wchar_t temporary[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, temporary)) return false;
    const auto root = std::filesystem::path(temporary) / (L"GuideNames-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    std::vector<BYTE> data(396 + 1024 * 32 + 104 * 25, 0);
    const BYTE diskPerson[] = {0xB2,0xDC,0xB2,0xD9};
    const BYTE diskItem[] = {0xCC,0xFA,0xBD,0xA3};
    for (size_t i = 0; i < 1024; ++i) memcpy(data.data() + 396 + i * 32, diskPerson, sizeof(diskPerson));
    for (size_t i = 0; i < 104; ++i) memcpy(data.data() + 33164 + i * 25, diskItem, sizeof(diskItem));
    FILE* file = nullptr;
    const auto dataPath = root / L"Data.e5";
    if (_wfopen_s(&file, dataPath.c_str(), L"wb")) return false;
    const bool written = fwrite(data.data(), 1, data.size(), file) == data.size();
    fclose(file);
    if (!written) return false;
    std::vector<BYTE> people(1024 * 0x48), items(255 * 25);
    for (unsigned i = 0; i < 1024; ++i) {
        const uint16_t tag = static_cast<uint16_t>(0x1000 + i);
        memcpy(people.data() + i * 0x48, &tag, sizeof(tag));
    }
    const BYTE runtimePerson[] = {0xCF,0xC4,0xBA,0xEE,0x90,0xAA}; // 夏侯惇 (CP936)
    const BYTE runtimeItem[] = {0xB6,0xCC,0xBD,0xA3};
    memcpy(people.data() + 8, runtimePerson, sizeof(runtimePerson));
    memcpy(items.data(), runtimeItem, sizeof(runtimeItem));
    uintptr_t personPointer = 0;
    ConfigureBattleResourceLayoutForTest(reinterpret_cast<uintptr_t>(&personPointer), reinterpret_cast<uintptr_t>(items.data()));
    const ULONGLONG started = GetTickCount64();
    std::thread publish([&]() {
        Sleep(150);
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&personPointer), people.data());
    });
    BattleResourceSnapshot snapshot;
    const bool captured = CaptureGuideResourcesForTest(root.c_str(), snapshot);
    publish.join();
    const ULONGLONG elapsed = GetTickCount64() - started;
    const bool okay = captured && elapsed >= 450 && elapsed < 5000 &&
        snapshot.peopleReady && snapshot.itemsReady && snapshot.people[0] != L"曹操" &&
        snapshot.people[1] == L"曹操" && snapshot.items[0] == L"短剑" && snapshot.items[1] == L"铁剑" &&
        snapshot.peopleOrigin == BattleResourceOrigin::RuntimeWithDiskFallback &&
        snapshot.itemOrigin == BattleResourceOrigin::RuntimeWithDiskFallback && IsBattleResourceSnapshotCurrent(snapshot);
    ResetBattleRuntime();
    std::filesystem::remove_all(root);
    return okay;
}

bool ValidateRWithoutBattlefield() {
    wchar_t temporary[MAX_PATH] = {}, unique[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, temporary) || !GetTempFileNameW(temporary, L"g32", 0, unique)) return false;
    DeleteFileW(unique);
    const std::wstring root = unique;
    const std::wstring rs = root + L"\\RS";
    if (!CreateDirectoryW(root.c_str(), nullptr) || !CreateDirectoryW(rs.c_str(), nullptr)) return false;
    std::vector<unsigned char> actions;
    ScriptAbility(actions, 123, 7, 1, 300);
    for (int person : {7, 7, 9, 11, 12}) {
        ScriptWord(actions, 0x3B);
        ScriptWord(actions, person == 12 ? 0x7F : 0x02); ScriptWord(actions, person);
        ScriptWord(actions, 0x0E); ScriptWord(actions, person == 11 ? 1 : 0);
        ScriptWord(actions, 0x3E); ScriptWord(actions, 0);
    }
    const auto bytes = ScriptEex(actions);
    const std::wstring path = rs + L"\\R_00.eex";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    const bool wrote = file != INVALID_HANDLE_VALUE &&
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    std::wstring text;
    std::vector<int> joins;
    std::wstring diagnostics;
    const bool okay = wrote && BuildGuideTextForTest(path.c_str(), root.c_str(), text) &&
        text.find(L"武将 123HP上限提升 300 点") != std::wstring::npos &&
        ExtractScenarioJoinCandidates(path, joins, diagnostics) && joins == std::vector<int>({7, 9});
    DeleteFileW(path.c_str());
    for (const wchar_t* generation : {L"64", L"65", L"66"}) {
        const std::wstring privateRoot = root + L"\\patch\\xiaoan" + generation;
        DeleteFileW((privateRoot + L"\\diagnostics\\guide.log").c_str());
        RemoveDirectoryW((privateRoot + L"\\diagnostics").c_str());
        RemoveDirectoryW(privateRoot.c_str());
    }
    RemoveDirectoryW((root + L"\\patch").c_str());
    RemoveDirectoryW(rs.c_str());
    RemoveDirectoryW(root.c_str());
    return okay;
}

bool ValidateAuthorGuideFormatting() {
    // The parser accepts both the compact heading requested for authored files
    // and the spaced heading used by the generated guide, then normalizes all
    // line endings for the returned body.
    const wchar_t authored[] =
        L"第01关简易攻略\n"
        L"第一关作者攻略。\n"
        L"第 00 关简易攻略\r\n"
        L"零关作者攻略。";
    std::vector<GuideAuthorSectionTest> sections;
    if (!ParseGuideAuthorTextForTest(authored, sections) || sections.size() != 2) return false;
    if (sections[0].number != 0 || sections[0].text != L"零关作者攻略。") return false;
    if (sections[1].number != 1 || sections[1].text != L"第一关作者攻略。") return false;

    std::wstring selected;
    if (!SelectGuideAuthorTextForTest(authored, 1, selected) ||
        selected != L"第一关作者攻略。") return false;

    // A matching authored section wins over generated content.  A missing
    // section reports a miss so the caller can retain its generated fallback.
    const std::wstring generated = L"自动生成攻略。";
    std::wstring displayed = generated;
    if (SelectGuideAuthorTextForTest(authored, 0, selected)) displayed = selected;
    if (displayed != L"零关作者攻略。") return false;
    displayed = generated;
    selected.clear();
    if (SelectGuideAuthorTextForTest(authored, 2, selected)) displayed = selected;
    if (displayed != generated || !selected.empty()) return false;

    // Empty or whitespace-only sections behave like an absent authored guide.
    // The helpers clear prior output on misses to prevent stale author text
    // from remaining visible after a reload or a change of battlefield.
    selected = L"stale";
    if (SelectGuideAuthorTextForTest(L"", 0, selected) || !selected.empty()) return false;
    selected = L"stale";
    if (SelectGuideAuthorTextForTest(L"第00关简易攻略\r\n \t\r\n", 0, selected) ||
        !selected.empty()) return false;
    sections.push_back({99, L"stale"});
    if (!ParseGuideAuthorTextForTest(L"", sections) || !sections.empty()) return false;

    const wchar_t duplicate[] =
        L"第03关简易攻略\r\n旧攻略\r\n"
        L"第 03 关简易攻略\r\n最后修订的攻略";
    if (!SelectGuideAuthorTextForTest(duplicate, 3, selected) ||
        selected != L"最后修订的攻略") return false;
    const wchar_t duplicateEmpty[] =
        L"第03关简易攻略\r\n旧攻略\r\n"
        L"第03关简易攻略\r\n\t ";
    selected = L"stale";
    if (SelectGuideAuthorTextForTest(duplicateEmpty, 3, selected) || !selected.empty()) return false;

    // Only a complete heading line starts a section.  Incidental references to
    // the title in an author's prose remain part of that author's text.
    const wchar_t prose[] =
        L"文件说明：第04关简易攻略不应成为分隔标记。\n"
        L"第04关简易攻略\n"
        L"请阅读第05关简易攻略作为参考。\n"
        L"第05关简易攻略附注\n"
        L"第05关简易攻略\n"
        L"第五关正文";
    if (!ParseGuideAuthorTextForTest(prose, sections) || sections.size() != 2 ||
        sections[0].number != 4 || sections[1].number != 5) return false;
    if (sections[0].text != L"请阅读第05关简易攻略作为参考。\r\n第05关简易攻略附注")
        return false;

    const wchar_t boundaries[] =
        L"第-1关简易攻略\n无效负编号\n"
        L"第1000关简易攻略\n无效超界编号\n"
        L"第000关简易攻略\n零号正文\n"
        L"第999关简易攻略\n最大编号正文";
    if (!ParseGuideAuthorTextForTest(boundaries, sections) || sections.size() != 2 ||
        sections[0].number != 0 || sections[0].text != L"零号正文" ||
        sections[1].number != 999 || sections[1].text != L"最大编号正文") return false;
    selected = L"stale";
    if (SelectGuideAuthorTextForTest(boundaries, -1, selected) || !selected.empty()) return false;
    selected = L"stale";
    if (SelectGuideAuthorTextForTest(boundaries, 1000, selected) || !selected.empty()) return false;

    const wchar_t layout[] =
        L"第10关简易攻略\n"
        L"\n"
        L"一、准备\r"
        L"\t先补给。\r\n"
        L"\r\n"
        L"二、进军\n"
        L"第11关简易攻略\n下一关正文";
    if (!SelectGuideAuthorTextForTest(layout, 10, selected) ||
        selected != L"\r\n一、准备\r\n\t先补给。\r\n\r\n二、进军") return false;

    // Export is sorted by battlefield number, de-duplicates repeated numbers
    // using the last supplied body, and removes trailing CRLF from each body.
    const std::vector<GuideExportSectionTest> exportSections = {
        {2, L"旧的二关内容。"},
        {0, L"零关自动攻略。"},
        {2, L"最终的二关内容。\r\n\r\n"}
    };
    std::wstring exported;
    if (!BuildGuideExportTextForTest(exportSections, exported)) return false;
    const std::wstring expected =
        L"第 00 关简易攻略\r\n"
        L"零关自动攻略。\r\n\r\n"
        L"第 02 关简易攻略\r\n"
        L"最终的二关内容。";
    if (exported != expected) return false;
    if (!BuildGuideExportTextForTest({{999, L"最大编号正文"}}, exported) ||
        exported != L"第 999 关简易攻略\r\n最大编号正文") return false;
    exported = L"stale";
    if (BuildGuideExportTextForTest({}, exported) || !exported.empty()) return false;
    exported = L"stale";
    if (BuildGuideExportTextForTest({{-1, L"无效编号"}}, exported) || !exported.empty()) return false;
    exported = L"stale";
    if (BuildGuideExportTextForTest({{1000, L"无效编号"}}, exported) || !exported.empty()) return false;
    return true;
}

size_t CountScenarioFiles(const std::wstring& root, const wchar_t* pattern) {
    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW((root + L"\\RS\\" + pattern).c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return 0;
    size_t count = 0;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const wchar_t* name = data.cFileName;
        if ((name[0] != L'R' && name[0] != L'S') || name[1] != L'_') continue;
        wchar_t* end = nullptr;
        const long number = wcstol(name + 2, &end, 10);
        if (end != name + 2 && number >= 0 && number <= 999 && !_wcsicmp(end, L".eex")) ++count;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return count;
}

bool ValidateScenarioSelection(const std::wstring& root) {
    std::vector<std::wstring> names;
    const size_t expected = CountScenarioFiles(root, L"R_*.eex") + CountScenarioFiles(root, L"S_*.eex");
    if (!expected || !EnumerateGuideScenariosForTest(root.c_str(), names) || names.size() != expected) return false;
    if (std::find_if(names.begin(), names.end(), [](const std::wstring& name) {
        return !_wcsicmp(name.c_str(), L"R_10.eex");
    }) == names.end()) return false;
    if (std::find_if(names.begin(), names.end(), [](const std::wstring& name) {
        return !_wcsicmp(name.c_str(), L"S_10.eex");
    }) == names.end()) return false;
    ObserveGuideScenarioPathA("RS\\R_10.eex");
    std::wstring selected;
    std::wstring target;
    if (!SelectObservedGuideScenarioForTest(root.c_str(), selected, target) ||
        _wcsicmp(selected.c_str(), L"R_10.eex") || !EndsWithInsensitive(target, L"\\RS\\S_10.eex"))
        return false;
    ObserveGuideScenarioPathA("RS\\S_10.eex");
    if (!SelectObservedGuideScenarioForTest(root.c_str(), selected, target) ||
        _wcsicmp(selected.c_str(), L"S_10.eex") || !EndsWithInsensitive(target, L"\\RS\\S_10.eex"))
        return false;
    return true;
}

bool ValidatePlayerText(const std::wstring& value) {
    static const wchar_t* forbidden[] = {
        L"来源", L"未确认", L"未完全解析", L"命令编号", L"偏移", L"[loose@",
        L".eex", L"满足战场条件", L"改变行动策略", L"原始人物", L"原始物品", L"建议攻略"
    };
    if (value.find(L"简易攻略") == std::wstring::npos) return false;
    if (value.find(L"\r\n\r\n") != std::wstring::npos) return false;
    for (const wchar_t* word : forbidden) if (value.find(word) != std::wstring::npos) return false;
    return true;
}

size_t CountOccurrences(const std::wstring& value, const std::wstring& needle) {
    if (needle.empty()) return 0;
    size_t count = 0;
    for (size_t at = 0; (at = value.find(needle, at)) != std::wstring::npos; at += needle.size()) ++count;
    return count;
}

bool ValidateShanZeiS00(const std::wstring& value, const GuideTestStats& stats) {
    static const wchar_t* required[] = {
        L"第 00 关简易攻略",
        L"刘备与裴元绍相邻且裴元绍HP上限低于 100时，刘备与裴元绍触发对话并且获得精壮果。",
        L"第 13 回合",
        L"6 支隐藏敌军",
        L"完成本关后，获得 20000 金钱",
        L"我方部队到达（9，17）",
        L"关羽撤退时，触发对话并且失去 500 金钱。",
        L"击退邓茂时，邓茂触发对话并且获得时空遗枝。",
        L"刘备HP上限低于 80时，刘备触发对话并且刘备全能力上升并解除异常。",
        L"关羽HP上限低于 80时，关羽触发对话并且关羽全能力上升并解除异常。",
        L"张飞HP上限低于 80时，张飞触发对话并且张飞全能力上升并解除异常。"
    };
    static const wchar_t* forbidden[] = {
        L"第 13 回合时，裴元绍、黄巾兵8等人触发对话。",
        L"第 14 回合时，黄巾兵22、刘备等人触发对话。",
        L"刘备撤退时，刘备触发对话。",
        L"击退邓茂时，邓茂触发对话。",
        L"击退程远志时，程远志触发对话。",
        L"击退裴元绍时，裴元绍触发对话。",
        L"完成本关时，刘备、张飞等人触发对话。",
        L"刘备撤退后，刘备死亡。"
    };
    if (!ValidatePlayerText(value) || value.find(L"本关暂无可整理的攻略信息") != std::wstring::npos)
        return false;
    for (const wchar_t* text : required) if (value.find(text) == std::wstring::npos) return false;
    for (const wchar_t* text : forbidden) if (value.find(text) != std::wstring::npos) return false;
    return stats.commands == 815 && stats.dialogEntries > 0 && stats.ambushEntries > 0 &&
        stats.aiEntries > 0 && stats.rewardEntries > 0;
}

bool ValidateShanZeiS85(const std::wstring& value, const GuideTestStats& stats) {
    static const wchar_t* required[] = {
        L"第 85 关简易攻略",
        L"程远志HP上限低于 200且关羽与程远志相邻时，程远志与关羽触发对话；关羽与程远志单挑，并且程远志死亡。",
        L"（5，3）至（15，10）区域内的敌军攻防下降",
        L"（1，2）至（2，3）区域内的敌军防御上升",
        L"（5，3）至（19，11）区域内的敌军防御上升",
        L"（6，8）至（8，10）区域内的敌军攻击上升",
        L"刘备HP上限低于 80时，刘备触发对话并且刘备全能力上升并解除异常",
        L"完成本关后，获得 30000 金钱"
    };
    if (!ValidatePlayerText(value) || value.find(L"本关暂无可整理的攻略信息") != std::wstring::npos)
        return false;
    for (const wchar_t* text : required) if (value.find(text) == std::wstring::npos) return false;
    if (value.find(L"击退姜维时，姜维触发对话") != std::wstring::npos ||
        value.find(L"击退姜维后，姜维死亡") != std::wstring::npos ||
        value.find(L"第 3 回合时，程远志触发对话。") != std::wstring::npos) return false;
    if (value.find(L"第 16 回合前，区域内的敌军") != std::wstring::npos) return false;
    // Each script branch retains its condition; dialogue and AI now both show
    // these outcomes, so a whole-document occurrence count is not meaningful.
    // This regression uses the National Day ShanZei fixture: both source 3F
    // conditions are [16, 1]. The earlier ShanZei fixture instead uses [17, 1].
    if (!GuideAiFixtures::LineHas(GuideAiFixtures::Ai(value),
        L"我方部队进入（0，6）至（6，10）区域", L"第 16 回合前")) return false;
    if (!GuideAiFixtures::LineHas(GuideAiFixtures::Ai(value),
        L"我方部队进入（8，8）至（13，13）区域", L"第 16 回合前")) return false;
    return stats.commands > 0 && stats.dialogEntries > 0 && stats.ambushEntries > 0 &&
        stats.aiEntries > 0 && stats.rewardEntries > 0;
}

bool ValidateStandard65(const std::wstring& value, const GuideTestStats& stats, bool finalStage) {
    if (!ValidatePlayerText(value) || stats.commands == 0 || stats.dialogEntries == 0 || stats.aiEntries == 0)
        return false;
    static const wchar_t* first[] = {
        L"第 00 关简易攻略",
        L"击退张宝时，张宝触发对话；获得恢复用麦，并且获得太平清领道。",
        L"击退张梁时，张梁触发对话并且获得恢复用麦。",
        L"我军阶段开始且第 1 回合时",
        L"完成本关后，获得 200 金钱。",
        L"我方部队到达（13，12）后，获得恢复用豆。"
    };
    static const wchar_t* last[] = {
        L"第 58 关简易攻略",
        L"（2，12）至（39，39）区域内的敌军数量等于0且我军阶段开始",
        // 72 renaming is a conservative narrative boundary; the subsequent
        // buff precedes the later 魔王 dialogue and must not attach to it.
        L"魔王触发对话",
        L"诸葛亮全能力上升"
    };
    if (finalStage) {
        for (const wchar_t* item : last) if (value.find(item) == std::wstring::npos) return false;
        const auto ai = GuideAiFixtures::Ai(value);
        if (!GuideAiFixtures::LineHas(ai, L"（0，0）", L"主动出击") ||
            !GuideAiFixtures::LineHas(ai, L"（39，39）", L"主动出击") ||
            GuideAiFixtures::LineHas(GuideAiFixtures::Dialogues(value), L"魔王触发对话", L"诸葛亮全能力上升")) return false;
        return stats.commands == 477 && stats.rewardEntries == 1;
    }
    for (const wchar_t* item : first) if (value.find(item) == std::wstring::npos) return false;
    const auto ai = GuideAiFixtures::Ai(value);
    if (!GuideAiFixtures::LineHas(ai, L"（0，0）", L"主动出击") ||
        !GuideAiFixtures::LineHas(ai, L"（19，19）", L"主动出击")) return false;
    // AI now covers all camps, unconditional changes and direct control writes.
    // Their entry count is not the former count of changing deployed enemies.
    return stats.commands == 819 && stats.dialogEntries > 0 && stats.aiEntries > 0 && stats.rewardEntries > 0;
}

bool ValidateXiSui(const std::wstring& value, const GuideTestStats& stats, bool finalStage) {
    if (!ValidatePlayerText(value) || value.find(L"本关暂无可整理的攻略信息") != std::wstring::npos)
        return false;
    static const wchar_t* first[] = {
        L"第 01 关简易攻略",
        L"第 2 回合时，西凉骑1、轻骑兵1等人触发对话",
        L"壮汉1爆发下降、陷入麻痹和混乱和中毒并HP减少 40",
        L"第 3 回合时，6 支隐藏敌军在（8，1）至（11，2）范围出现。"
    };
    static const wchar_t* last[] = {
        L"第 61 关简易攻略",
        L"杨双与杜伏威相邻后，触发对话并且杜伏威爆发下降。",
        L"武将 2与夏溪相邻后，触发对话；武将 2精神上升，并且夏溪攻击上升。",
        L"普罗与苗海潮相邻后，触发对话；触发单挑；苗海潮防御下降、陷入麻痹并HP减少 200，并且获得将军印。",
        L"击退辅公祏时，辅公祏触发对话并且获得腾蛇越涧。",
        L"完成本关后，获得将军印并且获得 5000 金钱。",
        L"我方部队到达（7，27）后，获得珍珠。"
    };
    if (finalStage) {
        for (const wchar_t* item : last) if (value.find(item) == std::wstring::npos) return false;
        // Person 2 has no physical name in this fixture. Retain its adjacent
        // dialogue and state-change event through the explicit ID fallback.
        return stats.commands == 1198 && stats.dialogEntries > 0 && stats.ambushEntries == 0 &&
            stats.aiEntries > 0 && stats.rewardEntries > 0 &&
            GuideAiFixtures::LineHas(GuideAiFixtures::Ai(value), L"（13，9）", L"主动出击") &&
            GuideAiFixtures::LineHas(GuideAiFixtures::Ai(value), L"（22，11）", L"主动出击");
    }
    for (const wchar_t* item : first) if (value.find(item) == std::wstring::npos) return false;
    // Assert intended policies, rather than comparing with the initial enemy
    // deployment (which cannot prove an event's previous policy).
    return stats.commands == 508 && stats.dialogEntries > 0 && stats.ambushEntries == 1 &&
        stats.aiEntries > 0 && stats.rewardEntries > 0 &&
        GuideAiFixtures::Has(GuideAiFixtures::Ai(value), L"主动出击") &&
        GuideAiFixtures::Has(GuideAiFixtures::Ai(value), L"攻击指定武将");
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && wcscmp(argv[1], L"--daming-s01") == 0) {
        std::ifstream input(std::filesystem::path(argv[2]), std::ios::binary);
        const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());
        std::vector<std::wstring> people(1024);
        people[0] = L"朱重八"; people[2] = L"周德兴";
        people[34] = L"梁必追"; people[239] = L"华云龙"; people[243] = L"费聚";
        std::wstring text, diagnostics;
        std::vector<ScenarioCommand> commands;
        const bool okay = !bytes.empty() && BuildGuideScriptTextForTest(bytes, people, text, diagnostics) &&
            GuideAiFixtures::ValidateDaming(text) && ParseScenarioScript(bytes, commands) &&
            GuideAiFixtures::ValidateDamingStructure(commands);
        printf("%s real Daming S01 parent dialogues and child control writes in both sections\n", okay ? "PASS" : "FAIL");
        return okay ? 0 : 22;
    }
    const bool aiFixtures = GuideAiFixtures::Run(BuildGuideScriptTextForTest,
        [](bool okay, const char* description, const std::wstring& text, const std::wstring& diagnostics) {
            printf("%s %s\n", okay ? "PASS" : "FAIL", description);
            if (!okay) {
                const std::wstring evidence = text + L"\nDiagnostics: " + diagnostics + L"\n";
                const int count = WideCharToMultiByte(CP_UTF8, 0, evidence.data(), static_cast<int>(evidence.size()),
                    nullptr, 0, nullptr, nullptr);
                std::string utf8(static_cast<size_t>(count), '\0');
                WideCharToMultiByte(CP_UTF8, 0, evidence.data(), static_cast<int>(evidence.size()), utf8.data(),
                    count, nullptr, nullptr);
                fwrite(utf8.data(), 1, utf8.size(), stdout);
            }
        });
    if (!aiFixtures) return 21;
    if (!ValidateAbilityCommands()) return 17;
    if (!ValidateConditionalExtension()) return 19;
    if (!ValidateGuideResourceReadiness()) return 20;
    if (!ValidateRWithoutBattlefield()) return 18;
    if (argc == 2 && (wcscmp(argv[1], L"--synthetic") == 0 || wcscmp(argv[1], L"--self-test") == 0)) return 0;
    if (argc != 3 && argc != 4) return 2;
    if (!ValidateStateFormatting()) return 7;
    if (!ValidateAuthorGuideFormatting()) return 16;
    if (argc == 4 && wcscmp(argv[3], L"--jumps") == 0) {
        std::vector<std::wstring> targets;
        if (!ListGuideScenarioJumpsForTest(argv[1], targets)) return 12;
        for (size_t i = 0; i < targets.size(); ++i) wprintf(i ? L",%ls" : L"%ls", targets[i].c_str());
        wprintf(L"\n");
        return 0;
    }
    if (argc == 4 && wcscmp(argv[3], L"--resolve") == 0) {
        std::wstring target;
        if (!ResolveGuideScenarioForTest(argv[1], argv[2], target)) return 11;
        const wchar_t* slash = wcsrchr(target.c_str(), L'\\');
        wprintf(L"%ls\n", slash ? slash + 1 : target.c_str());
        return 0;
    }
    if (argc == 4 && wcscmp(argv[3], L"--stats") == 0) {
        GuideTestStats stats;
        if (!BuildGuideStatsForTest(argv[1], argv[2], stats)) return 5;
        wprintf(L"commands=%zu ambushCommands=%zu aiCommands=%zu conditionedAmbushCommands=%zu "
            L"matchedAmbushCommands=%zu conditionedAiCommands=%zu matchedAiCommands=%zu "
            L"changingAiCommands=%zu dialogEntries=%zu ambushEntries=%zu aiEntries=%zu rewardEntries=%zu\n",
            stats.commands, stats.ambushCommands, stats.aiCommands, stats.conditionedAmbushCommands,
            stats.matchedAmbushCommands, stats.conditionedAiCommands, stats.matchedAiCommands,
            stats.changingAiCommands, stats.dialogEntries, stats.ambushEntries, stats.aiEntries,
            stats.rewardEntries);
        wprintf(L"ambushConditionKinds=");
        for (size_t i = 0; i < _countof(stats.ambushConditions); ++i)
            if (stats.ambushConditions[i]) wprintf(L"%02zX:%zu,", i, stats.ambushConditions[i]);
        wprintf(L" aiConditionKinds=");
        for (size_t i = 0; i < _countof(stats.aiConditions); ++i)
            if (stats.aiConditions[i]) wprintf(L"%02zX:%zu,", i, stats.aiConditions[i]);
        wprintf(L"\n");
        return 0;
    }
    std::wstring text;
    if (!BuildGuideTextForTest(argv[1], argv[2], text)) return 3;
    if (!ValidatePlayerText(text)) return 4;
    if (argc == 4 && (wcscmp(argv[3], L"--shan-ze-s00") == 0 ||
        wcscmp(argv[3], L"--shan-ze-s85") == 0)) {
        GuideTestStats stats;
        if (!BuildGuideStatsForTest(argv[1], argv[2], stats)) return 6;
        if (wcscmp(argv[3], L"--shan-ze-s85") == 0) {
            if (!ValidateShanZeiS85(text, stats)) return 13;
        } else if (!ValidateShanZeiS00(text, stats)) return 6;
        if (wcscmp(argv[3], L"--shan-ze-s85") == 0) {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                nullptr, 0, nullptr, nullptr);
            std::string utf8(static_cast<size_t>(bytes), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                utf8.data(), bytes, nullptr, nullptr);
            fwrite(utf8.data(), 1, utf8.size(), stdout);
            return 0;
        }
        const std::wstring r00 = std::wstring(argv[2]) + L"\\RS\\R_00.eex";
        std::wstring target;
        if (!ResolveGuideScenarioForTest(r00.c_str(), argv[2], target) ||
            !EndsWithInsensitive(target, L"\\RS\\S_00.eex")) return 8;
        if (!ValidateScenarioSelection(argv[2])) return 10;
        std::wstring fromR;
        if (!BuildGuideTextForTest(r00.c_str(), argv[2], fromR) ||
            fromR.find(L"刘备HP上限低于 80时，刘备触发对话并且刘备全能力上升并解除异常") ==
                std::wstring::npos)
            return 9;
    } else if (argc == 4 && (wcscmp(argv[3], L"--standard65-s00") == 0 ||
        wcscmp(argv[3], L"--standard65-s58") == 0)) {
        GuideTestStats stats;
        if (!BuildGuideStatsForTest(argv[1], argv[2], stats) ||
            !ValidateStandard65(text, stats, wcscmp(argv[3], L"--standard65-s58") == 0) ||
            !ValidateScenarioSelection(argv[2])) return 14;
    } else if (argc == 4 && (wcscmp(argv[3], L"--xisui-s01") == 0 ||
        wcscmp(argv[3], L"--xisui-s61") == 0)) {
        GuideTestStats stats;
        if (!BuildGuideStatsForTest(argv[1], argv[2], stats) ||
            !ValidateXiSui(text, stats, wcscmp(argv[3], L"--xisui-s61") == 0) ||
            !ValidateScenarioSelection(argv[2])) return 15;
    } else if (argc == 4) {
        return 2;
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), bytes, nullptr, nullptr);
    fwrite(utf8.data(), 1, utf8.size(), stdout);
    return 0;
}
