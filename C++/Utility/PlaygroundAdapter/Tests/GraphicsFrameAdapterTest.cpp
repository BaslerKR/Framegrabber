#include "FramegrabberGraphicsFrameAdapter.h"
#include "FramegrabberGraphicsFrameStream.h"
#include "FramegrabberImageLayout.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace
{

class TestCopyGate final : public FramegrabberDetail::CaptureCopyGate
{
public:
    explicit TestCopyGate(std::vector<std::uint8_t> source, const bool rejectOverwrite = false)
        : _source(std::move(source))
        , _rejectOverwrite(rejectOverwrite)
    {
    }

    bool copyAndVerify(std::uint8_t* destination,
                       const FramegrabberDetail::CaptureCopyPlan& plan) noexcept override
    {
        ++copyAttempts;
        lastPlan = plan;
        if (!destination || plan.destinationStride == 0 || plan.rows == 0
            || plan.rows > plan.capacity / plan.destinationStride)
        {
            return false;
        }
        // Same row-by-row, zero-filling copy as the DMA gate: only bytes that exist are read.
        FramegrabberDetail::copyPayloadRows(destination, plan.destinationStride, plan.rows,
            _source.data(), plan.sourceStride, _source.size());
        if (cancelOnCopy) cancelOnCopy->store(true);
        if (onCopy) onCopy();
        return !_rejectOverwrite;
    }

    std::atomic<bool>* cancelOnCopy = nullptr;
    std::function<void()> onCopy;
    int copyAttempts = 0;
    FramegrabberDetail::CaptureCopyPlan lastPlan;

private:
    std::vector<std::uint8_t> _source;
    bool _rejectOverwrite = false;
};

class RefusingSource final : public GraphicsFrameContextSource
{
public:
    explicit RefusingSource(GraphicsFrameError error) : error(error) {}
    GraphicsResult<GraphicsFrameContext> beginFrame(std::size_t bytes) noexcept override
    {
        ++calls;
        requested = bytes;
        if (error == GraphicsFrameError::None) return local.beginFrame(bytes);
        return error;
    }
    GraphicsFrameError error;
    GraphicsLocalFrameContextSource local;
    int calls = 0;
    std::size_t requested = 0;
};

class TrackedAllocation final : public GraphicsAllocation
{
public:
    explicit TrackedAllocation(std::atomic<int>& live, std::byte* data)
        : GraphicsAllocation(data, 64, GraphicsAllocationOrigin::Managed, 1,
                             GraphicsAllocationState::Writable, 42), live(live), data(data) { ++live; }
    ~TrackedAllocation() override { --live; delete[] data; }
    void destroy() noexcept override { delete this; }
    std::atomic<int>& live;
    std::byte* data;
};
class TrackedAllocator final : public GraphicsFrameAllocator
{
public:
    TrackedAllocator(GraphicsAllocationRef allocation, std::atomic<int>& contexts)
        : allocation(std::move(allocation)), contexts(contexts) { ++contexts; }
    ~TrackedAllocator() override { --contexts; }
    GraphicsResult<GraphicsAllocationSlice> allocate(std::size_t bytes, std::size_t alignment) noexcept override
    {
        if (bytes > 64 || reinterpret_cast<std::uintptr_t>(allocation->base()) % alignment) return GraphicsFrameError::Capacity;
        return GraphicsAllocationSlice{allocation, 0, bytes};
    }
    bool isManaged() const noexcept override { return true; }
    std::uint64_t domainId() const noexcept override { return 42; }
    GraphicsAllocationRef allocation;
    std::atomic<int>& contexts;
};
class TrackedContextSource final : public GraphicsFrameContextSource
{
public:
    std::atomic<int> liveAllocations{0}, contexts{0};
    GraphicsResult<GraphicsFrameContext> beginFrame(std::size_t) noexcept override
    {
        auto* data = new std::byte[64];
        auto backing = GraphicsAllocationRef::adoptNew(new TrackedAllocation(liveAllocations, data));
        return GraphicsFrameContext(std::make_shared<TrackedAllocator>(std::move(backing), contexts));
    }
};

} // namespace

