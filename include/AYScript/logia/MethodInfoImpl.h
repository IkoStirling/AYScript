#pragma once
// ============================================================
// AYScript/logia/AYScript/logia/AYScript/logia/AYScript/logia/MethodInfoImpl.h - AYScript-private MethodInfoImpl<T,Ret,Args...>
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
// Non-void returns are kept in a typed thread-local slot until the
// bridge has copied them to Lua.
// ============================================================

#include "AYReflect/IReflect.h"
#include "AYReflect.h"

#include <cstring>
#include <memory>
#include <optional>
#include <type_traits>
#include <tuple>
#include <utility>
#include <vector>
#include <array>   // R4.1 (2026-07-13): std::array<T, N> in readArg
#include <typeinfo>

namespace ayt::script::logia::reflect
{

namespace detail
{
    template <typename R>
    const void* storeReturnValue(R&& value)
    {
        using Stored = std::remove_cv_t<std::remove_reference_t<R>>;
        // A typed optional provides correct alignment and runs the previous
        // value's destructor before replacement. This avoids constructing
        // arbitrary objects inside std::string::data(), which is undefined
        // for over-aligned and non-trivial return types.
        thread_local std::optional<Stored> slot;
        slot.emplace(std::forward<R>(value));
        return std::addressof(*slot);
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
    //
    // Note: readArg's ArgT is the *raw* PMF param type, which may
    // be const std::vector<int>& (input) or std::vector<int>&
    // (out-param) or std::vector<int> (by-value). The trait must
    // match the *underlying* std::vector, so we strip ref/const
    // in the trait's argument before dispatching.
    template <typename>
    struct is_std_vector : std::false_type {};
    template <typename T, typename A>
    struct is_std_vector<std::vector<T, A>> : std::true_type {};
    template <typename X>
    inline constexpr bool is_std_vector_v = is_std_vector<
        std::remove_cv_t<std::remove_reference_t<X>>>::value;

    template <typename>
    struct is_std_array : std::false_type {};
    template <typename T, size_t N>
    struct is_std_array<std::array<T, N>> : std::true_type {};
    template <typename X>
    inline constexpr bool is_std_array_v = is_std_array<
        std::remove_cv_t<std::remove_reference_t<X>>>::value;

    // R4.2 (2026-07-13): is_out_param_v<ArgT> is true iff ArgT is
    // a non-const lvalue reference or a non-const pointer. The
    // bridge uses this to heap-allocate a fresh T and pass a
    // writable reference/pointer; readArg's out-param branch
    // returns a reference/pointer into the slot. const T& /
    // const T* / by-value T are NOT out-params (input-only).
    //
    // Implementation note: the partial specializations match the
    // *stripped* T, then we re-check the const qualification of
    // the original ArgT. `T&` would match both `const int&` and
    // `int&` because the partial spec doesn't see const; we
    // therefore use SFINAE on the original ArgT to disambiguate.
    template <typename ArgT, typename = void>
    struct is_out_param : std::false_type {};
    // non-const T&: enable_if checks the original ArgT is not const.
    template <typename ArgT>
    struct is_out_param<ArgT, std::enable_if_t<
        std::is_lvalue_reference_v<ArgT> &&
        !std::is_const_v<std::remove_reference_t<ArgT>>>> : std::true_type {};
    // non-const T*: similar.
    template <typename ArgT>
    struct is_out_param<ArgT, std::enable_if_t<
        std::is_pointer_v<ArgT> &&
        !std::is_const_v<std::remove_pointer_t<ArgT>>>> : std::true_type {};
    template <typename T>
    inline constexpr bool is_out_param_v = is_out_param<T>::value;
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
        // R4.2 (2026-07-13): direct pack expansion preserves
        // lvalue ref category for out-params (T& / T*). For
        // input-only types the result is by value; the PMF binds
        // a const ref or by-value param to it. Both impls here
        // and in the const specialization use this form.
        if constexpr (std::is_same_v<Ret, void>) {
            (self->*_pmf)(read<I>(args)...);
            return nullptr;
        } else {
            Ret r = (self->*_pmf)(read<I>(args)...);
            return detail::storeReturnValue(std::move(r));
        }
    }

