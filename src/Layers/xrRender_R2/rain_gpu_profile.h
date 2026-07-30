#pragma once

#if defined(USE_DX11)

namespace xray::render::RENDER_NAMESPACE
{
template <size_t IntervalCount>
class QaGpuTimestampProfiler
{
    static constexpr size_t querySlotCount = 8;
    static constexpr size_t timestampCount = IntervalCount + 1;
    static constexpr u32 warmupSampleCount = 60;
    static constexpr u32 reportSampleCount = 120;

    struct QuerySlot
    {
        ID3D11Query* disjoint{};
        std::array<ID3D11Query*, timestampCount> timestamps{};
        bool pending{};
    };

public:
    QaGpuTimestampProfiler(pcstr profileName, const std::array<pcstr, IntervalCount>& intervalNames)
        : name(profileName), labels(intervalNames), enabled(strstr(Core.Params, "-qa_rain_gpu_profile"))
    {
    }

    void Begin(ID3D11DeviceContext* context)
    {
        if (!enabled || reported)
            return;

        if (!started)
        {
            Msg("* QA GPU PROFILE %s: started", name);
            started = true;
        }

        Poll(HW.get_context(CHW::IMM_CTX_ID));
        QuerySlot& slot = slots[nextSlot];
        if (slot.pending)
        {
            current = nullptr;
            return;
        }

        Initialize(slot);
        current = &slot;
        currentTimestamp = 0;
        context->Begin(slot.disjoint);
        context->End(slot.timestamps[0]);
    }

    void Mark(ID3D11DeviceContext* context)
    {
        if (!current || currentTimestamp >= IntervalCount)
            return;

        context->End(current->timestamps[++currentTimestamp]);
    }

    void End(ID3D11DeviceContext* context)
    {
        if (!current)
            return;

        VERIFY(currentTimestamp == IntervalCount);
        context->End(current->disjoint);
        current->pending = true;
        current = nullptr;
        nextSlot = (nextSlot + 1) % querySlotCount;
    }

private:
    void Initialize(QuerySlot& slot)
    {
        if (slot.disjoint)
            return;

        D3D11_QUERY_DESC description{};
        description.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        CHK_DX(HW.pDevice->CreateQuery(&description, &slot.disjoint));

        description.Query = D3D11_QUERY_TIMESTAMP;
        for (ID3D11Query*& timestamp : slot.timestamps)
            CHK_DX(HW.pDevice->CreateQuery(&description, &timestamp));
    }