int main()
{
    GraphicsLocalFrameContextSource localSource;

    {
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 4U;
        descriptor.width = 2;
        descriptor.height = 2;
        descriptor.stride = 2;
        descriptor.bitsPerPixel = 8;
        descriptor.pixelFormat = Framegrabber::PixelFormat::Mono8;
        descriptor.bitAlignment = Framegrabber::BitAlignment::Packed;
        descriptor.frameSeq = 9U;

        TestCopyGate gate({1U, 2U, 3U, 4U});
        GraphicsImageFailure failure = GraphicsImageFailure::None;
        GraphicsImage rejected;
        auto result = FramegrabberGraphicsFrameAdapter::captureFrame(
            localSource, descriptor, gate, &failure, &rejected);
        assert(result.hasValue());
        assert(failure == GraphicsImageFailure::None);
        assert(gate.copyAttempts == 1);

        const GraphicsFrame frame = std::move(result).value();
        assert(frame.isValid());
        assert(frame.firstImage() != nullptr);
        const GraphicsImage& image = frame.firstImage()->payload;
        assert(image.isValid());
        assert(image.pixelFormat == GraphicsImagePixelFormat::Mono8);
        assert(image.frameSequence == 9U);
        assert(image.pixels.size() == 4U);
        assert(image.data() != nullptr);
        assert(image.data()[0] == 1U);
        assert(image.data()[3] == 4U);
        assert(image.pixels.allocation());
        assert(image.pixels.allocation()->origin() == GraphicsAllocationOrigin::LocalHeap);
        assert(FramegrabberGraphicsFrameAdapter::imageFailure(image) == GraphicsImageFailure::None);
        assert(frame.metadata().sourceName == "Framegrabber");
        assert(frame.metadata().frameIndex == 9U);
        // The pre-migration stream set no frameId or clock.
        assert(frame.metadata().frameId.empty());

        auto checked = image;
        checked.stride = 1;
        assert(FramegrabberGraphicsFrameAdapter::imageFailure(checked) == GraphicsImageFailure::StrideBelowMinimum);
        checked.stride = 2;
        checked.pixels = checked.pixels.subspan(0U, 3U);
        assert(FramegrabberGraphicsFrameAdapter::imageFailure(checked) == GraphicsImageFailure::TruncatedBytes);
        checked.pixels = {};
        assert(FramegrabberGraphicsFrameAdapter::imageFailure(checked) == GraphicsImageFailure::MissingBytes);
    }

    {
        // Changed expectation: a descriptor smaller than stride * height used to be refused as
        // TruncatedBytes; it is now planned at stride * height, copied as far as it exists and
        // zero-filled, and counted.
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 3U;
        descriptor.width = 2;
        descriptor.height = 2;
        descriptor.stride = 2;
        descriptor.bitsPerPixel = 8;
        descriptor.pixelFormat = Framegrabber::PixelFormat::Mono8;
        descriptor.frameSeq = 10U;

        TestCopyGate gate({1U, 2U, 3U});
        GraphicsImageFailure failure = GraphicsImageFailure::None;
        GraphicsImage rejected;
        auto result = FramegrabberGraphicsFrameAdapter::captureFrame(
            localSource, descriptor, gate, &failure, &rejected);
        assert(result.hasValue());
        assert(failure == GraphicsImageFailure::None);
        assert(gate.copyAttempts == 1);
        const GraphicsFrame frame = std::move(result).value();
        const GraphicsImage& image = frame.firstImage()->payload;
        assert(image.isValid() && image.pixels.size() == 4U);
        assert(image.data()[2] == 3U && image.data()[3] == 0U);
    }

    {
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 0U;
        descriptor.width = 2;
        descriptor.height = 2;
        descriptor.stride = 2;
        descriptor.bitsPerPixel = 8;
        descriptor.pixelFormat = Framegrabber::PixelFormat::Mono8;
        descriptor.frameSeq = 11U;

        TestCopyGate gate({});
        GraphicsImageFailure failure = GraphicsImageFailure::None;
        auto result = FramegrabberGraphicsFrameAdapter::captureFrame(
            localSource, descriptor, gate, &failure);
        assert(!result.hasValue());
        assert(failure == GraphicsImageFailure::MissingBytes);
        assert(gate.copyAttempts == 0);
    }

    {
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 4U;
        descriptor.width = 2;
        descriptor.height = 2;
        descriptor.stride = 0;
        descriptor.bitsPerPixel = 8;
        descriptor.pixelFormat = Framegrabber::PixelFormat::Mono8;

        // Changed expectation: a zero stride used to be refused as InvalidStride; the bytes are
        // now published as Mono8 rows and the fit is counted.
        GraphicsImageFailure failure = GraphicsImageFailure::None;
        FramegrabberDetail::FrameAnomalies anomalies;
        auto plan = FramegrabberGraphicsFrameAdapter::planImage(descriptor, &failure, &anomalies);
        assert(plan.hasValue());
        assert(plan.value().format == GraphicsImagePixelFormat::Mono8);
        assert(plan.value().bytes == 4U && plan.value().strideBytes == 4 && plan.value().height == 1);
        assert(anomalies.countOf(FramegrabberDetail::FrameAnomalyReason::InvalidGeometryRaw) == 1);
    }

    {
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 4; descriptor.width = 2; descriptor.height = 2;
        descriptor.stride = 2; descriptor.bitsPerPixel = 8;
        descriptor.pixelFormat = Framegrabber::PixelFormat::Mono8;
        std::atomic<bool> localCancel{false}, externalCancel{false};
        TestCopyGate gate({1, 2, 3, 4});
        gate.cancelOnCopy = &localCancel;
        auto result = FramegrabberGraphicsFrameAdapter::captureFrame(
            localSource, descriptor, gate, nullptr, nullptr, &localCancel, &externalCancel);
        assert(!result.hasValue());
        assert(result.error() == GraphicsFrameError::Disconnected);
        assert(gate.copyAttempts == 1);
    }

    {
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 4; descriptor.width = 2; descriptor.height = 2;
        descriptor.stride = 2; descriptor.bitsPerPixel = 8;
        descriptor.pixelFormat = Framegrabber::PixelFormat::Mono8;
        TestCopyGate gate({1, 2, 3, 4}, true);
        auto result = FramegrabberGraphicsFrameAdapter::captureFrame(localSource, descriptor, gate);
        assert(!result.hasValue());
        assert(gate.copyAttempts == 1);
    }

    for (const auto error : {GraphicsFrameError::NoReadyCredit, GraphicsFrameError::ClassRefused})
    {
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 4; descriptor.width = 2; descriptor.height = 2;
        descriptor.stride = 2; descriptor.bitsPerPixel = 8;
        descriptor.pixelFormat = Framegrabber::PixelFormat::Mono8;
        RefusingSource source(error);
        TestCopyGate gate({1, 2, 3, 4});
        auto result = FramegrabberGraphicsFrameAdapter::captureFrame(source, descriptor, gate);
        assert(!result.hasValue() && result.error() == error);
        assert(source.calls == 1 && source.requested == 4);
        assert(gate.copyAttempts == 0);
    }

    {
        Framegrabber grabber(nullptr);
        auto source = std::make_shared<RefusingSource>(GraphicsFrameError::Disconnected);
        int delivered = 0, outcomes = 0;
        FramegrabberGraphicsFrameStream stream(&grabber, source,
            [&](GraphicsFrame&&, unsigned) { ++delivered; }, {},
            [&](GraphicsFrameError) { ++outcomes; });
        auto slot = FramegrabberDetail::CaptureTransactionAccess::snapshot(grabber);
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 4; descriptor.width = 2; descriptor.height = 2;
        descriptor.stride = 2; descriptor.bitsPerPixel = 8;
        descriptor.pixelFormat = Framegrabber::PixelFormat::Mono8;
        TestCopyGate gate({1, 2, 3, 4});
        assert(slot->fn(descriptor, gate) == FramegrabberDetail::CaptureTransactionOutcome::AbortedStop);
        assert(slot->fn(descriptor, gate) == FramegrabberDetail::CaptureTransactionOutcome::AbortedStop);
        assert(source->calls == 2 && gate.copyAttempts == 0 && delivered == 0 && outcomes == 0);
    }

    {
        Framegrabber grabber(nullptr);
        auto source = std::make_shared<RefusingSource>(GraphicsFrameError::ClassRefused);
        int failures = 0, delivered = 0;
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 4; descriptor.width = 2; descriptor.height = 2;
        descriptor.stride = 2; descriptor.bitsPerPixel = 8;
        descriptor.pixelFormat = Framegrabber::PixelFormat::Mono8;
        TestCopyGate gate({1, 2, 3, 4});
        GraphicsFrameError lastError = GraphicsFrameError::None;
        FramegrabberGraphicsFrameStream stream(&grabber, source,
            [&](GraphicsFrame&&, unsigned) { ++delivered; }, {},
            [&](GraphicsFrameError error) { lastError = error; ++failures; });
        auto slot = FramegrabberDetail::CaptureTransactionAccess::snapshot(grabber);
        // Each refused frame is dropped and counted; the next frame asks the source again.
        assert(slot->fn(descriptor, gate) == FramegrabberDetail::CaptureTransactionOutcome::DroppedStorageRefused);
        assert(slot->fn(descriptor, gate) == FramegrabberDetail::CaptureTransactionOutcome::DroppedStorageRefused);
        assert(source->calls == 2 && failures == 2 && lastError == GraphicsFrameError::ClassRefused);
        source->error = GraphicsFrameError::Capacity;
        assert(slot->fn(descriptor, gate) == FramegrabberDetail::CaptureTransactionOutcome::DroppedStorageRefused);
        assert(source->calls == 3 && failures == 3 && lastError == GraphicsFrameError::Capacity);
        assert(gate.copyAttempts == 0 && delivered == 0);
        // A later frame publishes once the source accepts the class again.
        source->error = GraphicsFrameError::None;
        assert(slot->fn(descriptor, gate) == FramegrabberDetail::CaptureTransactionOutcome::Delivered);
        assert(delivered == 1 && gate.copyAttempts == 1 && failures == 3);
    }

    {
        Framegrabber grabber(nullptr);
        auto oldSource = std::make_shared<RefusingSource>(GraphicsFrameError::NoReadyCredit);
        auto newSource = std::make_shared<RefusingSource>(GraphicsFrameError::NoReadyCredit);
        std::shared_ptr<const FramegrabberDetail::CaptureTransactionSlot> late;
        int delivered = 0;
        {
            FramegrabberGraphicsFrameStream oldStream(&grabber, oldSource,
                [&](GraphicsFrame&&, unsigned) { ++delivered; });
            late = FramegrabberDetail::CaptureTransactionAccess::snapshot(grabber);
        }
        FramegrabberGraphicsFrameStream newStream(&grabber, newSource,
            [&](GraphicsFrame&&, unsigned) { ++delivered; });
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 4; descriptor.width = 2; descriptor.height = 2;
        descriptor.stride = 2; descriptor.bitsPerPixel = 8;
        descriptor.pixelFormat = Framegrabber::PixelFormat::Mono8;
        TestCopyGate gate({1, 2, 3, 4});
        assert(late->fn(descriptor, gate) == FramegrabberDetail::CaptureTransactionOutcome::AbortedStop);
        assert(oldSource->calls == 0 && newSource->calls == 0 && delivered == 0);
    }

    {
        Framegrabber grabber(nullptr);
        auto source = std::make_shared<TrackedContextSource>();
        int delivered = 0, outcomes = 0;
        FramegrabberGraphicsFrameStream stream(&grabber, source,
            [&](GraphicsFrame&&, unsigned) { ++delivered; }, {},
            [&](GraphicsFrameError) { ++outcomes; });
        auto slot = FramegrabberDetail::CaptureTransactionAccess::snapshot(grabber);
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 4; descriptor.width = 2; descriptor.height = 2;
        descriptor.stride = 2; descriptor.bitsPerPixel = 8;
        descriptor.pixelFormat = Framegrabber::PixelFormat::Mono8;
        TestCopyGate gate({1, 2, 3, 4});
        gate.onCopy = [&] { assert(source->liveAllocations == 1 && source->contexts == 1); stream.cancel(); };
        assert(slot->fn(descriptor, gate) == FramegrabberDetail::CaptureTransactionOutcome::AbortedStop);
        assert(delivered == 0 && outcomes == 0 && gate.copyAttempts == 1);
        assert(source->liveAllocations == 0 && source->contexts == 0);
    }

    {
        Framegrabber grabber(nullptr);
        auto source = std::make_shared<GraphicsLocalFrameContextSource>();
        int received[2] = {0, 0};
        FramegrabberGraphicsFrameStream stream(&grabber, source,
            [&](GraphicsFrame&&, unsigned dma) { assert(dma < 2); ++received[dma]; });
        auto slot = FramegrabberDetail::CaptureTransactionAccess::snapshot(grabber);
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 4; descriptor.width = 2; descriptor.height = 2;
        descriptor.stride = 2; descriptor.bitsPerPixel = 8;
        descriptor.pixelFormat = Framegrabber::PixelFormat::Mono8;
        TestCopyGate gate({1, 2, 3, 4});
        descriptor.dmaIndex = 0;
        assert(slot->fn(descriptor, gate) == FramegrabberDetail::CaptureTransactionOutcome::Delivered);
        descriptor.dmaIndex = 1;
        assert(slot->fn(descriptor, gate) == FramegrabberDetail::CaptureTransactionOutcome::Delivered);
        assert(received[0] == 1 && received[1] == 1);
    }

    auto captureNative = [&localSource](const Framegrabber::PixelFormat pixelFormat,
                                        const GraphicsImagePixelFormat expected,
                                        const int width,
                                        const int height,
                                        const int stride,
                                        const int bitsPerPixel)
    {
        const std::size_t bytes = static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
        std::vector<std::uint8_t> payload(bytes);
        for (std::size_t i = 0; i < bytes; ++i)
        {
            payload[i] = static_cast<std::uint8_t>(i & 0xFFU);
        }

        FramegrabberDetail::CaptureFrameDescriptor input;
        input.size = bytes;
        input.width = width;
        input.height = height;
        input.stride = stride;
        input.bitsPerPixel = bitsPerPixel;
        input.pixelFormat = pixelFormat;
        input.bitAlignment = Framegrabber::BitAlignment::Packed;
        input.frameSeq = 3U;

        TestCopyGate gate(payload);
        GraphicsImageFailure nativeFailure = GraphicsImageFailure::None;
        auto result = FramegrabberGraphicsFrameAdapter::captureFrame(
            localSource, input, gate, &nativeFailure);
        assert(result.hasValue());
        assert(nativeFailure == GraphicsImageFailure::None);
        assert(gate.copyAttempts == 1);
        const GraphicsFrame frame = std::move(result).value();
        assert(frame.isValid());
        assert(frame.firstImage() != nullptr);
        const GraphicsImage& wrapped = frame.firstImage()->payload;
        assert(wrapped.isValid());
        assert(wrapped.pixelFormat == expected);
        assert(wrapped.pixels.size() == bytes);
        assert(std::equal(payload.begin(), payload.end(), wrapped.data()));
        assert(wrapped.width == width);
        assert(wrapped.height == height);
        assert(wrapped.stride == stride);
    };

    captureNative(
        Framegrabber::PixelFormat::BayerRG8,
        GraphicsImagePixelFormat::BayerRG8,
        4,
        4,
        4,
        8);
    captureNative(
        Framegrabber::PixelFormat::YCbCr422_8,
        GraphicsImagePixelFormat::YCbCr422_8,
        4,
        2,
        8,
        16);
    captureNative(
        Framegrabber::PixelFormat::RGB10Packed,
        GraphicsImagePixelFormat::RGB10Packed,
        4,
        2,
        15,
        30);

    using Reason = FramegrabberDetail::FrameAnomalyReason;
    const auto capture = [&localSource](FramegrabberDetail::CaptureFrameDescriptor descriptor,
                                        std::vector<std::uint8_t> bytes,
                                        TestCopyGate*& outGate,
                                        std::unique_ptr<TestCopyGate>& holder) {
        holder = std::make_unique<TestCopyGate>(std::move(bytes));
        outGate = holder.get();
        return FramegrabberGraphicsFrameAdapter::captureFrame(localSource, descriptor, *holder);
    };
    TestCopyGate* gate = nullptr;
    std::unique_ptr<TestCopyGate> holder;

    {
        // An SDK format without a GraphicsEngine layout maps to the nearest format by bit depth.
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 8; descriptor.width = 4; descriptor.height = 2;
        descriptor.stride = 4; descriptor.bitsPerPixel = 8;
        descriptor.fgFormat = 4242;
        descriptor.pixelFormat = Framegrabber::PixelFormat::Unknown;
        auto result = capture(descriptor, {1, 2, 3, 4, 5, 6, 7, 8}, gate, holder);
        assert(result.hasValue());
        const auto frame = std::move(result).value();
        const GraphicsImage& image = frame.firstImage()->payload;
        assert(image.isValid() && image.pixelFormat == GraphicsImagePixelFormat::Mono8);
        assert(image.width == 4 && image.height == 2 && image.stride == 4);
        assert(image.sourceFormat == 4242);
        assert(gate->anomalies.countOf(Reason::UnknownFormatMapped) == 1);
        assert(gate->anomalies.total() == 1);
    }

    for (const auto format : {Framegrabber::PixelFormat::Unknown, Framegrabber::PixelFormat::Raw,
                              Framegrabber::PixelFormat::Jpeg})
    {
        // No layout fits: the bytes are published unchanged as Mono8 rows.
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 12; descriptor.width = 6; descriptor.height = 2;
        descriptor.stride = 6; descriptor.bitsPerPixel = 0;
        descriptor.pixelFormat = format;
        std::vector<std::uint8_t> bytes(12);
        for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(i + 1);
        auto result = capture(descriptor, bytes, gate, holder);
        assert(result.hasValue());
        const auto frame = std::move(result).value();
        const GraphicsImage& image = frame.firstImage()->payload;
        assert(image.isValid() && image.pixelFormat == GraphicsImagePixelFormat::Mono8);
        assert(image.width == 6 && image.height == 2 && image.stride == 6);
        assert(std::equal(bytes.begin(), bytes.end(), image.data()));
        assert(gate->anomalies.countOf(Reason::UnknownFormatRaw) == 1);
    }

    {
        // A stride below the format's row size is widened row by row, never dropped.
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 6; descriptor.width = 4; descriptor.height = 2;
        descriptor.stride = 3; descriptor.bitsPerPixel = 16;
        descriptor.pixelFormat = Framegrabber::PixelFormat::Mono16;
        auto result = capture(descriptor, {1, 2, 3, 4, 5, 6}, gate, holder);
        assert(result.hasValue());
        const auto frame = std::move(result).value();
        const GraphicsImage& image = frame.firstImage()->payload;
        assert(image.isValid() && image.width == 4 && image.stride == 8 && image.pixels.size() == 16U);
        const std::uint8_t expected[16] = {1, 2, 3, 0, 0, 0, 0, 0, 4, 5, 6, 0, 0, 0, 0, 0};
        assert(std::equal(expected, expected + 16, image.data()));
        assert(gate->lastPlan.sourceStride == 3 && gate->lastPlan.destinationStride == 8);
        assert(gate->anomalies.countOf(Reason::StridePadded) == 1);
    }

    {
        // An odd YCbCr 4:2:2 width loses its last column instead of refusing the frame.
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 20; descriptor.width = 5; descriptor.height = 2;
        descriptor.stride = 10; descriptor.bitsPerPixel = 16;
        descriptor.pixelFormat = Framegrabber::PixelFormat::YCbCr422_8;
        auto result = capture(descriptor, std::vector<std::uint8_t>(20, 9), gate, holder);
        assert(result.hasValue());
        const auto frame = std::move(result).value();
        const GraphicsImage& image = frame.firstImage()->payload;
        assert(image.isValid() && image.width == 4 && image.stride == 10);
        assert(gate->anomalies.countOf(Reason::OddWidthTrimmed) == 1);
    }

    {
        // Unusable geometry with data present falls back to Mono8 rows; a huge size stays addressable.
        FramegrabberDetail::CaptureFrameDescriptor descriptor;
        descriptor.size = 10; descriptor.width = 0; descriptor.height = -1;
        descriptor.stride = 0; descriptor.bitsPerPixel = 16;
        descriptor.pixelFormat = Framegrabber::PixelFormat::Mono16;
        auto result = capture(descriptor, std::vector<std::uint8_t>(10, 5), gate, holder);
        assert(result.hasValue());
        const auto frame = std::move(result).value();
        assert(frame.firstImage()->payload.isValid());
        assert(gate->anomalies.countOf(Reason::InvalidGeometryRaw) == 1);
        FramegrabberDetail::CaptureFrameDescriptor huge;
        huge.size = static_cast<std::size_t>(5) << 30; huge.pixelFormat = Framegrabber::PixelFormat::Unknown;
        const auto plan = FramegrabberGraphicsFrameAdapter::planImage(huge);
        assert(plan.hasValue() && plan.value().strideBytes == 4096
               && plan.value().bytes >= huge.size && plan.value().bytes < huge.size + 4096U);
    }

    {
        // Every format with every awkward geometry plans: nothing is refused while bytes exist.
        const std::size_t formatCount = GraphicsImagePixelFormatTable::kFormatCount;
        for (std::size_t index = 0; index < formatCount; ++index)
        {
            for (const int width : {1, 2, 3, 5})
            for (const int stride : {1, 3, 7, 64})
            {
                FramegrabberDetail::CaptureFrameDescriptor descriptor;
                descriptor.width = width; descriptor.height = 3; descriptor.stride = stride;
                descriptor.size = static_cast<std::size_t>(stride) * 3U;
                descriptor.bitsPerPixel = static_cast<int>(index) % 70;
                descriptor.pixelFormat = static_cast<Framegrabber::PixelFormat>(index % 70);
                FramegrabberDetail::FrameAnomalies anomalies;
                GraphicsImageFailure failure = GraphicsImageFailure::None;
                const auto plan = FramegrabberGraphicsFrameAdapter::planImage(descriptor, &failure, &anomalies);
                assert(plan.hasValue());
                assert(failure == GraphicsImageFailure::None);
                assert(GraphicsImageValidation::hasValidLayout(plan.value().format, plan.value().width,
                    plan.value().height, plan.value().strideBytes, plan.value().bytes));
            }
        }
    }

    {
        // One DMA's odd frame never costs another DMA its frame.
        Framegrabber grabber(nullptr);
        auto source = std::make_shared<GraphicsLocalFrameContextSource>();
        int received[2] = {0, 0};
        FramegrabberGraphicsFrameStream stream(&grabber, source,
            [&](GraphicsFrame&&, unsigned dma) { assert(dma < 2); ++received[dma]; });
        auto slot = FramegrabberDetail::CaptureTransactionAccess::snapshot(grabber);
        FramegrabberDetail::CaptureFrameDescriptor odd;
        odd.size = 7; odd.width = 3; odd.height = 2; odd.stride = 3; odd.bitsPerPixel = 0;
        odd.pixelFormat = Framegrabber::PixelFormat::Unknown; odd.dmaIndex = 0;
        FramegrabberDetail::CaptureFrameDescriptor good;
        good.size = 4; good.width = 2; good.height = 2; good.stride = 2; good.bitsPerPixel = 8;
        good.pixelFormat = Framegrabber::PixelFormat::Mono8; good.dmaIndex = 1;
        TestCopyGate oddGate({1, 2, 3, 4, 5, 6, 7});
        TestCopyGate goodGate({1, 2, 3, 4});
        assert(slot->fn(odd, oddGate) == FramegrabberDetail::CaptureTransactionOutcome::Delivered);
        assert(slot->fn(good, goodGate) == FramegrabberDetail::CaptureTransactionOutcome::Delivered);
        assert(received[0] == 1 && received[1] == 1);
        assert(oddGate.anomalies.total() == 1 && goodGate.anomalies.total() == 0);
    }
    return 0;
}