    // readArg<I>(args) — uniform slot-dispatch.
    //
    // The bridge places a pointer at args[I] for every parameter
    // (see file-level "Slot convention" comment). readArg returns the
    // parameter by value to the PMF call; for const T& / T* / etc.,
    // the implicit conversion gives the PMF the right reference.
    // R4.2 (2026-07-13): out-param readArg — returns T& (for T&)
    // or T* (for T*) into the slot. Called by invokeImpl's
    // if-constexpr dispatch when ArgT is a non-const lvalue ref
    // or non-const pointer (the C++ "out-param" idiom). The PMF
    // writes through this; the bridge reads the heap T back to
    // Lua in the post-invoke write-back loop.
    //
    // Two overloads — one for T&, one for T* — so each has a
    // single deduced return type (avoids MSVC C3487 with mixed
    // ref/pointer return paths in the same template).
    template <std::size_t I, typename ArgT_ = std::tuple_element_t<I, std::tuple<Args...>>,
              std::enable_if_t<std::is_lvalue_reference_v<ArgT_>, int> = 0>
    static ArgT_ readOutArg(const void* const* args) {
        using ArgT = ArgT_;
        using Target = std::remove_reference_t<ArgT>;
        const void* slot = (args && args[I]) ? args[I] : nullptr;
        if (!slot) {
            static thread_local Target fallback{};
            return fallback;
        }
        Target* p = static_cast<Target*>(const_cast<void*>(slot));
        return static_cast<ArgT>(*p);
    }

    template <std::size_t I, typename ArgT_ = std::tuple_element_t<I, std::tuple<Args...>>,
              std::enable_if_t<std::is_pointer_v<ArgT_>, int> = 0>
    static ArgT_ readOutArg(const void* const* args) {
        using ArgT = ArgT_;
        using Target = std::remove_pointer_t<ArgT>;
        const void* slot = (args && args[I]) ? args[I] : nullptr;
        if (!slot) {
            static thread_local Target zero{};
            return &zero;
        }
        Target* p = static_cast<Target*>(const_cast<void*>(slot));
        return p;
    }

