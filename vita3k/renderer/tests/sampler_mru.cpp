// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// Licensed under GPLv2 or any later version.

#include <util/containers.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <list>
#include <random>

struct Entry {
    uint32_t value = 0;
    int index = 0;
};

static void check(bool condition) {
    if (!condition) {
        std::cerr << "Sampler MRU check failed\n";
        std::exit(1);
    }
}

// Exercise the real queue and lookup container against an independent list
// model. The fast-path invariant is that a matching head is the same cache
// entry a lookup would return, and its recency already equals a normal hit.
static void replay(size_t capacity) {
    lru::Queue<Entry> queue;
    queue.init(capacity);
    unordered_map_fast<uint32_t, Entry *> lookup;
    std::list<Entry> reference;
    for (size_t index = 0; index < capacity; ++index) {
        queue.items[index].content.index = static_cast<int>(index);
        reference.push_front({ 0, static_cast<int>(index) });
    }

    std::mt19937 random(0xcac4e);
    uint32_t previous = 1;
    for (uint32_t step = 0; step < 20000; ++step) {
        // Include repeated samplers, changing force-nearest, and eviction.
        const uint32_t key = step % 3 == 0 ? previous
            : (uint32_t(random() % (capacity + 7)) << 2) | 1U
                | (uint32_t(random() % 2) << 23);
        previous = key;

        auto expected = std::find_if(reference.begin(), reference.end(),
            [key](const Entry &entry) { return entry.value == key; });
        const bool miss = expected == reference.end();
        if (miss) {
            expected = std::prev(reference.end());
            expected->value = key;
        }
        const int expected_index = expected->index;
        reference.splice(reference.begin(), reference, expected);

        auto found = lookup.find(key);
        if (queue.head->content.value == key) {
            check(found != lookup.end());
            check(found->second == &queue.head->content);
            check(!miss);
        } else if (found != lookup.end()) {
            queue.set_as_mru(found->second);
            check(!miss);
        } else {
            Entry *entry = queue.get_lru();
            lookup.erase(entry->value);
            entry->value = key;
            lookup[key] = entry;
            queue.set_as_mru(entry);
            check(miss);
        }

        check(queue.head->content.index == expected_index);
        auto *item = queue.head;
        for (const auto &entry : reference) {
            check(item->content.value == entry.value);
            check(item->content.index == entry.index);
            item = item->next;
        }
        check(item == queue.head);
    }
}

int main() {
    for (const size_t capacity : { 1U, 2U, 3U, 16U, 512U })
        replay(capacity);
    std::cout << "Sampler MRU checks passed\n";
}
