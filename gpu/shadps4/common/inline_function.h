// sotc: SOTC_WD_FUSE / SOTC_INLINE_FUNC. SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace Common {

/// Set once at start-up, before the threads that build functions run. Off: every function is
/// heap allocated (what UniqueFunction does); on: closures up to InlineFunction::Capacity bytes
/// are stored in the object (no allocation, no cross-thread free).
inline bool g_inline_function_enabled = false;

/// Move-only void() function with a small buffer, for the draw thread's op ring (Liverpool).
/// Calling an empty one is undefined (as for UniqueFunction).
class InlineFunction {
public:
    static constexpr size_t Capacity = 64;

    InlineFunction() = default;

    template <typename F, typename D = std::decay_t<F>,
              typename = std::enable_if_t<!std::is_same_v<D, InlineFunction>>>
    InlineFunction(F&& functor) {
        if (g_inline_function_enabled && sizeof(D) <= Capacity &&
            alignof(D) <= alignof(std::max_align_t) && std::is_nothrow_move_constructible_v<D>) {
            ::new (static_cast<void*>(storage)) D(std::forward<F>(functor));
            vtable = &InlineTable<D>;
        } else {
            *reinterpret_cast<D**>(storage) = new D(std::forward<F>(functor));
            vtable = &HeapTable<D>;
        }
    }

    InlineFunction(InlineFunction&& rhs) noexcept {
        MoveFrom(rhs);
    }
    InlineFunction& operator=(InlineFunction&& rhs) noexcept {
        if (this != &rhs) {
            Reset();
            MoveFrom(rhs);
        }
        return *this;
    }
    InlineFunction(const InlineFunction&) = delete;
    InlineFunction& operator=(const InlineFunction&) = delete;
    ~InlineFunction() {
        Reset();
    }

    void operator()() {
        vtable->call(storage);
    }

    explicit operator bool() const noexcept {
        return vtable != nullptr;
    }

    void Reset() noexcept {
        if (vtable) {
            vtable->destroy(storage);
            vtable = nullptr;
        }
    }

private:
    struct Table {
        void (*call)(void*);
        void (*move)(void* dst, void* src) noexcept; ///< move-construct dst, destroy src
        void (*destroy)(void*) noexcept;
    };

    template <typename D>
    static constexpr Table InlineTable{
        [](void* p) { (*static_cast<D*>(p))(); },
        [](void* dst, void* src) noexcept {
            ::new (dst) D(std::move(*static_cast<D*>(src)));
            static_cast<D*>(src)->~D();
        },
        [](void* p) noexcept { static_cast<D*>(p)->~D(); },
    };

    template <typename D>
    static constexpr Table HeapTable{
        [](void* p) { (**static_cast<D**>(p))(); },
        [](void* dst, void* src) noexcept {
            *static_cast<D**>(dst) = *static_cast<D**>(src);
            *static_cast<D**>(src) = nullptr;
        },
        [](void* p) noexcept { delete *static_cast<D**>(p); },
    };

    void MoveFrom(InlineFunction& rhs) noexcept {
        vtable = rhs.vtable;
        if (vtable) {
            vtable->move(storage, rhs.storage);
            rhs.vtable = nullptr;
        }
    }

    alignas(std::max_align_t) unsigned char storage[Capacity];
    const Table* vtable{};
};

} // namespace Common
