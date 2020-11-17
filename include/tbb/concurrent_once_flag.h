#ifndef __TBB_concurrent_once_flag
#define __TBB_concurrent_once_flag

#include <atomic>
#include <cassert>

#include "tbb/task.h"
#include "tbb/task_arena.h"
#include "tbb/task_group_context.h"

namespace tbb {


  
template <typename F>
auto execute_isolated(task_arena& arena, F&& f) {
    return arena.execute([&f]() { return tbb::this_task_arena::isolate(f); });
}


class concurrent_once_flag {
    enum class state { uninitialized, in_progress, done };
    std::atomic<state> my_state = state::uninitialized;

    // The run_once is shared so it doesn't have to stick around
    // forever. Also, so others can wait on it while the calling
    // function is done with it.
    // C++20: std::atomic_shared_ptr<run_once>:
    std::shared_ptr<run_once> my_runner = nullptr;

    class run_once {
      //! A tbb::empty_task allows provides something for other threads to wait on.
      task_arena my_arena;
      task_group_context my_context;
      empty_task* my_empty_task;

    public:
        run_once()
	  : m_arena(task_arena::attach{})
	  , m_context(task_group_context::bound,
		      task_group_context::default_traits | task_group_context::concurrent_wait) {
	// We start by making an empty task with a ref_count of 2.
	// That maakes anyone else who waits on it block.
	my_empty_task = new (task::allocate_root(my_context)) empty_task{};
	my_empty_task->set_ref_count(2);
      }


      // Not copyable:
      run_once(const run_once&) = delete;
      run_once& operator=(const run_once&) = delete;
      ~run_once() noexcept {
	// Caller is responsable for not pulling this out from under someone else.
	task::destroy(*my_empty_task);
      }

      [[nodiscard]] bool is_canceling() const { return my_context.is_group_execution_cancelled(); }
      [[nodiscard]] int ref_count() const { return my_emptyTask->ref_count(); }; // Primarily for debugging.

      template <typename F>
      task_group_status run_and_wait(F&& f) {
	if (is_canceling()) {
	  return task_group_status::canceled;
	}
	try {
	  // We need to increase the reference count of the root task to notify waiters that
	  // this task group has some work in progress.
	  assert(ref_count() == 2); // This is how we start things off. This class isn't reusable.
	  execute_isolated(my_arena, std::forward<F>(f));
	  my_emptyTask->set_ref_count(1); // Successfully completed.
	} catch (...) {
	  my_context.cancel_group_execution();
	  my_emptyTask->set_ref_count(1); // Unsuccessfully completed.
	  throw;
	}
	return is_canceling() ? task_group_status::canceled : task_group_status::complete;
      }
      /*! A call to wait() potentially participates in the work of run_and_wait().
       * If run_and_wait() throws or is canceled, this returns canceled.
       */
      task_group_status wait() {
	try {
	  execute_isolated(my_arena, [this]() { my_empty_task->wait_for_all(); }); // Wait for refcount to become 1.
	  assert(m_empty_task.ref_count() == 1); // Because we set it to tbb::task_group_context::concurrent_wait mode.
	} catch (...) {
	  // I believe wait_for_all should never throw.
	  assert(false);
	  return canceled;
	}
	return is_canceling() ? task_group_status::canceled : task_group_status::complete;
      }
    };

 public:
    template <typename F>
    friend task_group_status call_once(concurrent_once_flag& once_flag, F&& f) {
      while (once_flag.my_state.load(std::memory_order_acquire) != state::done) {
	// Loop until someone else finishes or it's Done.
	state expected = state::uninitialized;
	if (once_flag.my_state.compare_exchange_weak(expected, state::in_progress)) {
	  // We won the race to be the primary thread.
	  auto runner = std::make_shared<run_once>();
	  std::atomic_store_explicit(&once_flag.my_runner, runner, std::memory_order_release);
	  try {
	    task_group_status waitResult = runner->run_and_wait(std::forward<F>(f));
	    assert(waitResult == task_group_status::complete);
	  } catch (...) {
	    // Re-set and re-throw:
	    std::atomic_store_explicit(&m_runner, std::shared_ptr<run_once>(nullptr), std::memory_order_release);
	    my_state.store(state::uninitialized, std::memory_order_release);
	    throw;
	  }
	  // Let go, mark done, and return:
	  std::atomic_store_explicit(&my_runner, std::shared_ptr<run_once>(nullptr), std::memory_order_release);
	  my_state.store(state::done, std::memory_order_release);
	  return;
	} else {
	  // Get our own pointer so it can't go away while we aren't looking.
	  if (auto runner = std::atomic_load_explicit(&m_runner, std::memory_order_acquire)) {
	    runner->wait();
	  }
	}
      }
    }
};

#endif // __TBB_concurrent_once_flag