    template <std::size_t I>
    static decltype(auto) read(const void* const* args) {
        using ArgT = std::tuple_element_t<I, std::tuple<Args...>>;
        if constexpr (detail::is_out_param_v<ArgT>) {
            return readOutArg<I>(args);
        } else {
            return readArg<I>(args);
        }
    }

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
            // Note: ArgT may be `const std::vector<T>&` (input) or
            // `std::vector<T>&` (out-param). Strip the reference
            // when casting slot to a pointer (you can't have a
            // pointer to a reference).
            if (!slot) return ArgT{};
            const auto* vp = static_cast<const std::remove_reference_t<ArgT>*>(slot);
            return ArgT(*vp);
        } else if constexpr (detail::is_std_array_v<ArgT>) {
            // R4.1 (2026-07-13): std::vector<T> by value (or by
            // const-ref, which is the common PMF signature). The
            // bridge heap-allocates the vector and stores the
            // pointer in the slot. We copy-construct the parameter
            // from that heap pointer; the bridge deletes the heap
            // pointer after invoke() returns via the cleanup queue.
            if (!slot) return ArgT{};
            const auto* vp = static_cast<const std::remove_reference_t<ArgT>*>(slot);
            return ArgT(*vp);
        } else if constexpr (detail::is_std_array_v<ArgT>) {
            // R4.1 (2026-07-13): std::array<T, N> by value (or by
            // const-ref). Same slot convention as std::vector.
            if (!slot) return ArgT{};
            const auto* ap = static_cast<const std::remove_reference_t<ArgT>*>(slot);
            return ArgT(*ap);
        } else if constexpr (std::is_pointer_v<ArgT>) {
            // T* / const T* / etc. — slot is a T* (pointer to T,
            // e.g. heap-allocated T for an out-param, or pointer
            // to a stack slot for an in-param). We pass through
            // with the right const qualification so the PMF gets
            // a pointer it can write through (T*) or read from
            // (const T*). R4.2: T* out-params are now correctly
            // supported because the bridge stores `argPtrs[i] =
            // heapPtr` (a T*) and the PMF gets a writable T*.
            if (!slot) return ArgT{};
            using Inner = std::remove_cv_t<std::remove_pointer_t<ArgT>>;
            auto* p = static_cast<Inner*>(const_cast<void*>(slot));
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
            // R4.2 (2026-07-13): decltype(auto) return preserves the
            // reference category. For non-const T& (out-param), the
            // PMF gets a writable reference into the slot; for const
            // T& (input), the static_cast adds the const so the
            // deduced return type matches ArgT exactly.
            return static_cast<ArgT>(*static_cast<Target*>(const_cast<void*>(slot)));
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
            using Stripped = std::remove_cv_t<std::remove_reference_t<Ret>>;
            using LookupT = typename detail::LookupType<Stripped>::type;
            static ayt::reflect::ITypeInfo* cache = nullptr;
            if (!cache) {
                cache = ayt::reflect::TypeRegistryImpl::instance().template findType<Stripped>();
                if (!cache) {
                    cache = ayt::reflect::TypeRegistryImpl::instance().template findType<LookupT>();
                }
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
        // the registered ITypeInfo. R3.5: prefer the stripped type
        // itself (so a registered enum resolves to EnumTypeInfo);
        // fall back to underlying integral for unregistered enums
        // (R3.0 int path via detail::LookupType).
        using Stripped = std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<ArgT>>>;
        using LookupT = typename detail::LookupType<Stripped>::type;
        static ayt::reflect::ITypeInfo* cache = nullptr;
        if (!cache) {
            cache = ayt::reflect::TypeRegistryImpl::instance().template findType<Stripped>();
            if (!cache) {
                cache = ayt::reflect::TypeRegistryImpl::instance().template findType<LookupT>();
            }
        }
        return cache;
    }

    // R4.2 (2026-07-13): out-param detection. True iff ArgT is
    // a non-const lvalue reference or a non-const pointer; the
    // C++ idiom for "write-back" parameters. const T& / const T* /
    // by-value T are NOT out-params.
    bool getParamIsOut(size_t index) const override {
        if (index >= sizeof...(Args)) return false;
        return paramIsOutAtImpl(index, std::index_sequence_for<Args...>{});
    }

    template <std::size_t... I>
    bool paramIsOutAtImpl(size_t index, std::index_sequence<I...>) const {
        bool result = false;
        ((index == I ? (result = paramIsOutForIndex<I>()), 0 : 0), ...);
        return result;
    }

    template <std::size_t I>
    static constexpr bool paramIsOutForIndex() {
        using ArgT = std::tuple_element_t<I, std::tuple<Args...>>;
        if constexpr (std::is_lvalue_reference_v<ArgT>) {
            // T& (non-const lvalue ref) is out; const T& is not.
            return !std::is_const_v<std::remove_reference_t<ArgT>>;
        } else if constexpr (std::is_pointer_v<ArgT>) {
            // T* (non-const pointer) is out; const T* is not.
            return !std::is_const_v<std::remove_pointer_t<ArgT>>;
        }
        return false;
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
        // R4.2 (2026-07-13): see non-const invokeImpl — direct
        // pack expansion preserves lvalue ref for out-params.
        if constexpr (std::is_same_v<Ret, void>) {
            (self->*_pmf)(read<I>(args)...);
            return nullptr;
        } else {
            Ret r = (self->*_pmf)(read<I>(args)...);
            return detail::storeReturnValue(std::move(r));
        }
    }

    // R4.2 (2026-07-13): out-param readArg — returns T& (for T&)
    // or T* (for T*) into the slot. Called by invokeImpl's
    // if-constexpr dispatch when ArgT is a non-const lvalue ref
    // or non-const pointer (the C++ "out-param" idiom). The PMF
    // writes through this; the bridge reads the heap T back to
    // Lua in the post-invoke write-back loop.
    //
    // Two overloads — one for T&, one for T* — so each has a
    // single deduced return type (avoids MSVC C3487 with mixed
    // ref/pointer return paths in the same template).
    template <std::size_t I, typename ArgT_ = std::tuple_element_t<I, std::tuple<Args...>>,
              std::enable_if_t<std::is_lvalue_reference_v<ArgT_>, int> = 0>
    static ArgT_ readOutArg(const void* const* args) {
        using ArgT = ArgT_;
        using Target = std::remove_reference_t<ArgT>;
        const void* slot = (args && args[I]) ? args[I] : nullptr;
        if (!slot) {
            static thread_local Target fallback{};
            return fallback;
        }
        Target* p = static_cast<Target*>(const_cast<void*>(slot));
        return static_cast<ArgT>(*p);
    }

    template <std::size_t I, typename ArgT_ = std::tuple_element_t<I, std::tuple<Args...>>,
              std::enable_if_t<std::is_pointer_v<ArgT_>, int> = 0>
    static ArgT_ readOutArg(const void* const* args) {
        using ArgT = ArgT_;
        using Target = std::remove_pointer_t<ArgT>;
        const void* slot = (args && args[I]) ? args[I] : nullptr;
        if (!slot) {
            static thread_local Target zero{};
            return &zero;
        }
        Target* p = static_cast<Target*>(const_cast<void*>(slot));
        return p;
    }

    template <std::size_t I>
    static decltype(auto) read(const void* const* args) {
        using ArgT = std::tuple_element_t<I, std::tuple<Args...>>;
        if constexpr (detail::is_out_param_v<ArgT>) {
            return readOutArg<I>(args);
        } else {
            return readArg<I>(args);
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
            // R4.1 (2026-07-13): std::vector<T> by value (or by
            // const-ref, which is the common PMF signature). The
            // bridge heap-allocates the vector and stores the
            // pointer in the slot. We copy-construct the parameter
            // from that heap pointer; the bridge deletes the heap
            // pointer after invoke() returns via the cleanup queue.
            // Note: ArgT may be `const std::vector<T>&` (input) or
            // `std::vector<T>&` (out-param). Strip the reference
            // when casting slot to a pointer (you can't have a
            // pointer to a reference).
            if (!slot) return ArgT{};
            const auto* vp = static_cast<const std::remove_reference_t<ArgT>*>(slot);
            return ArgT(*vp);
        } else if constexpr (detail::is_std_array_v<ArgT>) {
            // R4.1: std::array<T, N> mirror. Strip ref from ArgT
            // before casting to pointer (see vector branch comment).
            if (!slot) return ArgT{};
            const auto* ap = static_cast<const std::remove_reference_t<ArgT>*>(slot);
            return ArgT(*ap);
        } else if constexpr (std::is_pointer_v<ArgT>) {
            // R4.2: slot is T* (pointer to T); pass through.
            // Mirror of the non-const impl's pointer branch.
            if (!slot) return ArgT{};
            using Inner = std::remove_cv_t<std::remove_pointer_t<ArgT>>;
            auto* p = static_cast<Inner*>(const_cast<void*>(slot));
            return const_cast<ArgT>(p);
        } else if constexpr (std::is_lvalue_reference_v<ArgT>) {
            if (!slot) {
                static thread_local std::remove_reference_t<ArgT> fallback{};
                return fallback;
            }
            using Target = std::remove_reference_t<ArgT>;
            // R4.2: decltype(auto) preserves the reference category.
            // const T& gets a const reference; T& gets a writable
            // reference into the slot (out-param write-back).
            return static_cast<ArgT>(*static_cast<Target*>(const_cast<void*>(slot)));
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
            using Stripped = std::remove_cv_t<std::remove_reference_t<Ret>>;
            using LookupT = typename detail::LookupType<Stripped>::type;
            static ayt::reflect::ITypeInfo* cache = nullptr;
            if (!cache) {
                cache = ayt::reflect::TypeRegistryImpl::instance().template findType<Stripped>();
                if (!cache) {
                    cache = ayt::reflect::TypeRegistryImpl::instance().template findType<LookupT>();
                }
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
        // R3.5: prefer stripped type (registered EnumTypeInfo); fall
        // back to underlying integral for unregistered enums.
        using Stripped = std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<ArgT>>>;
        using LookupT = typename detail::LookupType<Stripped>::type;
        static ayt::reflect::ITypeInfo* cache = nullptr;
        if (!cache) {
            cache = ayt::reflect::TypeRegistryImpl::instance().template findType<Stripped>();
            if (!cache) {
                cache = ayt::reflect::TypeRegistryImpl::instance().template findType<LookupT>();
            }
        }
        return cache;
    }

    // R4.2 (2026-07-13): out-param detection (mirrors non-const impl).
    bool getParamIsOut(size_t index) const override {
        if (index >= sizeof...(Args)) return false;
        return paramIsOutAtImpl(index, std::index_sequence_for<Args...>{});
    }

    template <std::size_t... I>
    bool paramIsOutAtImpl(size_t index, std::index_sequence<I...>) const {
        bool result = false;
        ((index == I ? (result = paramIsOutForIndex<I>()), 0 : 0), ...);
        return result;
    }

    template <std::size_t I>
    static constexpr bool paramIsOutForIndex() {
        using ArgT = std::tuple_element_t<I, std::tuple<Args...>>;
        if constexpr (std::is_lvalue_reference_v<ArgT>) {
            return !std::is_const_v<std::remove_reference_t<ArgT>>;
        } else if constexpr (std::is_pointer_v<ArgT>) {
            return !std::is_const_v<std::remove_pointer_t<ArgT>>;
        }
        return false;
    }
};

} // namespace ayt::script::logia::reflect
