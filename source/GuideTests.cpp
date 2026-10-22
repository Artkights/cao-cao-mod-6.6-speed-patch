#include "Guide.h"
#include <algorithm>
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
        L"刘备与裴元绍相邻且裴元绍当前 HP 低于 100时，刘备与裴元绍触发对话并且获得精壮果。",
        L"第 13 回合",
        L"6 支隐藏敌军",
        L"初始部署于",
        L"完成本关后，获得 20000 金钱",
        L"我方部队到达（9，17）",
        L"关羽撤退时，关羽触发对话并且失去 500 金钱。",
        L"击退邓茂时，邓茂触发对话并且获得时空遗枝。",
        L"刘备当前 HP 低于 80时，刘备触发对话并且刘备全能力上升并解除异常。",
        L"关羽当前 HP 低于 80时，关羽触发对话并且关羽全能力上升并解除异常。",
        L"张飞当前 HP 低于 80时，张飞触发对话并且张飞全能力上升并解除异常。"
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
        L"程远志当前 HP 低于 200且关羽与程远志相邻时，程远志、关羽等人触发对话；关羽与程远志单挑，并且程远志死亡。",
        L"（5，3）至（15，10）区域内的敌军攻防下降",
        L"（1，2）至（2，3）区域内的敌军防御上升",
        L"（5，3）至（19，11）区域内的敌军防御上升",
        L"（6，8）至（8，10）区域内的敌军攻击上升",
        L"刘备当前 HP 低于 80时，刘备触发对话并且刘备全能力上升并解除异常",
        L"完成本关后，获得 30000 金钱"
    };
    if (!ValidatePlayerText(value) || value.find(L"本关暂无可整理的攻略信息") != std::wstring::npos)
        return false;
    for (const wchar_t* text : required) if (value.find(text) == std::wstring::npos) return false;
    if (value.find(L"击退姜维时，姜维触发对话") != std::wstring::npos ||
        value.find(L"击退姜维后，姜维死亡") != std::wstring::npos ||
        value.find(L"第 3 回合时，程远志触发对话。") != std::wstring::npos) return false;
    if (value.find(L"第 16 回合前，区域内的敌军") != std::wstring::npos) return false;
    if (CountOccurrences(value,
        L"我方部队进入（0，6）至（6，10）区域且第 16 回合前") != 2) return false;
    if (CountOccurrences(value,
        L"我方部队进入（8，8）至（13，13）区域且第 16 回合前") != 2) return false;
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
        L"（5，10）至（12，12）范围的敌军由被动出击改为主动出击",
        L"完成本关后，获得 200 金钱。",
        L"我方部队到达（13，12）后，获得恢复用豆。"
    };
    static const wchar_t* last[] = {
        L"第 58 关简易攻略",
        L"（2，12）至（39，39）区域内的敌军数量等于0且我军阶段开始",
        L"曹操触发对话；诸葛亮触发对话，并且诸葛亮全能力上升。",
        L"（9，2）至（23，18）范围的敌军由坚守原地改为主动出击。"
    };
    if (finalStage) {
        for (const wchar_t* item : last) if (value.find(item) == std::wstring::npos) return false;
        return stats.commands == 477 && stats.rewardEntries == 1;
    }
    for (const wchar_t* item : first) if (value.find(item) == std::wstring::npos) return false;
    return stats.commands == 819 && stats.dialogEntries == 2 && stats.aiEntries == 1 && stats.rewardEntries == 7;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 && argc != 4) return 2;
    if (!ValidateStateFormatting()) return 7;
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
            fromR.find(L"刘备当前 HP 低于 80时，刘备触发对话并且刘备全能力上升并解除异常") ==
                std::wstring::npos)
            return 9;
    } else if (argc == 4 && (wcscmp(argv[3], L"--standard65-s00") == 0 ||
        wcscmp(argv[3], L"--standard65-s58") == 0)) {
        GuideTestStats stats;
        if (!BuildGuideStatsForTest(argv[1], argv[2], stats) ||
            !ValidateStandard65(text, stats, wcscmp(argv[3], L"--standard65-s58") == 0) ||
            !ValidateScenarioSelection(argv[2])) return 14;
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
