#pragma once

// Shared semantic regressions for the proxy and the standalone generator.
// This file deliberately uses only the C++ standard library. Keep the copy in
// the standalone source package identical, so that package builds in isolation.
#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace GuideAiFixtures {
using Bytes = std::vector<unsigned char>;

inline void Word(Bytes& bytes, unsigned value) {
    bytes.push_back(static_cast<unsigned char>(value));
    bytes.push_back(static_cast<unsigned char>(value >> 8));
}
inline void Dword(Bytes& bytes, int32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<unsigned char>(static_cast<uint32_t>(value) >> shift));
}
inline void Append(Bytes& bytes, const Bytes& suffix) {
    bytes.insert(bytes.end(), suffix.begin(), suffix.end());
}
template<size_t N>
inline void Command(Bytes& bytes, unsigned opcode, const std::array<unsigned, N>& tags,
    const std::array<int, N>& values, unsigned dwords = 0) {
    Word(bytes, opcode);
    for (size_t slot = 0; slot < N; ++slot) {
        Word(bytes, tags[slot]);
        if (dwords & (1u << slot)) Dword(bytes, values[slot]);
        else Word(bytes, static_cast<unsigned>(values[slot]));
    }
}
inline void Text(Bytes& bytes, unsigned opcode, const char* utf8) {
    Word(bytes, opcode); Word(bytes, 5);
    while (*utf8) bytes.push_back(static_cast<unsigned char>(*utf8++));
    bytes.push_back(0);
}
inline void Dialogue(Bytes& bytes, const char* speaker = "ParentSpeaker") {
    std::string text = "&"; text += speaker; text += "\nFixture dialogue.";
    Text(bytes, 0x14, text.c_str());
}
inline Bytes Adjacent(int first = 0, int second = 1) {
    Bytes bytes;
    Command(bytes, 0x2E, std::array<unsigned, 3>{2, 2, 0x26},
        std::array<int, 3>{first, second, 1});
    return bytes;
}
inline Bytes Turn(int turn) {
    Bytes bytes;
    Command(bytes, 0x3F, std::array<unsigned, 2>{4, 0x24},
        std::array<int, 2>{turn, 0}, 1);
    return bytes;
}
inline void Child(Bytes& parent, const Bytes& body, const Bytes& condition = {}) {
    Word(parent, 1);
    if (condition.empty()) Text(parent, 2, "fixture child");
    else Append(parent, condition);
    Word(parent, static_cast<unsigned>(body.size()));
    Append(parent, body);
}
inline Bytes Event(const Bytes& body, const Bytes& conditions = {}) {
    Bytes section = conditions;
    Word(section, 0); Word(section, static_cast<unsigned>(body.size()));
    Append(section, body);
    return section;
}
inline Bytes Scenario(const std::vector<Bytes>& sections) {
    Bytes bytes = {'E', 'E', 'X', 0, 0, 0, 0, 0, 0, 0};
    Dword(bytes, 14);
    Word(bytes, static_cast<unsigned>(sections.size()));
    for (const Bytes& section : sections) {
        Word(bytes, static_cast<unsigned>(section.size())); Append(bytes, section);
    }
    return bytes;
}
inline Bytes Scenario(const Bytes& actions, const Bytes& conditions = {}) {
    return Scenario(std::vector<Bytes>{Event(actions, conditions)});
}

