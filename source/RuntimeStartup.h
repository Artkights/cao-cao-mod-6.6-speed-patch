#pragma once

#include <windows.h>
#include <cstdint>

// Packed/protected and version-selected builds receive their known early
// limitations once in the installer report. Startup must not repeat that
// acknowledgement; ordinary installs keep the existing partial notice.
inline bool ShouldShowStartupReadinessNotice(bool noticeHandledAtInstall,
    unsigned readyFeatures, unsigned partialFeatures, unsigned requestedFeatures) {
    return !noticeHandledAtInstall &&
        (partialFeatures != 0 || readyFeatures != requestedFeatures);
}

enum class RuntimeStartupStage {
    WaitingForImage, ImportsReady, LayoutReady, FeaturesReady, Partial, Failed, TimedOut
};

struct RuntimeStartupObservation {
    bool imageReady = false;
    bool importsReady = false;
    bool layoutReady = false;
    bool rejected = false;
    uint64_t importFingerprint = 0;
    uint64_t layoutFingerprint = 0;
    unsigned availableFeatures = 0;
};

// Pure state machine: the worker owns scheduling and I/O. Observations must
// describe fresh runtime bytes; no game thread waits for this tracker.
class RuntimeStartupTracker {
public:
    static constexpr ULONGLONG PollIntervalMs = 100;
    static constexpr ULONGLONG TimeoutMs = 15000;

    explicit RuntimeStartupTracker(ULONGLONG started, unsigned requiredFeatures)
        : started_(started), required_(requiredFeatures) {}

    bool ShouldPoll(ULONGLONG now) const noexcept {
        return !Finished() && (!observed_ || now - lastObserved_ >= PollIntervalMs);
    }
    RuntimeStartupStage Observe(ULONGLONG now,
        const RuntimeStartupObservation& value) noexcept {
        if (Finished()) return stage_;
        if (value.rejected) {
            stableFeatures_ = 0;
            stage_ = RuntimeStartupStage::Failed;
            return stage_;
        }
        const bool timedOut = now - started_ >= TimeoutMs;
        const bool sampleDue = ShouldPoll(now);
        // Even at the deadline the caller supplies fresh bytes. Never retain
        // a previously stable capability after its last observation changed.
        if (!sampleDue && !timedOut) return stage_;
        const bool same = observed_ && value.imageReady && value.importsReady &&
            value.layoutReady && previous_.imageReady && previous_.importsReady &&
            previous_.layoutReady && value.importFingerprint == previous_.importFingerprint &&
            value.layoutFingerprint == previous_.layoutFingerprint &&
            value.availableFeatures == previous_.availableFeatures;
        stable_ = same ? stable_ + (sampleDue ? 1u : 0u) : 1;
        previous_ = value;
        observed_ = true;
        lastObserved_ = now;
        stableFeatures_ = same && stable_ >= 2 ? value.availableFeatures : 0;
        if (timedOut) {
            stage_ = stableFeatures_ && (stableFeatures_ & required_) == required_ ?
                RuntimeStartupStage::FeaturesReady : stableFeatures_ ?
                RuntimeStartupStage::Partial : RuntimeStartupStage::TimedOut;
            return stage_;
        }
        if (!value.imageReady) stage_ = RuntimeStartupStage::WaitingForImage;
        else if (!value.importsReady) stage_ = RuntimeStartupStage::WaitingForImage;
        else if (!value.layoutReady) stage_ = RuntimeStartupStage::ImportsReady;
        else if (stable_ < 2 || (stableFeatures_ & required_) != required_)
            stage_ = RuntimeStartupStage::LayoutReady;
        else stage_ = RuntimeStartupStage::FeaturesReady;
        return stage_;
    }
    RuntimeStartupStage Stage() const noexcept { return stage_; }
    unsigned ReadyFeatures() const noexcept { return stableFeatures_; }
    bool FeatureReady(unsigned mask) const noexcept {
        return mask != 0 && (stableFeatures_ & mask) == mask;
    }
    bool Finished() const noexcept {
        return stage_ == RuntimeStartupStage::FeaturesReady || stage_ == RuntimeStartupStage::Partial ||
            stage_ == RuntimeStartupStage::Failed || stage_ == RuntimeStartupStage::TimedOut;
    }
private:
    ULONGLONG started_ = 0;
    ULONGLONG lastObserved_ = 0;
    unsigned required_ = 0;
    unsigned stableFeatures_ = 0;
    unsigned stable_ = 0;
    bool observed_ = false;
    RuntimeStartupStage stage_ = RuntimeStartupStage::WaitingForImage;
    RuntimeStartupObservation previous_;
};
