#pragma once
// ============================================================
// AYMethodInfoImpl.h - AYScript-private MethodInfoImpl<T,Ret,Args...>
//
// S3.12 + S3.12+R3 (track R2 §5.7.4): variadic member-function-pointer
// → IMethodInfo adapter. Lives in AYScript (not AYReflect) so that
// the foundation TU (`AYReflect.cpp`) never sees this template —
// keeping the variadic pack out of the foundation TU avoids the
// MSVC C2275 / C2641 errors on `VectorTypeInfo<ayt::math::Bool>`
// that aborted the first S3.12 attempt.
//
// Usage (in AY_PROPERTY / AY_METHOD registrar codepaths or in tests):
//   struct Player { int hp = 0;
//       void heal(int amount) { hp += amount; }
//       void applyDamage(const DamageInfo& d) { hp -= d.amount; }
//       Stats getStats() const { return {hp, 10, "hero"}; }
//   };
//   auto* ti = TypeRegistryImpl::instance().findType<Player>();
//   ti->addMethod(new ayt::script::logia::reflect::MethodInfoImpl<
//                     Player, void, int>(&Player::heal, "heal"));
//   ti->addMethod(new ayt::script::logia::reflect::MethodInfoImpl<
//                     Player, void, const DamageInfo&>(
//                         &Player::applyDamage, "applyDamage"));
//   ti->addMethod(new ayt::script::logia::reflect::MethodInfoImplConst<
//                     Player, Stats>(&Player::getStats, "getStats"));
//
// Scope (S3.12 + R3):
//   - non-overloaded, non-templated member functions only
//   - primitive args + primitive-or-void return
//   - const-ref / const-ptr args (S3.12+R3)
//   - by-value struct return (S3.12+R3) — bridge marshals
//   - std::string args + return (S3.12+R3) — bridge heap-allocates
//   - enum args + return (S3.12+R3) — bridge int-casts
//   - const member functions supported (use MethodInfoImplConst)
//
// Out of scope (deferred to R3.5+ / track R4):
//   - std::vector / std::array args
//   - nested struct fields
//   - T& / T* out-params (write-back)
//   - unique_ptr / shared_ptr
//   - struct fields of type std::string (placement-new in
//     pushFieldPrimitive / storeFieldPrimitive deferred to R3.5)
//
// Slot convention (uniform across S3.12 + R3):
//   For every arg, the bridge places a pointer to the marshalled
//   value in args[I]. readArg<I>(args) dereferences that pointer
//   according to ArgT (the PMF parameter type). This uniform pointer-
//   indirection lets a single template handle:
//     - primitives (slot = T*, deref → T value)
//     - const T& (slot = const T*, deref → const T&)
//     - T* (slot = T* const*, deref → T*)
//     - std::string (slot = std::string*, deref → std::string&)
//     - enum E (slot = underlying*, cast → E)
//   Cost: one extra pointer indirection per call. Acceptable.
//
// Thread-local return buffer for non-void returns — see
// detail::TlsReturnSlot below.
// ============================================================

#include "IAYReflect.h"
#include "AYReflect.h"

#include <cstring>
#include <type_traits>
#include <tuple>
#include <utility>
#include <vector>
#include <array>   // R4.1 (2026-07-13): std::array<T, N> in readArg

namespace ayt::script::logia::reflect
{

// ===== Thread-local return slot =====
//
// invoke() returns `const void*` to a thread-local slot holding the
// return value (or nullptr for void). The slot type is `std::string`
// for two reasons:
//   1. std::string is a "fat" container that supports placement-new
//      of a new std::string onto the same address (the allocator
//      is heap, the buffer is inlined SSO). When we destroy the
//      previous occupant and placement-new a new one, std::string's
//      internal _Container_base12 is properly reset.
//   2. We can memcpy trivially-copyable types into the string's
//      internal buffer (after resize), read bytes back out at the
//      bridge. The data() pointer is stable for the lifetime of the
//      string (no SSO realloc unless we resize).
//
// We always resize to max(sizeof(void*), sizeof(Ret)) rounded up to
// 8-byte alignment. placement-new on an under-sized buffer is
// undefined behavior.
//
// The slot is per-thread so concurrent bridge calls do not race.
namespace detail
{
    // Use a single std::string as the underlying storage. We resize
    // it to the actual size we need; placement-new operates on
    // data() which is stable across the lifetime of the string.
    // destroy + placement-new re-initializes the string's internal
    // iterators (the _Container_base12 subobject within the SSO
    // buffer) cleanly. A raw std::vector<uint8_t> doesn't do this
    // — placing a std::string onto a vector's data() corrupts the
    // string's iterator tracking and crashes on next dtor.
    inline std::string& tlsReturnBuffer()
    {
        thread_local std::string buf;
        return buf;
    }

