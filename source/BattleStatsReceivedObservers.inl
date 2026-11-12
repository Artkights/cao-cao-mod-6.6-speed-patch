// Immediate target commits are stamped by the exact native owner invocation.
// No native return slot is replaced; a reset invalidates every retained stamp.
struct ReceivedScope {
    uintptr_t frame=0,context=0,parent=0,returnAddress=0;
    uint64_t generation=0,serial=0;
    ReceivedRole role=ReceivedPhysical;
    bool genuine=false;
};
__declspec(thread) ReceivedScope gReceivedScopes[64];
__declspec(thread) unsigned gReceivedScopeCursor=0;
__declspec(thread) uint64_t gReceivedSerial=0;

bool NativeReceivedCaller(uintptr_t frame,uintptr_t& caller) {
    if (!FrameWord(frame,4,caller)) return false;
    uintptr_t original=0;
    if (ResolveNativeAnimationReturn(frame+sizeof(uintptr_t),caller,original)) caller=original;
    return true;
}
bool GenuineReceivedAction(uintptr_t frame,uintptr_t context,bool strategy) {
    const ReceivedLayout& layout=gLayout.received;
    if (!strategy) {
        uintptr_t caller=0,ownerContext=0;
        return NativeReceivedCaller(frame,caller) &&
            std::find(layout.physicalActions.begin(),layout.physicalActions.end(),caller)!=layout.physicalActions.end() &&
            FrameWord(frame,-4,ownerContext) && ownerContext==context;
    }
    for (unsigned depth=0;depth<12;++depth) {
        uintptr_t caller=0,parent=0,ownerContext=0;
        if (!NativeReceivedCaller(frame,caller)) return false;
        if (std::find(layout.strategyActions.begin(),layout.strategyActions.end(),caller)!=layout.strategyActions.end())
            return FrameWord(frame,-4,ownerContext) && ownerContext==context;
        if (!FrameWord(frame,0,parent) || parent<=frame || parent-frame>=0x10000) return false;
        frame=parent;
    }
    return false;
}
void BeginReceivedScope(const NativeHitSnapshot& snapshot,const ReceivedCall& call) {
    ReceivedScope scope;
    // This is ESP after PUSHFD at native entry, hence the EBP that the
    // original owner's PUSH EBP / MOV EBP,ESP is about to establish.
    scope.frame=snapshot.esp;
    scope.context=snapshot.ecx;scope.parent=snapshot.ebp;
    scope.returnAddress=snapshot.returnAddress;scope.role=call.role;
    scope.serial=++gReceivedSerial;
    scope.genuine=GenuineReceivedAction(snapshot.ebp,snapshot.ecx,call.role!=ReceivedPhysical);
    { ScopedStatsLock lock(false);scope.generation=gDodgeGeneration;
      scope.genuine=scope.genuine && gActive && !gLoading; }
    gReceivedScopes[gReceivedScopeCursor++%64]=scope;
}
const ReceivedScope* FindReceivedScope(uintptr_t frame,uintptr_t context,ReceivedRole role) {
    uintptr_t parent=0,caller=0;
    if (!FrameWord(frame,0,parent) || !NativeReceivedCaller(frame,caller)) return nullptr;
    for (unsigned i=0;i<64;++i) {
        const ReceivedScope& scope=gReceivedScopes[(gReceivedScopeCursor-1-i)%64];
        if (scope.frame==frame && scope.context==context && scope.role==role &&
            scope.parent==parent && scope.returnAddress==caller) return &scope;
    }
    return nullptr;
}
void RecordReceivedAttempt(bool strategy,int actorSlot,int targetSlot,uint64_t generation) {
    ScopedStatsLock lock;
    if (!gLoading) {
        ObserveSlotCampUnlocked(actorSlot);
        ObserveSlotCampUnlocked(targetSlot);
    }
    int actorCamp=-1,targetCamp=-1;
    if (!gActive || gLoading || generation!=gDodgeGeneration ||
        !(strategy ? gStrategyReceivedAvailable : gPhysicalReceivedAvailable) ||
        !UnitCamp(actorSlot,actorCamp) || actorCamp<2 ||
        !UnitCamp(targetSlot,targetCamp) || targetCamp!=0) return;
    AddMetricUnlocked(targetSlot,strategy ? &BattleUnitTotals::strategyReceivedCount :
        &BattleUnitTotals::physicalReceivedCount,1,strategy ? BattleStatsStrategyReceived : BattleStatsPhysicalReceived);
}
void CommitReceivedTarget(const NativeHitSnapshot& snapshot,const ReceivedCall& call) {
    const bool strategy=call.role!=ReceivedPhysical;
    uintptr_t context=0,actor=0;
    BYTE actorSlot=0xFF,targetSlot=0xFF,currentTarget=0xFF,effect=0;
    int actorIndex=-1;
    if (!FrameWord(snapshot.ebp,-call.contextLocal,context)) return;
    const ReceivedScope* scope=FindReceivedScope(snapshot.ebp,context,call.role);
    if (!scope || !scope->genuine || !scope->serial ||
        !GenuineReceivedAction(scope->parent,context,strategy)) return;
    if (!ReadBytes(context+(strategy ? 1 : 0),&actorSlot,1) ||
        !ReadBytes(context+(strategy ? 8 : 12),&actor,sizeof(actor)) ||
        !UnitSlotFromPointer(actor,actorIndex) || actorIndex!=actorSlot ||
        !ReadBytes(context+(strategy ? 12 : 1),&targetSlot,1) || targetSlot>=kUnitCount) return;
    if (!strategy) {
        BYTE ordinal=0xFF;
        if (!ReadBytes(snapshot.ebp-4,&ordinal,1) || ordinal>=116 ||
            !ReadBytes(context+0x10+ordinal,&currentTarget,1) || currentTarget!=targetSlot ||
            static_cast<BYTE>(snapshot.ecx)!=targetSlot) return;
    } else if (call.role==ReceivedScatter) {
        uintptr_t target=0;int resolvedTarget=-1;
        const int targetLocal=CompiledEngineProfile()==EngineProfileId::Ccz64 ? 12 : 8;
        if (!FrameWord(snapshot.ebp,-targetLocal,target) ||
            !UnitSlotFromPointer(target,resolvedTarget) || resolvedTarget!=targetSlot ||
            !ReadBytes(context+0x0D,&currentTarget,1) || currentTarget!=targetSlot) return;
    } else if (call.role!=ReceivedRandom && static_cast<BYTE>(snapshot.ecx)!=targetSlot) return;
    if (strategy) {
        if (call.role==ReceivedRandom) {
            uintptr_t subtype=0;
            if (!FrameWord(snapshot.ebp,-12,subtype) || subtype>5) return;
        } else if (!ReadBytes(context+0x400,&effect,1) || effect<0x40 || effect>0x4C) return;
    }
    // Every patched getter is reached once per actual target iteration. The
    // merged damage/status result shares one getter; independent later passes
    // and scatter strikes execute it again and deliberately count again.
    RecordReceivedAttempt(strategy,actorIndex,targetSlot,scope->generation);
}
extern "C" void __cdecl BattleStatsObservePhysicalEntry(NativeHitSnapshot* snapshot) {
    if (InterlockedCompareExchange(&gStatsDetaching,FALSE,FALSE)!=FALSE || !gLayout.received.physical) return;
    const ReceivedCall call={snapshot->returnAddress-5,0,ReceivedPhysical,true,12,4};
    try { BeginReceivedScope(*snapshot,call); } catch (...) {}
}
extern "C" void __cdecl BattleStatsObserveReceivedSite(NativeHitSnapshot* snapshot) {
    InterlockedIncrement(&gNativeHitActive);
    if (InterlockedCompareExchange(&gStatsDetaching,FALSE,FALSE)==FALSE) {
        const uintptr_t site=snapshot->args[0]-5;
        const ReceivedCall* selected=nullptr;
        const auto found=std::find_if(gLayout.received.calls.begin(),gLayout.received.calls.end(),
            [site,snapshot](const ReceivedCall& call){ return call.site==site && call.target==snapshot->returnAddress; });
        if (found!=gLayout.received.calls.end()) selected=&*found;
        if (selected) {
            NativeHitSnapshot native=*snapshot;
            // The thunk pushed the native tail target above the game's return.
            // The static bridge RETs directly there, including while detaching;
            // it never executes a freed thunk after its observer count drops.
            native.esp+=4;native.returnAddress=snapshot->args[0];
            for (unsigned i=0;i<3;++i) native.args[i]=snapshot->args[i+1];
            try { if (selected->entry) BeginReceivedScope(native,*selected);else CommitReceivedTarget(native,*selected); }
            catch (...) {} // Never unwind into a native game CALL.
        }
    }
    InterlockedDecrement(&gNativeHitActive);
}
