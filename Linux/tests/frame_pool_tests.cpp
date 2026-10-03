#include "opendisplay/frame_pool.hpp"

#include <cassert>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

int main() {
    // A buffer reports the size it was asked for.
    const auto pool = od::FramePool::create();
    {
        auto buffer = pool->acquire(1024);
        assert(buffer.size() == 1024);
        assert(buffer.capacity() >= 1024);
        assert(!buffer.empty());
    }
    // Dropping it hands the storage back rather than freeing it.
    assert(pool->retainedCount() == 1);

    // The whole point: the next frame reuses that allocation instead of
    // paying for a new one. Same address means no allocation, no zero-fill,
    // and no page faults on the capture thread.
    const void* firstAddress = nullptr;
    {
        auto buffer = pool->acquire(1024);
        firstAddress = buffer.data();
        assert(pool->retainedCount() == 0);
    }
    {
        auto buffer = pool->acquire(1024);
        assert(buffer.data() == firstAddress);
    }

    // A smaller request reuses a larger retained buffer, keeping its capacity.
    {
        auto buffer = pool->acquire(16);
        assert(buffer.size() == 16);
        assert(buffer.capacity() >= 1024);
    }

    // Growing past everything retained discards the stale smaller storage, so
    // a resolution change does not leave unusable buffers parked forever.
    {
        auto buffer = pool->acquire(4096);
        assert(buffer.size() == 4096);
    }
    assert(pool->retainedCount() == 1);
    {
        auto big = pool->acquire(4096);
        assert(big.capacity() >= 4096);
    }

    // The pool keeps a bounded number of spares.
    {
        const auto bounded = od::FramePool::create(2);
        std::vector<od::FrameBuffer> held;
        held.reserve(5);
        for (int index = 0; index < 5; ++index) {
            held.push_back(bounded->acquire(64));
        }
        held.clear();
        assert(bounded->retainedCount() == 2);
    }

    // Moving transfers ownership exactly once: the source must not also
    // recycle, or two frames would end up sharing one allocation.
    {
        const auto single = od::FramePool::create();
        {
            auto first = single->acquire(32);
            auto second = std::move(first);
            assert(second.size() == 32);
            assert(first.size() == 0);  // NOLINT(bugprone-use-after-move)
            assert(first.data() == nullptr);
        }
        assert(single->retainedCount() == 1);
    }

    // Content survives the round trip, and assign() fills as asked.
    {
        auto buffer = pool->acquire(8);
        std::memcpy(buffer.data(), "abcdefgh", 8);
        assert(std::string_view(buffer) == "abcdefgh");
        buffer.assign(4, 'z');
        assert(std::string_view(buffer) == "zzzz");
    }

    // A buffer with no pool behind it still works, which is what tests and
    // any non-capture caller construct.
    {
        od::FrameBuffer standalone;
        assert(standalone.empty());
        const std::vector<char> source{'1', '2', '3'};
        standalone.assign(source.begin(), source.end());
        assert(std::string_view(standalone) == "123");
        standalone.assign(2, 'q');
        assert(std::string_view(standalone) == "qq");
    }
    return 0;
}