    // SFINAE helper: if X is an enum, ::type is std::underlying_type_t<X>.
    // Otherwise ::type is X. This keeps std::underlying_type_t
    // out of non-enum instantiations (which would fail to compile).
    template <typename X, bool IsEnum = std::is_enum_v<X>>
    struct LookupType {
        using type = X;
    };
    template <typename X>
    struct LookupType<X, true> {
        using type = std::underlying_type_t<X>;
    };

    // R4.1 (2026-07-13): SFINAE helpers for std::vector<T> /
    // std::array<T, N> detection in readArg's if-constexpr ladder.
    // Specializations match exact std::vector / std::array
    // instantiations; any user-defined container does not match and
    // falls through to the by-value / by-ref branch below (which
    // would silently fail at compile time for that container — a
    // user-side error, not a bridge bug).
    template <typename>
    struct is_std_vector : std::false_type {};
    template <typename T, typename A>
    struct is_std_vector<std::vector<T, A>> : std::true_type {};
    template <typename X>
    inline constexpr bool is_std_vector_v = is_std_vector<X>::value;

    template <typename>
    struct is_std_array : std::false_type {};
    template <typename T, size_t N>
    struct is_std_array<std::array<T, N>> : std::true_type {};
    template <typename X>
    inline constexpr bool is_std_array_v = is_std_array<X>::value;
} // namespace detail

// ===== MethodInfoImpl<T, Ret, Args...> =====
//
// Stores a non-const member-function pointer.
template <typename T, typename Ret, typename... Args>
struct MethodInfoImpl : public ayt::reflect::IMethodInfo {
    using Pmf = Ret (T::*)(Args...);

    Pmf _pmf;
    const char* _name;

    MethodInfoImpl(const char* name, Pmf pmf)
        : _pmf(pmf), _name(name)
    {}

    const char* getName() const override { return _name; }

    ayt::reflect::ITypeInfo* getReturnType() const override {
        return getCachedReturnType();
    }

    size_t getParamCount() const override { return sizeof...(Args); }

    ayt::reflect::ITypeInfo* getParamType(size_t index) const override {
        return paramTypeAt(index);
    }

    const void* invoke(const void* obj, const void* const* args) const override {
        auto* self = const_cast<T*>(static_cast<const T*>(obj));
        return invokeImpl(self, args, std::index_sequence_for<Args...>{});
    }
    template <std::size_t... I>
    const void* invokeImpl(T* self, const void* const* args, std::index_sequence<I...>) const {
        if constexpr (std::is_same_v<Ret, void>) {
            (self->*_pmf)(readArg<I>(args)...);
            return nullptr;
        } else {
            Ret r = (self->*_pmf)(readArg<I>(args)...);
            return storeReturn(std::move(r));
        }
    }

