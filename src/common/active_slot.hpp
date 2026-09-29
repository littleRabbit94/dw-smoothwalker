// A pointer other threads call through, and the count of calls in flight through it, so an owner can take the object
// away and know when the last call has left it: the hook's Pipeline (camera/hook.cpp) and the Lua closures' Authority
// (camera/lua_api.hpp). Both slots are image-level statics that outlive every instance on the pinned image; the
// objects they point at are an instance's.
//
// Order: enter() counts itself in before it loads the pointer, clear_and_drain() stores null before it reads the
// count, all sequentially consistent. A caller that loaded the object therefore counted itself in before the null
// store, and the drain sees it until its Entry is destroyed; a caller that loads after the store gets null.
//
// Needs the Windows types included before it (Sleep).
#pragma once

#include <atomic>

namespace dw
{
    // Polls `count` every 1 ms (Sleep(1)), at most timeout_ms times. False: still not 0.
    inline auto wait_for_zero(const std::atomic<int>& count, int timeout_ms) -> bool
    {
        for (int i = 0; i < timeout_ms && count.load() != 0; ++i) Sleep(1);
        return count.load() == 0;
    }

    template <typename T>
    class ActiveSlot
    {
      public:
        // One counted call: in from enter(), out when destroyed. Never alive across a call that can longjmp (a raising
        // lua_* call): the skipped destructor leaves the count raised and every later drain times out.
        class Entry
        {
          public:
            Entry() = default;
            ~Entry()
            {
                if (m_in) m_in->fetch_sub(1);
            }
            Entry(const Entry&) = delete;
            auto operator=(const Entry&) -> Entry& = delete;

          private:
            friend class ActiveSlot;
            std::atomic<int>* m_in = nullptr;
        };

        // The object callers reach from now on. The owner keeps it alive until clear_and_drain() returns true.
        auto set(T& object) -> void { m_object.store(&object); }

        // Counts the caller in (once per Entry), then loads the object: null while none is set.
        auto enter(Entry& entry) -> T*
        {
            m_in.fetch_add(1);
            entry.m_in = &m_in;
            return m_object.load();
        }

        // No new call reaches the object; then waits up to timeout_ms for the calls in flight (wait_for_zero). False:
        // one is still running, and the object must stay allocated.
        auto clear_and_drain(int timeout_ms) -> bool
        {
            m_object.store(nullptr);
            return wait_for_zero(m_in, timeout_ms);
        }

      private:
        std::atomic<T*> m_object{nullptr};
        std::atomic<int> m_in{0};
    };
} // namespace dw
