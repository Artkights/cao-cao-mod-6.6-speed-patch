#include "RuntimeStartup.h"
#include <cstdio>

namespace {
unsigned failures = 0;
void Check(bool value, const char* label) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", label);
    if (!value) ++failures;
}
RuntimeStartupObservation Ready(uint64_t importId, uint64_t layoutId, unsigned features) {
    RuntimeStartupObservation value;
    value.imageReady = value.importsReady = value.layoutReady = true;
    value.importFingerprint = importId;
    value.layoutFingerprint = layoutId;
    value.availableFeatures = features;
    return value;
}
}

int main() {
    constexpr unsigned accelerator = 1, guide = 2, stats = 4;
    RuntimeStartupTracker tracker(100, accelerator | guide | stats);
    Check(tracker.Stage() == RuntimeStartupStage::WaitingForImage && tracker.ShouldPoll(100),
        "startup begins by polling outside the loader lock");

    RuntimeStartupObservation waiting;
    Check(tracker.Observe(100, waiting) == RuntimeStartupStage::WaitingForImage,
        "unexpanded image remains pending");
    Check(!tracker.ShouldPoll(199) && tracker.ShouldPoll(200),
        "poll interval is bounded to 100 milliseconds");

    const auto twoFeatures = Ready(11, 21, accelerator | guide);
    Check(tracker.Observe(200, twoFeatures) == RuntimeStartupStage::LayoutReady,
        "first valid sample is not considered stable yet");
    Check(tracker.Observe(300, twoFeatures) == RuntimeStartupStage::LayoutReady &&
        tracker.ReadyFeatures() == (accelerator | guide),
        "two identical samples publish independently ready feature capabilities");
    Check(!tracker.FeatureReady(accelerator | stats) && tracker.FeatureReady(guide),
        "missing capability does not mask an independently ready feature");

    const auto changedEntry = Ready(12, 21, accelerator | guide);
    Check(tracker.Observe(400, changedEntry) == RuntimeStartupStage::LayoutReady &&
        tracker.ReadyFeatures() == 0,
        "changed import entry invalidates prior stability");
    Check(tracker.Observe(500, changedEntry) == RuntimeStartupStage::LayoutReady &&
        tracker.ReadyFeatures() == (accelerator | guide),
        "modified entry requires two fresh stable observations");

    RuntimeStartupTracker partial(1000, accelerator | guide | stats);
    partial.Observe(1000, twoFeatures);
    partial.Observe(1100, twoFeatures);
    Check(partial.Observe(16000, twoFeatures) == RuntimeStartupStage::Partial &&
        partial.Finished() && partial.ReadyFeatures() == (accelerator | guide),
        "15 second timeout retains stable partial feature capabilities");

    RuntimeStartupTracker changedAtDeadline(1000, accelerator | guide | stats);
    changedAtDeadline.Observe(1000, twoFeatures);
    changedAtDeadline.Observe(1100, twoFeatures);
    Check(changedAtDeadline.Observe(16000, changedEntry) == RuntimeStartupStage::TimedOut &&
        changedAtDeadline.ReadyFeatures() == 0,
        "deadline revalidates fresh entry bytes and discards stale ready capabilities");

    RuntimeStartupTracker inaccessibleAtDeadline(1000, accelerator | guide | stats);
    inaccessibleAtDeadline.Observe(1000, twoFeatures);
    inaccessibleAtDeadline.Observe(1100, twoFeatures);
    Check(inaccessibleAtDeadline.Observe(16000, waiting) == RuntimeStartupStage::TimedOut &&
        inaccessibleAtDeadline.ReadyFeatures() == 0,
        "deadline does not retain capabilities after their image or import pages disappear");

    RuntimeStartupTracker lateFirstReady(1000, accelerator | guide | stats);
    lateFirstReady.Observe(15950, twoFeatures);
    Check(lateFirstReady.Observe(16000, twoFeatures) == RuntimeStartupStage::TimedOut &&
        lateFirstReady.ReadyFeatures() == 0,
        "deadline cannot count observations less than 100 milliseconds apart as stable");

    RuntimeStartupTracker completeAtDeadline(1000, accelerator | guide);
    completeAtDeadline.Observe(15900, twoFeatures);
    Check(completeAtDeadline.Observe(16000, twoFeatures) == RuntimeStartupStage::FeaturesReady &&
        completeAtDeadline.ReadyFeatures() == (accelerator | guide),
        "a fully stable requested feature set at the deadline is Ready rather than Partial");

    RuntimeStartupTracker timedOut(20000, accelerator);
    Check(timedOut.Observe(35000, waiting) == RuntimeStartupStage::TimedOut &&
        timedOut.ReadyFeatures() == 0,
        "timeout with no validated feature fails closed");

    RuntimeStartupTracker ready(40000, accelerator);
    const auto complete = Ready(31, 41, accelerator | guide | stats);
    ready.Observe(40000, complete);
    Check(ready.Observe(40100, complete) == RuntimeStartupStage::FeaturesReady &&
        ready.FeatureReady(accelerator | guide | stats),
        "all requested capabilities complete after two stable samples");

    RuntimeStartupTracker rejected(50000, accelerator);
    RuntimeStartupObservation invalid;
    invalid.rejected = true;
    Check(rejected.Observe(50000, invalid) == RuntimeStartupStage::Failed && rejected.Finished(),
        "invalid runtime evidence terminates startup safely");
    Check(!ShouldShowStartupReadinessNotice(true, 1, 4, 7),
        "packed or version-selected installs do not repeat the readiness notice at startup");
    Check(ShouldShowStartupReadinessNotice(false, 1, 4, 7),
        "ordinary installs keep the partial-readiness startup notice");
    Check(!ShouldShowStartupReadinessNotice(false, 7, 0, 7),
        "fully ready ordinary installs show no startup notice");
    std::printf("RuntimeStartupTests: %u failures\n", failures);
    return failures ? 1 : 0;
}