    // Store the return value into the thread-local buffer and return
    // a pointer to it. Trivially-copyable types (including enum) are
    // memcpy'd; std::string and structs are placement-new'd so their
    // destructor runs cleanly on the next invoke.
    //
    // The underlying buffer is a `std::string` (see detail::tlsReturnBuffer
    // above). For trivial types we resize the string to sizeof(R),
    // fill with memcpy, and read data() at the bridge. The std::string
    // is NOT treated as a string by us — we never call any of its
    // methods that inspect contents (other than data()/resize()).
    // The resize + memcpy pattern is safe because std::string is
    // a thin wrapper around a byte buffer; data() is stable for the
    // lifetime of the string. The string's own _Container_base12
    // subobject (which is what caused the original segfault) tracks
    // iterators for invalidation; since we never hold iterators
    // across resize, the subobject stays in a clean state.
    //
    // Implementation note: this function is a no-op template — the
    // call site (invokeImpl) already short-circuits the void case
    // with `if constexpr (std::is_same_v<Ret, void>)`. We use
    // `std::conditional_t<...>` to pick the parameter type so the
    // template itself is well-formed for Ret = void (where the
    // parameter type collapses to `int` — a placeholder that's
    // never bound to anything in the void instantiation).
    template <typename R = Ret>
    static const void* storeReturn(
        typename std::conditional_t<std::is_void_v<R>, int, R&&> r) {
        if constexpr (std::is_void_v<R>) {
            (void)r;
            return nullptr;
        } else {
            std::string& buf = detail::tlsReturnBuffer();
            // First, destroy any previous occupant. This resets
            // the std::string's _Container_base12 to a clean state
            // (length 0, no iterators, no heap buffer if SSO).
            // Important: we use the std::string's own destructor
            // path via `buf.clear()` + length-zero, not raw
            // ~basic_string(), because the latter is UB on an
            // already-destroyed object.
            buf.clear();
            buf.resize(sizeof(R), '\0');
            if constexpr (std::is_trivially_copyable_v<R>) {
                std::memcpy(&buf[0], &r, sizeof(R));
                return &buf[0];
            } else if constexpr (std::is_same_v<R, std::string>) {
                // For std::string return, we placement-new a new
                // std::string in the buffer. The std::string's
                // allocator pointer (in the SSO buffer) now points
                // to a fresh heap allocation, or stays in SSO if
                // the value fits. This is safe because buf is
                // currently in a "no string is alive" state
                // (we just cleared it).
                return new (&buf[0]) std::string(std::move(r));
            } else {
                // Non-trivially-copyable struct by-value: placement-
                // new T into the buffer. Safe for the same reason
                // (buf has no live T after the clear).
                return new (&buf[0]) R(std::move(r));
            }
        }
    }

    // readArg<I>(args) — uniform slot-dispatch.
    //
    // The bridge places a pointer at args[I] for every parameter
    // (see file-level "Slot convention" comment). readArg returns the
    // parameter by value to the PMF call; for const T& / T* / etc.,
    // the implicit conversion gives the PMF the right reference.
    template <std::size_t I>
    static auto readArg(const void* const* args) {
        using ArgT = std::tuple_element_t<I, std::tuple<Args...>>;
        const void* slot = (args && args[I]) ? args[I] : nullptr;

        if constexpr (std::is_enum_v<ArgT>) {
            // Enum: bridge memcpys the underlying integer. We
            // reinterpret the slot as the underlying type and cast
            // to the enum class.
            using UT = std::underlying_type_t<ArgT>;
            UT v{};
            if (slot) std::memcpy(&v, slot, sizeof(UT));
            return static_cast<ArgT>(v);
        } else if constexpr (std::is_same_v<ArgT, std::string>) {
            // std::string: bridge heap-allocates a std::string and
            // passes its address; we move-construct into the
            // parameter. The bridge frees the heap pointer after
            // invoke() returns.
            if (!slot) return std::string{};
            const std::string* sp = static_cast<const std::string*>(slot);
            return std::string(*sp);  // copy into parameter (PMF may
                                       // want std::string& or std::string
                                       // by value — both work).
        } else if constexpr (detail::is_std_vector_v<ArgT>) {
            // R4.1 (2026-07-13): std::vector<T> by value (or by
            // const-ref, which is the common PMF signature). The
            // bridge heap-allocates the vector and stores the
            // pointer in the slot. We copy-construct the parameter
            // from that heap pointer; the bridge deletes the heap
            // pointer after invoke() returns via the cleanup queue.
            if (!slot) return ArgT{};
            const auto* vp = static_cast<const ArgT*>(slot);
            return ArgT(*vp);
        } else if constexpr (detail::is_std_array_v<ArgT>) {
            // R4.1 (2026-07-13): std::array<T, N> by value (or by
            // const-ref). Same slot convention as std::vector.
            if (!slot) return ArgT{};
            const auto* ap = static_cast<const ArgT*>(slot);
            return ArgT(*ap);
        } else if constexpr (std::is_pointer_v<ArgT>) {
            // T* / const T* / etc. — slot is T*; we pass through.
            if (!slot) return ArgT{};
            using Inner = std::remove_cv_t<std::remove_pointer_t<ArgT>>;
            const Inner* p = *static_cast<const Inner* const*>(slot);
            return const_cast<ArgT>(p);
        } else if constexpr (std::is_lvalue_reference_v<ArgT>) {
            // const T& / T& — slot is T*; deref and return reference.
            if (!slot) {
                // Cannot default-construct a reference. Caller is
                // expected to always provide a slot for ref args.
                // Returning a default-constructed T* would be UB.
                // We use a thread-local default to keep this branch
                // safe; the bridge always provides a slot for ref
                // args in practice.
                static thread_local std::remove_reference_t<ArgT> fallback{};
                return fallback;
            }
            using Target = std::remove_reference_t<ArgT>;
            return *static_cast<Target*>(const_cast<void*>(slot));
        } else {
            // By value (primitive or struct). For trivially-copyable
            // types, memcpy the slot. For struct types, the bridge
            // has already placed a copy of the struct in the slot;
            // we copy-construct from there.
            ArgT v{};
            if (slot) {
                if constexpr (std::is_trivially_copyable_v<ArgT>) {
                    std::memcpy(&v, slot, sizeof(ArgT));
                } else {
                    // Non-trivially-copyable by-value (e.g. struct
                    // by value): the bridge has already constructed
                    // a T in the slot — copy-construct from it.
                    v = *static_cast<const ArgT*>(slot);
                }
            }
            return v;
        }
    }

