#ifndef __TBB_concurrent_once_flag
#define __TBB_concurrent_once_flag

#include <atomic>
#include <cassert>
#include <memory>

#include "tbb/task.h"
#include "tbb/task_arena.h"
#include "tbb/task_group.h"

namespace tbb {


template <typename F>
auto execute_isolated(task_arena& arena, F&& f) {
    return arena.execute([&f]() { return tbb::this_task_arena::isolate(f); });
}


class concurrent_once_flag : detail::d0::no_copy {
    enum class state { uninitialized, in_progress, done };
    std::atomic<state> m_state = state::uninitialized;

    // The run_once is shared so it doesn't have to stick around
    // forever. Also, so others can wait on it while the calling
    // function is done with it.
    // C++20: std::atomic_shared_ptr<run_once>:
    class run_once;
    std::shared_ptr<run_once> m_runner = nullptr;

    class run_once {
        task_arena m_arena;
        detail::d1::wait_context m_wait_ctx;
        mutable task_group_context m_context; ///< mutable so we can get the status in a const.

    public:
        run_once()
            : m_arena(task_arena::attach{})
            , m_wait_ctx(0)
            , m_context(task_group_context::bound,
                        task_group_context::default_traits | task_group_context::concurrent_wait) {}

        ~run_once() {
            // Caller is responsable for not pulling this out from under someone else.
            // We could use a std::shared_mutex though...
        }

        [[nodiscard]] bool is_canceling() const { return m_context.is_group_execution_cancelled(); }

        template <typename F>
        task_group_status run_and_wait(F&& f) {
            detail::d1::function_stack_task<F> t{f, m_wait_ctx};
            m_wait_ctx.reserve();
            bool cancellation_status = false;
            execute_isolated(m_arena, [&]() {
                detail::try_call([&] { execute_and_wait(t, m_context, m_wait_ctx, m_context); }).on_completion([&] {
                    cancellation_status = m_context.is_group_execution_cancelled();
                    // Don't bother resetting the context. This isn't reusable.
                });
            });
            return cancellation_status ? canceled : complete;
        }

        /*! A call to wait() potentially participates in the work of run_and_wait().
         * If run_and_wait() throws or is canceled, this returns canceled.
         */
        task_group_status wait() {
            bool cancellation_status = false;
            execute_isolated(m_arena, [&]() {
                detail::try_call([&] { detail::d1::wait(m_wait_ctx, m_context); }).on_completion([&] {
                    cancellation_status = m_context.is_group_execution_cancelled();
                });
            });
            return cancellation_status ? canceled : complete;
        }
    };

public:
    template <typename F>
    friend task_group_status call_once(concurrent_once_flag& once_flag, F&& f) {
        task_group_status waitResult = task_group_status::not_complete;
        while (once_flag.m_state.load(std::memory_order_acquire) != state::done) {
            // Loop until someone else finishes or it's Done.
            state expected = state::uninitialized;
            if (once_flag.m_state.compare_exchange_weak(expected, state::in_progress)) {
                // We won the race to be the primary thread.
                auto runner = std::make_shared<run_once>();
                std::atomic_store_explicit(&once_flag.m_runner, runner, std::memory_order_release);
                try {
                    waitResult = runner->run_and_wait(std::forward<F>(f));
                    assert(waitResult == task_group_status::complete);
                } catch (...) {
                    // Re-set and re-throw:
                    std::atomic_store_explicit(&once_flag.m_runner, std::shared_ptr<run_once>(nullptr),
                                               std::memory_order_release);
                    once_flag.m_state.store(state::uninitialized, std::memory_order_release);
                    throw;
                }
                // Let go, mark done, and return:
                std::atomic_store_explicit(&once_flag.m_runner, std::shared_ptr<run_once>(nullptr),
                                           std::memory_order_release);
                once_flag.m_state.store(state::done, std::memory_order_release);
                return waitResult;
            } else {
                // Get our own pointer so it can't go away while we aren't looking.
                if (auto runner = std::atomic_load_explicit(&once_flag.m_runner, std::memory_order_acquire)) {
                    waitResult = runner->wait();
                }
            }
        }
        return waitResult;
    }
};

} // namespace tbb

#endif // __TBB_concurrent_once_flag