    void Poll(ID3D11DeviceContext* context)
    {
        for (QuerySlot& slot : slots)
        {
            if (!slot.pending)
                continue;

            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            const HRESULT disjointResult =
                context->GetData(slot.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (disjointResult == S_FALSE)
                continue;

            if (FAILED(disjointResult) || disjoint.Disjoint)
            {
                slot.pending = false;
                continue;
            }

            std::array<u64, timestampCount> timestamps{};
            bool ready = true;
            for (size_t index = 0; index < timestampCount; ++index)
            {
                const HRESULT timestampResult = context->GetData(
                    slot.timestamps[index], &timestamps[index], sizeof(timestamps[index]),
                    D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (timestampResult == S_FALSE)
                {
                    ready = false;
                    break;
                }
                if (FAILED(timestampResult))
                {
                    slot.pending = false;
                    ready = false;
                    break;
                }
            }

            if (!ready)
                continue;

            slot.pending = false;
            if (warmupSamples < warmupSampleCount)
            {
                ++warmupSamples;
                continue;
            }

            const double millisecondsPerTick = 1000.0 / static_cast<double>(disjoint.Frequency);
            for (size_t interval = 0; interval < IntervalCount; ++interval)
            {
                const double milliseconds =
                    static_cast<double>(timestamps[interval + 1] - timestamps[interval]) * millisecondsPerTick;
                measurements[interval][samples] = milliseconds;
                sums[interval] += milliseconds;
                minima[interval] = std::min(minima[interval], milliseconds);
                maxima[interval] = std::max(maxima[interval], milliseconds);
            }

            if (++samples >= reportSampleCount)
            {
                Report();
                return;
            }
        }
    }

    void Report()
    {
        double total{};
        Msg("* QA GPU PROFILE %s: %u samples", name, samples);
        for (size_t interval = 0; interval < IntervalCount; ++interval)
        {
            auto sortedMeasurements = measurements[interval];
            std::sort(sortedMeasurements.begin(), sortedMeasurements.end());
            const double average = sums[interval] / samples;
            const double median =
                (sortedMeasurements[reportSampleCount / 2 - 1] + sortedMeasurements[reportSampleCount / 2]) * 0.5;
            constexpr size_t percentile95Index = (reportSampleCount * 95 + 99) / 100 - 1;
            const double percentile95 = sortedMeasurements[percentile95Index];
            total += average;
            Msg("* QA GPU PROFILE %s.%s: avg %.4f ms, median %.4f ms, p95 %.4f ms, min %.4f ms, max %.4f ms",
                name, labels[interval], average, median, percentile95, minima[interval], maxima[interval]);
        }
        Msg("* QA GPU PROFILE %s.total: avg %.4f ms", name, total);
        reported = true;
    }

private:
    pcstr name;
    std::array<pcstr, IntervalCount> labels;
    std::array<QuerySlot, querySlotCount> slots{};
    std::array<std::array<double, reportSampleCount>, IntervalCount> measurements{};
    std::array<double, IntervalCount> sums{};
    std::array<double, IntervalCount> minima = []
    {
        std::array<double, IntervalCount> values{};
        values.fill(std::numeric_limits<double>::max());
        return values;
    }();
    std::array<double, IntervalCount> maxima{};
    QuerySlot* current{};
    size_t nextSlot{};
    size_t currentTimestamp{};
    u32 warmupSamples{};
    u32 samples{};
    bool enabled{};
    bool started{};
    bool reported{};
};

template <size_t IntervalCount>
class QaCpuIntervalProfiler
{
    static constexpr size_t timestampCount = IntervalCount + 1;
    static constexpr u32 warmupSampleCount = 60;
    static constexpr u32 reportSampleCount = 120;

public:
    QaCpuIntervalProfiler(pcstr profileName, const std::array<pcstr, IntervalCount>& intervalNames)
        : name(profileName), labels(intervalNames), enabled(strstr(Core.Params, "-qa_rain_cpu_profile"))
    {
    }

    void Begin()
    {
        if (!enabled || reported)
            return;

        if (!started)
        {
            Msg("* QA CPU PROFILE %s: started", name);
            started = true;
        }

        currentTimestamp = 0;
        timestamps[0] = CPU::QPC();
        active = true;
    }

    void Mark()
    {
        if (!active || currentTimestamp >= IntervalCount)
            return;

        timestamps[++currentTimestamp] = CPU::QPC();
    }

    void End()
    {
        if (!active)
            return;

        Mark();
        VERIFY(currentTimestamp == IntervalCount);
        active = false;

        if (warmupSamples < warmupSampleCount)
        {
            ++warmupSamples;
            return;
        }

        const double millisecondsPerTick = 1000.0 / static_cast<double>(CPU::qpc_freq);
        for (size_t interval = 0; interval < IntervalCount; ++interval)
        {
            const double milliseconds =
                static_cast<double>(timestamps[interval + 1] - timestamps[interval]) * millisecondsPerTick;
            measurements[interval][samples] = milliseconds;
            sums[interval] += milliseconds;
            minima[interval] = std::min(minima[interval], milliseconds);
            maxima[interval] = std::max(maxima[interval], milliseconds);
        }

        if (++samples >= reportSampleCount)
            Report();
    }

private:
    void Report()
    {
        double total{};
        Msg("* QA CPU PROFILE %s: %u samples", name, samples);
        for (size_t interval = 0; interval < IntervalCount; ++interval)
        {
            auto sortedMeasurements = measurements[interval];
            std::sort(sortedMeasurements.begin(), sortedMeasurements.end());
            const double average = sums[interval] / samples;
            const double median =
                (sortedMeasurements[reportSampleCount / 2 - 1] + sortedMeasurements[reportSampleCount / 2]) * 0.5;
            constexpr size_t percentile95Index = (reportSampleCount * 95 + 99) / 100 - 1;
            const double percentile95 = sortedMeasurements[percentile95Index];
            total += average;
            Msg("* QA CPU PROFILE %s.%s: avg %.4f ms, median %.4f ms, p95 %.4f ms, min %.4f ms, max %.4f ms",
                name, labels[interval], average, median, percentile95, minima[interval], maxima[interval]);
        }
        Msg("* QA CPU PROFILE %s.total: avg %.4f ms", name, total);
        reported = true;
    }

private:
    pcstr name;
    std::array<pcstr, IntervalCount> labels;
    std::array<u64, timestampCount> timestamps{};
    std::array<std::array<double, reportSampleCount>, IntervalCount> measurements{};
    std::array<double, IntervalCount> sums{};
    std::array<double, IntervalCount> minima = []
    {
        std::array<double, IntervalCount> values{};
        values.fill(std::numeric_limits<double>::max());
        return values;
    }();
    std::array<double, IntervalCount> maxima{};
    size_t currentTimestamp{};
    u32 warmupSamples{};
    u32 samples{};
    bool enabled{};
    bool started{};
    bool reported{};
    bool active{};
};
} // namespace xray::render::RENDER_NAMESPACE

#endif
