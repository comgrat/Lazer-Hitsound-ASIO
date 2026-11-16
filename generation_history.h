#pragma once
#include <windows.h>
#include <deque>
#include <unordered_map>
#include "sharepool.h"

struct SeenGeneration { DWORD value; bool retired; };

// A watermark preserves replay protection after retired entries are evicted.
// Live entries are retained separately. Serial comparisons require fewer than
// 2^31 generation increments between an event and the current session state.
class GenerationHistory {
    struct Retired { HCHANNEL channel; DWORD generation; };
    std::unordered_map<HCHANNEL, SeenGeneration> entries;
    std::deque<Retired> retiredOrder;
    size_t limit;
    DWORD maxEvictedGeneration = 0;
    bool hasWatermark = false;
public:
    explicit GenerationHistory(size_t maximumRetired = 4096) : limit(maximumRetired) {}

    bool accept(HCHANNEL channel, DWORD generation, bool created, bool tracked = false, bool mayCreate = true)
    {
        if (!generation) return false;
        if (!tracked && hasWatermark && LONG(generation - maxEvictedGeneration) <= 0) return false;
        auto previous = entries.find(channel);
        if (previous == entries.end())
        {
            if (!mayCreate) return false;
            entries.emplace(channel, SeenGeneration{generation, false});
            return true;
        }
        LONG delta = LONG(generation - previous->second.value);
        if (delta < 0 || (delta == 0 && previous->second.retired)) return false;
        if (created && delta == 0) return false;
        if (delta > 0 && !mayCreate) return false;
        if (delta > 0) previous->second = {generation, false};
        return true;
    }

    void retire(HCHANNEL channel, DWORD generation)
    {
        if (!generation) return;
        auto previous = entries.find(channel);
        if (previous == entries.end() && hasWatermark && LONG(generation - maxEvictedGeneration) <= 0) return;
        if (previous != entries.end() && LONG(generation - previous->second.value) < 0) return;
        if (previous != entries.end() && previous->second.value == generation && previous->second.retired) return;
        entries[channel] = {generation, true};
        retiredOrder.push_back({channel, generation});
        while (retiredOrder.size() > limit)
        {
            auto oldest = retiredOrder.front();
            retiredOrder.pop_front();
            auto current = entries.find(oldest.channel);
            if (current == entries.end() || current->second.value != oldest.generation || !current->second.retired) continue;
            if (!hasWatermark || LONG(oldest.generation - maxEvictedGeneration) > 0)
                maxEvictedGeneration = oldest.generation;
            hasWatermark = true;
            entries.erase(current);
        }
    }

    bool retired(HCHANNEL channel) const
    {
        auto entry = entries.find(channel);
        return entry != entries.end() && entry->second.retired;
    }
    DWORD value(HCHANNEL channel) const { return entries.at(channel).value; }
    size_t size() const { return entries.size(); }
    size_t retirementQueueSize() const { return retiredOrder.size(); }
};
