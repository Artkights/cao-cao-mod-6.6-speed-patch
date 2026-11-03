// A tail-call bridge retains every native return identity. Original getters,
// RNG and result handlers execute once, with unchanged registers and EFLAGS.
extern "C" __declspec(naked) void BattleStatsReceivedBridge() {
    __asm {
        pushfd
        pushad
        mov eax,esp
        push eax
        call BattleStatsObserveReceivedSite
        add esp,4
        popad
        popfd
        ret
    }
}
void* PrepareReceivedThunk(BYTE* site,uintptr_t target,CodePatch& patch) {
    // In the x86 process rel32 arithmetic is modulo 2^32, so every 32-bit
    // allocation can reach the bridge and the game CALL can reach this thunk.
    BYTE* thunk=static_cast<BYTE*>(VirtualAlloc(nullptr,16,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    if (!thunk) return nullptr;
    // Never return to this allocation after entering the static bridge. An
    // idle shutdown may release it as soon as its observer count reaches zero.
    // Its saved native target is a tail-return address above the game's own
    // return and arguments, preserving every GPR and EFLAGS even while detaching.
    thunk[0]=0x68;const uint32_t absolute=static_cast<uint32_t>(target);memcpy(thunk+1,&absolute,4);
    thunk[5]=0xE9;
    const uint32_t callDisp=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&BattleStatsReceivedBridge)-
        reinterpret_cast<uintptr_t>(thunk+10));
    memcpy(thunk+6,&callDisp,4);
    if (!FlushInstructionCache(GetCurrentProcess(),thunk,10)) { VirtualFree(thunk,0,MEM_RELEASE);return nullptr; }
    patch.thunk=thunk;(void)site;return thunk;
}
bool PrepareReceivedCallPatch(const PeImage& image,const ReceivedCall& call,CodePatch& patch) {
    BYTE* site=reinterpret_cast<BYTE*>(call.site);
    if (!site || site[0]!=0xE8 || CallTarget(image,site)!=call.target) return false;
    patch=CodePatch();patch.site=site;patch.length=5;memcpy(patch.original.data(),site,5);
    if (!PreparePatchMemory(patch)) return false;
    void* thunk=PrepareReceivedThunk(site,call.target,patch);
    if (!thunk) return false;
    patch.patched[0]=0xE8;
    const int32_t displacement=static_cast<int32_t>(reinterpret_cast<BYTE*>(thunk)-(site+5));
    memcpy(patch.patched.data()+1,&displacement,4);return true;
}
bool PrepareReceivedObservers(const PeImage& image,const ReceivedLayout& resolved,RuntimeLayout& pending) {
    pending.received=resolved;
    for (const auto& call:resolved.calls) {
        CodePatch patch;
        if (!PrepareReceivedCallPatch(image,call,patch)) {
            if (call.role==ReceivedPhysical) pending.received.physical=false;else pending.received.strategy=false;
            continue;
        }
        pending.receivedPatches.push_back(patch);
    }
    // A failed site marks its class unavailable above; prepared patches in that
    // class are discarded so no partial capability can be published.
    for (size_t i=pending.receivedPatches.size();i--;) {
        const uintptr_t site=reinterpret_cast<uintptr_t>(pending.receivedPatches[i].site);
        const auto found=std::find_if(resolved.calls.begin(),resolved.calls.end(),
            [site](const ReceivedCall& call) { return call.site==site; });
        if (found==resolved.calls.end() || (found->role==ReceivedPhysical ? !pending.received.physical : !pending.received.strategy)) {
            if (pending.receivedPatches[i].thunk) VirtualFree(pending.receivedPatches[i].thunk,0,MEM_RELEASE);
            pending.receivedPatches.erase(pending.receivedPatches.begin()+i);
        }
    }
    return pending.received.physical || pending.received.strategy;
}