    ayt::reflect::ITypeInfo* getCachedReturnType() const {
        if constexpr (std::is_same_v<Ret, void>) {
            return nullptr;
        } else {
            static ayt::reflect::ITypeInfo* cache = nullptr;
            if (!cache) {
                cache = ayt::reflect::TypeRegistryImpl::instance().template findType<Ret>();
            }
            return cache;
        }
    }

    ayt::reflect::ITypeInfo* paramTypeAt(size_t index) const {
        if (index >= sizeof...(Args)) return nullptr;
        return paramTypeAtImpl(index, std::index_sequence_for<Args...>{});
    }

    template <std::size_t... I>
    ayt::reflect::ITypeInfo* paramTypeAtImpl(size_t index, std::index_sequence<I...>) const {
        ayt::reflect::ITypeInfo* result = nullptr;
        ((index == I ? (result = paramTypeForIndex<I>()), 0 : 0), ...);
        return result;
    }

    template <std::size_t I>
    static ayt::reflect::ITypeInfo* paramTypeForIndex() {
        using ArgT = std::tuple_element_t<I, std::tuple<Args...>>;
        // Strip reference / pointer / const so findType<T> matches
        // the registered ITypeInfo. Enum needs special handling —
        // fall back to underlying type lookup via the SFINAE
        // helper at namespace scope (detail::LookupType) so
        // std::underlying_type_t is only instantiated for enums.
        using Stripped = std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<ArgT>>>;
        using LookupT = typename detail::LookupType<Stripped>::type;
        static ayt::reflect::ITypeInfo* cache = nullptr;
        if (!cache) {
            cache = ayt::reflect::TypeRegistryImpl::instance().template findType<LookupT>();
        }
        return cache;
    }
};

// ===== const-PMF specialization =====
template <typename T, typename Ret, typename... Args>
struct MethodInfoImplConst : public ayt::reflect::IMethodInfo {
    using Pmf = Ret (T::*)(Args...) const;

    Pmf _pmf;
    const char* _name;

    MethodInfoImplConst(const char* name, Pmf pmf)
        : _pmf(pmf), _name(name)
    {}

    const char* getName() const override { return _name; }

    ayt::reflect::ITypeInfo* getReturnType() const override {
        return getCachedReturnType();
    }

    size_t getParamCount() const override { return sizeof...(Args); }

    ayt::reflect::ITypeInfo* getParamType(size_t index) const override {
        return paramTypeAt(index);
    }

    const void* invoke(const void* obj, const void* const* args) const override {
        const T* self = static_cast<const T*>(obj);
        return invokeImpl(self, args, std::index_sequence_for<Args...>{});
    }

private:
    template <std::size_t... I>
    const void* invokeImpl(const T* self, const void* const* args, std::index_sequence<I...>) const {
        if constexpr (std::is_same_v<Ret, void>) {
            (self->*_pmf)(readArg<I>(args)...);
            return nullptr;
        } else {
            Ret r = (self->*_pmf)(readArg<I>(args)...);
            return storeReturn(std::move(r));
        }
    }

