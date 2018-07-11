/*
 * Copyright 2017-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <experimental/coroutine>

#include <folly/ExceptionString.h>
#include <folly/Executor.h>
#include <folly/Optional.h>

namespace folly {
namespace coro {
namespace detail {

template <typename T>
T&& getRef(T&& t) {
  return std::forward<T>(t);
}

template <typename T>
T& getRef(std::reference_wrapper<T> t) {
  return t.get();
}

template <typename Awaitable>
class AwaitWrapper {
 public:
  struct promise_type {
    std::experimental::suspend_always initial_suspend() {
      return {};
    }

    auto final_suspend() {
      struct FinalAwaiter {
        bool await_ready() noexcept {
          return false;
        }
        void await_suspend(
            std::experimental::coroutine_handle<promise_type> h) noexcept {
          auto& p = h.promise();
          p.executor_->add(p.awaiter_);
        }
        void await_resume() noexcept {}
      };
      return FinalAwaiter{};
    }

    void return_void() {}

    void unhandled_exception() {
      LOG(FATAL) << "Failed to schedule a task to awake a coroutine: "
                 << exceptionStr(std::current_exception());
    }

    AwaitWrapper get_return_object() {
      return {*this};
    }

    Executor* executor_;
    std::experimental::coroutine_handle<> awaiter_;
  };

  AwaitWrapper(AwaitWrapper&& other)
      : promise_(std::exchange(other.promise_, nullptr)),
        awaitable_(std::move(other.awaitable_)) {}
  AwaitWrapper& operator=(AwaitWrapper&&) = delete;

  static AwaitWrapper create(Awaitable&& awaitable) {
    return {std::move(awaitable)};
  }

  static AwaitWrapper create(Awaitable&& awaitable, Executor* executor) {
    auto ret = awaitWrapper();
    ret.awaitable_.emplace(std::move(awaitable));
    ret.promise_->executor_ = executor;
    return ret;
  }

  bool await_ready() {
    return getRef(*awaitable_).await_ready();
  }

  decltype(auto) await_suspend(std::experimental::coroutine_handle<> awaiter) {
    if (promise_) {
      promise_->awaiter_ = std::move(awaiter);
      return getRef(*awaitable_)
          .await_suspend(
              std::experimental::coroutine_handle<promise_type>::from_promise(
                  *promise_));
    }

    return getRef(*awaitable_).await_suspend(awaiter);
  }

  decltype(auto) await_resume() {
    return getRef(*awaitable_).await_resume();
  }

  ~AwaitWrapper() {
    if (promise_) {
      std::experimental::coroutine_handle<promise_type>::from_promise(*promise_)
          .destroy();
    }
  }

 private:
  AwaitWrapper(Awaitable&& awaitable) {
    awaitable_.emplace(std::move(awaitable));
  }
  AwaitWrapper(promise_type& promise) : promise_(&promise) {}

  static AwaitWrapper awaitWrapper() {
    co_return;
  }

  promise_type* promise_{nullptr};

  Optional<Awaitable> awaitable_;
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++17-extensions"

FOLLY_CREATE_MEMBER_INVOKE_TRAITS(
    member_operator_co_await_traits,
    operator co_await);

template <typename Awaitable>
inline constexpr bool has_member_operator_co_await_v =
    member_operator_co_await_traits::is_invocable<Awaitable>::value;

FOLLY_CREATE_FREE_INVOKE_TRAITS(
    non_member_operator_co_await_traits,
    operator co_await);

template <typename Awaitable>
inline constexpr bool has_non_member_operator_co_await_v =
    non_member_operator_co_await_traits::is_invocable<Awaitable>::value;
} // namespace detail

template <typename Awaitable>
decltype(auto) get_awaiter(Awaitable&& awaitable) {
  if constexpr (detail::has_member_operator_co_await_v<Awaitable&&>) {
    return std::forward<Awaitable>(awaitable).operator co_await();
  } else if constexpr (detail::has_non_member_operator_co_await_v<
                           Awaitable&&>) {
    return operator co_await(std::forward<Awaitable>(awaitable));
  } else {
    // This is necessary for it to work with std::reference_wrapper
    return static_cast<Awaitable&>(awaitable);
  }
}

template <typename Awaitable>
auto createAwaitWrapper(Awaitable&& awaitable) {
  using Awaiter =
      decltype(::folly::coro::get_awaiter(std::declval<Awaitable&&>()));
  using Wrapper = std::conditional_t<
      std::is_reference<Awaiter>::value,
      std::reference_wrapper<std::remove_reference_t<Awaiter>>,
      Awaiter>;
  return detail::AwaitWrapper<Wrapper>::create(
      ::folly::coro::get_awaiter(std::forward<Awaitable>(awaitable)));
}

template <typename Awaitable>
auto createAwaitWrapper(Awaitable&& awaitable, folly::Executor* executor) {
  using Awaiter =
      decltype(::folly::coro::get_awaiter(std::declval<Awaitable&&>()));
  using Wrapper = std::conditional_t<
      std::is_reference<Awaiter>::value,
      std::reference_wrapper<std::remove_reference_t<Awaiter>>,
      Awaiter>;
  return detail::AwaitWrapper<Wrapper>::create(
      ::folly::coro::get_awaiter(std::forward<Awaitable>(awaitable)), executor);
}

#pragma clang diagnostic pop

} // namespace coro
} // namespace folly
