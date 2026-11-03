uint32_t receivedInputEax=0,receivedInputEcx=0,receivedInputEdx=0,receivedInputFlags=0,receivedNativeReturn=0;
uint32_t receivedOwnerEax=0,receivedNativeCalls=0,receivedRngCalls=0,receivedSubtype=0,receivedSkip=0;
uint32_t receivedNativeResult=0x76543210;
int receivedLifecycleMode=0;
struct ReceivedRegisterResult {uint32_t eax,ecx,edx,flags;};
__declspec(noinline) ReceivedRegisterResult InvokeReceivedDriver(uintptr_t driver) {
    uint32_t outputEax=0,outputEcx=0,outputEdx=0,outputFlags=0;
    __asm {
        call dword ptr [driver]
        mov outputEax,eax
        mov outputEcx,ecx
        mov outputEdx,edx
        pushfd
        pop outputFlags
    }
    return {outputEax,outputEcx,outputEdx,outputFlags};
}
void __cdecl ReceivedLifecycleBoundary() {
    if (receivedLifecycleMode==1) { SetBattleStatsLoading(true);SetBattleStatsLoading(false); }
    if (receivedLifecycleMode==2) { ResetBattleStats(L"received-native-test");SetBattleStatsModelActiveForTest(true); }
    if (receivedLifecycleMode==3) NotifyBattleStatsScenarioInstalled(3,false,true,GetTickCount64());
    if (receivedLifecycleMode==4) ResetBattleStatsForLoadedScenario(L"received-load-interrupted",1);
}
struct ReceivedCode {
    BYTE* cursor;
    void Byte(BYTE b) {*cursor++=b;}
    void Dword(uintptr_t v) {const uint32_t n=static_cast<uint32_t>(v);memcpy(cursor,&n,4);cursor+=4;}
    void Jump(uintptr_t target) {Byte(0xE9);Dword(target-reinterpret_cast<uintptr_t>(cursor)-4);}
    void Call(uintptr_t target) {Byte(0xE8);Dword(target-reinterpret_cast<uintptr_t>(cursor)-4);}
    void Mov(unsigned reg,uintptr_t v) {Byte(static_cast<BYTE>(0xB8+reg));Dword(v);}
    void Store(unsigned reg,uintptr_t v) {Byte(0x89);Byte(static_cast<BYTE>(5+reg*8));Dword(v);}
    void Local(BYTE offset,uintptr_t value) {Byte(0xC7);Byte(0x45);Byte(offset);Dword(value);}
    void Prolog() {Byte(0x55);Byte(0x8B);Byte(0xEC);Byte(0x83);Byte(0xEC);Byte(0x40);}
    void Epilog() {Byte(0xC9);Byte(0xC3);}
};
long long ReceivedCount(bool strategy,int slot=7) {
    BattleStatsSnapshot snapshot;ReadBattleStatsSnapshot(snapshot);
    const auto* unit=FindUnit(snapshot,slot);
    return unit ? (strategy ? unit->strategyReceivedCount : unit->physicalReceivedCount) : 0;
}
void RunReceivedBridgeTests(const std::vector<BYTE>& image,DWORD base) {
    const bool v64=CompiledEngineProfile()==EngineProfileId::Ccz64;
    const bool v66=CompiledEngineProfile()==EngineProfileId::Ccz66;
    BattleStatsImageLayout layout;
    Check(ResolveBattleStatsImageLayoutForTest(image.data(),base,layout) &&
        layout.physicalReceivedAvailable && layout.strategyReceivedAvailable,
        "verify received profiles before executing their installed production bridges");
    auto changed=image;BattleStatsImageLayout disabled;
    changed[0x4785C6-base]^=1;
    Check(ResolveBattleStatsImageLayoutForTest(changed.data(),base,disabled) &&
        disabled.physicalReceivedAvailable && !disabled.strategyReceivedAvailable && disabled.dodgeAvailable,
        "unknown random effect table disables only strategy received while preserving physical received and dodge");
    changed=image;changed[0x405E37-base]=0x90;
    Check(ResolveBattleStatsImageLayoutForTest(changed.data(),base,disabled) &&
        !disabled.physicalReceivedAvailable && disabled.strategyReceivedAvailable && disabled.dodgeAvailable,
        "unknown physical target commit disables only physical received");
    BYTE* mapped=static_cast<BYTE*>(VirtualAlloc(nullptr,image.size(),MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    BYTE* scratch=static_cast<BYTE*>(VirtualAlloc(nullptr,8192,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    Check(mapped && scratch,"allocate isolated native image and code for received production bridge execution");
    if (!mapped || !scratch) {if(mapped)VirtualFree(mapped,0,MEM_RELEASE);if(scratch)VirtualFree(scratch,0,MEM_RELEASE);return;}
    memcpy(mapped,image.data(),image.size());
    const uintptr_t mappedBase=reinterpret_cast<uintptr_t>(mapped);
    auto at=[mappedBase](uintptr_t va){return mappedBase+va-0x400000;};
    const uintptr_t units=at(layout.unitBase);
    for (int slot=0;slot<250;++slot) {
        PutPerson(reinterpret_cast<BYTE*>(units+slot*0x30),static_cast<uint16_t>(1000+slot));
        PutByte(reinterpret_cast<BYTE*>(units+slot*0x30+5),slot>=100 ? 2 : 0);
    }
    InitializeBattleStatsModelForTest(units);SetBattleStatsModelActiveForTest(true);
    std::vector<uintptr_t> sites;
    const bool installed=InstallAllBattleStatsHooksForTest(mapped,base,sites);
    BattleStatsSnapshot capabilities;ReadBattleStatsSnapshot(capabilities);
    Check(installed && capabilities.physicalReceivedAvailable && capabilities.strategyReceivedAvailable &&
        sites.size()>=25,
        "install every received CALL thunk on a relocated image and publish both independent capabilities");
    if (!installed) {ShutdownBattleStats();VirtualFree(scratch,0,MEM_RELEASE);VirtualFree(mapped,0,MEM_RELEASE);return;}
    std::vector<std::array<BYTE,10>> owned(sites.size());
    for(size_t i=0;i<sites.size();++i)memcpy(owned[i].data(),reinterpret_cast<void*>(sites[i]),i==5 ? 10 : 5);
    auto nativeTarget=[&](uintptr_t address,unsigned cleanup) {
        ReceivedCode code{reinterpret_cast<BYTE*>(address)};
        code.Store(0,reinterpret_cast<uintptr_t>(&receivedInputEax));
        code.Store(1,reinterpret_cast<uintptr_t>(&receivedInputEcx));
        code.Store(2,reinterpret_cast<uintptr_t>(&receivedInputEdx));
        code.Byte(0x9C);code.Byte(0x8F);code.Byte(0x05);code.Dword(reinterpret_cast<uintptr_t>(&receivedInputFlags));
        code.Byte(0x8B);code.Byte(0x04);code.Byte(0x24);code.Store(0,reinterpret_cast<uintptr_t>(&receivedNativeReturn));
        code.Byte(0xFF);code.Byte(0x05);code.Dword(reinterpret_cast<uintptr_t>(&receivedNativeCalls));
        code.Byte(0xA1);code.Dword(reinterpret_cast<uintptr_t>(&receivedNativeResult));
        code.Mov(1,0x22334455);code.Mov(2,0x66778899);code.Byte(0xF9);
        if(cleanup){code.Byte(0xC2);code.Byte(static_cast<BYTE>(cleanup));code.Byte(0);}else code.Byte(0xC3);
    };
    nativeTarget(at(0x4061F9),0);nativeTarget(at(0x40658F),0);nativeTarget(at(0x44DE9E),4);
    ReceivedCode rng{reinterpret_cast<BYTE*>(at(0x47FB13))};
    rng.Byte(0xFF);rng.Byte(0x05);rng.Dword(reinterpret_cast<uintptr_t>(&receivedRngCalls));
    rng.Byte(0xA1);rng.Dword(reinterpret_cast<uintptr_t>(&receivedSubtype));rng.Byte(0xC2);rng.Byte(4);rng.Byte(0);
    std::array<BYTE,0x1000> context={};
    const uintptr_t ctx=reinterpret_cast<uintptr_t>(context.data());
    auto setupContext=[&](bool strategy,BYTE target=7,BYTE actor=100) {
        context.fill(0);context[strategy ? 1 : 0]=actor;context[strategy ? 12 : 1]=target;
        context[strategy ? 13 : 16]=target;context[strategy ? 14 : 17]=0xFF;
        context[0x400]=0x40;PutPointer(context.data()+(strategy ? 8 : 12),units+actor*0x30);
    };
    uintptr_t expectedReturn=0;ReceivedRegisterResult result={};
    // All native CALL addresses remain production patched. Stubs replace only
    // their original target bodies and the unowned continuation instructions.
    auto run=[&](bool strategy,unsigned role,bool repeat=false,unsigned callerMode=0) {
        memset(scratch,0x90,8192);
        const uintptr_t driver=reinterpret_cast<uintptr_t>(scratch);
        const uintptr_t actionBody=driver+128,ownerBody=driver+256,outerBody=driver+512;
        const uintptr_t ownerEnd=driver+768,actionEnd=driver+800,afterRng=driver+832;
        const uintptr_t callerSite=callerMode==2 ? driver+1600 :
            at(strategy ? (v66 ? 0x4179DA : 0x4179D8) :
                (callerMode==1 ? (v66 ? 0x4179A9 : 0x4179A7) : 0x406545));
        const uintptr_t actionEntry=at(strategy ? 0x425627 : 0x4527FE);
        uintptr_t ownerSite=at(0x452802),ownerEntry=at(0x405DE1),getter=at(0x405E37);
        int local=12;
        if(strategy) {
            if(role==1){ownerSite=at(v66 ? 0x4256BC : 0x4256DC);ownerEntry=at(v66 ? 0x423198 : 0x42314D);getter=at(v66 ? 0x4235B6 : 0x42318C);local=v66 ? 4 : 8;}
            if(role==2){ownerSite=at(0x42575D);ownerEntry=at(v66 ? 0x4231A8 : 0x42356E);getter=at(v66 ? 0x4235B6 : 0x4235BB);local=4;}
            if(role==3){ownerSite=at(0x425737);ownerEntry=at(v66 ? 0x423230 : 0x42322C);getter=at(v66 ? 0x42326D : 0x42326E);local=8;}
            if(role==4){ownerSite=at(0x425786);ownerEntry=at(v66 ? 0x423AA1 : 0x423A9A);getter=at(v66 ? 0x423ACF : 0x423AD2);local=4;}
            if(role==5){ownerSite=at(v66 ? 0x4256C9 : 0x4256E9);ownerEntry=at(0x42427B);getter=at(v66 ? 0x424323 : 0x424344);local=12;}
            if(role==6){ownerSite=at(0x425767);ownerEntry=at(v66 ? 0x4243C6 : 0x4243E4);getter=at(v66 ? 0x4244CC : (v64 ? 0x4244EF : 0x4244EA));local=8;}
            if(role==7){ownerSite=at(0x42595D);ownerEntry=at(v66 ? 0x4244E5 : (v64 ? 0x424505 : 0x424503));getter=at(v66 ? 0x424623 : 0x424624);local=4;}
        }
        ReceivedCode d{scratch};d.Mov(1,ctx);d.Mov(0,0x12345678);d.Byte(0xF9);d.Jump(callerSite);
        ReceivedCode call{reinterpret_cast<BYTE*>(callerSite)};call.Call(actionEntry);call.Byte(0xC3);
        if(strategy) {
            ReceivedCode actionJump{reinterpret_cast<BYTE*>(actionEntry)};actionJump.Jump(actionBody);
            ReceivedCode action{reinterpret_cast<BYTE*>(actionBody)};action.Prolog();action.Local(0xFC,ctx);action.Mov(1,ctx);action.Mov(0,0x12345678);action.Byte(0xF9);action.Jump(ownerSite);
        }
        ReceivedCode actionPost{reinterpret_cast<BYTE*>(ownerSite+5)};actionPost.Jump(actionEnd);
        ReceivedCode actionReturn{reinterpret_cast<BYTE*>(actionEnd)};actionReturn.Epilog();
        if(strategy && v66 && (role==1 || role==2)) {
            ReceivedCode outerJump{reinterpret_cast<BYTE*>(ownerEntry)};outerJump.Jump(outerBody);
            ReceivedCode outer{reinterpret_cast<BYTE*>(outerBody)};outer.Prolog();outer.Local(0xFC,ctx);outer.Mov(1,ctx);outer.Jump(at(0x4231E5));
            ReceivedCode outerPost{reinterpret_cast<BYTE*>(at(0x4231EA))};outerPost.Epilog();
            ownerEntry=at(0x423594);
        }
        ReceivedCode ownerJump{reinterpret_cast<BYTE*>(ownerEntry)};ownerJump.Jump(ownerBody);
        ReceivedCode owner{reinterpret_cast<BYTE*>(ownerBody)};
        owner.Store(0,reinterpret_cast<uintptr_t>(&receivedOwnerEax));owner.Prolog();owner.Local(static_cast<BYTE>(-local),ctx);
        owner.Call(reinterpret_cast<uintptr_t>(&ReceivedLifecycleBoundary));
        owner.Local(0xFC,local==4 ? ctx : 0);
        if(local!=4)owner.Local(static_cast<BYTE>(-local),ctx);
        const BYTE actualTarget=context[strategy ? 12 : 1];
        if(role==7){owner.Local(static_cast<BYTE>(v64 ? -12 : -8),units+actualTarget*0x30);owner.Local(static_cast<BYTE>(v64 ? -8 : -24),0);}
        owner.Mov(1,actualTarget);owner.Mov(0,0x11223344);owner.Mov(2,0x55667788);owner.Byte(0xF9);
        if(strategy && role==6) {
            const uintptr_t rngSite=at(v66 ? 0x42442F : (v64 ? 0x424452 : 0x42444D));
            owner.Byte(0x6A);owner.Byte(8);owner.Jump(rngSite);
            ReceivedCode rngPost{reinterpret_cast<BYTE*>(rngSite+5)};rngPost.Jump(afterRng);
            ReceivedCode after{reinterpret_cast<BYTE*>(afterRng)};after.Byte(0x89);after.Byte(0x45);after.Byte(0xF4);
            after.Mov(1,ctx);after.Byte(0x6A);after.Byte(0);after.Jump(getter);
        } else if(strategy && role==7) {
            owner.Byte(0x83);owner.Byte(0x3D);owner.Dword(reinterpret_cast<uintptr_t>(&receivedSkip));owner.Byte(0);
            owner.Byte(0x0F);owner.Byte(0x85);owner.Dword(ownerEnd-reinterpret_cast<uintptr_t>(owner.cursor)-4);
            owner.Jump(at(v66 ? 0x42461D : 0x42461E));
        } else owner.Jump(getter);
        ReceivedCode post{reinterpret_cast<BYTE*>(getter+5)};post.Jump(ownerEnd);
        ReceivedCode end{reinterpret_cast<BYTE*>(ownerEnd)};end.Epilog();
        expectedReturn=getter+5;
        FlushInstructionCache(GetCurrentProcess(),mapped,image.size());FlushInstructionCache(GetCurrentProcess(),scratch,8192);
        result=InvokeReceivedDriver(driver);
        if(repeat)InvokeReceivedDriver(driver);
    };
    setupContext(false);receivedLifecycleMode=0;receivedNativeCalls=0;
    run(false,0);
    Check(ReceivedCount(false)==1 && receivedOwnerEax==0x12345678 && receivedInputEax==0x11223344 &&
        receivedInputEcx==7 && receivedInputEdx==0x55667788 && (receivedInputFlags&1) &&
        receivedNativeReturn==expectedReturn && receivedNativeCalls==1 && result.eax==0x76543210 &&
        result.ecx==0x22334455 && result.edx==0x66778899 && (result.flags&1),
        "installed physical owner and target thunks preserve EAX ECX EDX EFLAGS stack and original native return exactly once");
    run(false,0,true);
    Check(ReceivedCount(false)==3,"ordinary attack counts one and a subsequent native double attack adds two for the same target");
    const long long beforeTriple=ReceivedCount(false);run(false,0);run(false,0,true);
    Check(ReceivedCount(false)==beforeTriple+3,"three independent native physical segments count a triple attack as three");
    const long long beforeCounter=ReceivedCount(false);run(false,0,false,1);
    setupContext(false,100,7);run(false,0,false,1);
    Check(ReceivedCount(false)==beforeCounter+1 && ReceivedCount(false,100)==0,
        "the second native physical parent counts enemy counterattacks and excludes the reversed friendly-to-enemy direction");
    setupContext(false);
    const long long beforeInvalidPhysical=ReceivedCount(false);
    run(false,0,false,2);setupContext(false,0xFF);run(false,0);
    setupContext(false,7,0xFF);run(false,0);setupContext(false,7,2);run(false,0);
    setupContext(false);PutPointer(context.data()+12,units+101*0x30);run(false,0);setupContext(false);
    Check(ReceivedCount(false)==beforeInvalidPhysical,
        "untrusted physical preview parents illegal slots friendly attackers and mismatched actor pointers never count");
    const long long beforeZero=ReceivedCount(false);
    receivedNativeResult=0;run(false,0,true);receivedNativeResult=0x76543210;
    Check(ReceivedCount(false)==beforeZero+2,
        "physical received does not depend on a positive native helper result and counts zero-result target iterations");
    setupContext(false,8);run(false,0);
    Check(ReceivedCount(false,8)==1 && ReceivedCount(false)==beforeZero+2,"physical guard or range target is attributed to the actual target-list slot");
    setupContext(false);receivedLifecycleMode=1;run(false,0);receivedLifecycleMode=0;
    Check(ReceivedCount(false)==beforeZero+2,"load freeze and thaw invalidates a physical owner before its pending target commit");
    setupContext(true);run(true,2);
    Check(ReceivedCount(true)==1,"one merged damage plus state target iteration counts once through the installed result owner");
    run(true,2,true);
    Check(ReceivedCount(true)==3,"independent repeated strategy passes count separately for the same target");
    for(unsigned role:{1u,3u,4u,5u})run(true,role);
    Check(ReceivedCount(true)==7,"damage status debuff special harmful and all-target native result owners count their actual target iterations");
    const long long beforeInvalidStrategy=ReceivedCount(true);
    run(true,2,false,2);setupContext(true,0xFF);run(true,2);
    setupContext(true,7,0xFF);run(true,2);setupContext(true,7,2);run(true,2);
    setupContext(true,100);run(true,2);setupContext(true);
    Check(ReceivedCount(true)==beforeInvalidStrategy && ReceivedCount(true,100)==0,
        "untrusted strategy preview parents illegal slots friendly attackers and enemy defenders never count");
    receivedNativeResult=0;run(true,2);receivedNativeResult=0x76543210;
    Check(ReceivedCount(true)==beforeInvalidStrategy+1,
        "strategy received counts a zero-result actual target iteration without damage or status helper inference");
    const long long beforeSupport=ReceivedCount(true);context[0x400]=0x59;run(true,4);
    for(BYTE effect:{BYTE(0x50),BYTE(0x51),BYTE(0x52),BYTE(0x57),BYTE(0x5A)}){context[0x400]=effect;run(true,3);}
    Check(ReceivedCount(true)==beforeSupport,"healing buffs re-action and cleanse planned effects are excluded before outcome");
    context[0x400]=0x40;receivedLifecycleMode=1;run(true,2);receivedLifecycleMode=0;
    Check(ReceivedCount(true)==beforeSupport,"load generation invalidates an already-entered strategy owner before its target commit");
    receivedRngCalls=0;const long long beforeRandom=ReceivedCount(true);
    for(receivedSubtype=0;receivedSubtype<8;++receivedSubtype)run(true,6);
    Check(receivedRngCalls==8 && ReceivedCount(true)==beforeRandom+6,
        "common random apply observer counts native subtypes zero through five and excludes buff and heal without an extra RNG call");
    const long long beforeScatter=ReceivedCount(true);receivedSkip=1;run(true,7);receivedSkip=0;run(true,7,true);
    Check(ReceivedCount(true)==beforeScatter+2,
        "scatter excludes rejected candidates and counts every actual consecutive strike on the same target");
    BattleStatsPersistedState saturated;
    CaptureBattleStatsState(saturated,GetTickCount64());
    for(auto* ledger:{&saturated.units,&saturated.campaignUnits})for(auto& unit:*ledger)if(unit.slot==7) {
        unit.physicalReceivedCount=0x7FFFFFFFFFFFFFFELL;unit.strategyReceivedCount=0x7FFFFFFFFFFFFFFELL;
    }
    saturated.knownMetrics|=BattleStatsPhysicalReceived|BattleStatsStrategyReceived;
    Check(RestoreBattleStatsState(saturated,GetTickCount64()),"restore near-saturated received counters in both ledgers");
    setupContext(false);run(false,0,true);setupContext(true);run(true,2,true);
    BattleStatsSnapshot saturatedResult;ReadBattleStatsSnapshot(saturatedResult);
    const BattleUnitTotals* campaignReceived=FindPerson(saturatedResult.campaignUnits,1007);
    Check(ReceivedCount(false)==0x7FFFFFFFFFFFFFFFLL && ReceivedCount(true)==0x7FFFFFFFFFFFFFFFLL &&
        campaignReceived && campaignReceived->physicalReceivedCount==0x7FFFFFFFFFFFFFFFLL &&
        campaignReceived->strategyReceivedCount==0x7FFFFFFFFFFFFFFFLL,
        "installed received bridges saturate current and campaign counters without overflow");
    receivedLifecycleMode=3;setupContext(true);run(true,2);receivedLifecycleMode=0;
    Check(ReceivedCount(false)==0 && ReceivedCount(true)==0,
        "new battlefield generation rejects the previous native owner while preserving the separate campaign ledger");
    setupContext(false);run(false,0);
    receivedLifecycleMode=4;run(false,0);receivedLifecycleMode=0;
    Check(ReceivedCount(false)==0 && ReceivedCount(true)==0,
        "an interrupted load clears the replaced timeline and invalidates its pending native target commit");
    receivedLifecycleMode=2;setupContext(false);run(false,0);receivedLifecycleMode=0;
    Check(ReceivedCount(false)==0 && ReceivedCount(true)==0,"explicit reset invalidates pending native owner scopes instead of crediting the new ledger");
    // Restore every production-owned instruction before asking its normal
    // conflict-aware shutdown to release thunks. Only the harness changed bodies.
    for(size_t i=0;i<sites.size();++i)memcpy(reinterpret_cast<void*>(sites[i]),owned[i].data(),i==5 ? 10 : 5);
    ShutdownBattleStats();
    Check(!BattleStatsRecoveryPendingForTest(),"received bridge scopes detach with all original targets and owned CALLs recoverable");
    VirtualFree(scratch,0,MEM_RELEASE);VirtualFree(mapped,0,MEM_RELEASE);
}