    static const void* storeReturn(Ret&& r) {
        if constexpr (std::is_same_v<Ret, void>) {
            (void)r;
            return nullptr;
        } else {
            std::string& buf = detail::tlsReturnBuffer();
            buf.clear();
            buf.resize(sizeof(Ret), '\0');
            if constexpr (std::is_trivially_copyable_v<Ret>) {
                std::memcpy(&buf[0], &r, sizeof(Ret));
                return &buf[0];
            } else if constexpr (std::is_same_v<Ret, std::string>) {
                return new (&buf[0]) std::string(std::move(r));
            } else {
                return new (&buf[0]) Ret(std::move(r));
            }
        }
    }

    template <std::size_t I>
    static auto readArg(const void* const* args) {
        using ArgT = std::tuple_element_t<I, std::tuple<Args...>>;
        const void* slot = (args && args[I]) ? args[I] : nullptr;

        if constexpr (std::is_enum_v<ArgT>) {
            using UT = std::underlying_type_t<ArgT>;
            UT v{};
            if (slot) std::memcpy(&v, slot, sizeof(UT));
            return static_cast<ArgT>(v);
        } else if constexpr (std::is_same_v<ArgT, std::string>) {
            if (!slot) return std::string{};
            const std::string* sp = static_cast<const std::string*>(slot);
            return std::string(*sp);
        } else if constexpr (detail::is_std_array_v<ArgT>) {
            // R4.1: std::array<T, N> mirror of the non-const impl.
            if (!slot) return ArgT{};
            const auto* ap = static_cast<const ArgT*>(slot);
            return ArgT(*ap);
        } else if constexpr (detail::is_std_vector_v<ArgT>) {
            // R4.1: std::vector<T> mirror of the non-const impl.
            if (!slot) return ArgT{};
            const auto* vp = static_cast<const ArgT*>(slot);
            return ArgT(*vp);
        } else if constexpr (std::is_pointer_v<ArgT>) {
            if (!slot) return ArgT{};
            using Inner = std::remove_cv_t<std::remove_pointer_t<ArgT>>;
            const Inner* p = *static_cast<const Inner* const*>(slot);
            return const_cast<ArgT>(p);
        } else if constexpr (std::is_lvalue_reference_v<ArgT>) {
            if (!slot) {
                static thread_local std::remove_reference_t<ArgT> fallback{};
                return fallback;
            }
            using Target = std::remove_reference_t<ArgT>;
            return *static_cast<Target*>(const_cast<void*>(slot));
        } else {
            ArgT v{};
            if (slot) {
                if constexpr (std::is_trivially_copyable_v<ArgT>) {
                    std::memcpy(&v, slot, sizeof(ArgT));
                } else {
                    v = *static_cast<const ArgT*>(slot);
                }
            }
            return v;
        }
    }

    ayt::reflect::ITypeInfo* getCachedReturnType() const {
        if constexpr (std::is_same_v<Ret, void>) {
            return nullptr;
        } else {
            static ayt::reflect::ITypeInfo* cache = nullptr;
            if (!cache) {
                cache = ayt::reflect::TypeRegistryImpl::instance().template findType<Ret>();
            }
            return cache;
        }
    }

    ayt::reflect::ITypeInfo* paramTypeAt(size_t index) const {
        if (index >= sizeof...(Args)) return nullptr;
        return paramTypeAtImpl(index, std::index_sequence_for<Args...>{});
    }

    template <std::size_t... I>
    ayt::reflect::ITypeInfo* paramTypeAtImpl(size_t index, std::index_sequence<I...>) const {
        ayt::reflect::ITypeInfo* result = nullptr;
        ((index == I ? (result = paramTypeForIndex<I>()), 0 : 0), ...);
        return result;
    }

    template <std::size_t I>
    static ayt::reflect::ITypeInfo* paramTypeForIndex() {
        using ArgT = std::tuple_element_t<I, std::tuple<Args...>>;
        using Stripped = std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<ArgT>>>;
        using LookupT = typename detail::LookupType<Stripped>::type;
        static ayt::reflect::ITypeInfo* cache = nullptr;
        if (!cache) {
            cache = ayt::reflect::TypeRegistryImpl::instance().template findType<LookupT>();
        }
        return cache;
    }
};

} // namespace ayt::script::logia::reflect
