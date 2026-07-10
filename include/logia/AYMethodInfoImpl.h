#pragma once
// ============================================================
// AYMethodInfoImpl.h - AYScript-private MethodInfoImpl<T,Ret,Args...>
//
// S3.12 (track R2 §5.7.4): variadic member-function-pointer → IMethodInfo
// adapter. Lives in AYScript (not AYReflect) so that the foundation TU
// (`AYReflect.cpp`) never sees this template — the previous S3.12 attempt
// hit MSVC C2275/C2641 errors on `VectorTypeInfo<ayt::math::Bool>` once
// MethodInfoImpl's `Args...` pack became visible to that TU. Keeping the
// variadic template in an AYScript-private header isolates it from
// every consumer that just needs `ITypeInfo::addMethod(IMethodInfo*)`.
//
// Usage (in AY_PROPERTY / AY_METHOD registrar codepaths or in tests):
//   struct Player { int hp = 0;
//       void heal(int amount) { hp += amount; }
//   };
//   auto* ti = static_cast<TypeInfoImpl<Player>*>(
//                  TypeRegistryImpl::instance().findType<Player>());
//   ti->addMethod(new ayt::script::logia::reflect::MethodInfoImpl<
//                     Player, void, int>(&Player::heal, "heal"));
//
// Scope (S3.12):
//   - non-overloaded, non-templated member functions only
//   - primitive args + primitive-or-void return
//   - const member functions supported (use MethodInfoImplConst)
// ============================================================

#include "IAYReflect.h"
#include "AYReflect.h"

#include <cstring>
#include <type_traits>
#include <tuple>
#include <utility>
#include <vector>

namespace ayt::script::logia::reflect
{

// ===== Thread-local return buffer =====
//
// invoke() returns `const void*` to a thread-local buffer holding the
// return value (or nullptr for void). The buffer is per-thread, so
// concurrent bridge calls do not race. The buffer's lifetime spans
// the C++ stack frame of invoke() — the bridge copies the result onto
// the Lua stack before any further script execution. For void returns
// the buffer is unused.
namespace detail
{
    inline std::vector<uint8_t>& tlsReturnBuffer()
    {
        thread_local std::vector<uint8_t> buf;
        return buf;
    }
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

private:
    template <std::size_t... I>
    const void* invokeImpl(T* self, const void* const* args, std::index_sequence<I...>) const {
        if constexpr (std::is_same_v<Ret, void>) {
            (self->*_pmf)(readArg<I>(args)...);
            return nullptr;
        } else {
            Ret r = (self->*_pmf)(readArg<I>(args)...);
            auto& buf = detail::tlsReturnBuffer();
            buf.assign(sizeof(Ret), 0);
            std::memcpy(buf.data(), &r, sizeof(Ret));
            return buf.data();
        }
    }

    template <std::size_t I>
    static auto readArg(const void* const* args) {
        using ArgT = std::tuple_element_t<I, std::tuple<Args...>>;
        if constexpr (std::is_same_v<ArgT, bool>) {
            // Bridge memcpys a uint8_t into our slot; cast back to bool.
            uint8_t v = 0;
            if (args && args[I]) std::memcpy(&v, args[I], sizeof(uint8_t));
            return static_cast<bool>(v != 0);
        } else {
            ArgT v{};
            if (args && args[I]) std::memcpy(&v, args[I], sizeof(ArgT));
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
        static ayt::reflect::ITypeInfo* cache = nullptr;
        if (!cache) {
            cache = ayt::reflect::TypeRegistryImpl::instance().template findType<ArgT>();
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
            auto& buf = detail::tlsReturnBuffer();
            buf.assign(sizeof(Ret), 0);
            std::memcpy(buf.data(), &r, sizeof(Ret));
            return buf.data();
        }
    }

    template <std::size_t I>
    static auto readArg(const void* const* args) {
        using ArgT = std::tuple_element_t<I, std::tuple<Args...>>;
        if constexpr (std::is_same_v<ArgT, bool>) {
            uint8_t v = 0;
            if (args && args[I]) std::memcpy(&v, args[I], sizeof(uint8_t));
            return static_cast<bool>(v != 0);
        } else {
            ArgT v{};
            if (args && args[I]) std::memcpy(&v, args[I], sizeof(ArgT));
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
        static ayt::reflect::ITypeInfo* cache = nullptr;
        if (!cache) {
            cache = ayt::reflect::TypeRegistryImpl::instance().template findType<ArgT>();
        }
        return cache;
    }
};

} // namespace ayt::script::logia::reflect
