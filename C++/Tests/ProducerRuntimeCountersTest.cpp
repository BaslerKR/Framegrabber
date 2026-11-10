#include "FramegrabberRuntimeDiagnostics.h"
#include <iostream>

int main()
{
    FramegrabberDetail::ProducerRuntimeCounters counters;
    if (counters.delivered() != 0 || counters.failed() != 0) return 1;
    counters.noteFrameDelivered(false); // No registered consumer, or all callbacks failed.
    if (counters.delivered() != 0) return 2;
    counters.noteFailure();
    counters.noteFrameDelivered(false);
    if (counters.delivered() != 0 || counters.failed() != 1) return 3;
    counters.noteFrameDelivered(true); // Multiple successful consumers still make one frame.
    if (counters.delivered() != 1 || counters.failed() != 1) return 4;
    counters.noteFailure();
    counters.noteFrameDelivered(true); // A sibling callback can succeed despite another failing.
    if (counters.delivered() != 2 || counters.failed() != 2) return 5;

    using WaitOutcome = FramegrabberDetail::ProducerRuntimeCounters::WaitOutcome;
    // These are the actions consumed by the real DMA loop, not a separate test classifier.
    if (counters.noteWaitResult(FG_NO_PICTURE_AVAILABLE, false) != WaitOutcome::Idle) return 12;
    if (counters.noteWaitResult(FG_TIMEOUT_ERR, false) != WaitOutcome::Idle) return 13;
    if (counters.noteWaitResult(42, false) != WaitOutcome::Frame) return 14;
    if (counters.noteWaitResult(-9999, true) != WaitOutcome::Ended) return 15;
    if (counters.failed() != 2) return 6;
    if (counters.noteWaitResult(-9999, false) != WaitOutcome::Ended) return 16;
    if (counters.failed() != 3) return 7;

    Framegrabber::ChannelStatistics first;
    first.dmaIndex = 0; first.delivered = 100;
    first.consumerDelivered = counters.delivered(); first.failed = counters.failed(); first.rejected = 4;
    first.skipped = 1000; first.overwritten = 2000; first.poolExhausted = 3000;
    Framegrabber::ChannelStatistics second;
    second.dmaIndex = 1; second.consumerDelivered = 5; second.failed = 6; second.rejected = 7;
    const std::vector<Framegrabber::ChannelStatistics> channels{first, second};
    const auto all = FramegrabberDetail::sumRuntimeDiagnostics(true, channels);
    if (!all.grabbing || all.delivered != 7 || all.failed != 20) return 8;
    const auto dma = FramegrabberDetail::sumRuntimeDiagnostics(false, channels, 1);
    if (dma.grabbing || dma.delivered != 5 || dma.failed != 13) return 9;
    const auto absent = FramegrabberDetail::sumRuntimeDiagnostics(false, channels, 9);
    if (absent.delivered != 0 || absent.failed != 0) return 10;
    const FramegrabberDetail::ProducerRuntimeCounters nextRun;
    if (nextRun.delivered() != 0 || nextRun.failed() != 0) return 11;
    std::cout << "Producer delivery, failure, idle-poll and per-DMA aggregation contracts passed.\n";
}