// The engine reads exactly eleven slots for 4E, including unused target slots.
inline void Policy(Bytes& bytes, int person, int policy, int target = -1,
    int targetX = 0, int targetY = 0, unsigned personTag = 2) {
    Command(bytes, 0x4E, std::array<unsigned, 11>{0x2C, personTag, 4, 4, 4, 4, 3, 7, 2, 4, 4},
        std::array<int, 11>{0, person, 0, 0, 0, 0, 0, policy, target, targetX, targetY}, 1596);
}
inline void AreaPolicy(Bytes& bytes, int camp, int policy, int x1 = 1, int y1 = 2,
    int x2 = 3, int y2 = 4) {
    Command(bytes, 0x4E, std::array<unsigned, 11>{0x2C, 2, 4, 4, 4, 4, 3, 7, 2, 4, 4},
        std::array<int, 11>{1, -1, x1, y1, x2, y2, camp, policy, -1, 0, 0}, 1596);
}
inline void Assign(Bytes& bytes, int variable, int value, int operation = 2, int source = 0) {
    Command(bytes, 0x77, std::array<unsigned, 5>{0x51, 4, 0x53, 0x52, 4},
        std::array<int, 5>{2, variable, operation, source, value}, 18);
}
inline void Control(Bytes& bytes, int variable, int person, int mode = 1,
    int attribute = 32, unsigned personTag = 2) {
    Command(bytes, 0x78, std::array<unsigned, 4>{4, 0x54, personTag, 0x55},
        std::array<int, 4>{variable, mode, person, attribute}, 1);
}
inline void Call(Bytes& bytes) {
    // 72 branch 13 invokes a script function and may mutate integer variables.
    Word(bytes, 0x72); Word(bytes, 4); Dword(bytes, 13);
    Word(bytes, 5); bytes.push_back('0'); bytes.push_back(0);
}
inline void FriendDeployment(Bytes& bytes, int person = 1, int policy = 1) {
    Word(bytes, 0x46);
    const std::array<unsigned, 11> tags{2, 0x26, 4, 4, 0x2B, 0x3E, 0x45, 7, 2, 4, 4};
    for (int record = 0; record < 20; ++record) {
        const std::array<int, 11> values{record ? -1 : person, 0, 2, 3, 0, 0, 0,
            policy, -1, 0, 0};
        for (size_t slot = 0; slot < tags.size(); ++slot) {
            Word(bytes, tags[slot]);
            if (1548 & (1u << slot)) Dword(bytes, values[slot]);
            else Word(bytes, static_cast<unsigned>(values[slot]));
        }
    }
}
inline void AllyPosition(Bytes& bytes, int sortieOrder = 0) {
    Command(bytes, 0x4B, std::array<unsigned, 5>{4, 4, 4, 0x2B, 0x26},
        std::array<int, 5>{sortieOrder, 2, 3, 0, 0}, 7);
}
inline bool Has(const std::wstring& text, const wchar_t* needle) {
    return text.find(needle) != std::wstring::npos;
}
inline std::wstring Section(const std::wstring& text, const wchar_t* heading,
    const wchar_t* next) {
    const size_t begin = text.find(heading);
    if (begin == std::wstring::npos) return {};
    const size_t end = next && *next ? text.find(next, begin) : std::wstring::npos;
    return text.substr(begin, end == std::wstring::npos ? end : end - begin);
}
inline std::wstring Dialogues(const std::wstring& text) {
    return Section(text, L"一、人物对话与单挑", L"二、伏兵");
}
inline std::wstring Ai(const std::wstring& text) {
    return Section(text, L"三、部队 AI 与控制变化", L"四、状态与奖惩");
}
inline bool LineHas(const std::wstring& text, const wchar_t* first, const wchar_t* second) {
    for (size_t start = 0; start < text.size();) {
        const size_t end = text.find(L'\n', start);
        const std::wstring line = text.substr(start, end == std::wstring::npos ? end : end - start);
        if (Has(line, first) && Has(line, second)) return true;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return false;
}
inline bool Unknown(const std::wstring& text) {
    return Has(text, L"脚本变量") && Has(text, L"决定");
}

// Build and Check are injected by each executable; no production algorithm is
// duplicated here. Assert meanings and association boundaries, not entry counts.
template<class Build, class Check>
inline bool Run(Build build, Check check) {
    bool okay = true;
    const std::vector<std::wstring> people{L"主角", L"友军甲", L"目标乙", L"无部署丙"};
    auto run = [&](const char* name, const Bytes& bytes, auto predicate) {
        std::wstring text, diagnostics;
        const bool parsed = build(bytes, people, text, diagnostics);
        const bool passed = parsed && predicate(text, diagnostics);
        check(passed, name, text, diagnostics);
        okay = passed && okay;
    };
    Bytes deploy; FriendDeployment(deploy);
    Bytes actions; Dialogue(actions); Policy(actions, 1, 7);
    run("4E friendly policy7 appears in dialogue and AI", Scenario({Event(deploy), Event(actions, Adjacent())}),
        [](const auto& t, const auto&) { return LineHas(Dialogues(t), L"主角", L"可由玩家控制") &&
            LineHas(Ai(t), L"友军甲", L"可由玩家控制"); });

    for (int camp = 0; camp <= 6; ++camp) {
        actions.clear(); AreaPolicy(actions, camp, 1);
        const wchar_t* camps[] = {L"我军", L"友军", L"敌军", L"援军", L"我军及友军", L"敌军及援军", L"全部部队"};
        run("4E full area and camp retained without deployments", Scenario(actions, Turn(3)),
            [camp, &camps](const auto& t, const auto&) { return Has(Ai(t), L"主动出击") && Has(Ai(t), L"（1，2）") &&
                Has(Ai(t), L"（3，4）") && (Has(Ai(t), camps[camp]) || (camp == 6 && Has(Ai(t), L"所有部队"))); });
    }
    actions.clear(); Policy(actions, 3, 2);
    run("4E explicit person retained without deployment", Scenario(actions, Turn(4)),
        [](const auto& t, const auto&) { return LineHas(Ai(t), L"无部署丙", L"坚守原地"); });
    actions.clear();
    Command(actions, 0x4E, std::array<unsigned, 11>{0x2C, 2, 4, 4, 4, 4, 3, 7, 2, 4, 4},
        std::array<int, 11>{-1, 1, 0, 0, 0, 0, -1, 1, -1, 0, 0}, 1596);
    run("4E mode minus one uses the native single-person alias", Scenario(actions),
        [](const auto& t, const auto& d) { return LineHas(Ai(t), L"友军甲", L"主动出击") &&
            !Has(d, L"ai-target-unresolved") && !Has(Ai(t), L"原始指令"); });
    actions.clear(); AllyPosition(actions, 0); Policy(actions, 3, 7);
    run("4B sortie order does not fabricate person identity", Scenario(actions),
        [](const auto& t, const auto&) { return LineHas(Ai(t), L"无部署丙", L"可由玩家控制") &&
            !LineHas(Ai(t), L"主角", L"可由玩家控制"); });

    Bytes first; Policy(first, 1, 2); Bytes second; Policy(second, 1, 1);
    run("4E later return to initial policy is retained", Scenario({Event(deploy), Event(first, Turn(2)), Event(second, Turn(3))}),
        [](const auto& t, const auto&) { return Has(Ai(t), L"坚守原地") && Has(Ai(t), L"主动出击"); });
    actions.clear(); Policy(actions, 1, 3, 2); Policy(actions, 1, 4, -1, 8, 9);
    Policy(actions, 1, 5, 2); Policy(actions, 1, 6, -1, 6, 7);
    run("4E person and point target slots are preserved", Scenario(actions),
        [](const auto& t, const auto&) { const auto a = Ai(t); return Has(a, L"攻击指定武将") && Has(a, L"目标乙") &&
            Has(a, L"跟随指定武将") && Has(a, L"8，9") && Has(a, L"6，7"); });

    actions.clear(); Policy(actions, -2, 7);
    run("4E variable person is not treated as a literal person ID", Scenario(actions),
        [](const auto& t, const auto& d) { return !Ai(t).empty() && (Has(Ai(t), L"变量") || !d.empty()) &&
            !Has(t, L"武将 -2") && !LineHas(Ai(t), L"主角", L"可由玩家控制"); });
    actions.clear(); AreaPolicy(actions, 1, 7, -1, 2, 3, 4);
    run("4E dynamic area remains explicit and does not guess coordinates", Scenario(actions),
        [](const auto& t, const auto& d) { return !Ai(t).empty() && (Has(Ai(t), L"变量") || !d.empty()) && !Has(t, L"（-1，2）"); });
    actions.clear(); Policy(actions, 1027, 7);
    run("4E special current-person selector is not a literal person ID", Scenario(actions),
        [](const auto& t, const auto& d) { return !Ai(t).empty() && !Has(t, L"武将 1027") &&
            (Has(Ai(t), L"当前") || Has(Ai(t), L"特殊人物选择器") || !d.empty()); });
    actions.clear(); Policy(actions, 1, 7, -1, 0, 0, 0x7F);
    run("4E unknown person tag cannot prove a direct control target", Scenario(actions),
        [](const auto& t, const auto& d) { return !d.empty() && !LineHas(Ai(t), L"友军甲", L"可由玩家控制"); });

    actions.clear(); Assign(actions, 0, 7); Control(actions, 0, 1); Assign(actions, 0, 8); Control(actions, 0, 1);
    run("78 raw7 enables control and raw8 disables it", Scenario(actions),
        [](const auto& t, const auto&) { return Has(Ai(t), L"可由玩家控制") && Has(Ai(t), L"设为不可控"); });
    actions.clear(); Assign(actions, 0, 0); Control(actions, 0, 1); Assign(actions, 0, 4); Control(actions, 0, 1);
    run("78 raw0 and raw4 do not invent policy targets", Scenario(actions),
        [](const auto& t, const auto&) { return Has(Ai(t), L"原始字节设为 0") &&
            Has(Ai(t), L"原始字节设为 4") && !Has(Ai(t), L"目标乙") &&
            !Has(Ai(t), L"被动出击") && !Has(Ai(t), L"可由玩家控制"); });
    actions.clear(); Control(actions, 5, 1);
    run("78 unknown variable keeps a control event", Scenario(actions),
        [](const auto& t, const auto&) { return Unknown(Ai(t)) && Has(Ai(t), L"5") && !Has(Ai(t), L"可由玩家控制"); });
    actions.clear(); Assign(actions, 0, 7); Assign(actions, 0, 8); Control(actions, 0, 1);
    run("77 latest constant overwrites earlier control value", Scenario(actions),
        [](const auto& t, const auto&) { return Has(Ai(t), L"设为不可控") && !Has(Ai(t), L"可由玩家控制"); });
    actions.clear(); Assign(actions, 0, 7); Assign(actions, 0, 1, 0); Control(actions, 0, 1);
    run("77 unsupported arithmetic invalidates known control value", Scenario(actions),
        [](const auto& t, const auto&) { return Unknown(Ai(t)) && !Has(Ai(t), L"可由玩家控制"); });
    actions.clear(); Assign(actions, 0, 7); Control(actions, 0, 2, 0, 2); Control(actions, 0, 1);
    run("78 read invalidates previously known variable", Scenario(actions),
        [](const auto& t, const auto&) { return Unknown(Ai(t)) && !Has(Ai(t), L"可由玩家控制"); });
    actions.clear(); Assign(actions, 0, 7); Call(actions); Control(actions, 0, 1);
    run("script function call invalidates constant assumptions", Scenario(actions),
        [](const auto& t, const auto&) { return Unknown(Ai(t)) && !Has(Ai(t), L"可由玩家控制"); });
    first.clear(); Assign(first, 0, 7); second.clear(); Control(second, 0, 1);
    actions.clear(); Child(actions, first, Adjacent(0, 1)); Child(actions, second, Adjacent(0, 2));
    run("constant from a sibling branch does not leak", Scenario(actions),
        [](const auto& t, const auto&) { return Unknown(Ai(t)) && !Has(Ai(t), L"可由玩家控制"); });
    run("constant from a different event does not leak", Scenario({Event(first), Event(second, Turn(2))}),
        [](const auto& t, const auto&) { return Unknown(Ai(t)) && !Has(Ai(t), L"可由玩家控制"); });
    actions.clear(); Assign(actions, 0, 7); Control(actions, 0, -2);
    run("78 dynamic person does not guess a literal identity", Scenario(actions),
        [](const auto& t, const auto& d) { return !Ai(t).empty() && (Has(Ai(t), L"变量") || !d.empty()) && !Has(t, L"武将 -2"); });
    actions.clear(); Assign(actions, 0, 7); Control(actions, 0, 1, 1, 31);
    run("78 other attributes are not control changes", Scenario(actions),
        [](const auto& t, const auto&) { return !Has(Ai(t), L"可由玩家控制"); });
    actions.clear(); Assign(actions, 0, 7); Control(actions, 0, 1, 1, 32, 0x7F);
    run("78 unknown tag cannot turn a variable into a proven target", Scenario(actions),
        [](const auto& t, const auto& d) { return !d.empty() && !LineHas(Ai(t), L"友军甲", L"可由玩家控制"); });

    Bytes child; Assign(child, 0, 7); Control(child, 0, 1);
    actions.clear(); Dialogue(actions); Child(actions, child);
    run("parent dialogue associates with a later nested control write", Scenario(actions, Adjacent()),
        [](const auto& t, const auto&) { return LineHas(Dialogues(t), L"主角", L"可由玩家控制") && Has(Ai(t), L"可由玩家控制"); });
    actions = child; Dialogue(actions, "LaterSpeaker");
    run("a later dialogue does not claim an earlier control write", Scenario(actions, Adjacent()),
        [](const auto& t, const auto&) { return Has(Ai(t), L"可由玩家控制") &&
            !Has(Dialogues(t), L"可由玩家控制"); });
    first.clear(); Dialogue(first, "SiblingSpeaker"); actions.clear();
    Child(actions, first, Adjacent(0, 1)); Child(actions, child, Adjacent(0, 2));
    run("dialogue in a sibling branch does not claim control write", Scenario(actions),
        [](const auto& t, const auto&) { return Has(Ai(t), L"可由玩家控制") &&
            !Has(Dialogues(t), L"可由玩家控制"); });

    actions.clear(); Dialogue(actions);
    Assign(actions, 0, 7); Control(actions, 0, 1);
    Assign(actions, 0, 0); Control(actions, 0, 1);
    Assign(actions, 0, 7); Control(actions, 0, 1);
    run("dialogue preserves control round trip in execution order", Scenario(actions, Adjacent()),
        [](const auto& t, const auto&) {
            const auto d = Dialogues(t);
            const size_t firstControl = d.find(L"可由玩家控制");
            const size_t middle = d.find(L"原始字节设为 0", firstControl);
            const size_t lastControl = d.find(L"可由玩家控制", middle);
            return firstControl != std::wstring::npos && middle != std::wstring::npos &&
                lastControl != std::wstring::npos && firstControl < middle && middle < lastControl;
        });
    actions.clear(); Dialogue(actions); Child(actions, child, Turn(3));
    run("nested later condition does not become the parent dialogue trigger", Scenario(actions),
        [](const auto& t, const auto&) {
            const auto d = Dialogues(t);
            const size_t dialogue = d.find(L"触发对话");
            const size_t turn = d.find(L"第 3 回合");
            const size_t control = d.find(L"可由玩家控制");
            return dialogue != std::wstring::npos && turn != std::wstring::npos &&
                control != std::wstring::npos && dialogue < turn && turn < control;
        });
    actions.clear(); Dialogue(actions);
    Command(actions, 0x76, std::array<unsigned, 1>{4}, std::array<int, 1>{0}, 1);
    Append(actions, child);
    run("unconditional jump closes preceding dialogue association", Scenario(actions, Adjacent()),
        [](const auto& t, const auto&) { return Has(Ai(t), L"可由玩家控制") &&
            !Has(Dialogues(t), L"可由玩家控制"); });
    Bytes duel;
    Command(duel, 0x68, std::array<unsigned, 3>{2, 2, 0x4F}, std::array<int, 3>{0, 1, 24});
    actions.clear(); Dialogue(actions); Child(actions, duel); Policy(actions, 1, 7);
    run("nested duel survives when later outcomes return to its parent", Scenario(actions, Adjacent()),
        [](const auto& t, const auto&) { return Has(Dialogues(t), L"触发单挑") &&
            Has(Dialogues(t), L"可由玩家控制"); });
    actions.clear(); Dialogue(actions); Policy(actions, 1, 7); Child(actions, duel);
    run("a later nested duel does not move an earlier control change after it", Scenario(actions, Adjacent()),
        [](const auto& t, const auto&) {
            const auto d = Dialogues(t);
            const size_t dialogue = d.find(L"触发对话");
            const size_t control = d.find(L"可由玩家控制");
            const size_t duelAt = d.find(L"触发单挑");
            return dialogue != std::wstring::npos && control != std::wstring::npos &&
                duelAt != std::wstring::npos && dialogue < control && control < duelAt &&
                LineHas(d, L"触发对话", L"可由玩家控制") &&
                !LineHas(d, L"单挑", L"可由玩家控制") && Has(Ai(t), L"可由玩家控制");
        });
    Bytes laterControl; Policy(laterControl, 1, 7);
    actions.clear(); Dialogue(actions); Child(actions, duel); Child(actions, laterControl, Turn(3));
    run("a control branch after a duel keeps its extra turn condition in both chapters", Scenario(actions, Adjacent()),
        [](const auto& t, const auto&) {
            const auto d = Dialogues(t);
            const size_t duelAt = d.find(L"触发单挑");
            const size_t later = d.find(L"；随后，");
            const size_t turn = d.find(L"第 3 回合开始");
            const size_t control = d.find(L"可由玩家控制");
            return LineHas(d, L"触发对话", L"可由玩家控制") &&
                duelAt != std::wstring::npos && later != std::wstring::npos &&
                turn != std::wstring::npos && control != std::wstring::npos &&
                duelAt < later && later < turn && turn < control &&
                LineHas(Ai(t), L"第 3 回合开始", L"可由玩家控制");
        });
    Bytes unknownCondition;
    Command(unknownCondition, 0x79, std::array<unsigned, 5>{0x52, 4, 0x56, 0x52, 4},
        std::array<int, 5>{4, 10, 2, 0, 120}, 18);
    actions.clear(); Dialogue(actions); Child(actions, duel); Child(actions, laterControl, unknownCondition);
    run("a control branch after a duel retains unknown conditions and diagnoses them", Scenario(actions, Adjacent()),
        [](const auto& t, const auto& diagnostics) {
            const auto d = Dialogues(t);
            const size_t duelAt = d.find(L"触发单挑");
            const size_t later = d.find(L"；随后，");
            const size_t condition = d.find(L"满足其他事件条件");
            const size_t control = d.find(L"可由玩家控制");
            return LineHas(d, L"触发对话", L"可由玩家控制") &&
                duelAt != std::wstring::npos && later != std::wstring::npos &&
                condition != std::wstring::npos && control != std::wstring::npos &&
                duelAt < later && later < condition && condition < control &&
                LineHas(Ai(t), L"满足其他事件条件", L"可由玩家控制") &&
                Has(diagnostics, L"condition-unresolved: id=121");
        });

    first.clear(); Assign(first, 0, 263); Control(first, 0, 1);
    second.clear(); Assign(second, 0, -248); Control(second, 0, 3);
    run("78 writes the native low byte for positive and negative integers", Scenario({Event(first), Event(second)}),
        [](const auto& t, const auto&) { return LineHas(Ai(t), L"友军甲", L"可由玩家控制") &&
            LineHas(Ai(t), L"无部署丙", L"设为不可控") &&
            !LineHas(Ai(t), L"友军甲", L"设为不可控") &&
            !LineHas(Ai(t), L"无部署丙", L"可由玩家控制"); });
    child.clear(); Assign(child, 0, 8);
    actions.clear(); Assign(actions, 0, 7); Child(actions, child, Turn(3)); Control(actions, 0, 1);
    run("conditional child overwrite invalidates its parent's old constant", Scenario(actions),
        [](const auto& t, const auto&) { return Unknown(Ai(t)) &&
            !Has(Ai(t), L"可由玩家控制") && !Has(Ai(t), L"设为不可控"); });
    first.clear(); Dialogue(first, "SameConditionSibling");
    second.clear(); Assign(second, 0, 7); Control(second, 0, 1);
    actions.clear(); Child(actions, first, Adjacent(0, 1)); Child(actions, second, Adjacent(0, 1));
    run("equal condition text does not join sibling dialogue and control", Scenario(actions),
        [](const auto& t, const auto&) { return Has(Ai(t), L"可由玩家控制") &&
            !Has(Dialogues(t), L"可由玩家控制"); });
    actions.clear(); Dialogue(actions);
    Command(actions, 0x38, std::array<unsigned, 4>{2, 0x23, 0x34, 4},
        std::array<int, 4>{0, 0, 1, 50}, 8);
    Policy(actions, 1, 7);
    run("dialogue owns both later ability reward and control while AI stays visible", Scenario(actions, Adjacent()),
        [](const auto& t, const auto&) {
            const auto d = Dialogues(t);
            const size_t reward = d.find(L"主角攻击提升 50 点");
            const size_t control = d.find(L"可由玩家控制");
            return LineHas(d, L"主角攻击提升 50 点", L"可由玩家控制") &&
                reward < control && LineHas(Ai(t), L"友军甲", L"可由玩家控制");
        });
    first.clear(); Dialogue(first, "SiblingA");
    second.clear(); Dialogue(second, "SiblingB");
    actions.clear(); Child(actions, first, Turn(3)); Child(actions, second, Turn(3));
    run("same event and condition keep sibling dialogues separate", Scenario(actions),
        [](const auto& t, const auto&) {
            const auto d = Dialogues(t);
            return !LineHas(d, L"SiblingA", L"SiblingB");
        });
    first.clear();
    Command(first, 0x38, std::array<unsigned, 4>{2, 0x23, 0x34, 4},
        std::array<int, 4>{0, 0, 1, 10}, 8);
    second.clear();
    Command(second, 0x38, std::array<unsigned, 4>{2, 0x23, 0x34, 4},
        std::array<int, 4>{1, 0, 1, 20}, 8);
    actions.clear(); Child(actions, first, Turn(3)); Child(actions, second, Turn(3));
    run("same event and condition merge sibling reward effects", Scenario(actions),
        [](const auto& t, const auto&) {
            const auto rewards = Section(t, L"四、状态与奖惩", L"");
            return LineHas(rewards, L"主角攻击提升 10 点", L"友军甲攻击提升 20 点");
        });
    first.clear(); Policy(first, 1, 7);
    second.clear(); Policy(second, 2, 2);
    actions.clear(); Child(actions, first, Turn(3)); Child(actions, second, Turn(3));
    run("same event and condition merge sibling AI effects", Scenario(actions),
        [](const auto& t, const auto&) {
            const auto ai = Ai(t);
            return LineHas(ai, L"友军甲", L"目标乙");
        });
    first.clear();
    Command(first, 0x38, std::array<unsigned, 4>{2, 0x23, 0x34, 4},
        std::array<int, 4>{0, 0, 1, 10}, 8);
    second.clear();
    Command(second, 0x38, std::array<unsigned, 4>{2, 0x23, 0x34, 4},
        std::array<int, 4>{1, 0, 1, 20}, 8);
    actions.clear();
    run("same prefix in different events stays separate",
        Scenario({Event(first, Turn(3)), Event(second, Turn(3))}),
        [](const auto& t, const auto&) {
            const auto rewards = Section(t, L"四、状态与奖惩", L"");
            return !LineHas(rewards, L"主角攻击提升 10 点", L"友军甲攻击提升 20 点");
        });
    actions.clear(); Dialogue(actions, "*.1");
    Command(actions, 0x38, std::array<unsigned, 4>{2, 0x23, 0x34, 4},
        std::array<int, 4>{1, 0, 1, 10}, 8);
    run("dialogue speaker pointer resolves to the person name", Scenario(actions),
        [](const auto& t, const auto&) {
            const auto d = Dialogues(t);
            return LineHas(d, L"友军甲", L"触发对话") && !Has(d, L"*.1");
        });
    actions.clear();
    static const wchar_t* abilityNames[] = {
        L"攻击", L"防御", L"精神", L"爆发", L"士气", L"HP", L"MP",
        L"当前HP", L"当前MP", L"等级", L"武力", L"统帅", L"智力", L"敏捷", L"运气"
    };
    for (int code = 0; code <= 14; ++code)
        Command(actions, 0x38, std::array<unsigned, 4>{2, 0x23, 0x34, 4},
            std::array<int, 4>{0, code, 1, 10 + code}, 8);
    Command(actions, 0x38, std::array<unsigned, 4>{2, 0x23, 0x34, 4},
        std::array<int, 4>{0, 15, 1, 50}, 8);
    Command(actions, 0x38, std::array<unsigned, 4>{2, 0x23, 0x34, 4},
        std::array<int, 4>{0, 17, 4, 50}, 8);
    run("ability codes use the full name map and exclude portrait code 15", Scenario(actions),
        [&](const auto& t, const auto&) {
            const auto rewards = Section(t, L"四、状态与奖惩", L"");
            for (int code = 0; code <= 14; ++code) {
                const std::wstring expected = L"主角" + std::wstring(abilityNames[code]) +
                    L"提升 " + std::to_wstring(10 + code) + L" 点";
                if (!Has(rewards, expected.c_str())) return false;
            }
            return !Has(rewards, L"头像") && !Has(rewards, L"能力码 15") &&
                Has(rewards, L"能力码 17");
        });
    return okay;
}

inline bool ValidateDaming(const std::wstring& text) {
    const auto dialogue = Dialogues(text), ai = Ai(text);
    // Real S_01 evidence: 78 offsets 26299 / 28388 / 28855. The source
    // dialogues live in their parents and are followed by named author notices.
    return LineHas(dialogue, L"梁必追", L"可由玩家控制") &&
        LineHas(dialogue, L"华云龙", L"可由玩家控制") &&
        LineHas(dialogue, L"费聚", L"可由玩家控制") &&
        LineHas(ai, L"梁必追", L"可由玩家控制") &&
        LineHas(ai, L"华云龙", L"可由玩家控制") &&
        LineHas(ai, L"费聚", L"可由玩家控制");
}

template<class Commands>
inline bool ValidateDamingStructure(const Commands& commands) {
    const size_t offsets[] = {26299, 28388, 28855};
    const int persons[] = {34, 239, 243};
    const size_t sections[] = {11, 15, 16};
    const size_t owners[] = {515, 604, 625};
    for (size_t i = 0; i < 3; ++i) {
        bool matched = false;
        for (const auto& command : commands) {
            if (command.offset != offsets[i]) continue;
            if (command.id != 0x78 || command.values != std::vector<int32_t>({0, 1, persons[i], 32}) ||
                command.parameterTags != std::vector<uint16_t>({4, 0x54, 2, 0x55}) ||
                command.section != sections[i] || command.parentBlock == SIZE_MAX ||
                command.blockOwnerOrdinal != owners[i]) return false;
            bool precedingParentDialogue = false, adjacent = false;
            for (const auto& previous : commands)
                if (previous.id == 0x14 && previous.block == command.parentBlock &&
                    previous.ordinal < command.blockOwnerOrdinal) precedingParentDialogue = true;
            for (const auto& condition : command.conditions)
                if (condition.id == 0x2E) adjacent = true;
            matched = precedingParentDialogue && adjacent;
        }
        if (!matched) return false;
    }
    return true;
}
} // namespace GuideAiFixtures
