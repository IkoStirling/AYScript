#pragma once
// ============================================================
// AYMethodRegistrarBridge.h - AYScript-side bridge between
// AY_PROPERTY/AY_METHOD registrars and the AYScript-private
// variadic MethodInfoImpl<T,Ret,Args...>.
//
// S3.12 (track R2 §5.7.4) — `AY_METHOD(Ret, name, args...)` macro
// generates an `AY_MethodRegistrarOf<T, Ret, Args...>` registrar
// inside the foundation AYPropertyMacros.h. The registrar's
// `buildMethodInfo()` is intentionally left undefined there so the
// foundation TU (`AYSerializer.cpp`) doesn't need to know about
// the variadic `MethodInfoImpl<T,Ret,Args...>` (which would re-trigger
// the MSVC C2275 / C3329 parser bug on `VectorTypeInfo<ayt::math::Bool>`
// that motivated keeping the variadic pack in an AYScript-private
// header in the first place).
//
// The body of `buildMethodInfo()` is provided inline below — when
// AYScript code includes this header, the variadic `MethodInfoImpl`
// becomes available and the registrar's builder resolves cleanly.
// User code that uses `AY_METHOD` must include this header (or a
// facade that includes it, like AYScriptComponent.h's reflect
// aggregator) before `AY_FINALIZE_REGISTRATION_METADATA` runs.
//
// Usage in a finalizer walk:
//   for (auto* reg : registrars) {
//       if (reg->isMethod()) {
//           typeInfo->addMethod(
//               static_cast<ayt::reflect::IMethodInfo*>(
//                   reg->buildMethodInfo()));
//       }
//   }
// ============================================================

#include "ayserializer/PropertyMacros.h"
#include "AYMethodInfoImpl.h"

namespace ayt::serializer::detail
{

// Build the IMethodInfo* for an AY_MethodRegistrarOf (non-const PMF).
// The Pmf template parameter must match the partial specialization's
// signature: `Ret (T::*)(Args...)` (no const). The MSVC parser
// resolves to the partial specialization at the call site based on
// whether the function is const-qualified.
template <typename T, typename Ret, typename... Args,
          Ret (T::*Pmf)(Args...), int Index>
void* AY_MethodRegistrarOf<T, Ret, AY_MethodArgs<Args...>, Pmf, Index>
    ::buildMethodInfo() const
{
    return new ::ayt::script::logia::reflect::MethodInfoImpl<
        T, Ret, Args...>(name, Pmf);
}

// Build the IMethodInfo* for an AY_MethodRegistrarOf (const PMF).
template <typename T, typename Ret, typename... Args,
          Ret (T::*Pmf)(Args...) const, int Index>
void* AY_MethodRegistrarOf<T, Ret, AY_MethodArgs<Args...>, Pmf, Index>
    ::buildMethodInfo() const
{
    return new ::ayt::script::logia::reflect::MethodInfoImplConst<
        T, Ret, Args...>(name, Pmf);
}

} // namespace ayt::serializer::detail

// =====================================================================
// AY_FINALIZE_METHODS(T)
// =====================================================================
// S3.12 / track R2 §5.7.4 — companion macro to AY_FINALIZE_REGISTRATION_METADATA.
// The foundation finalize walk skip-skips AY_METHOD entries (the
// variadic MethodInfoImpl lives in an AYScript-private header to keep
// the foundation TU free of the variadic pack). After the foundation
// metadata walk, the user invokes `AY_FINALIZE_METHODS(T)` (which
// must be in a TU that includes this header) to walk the registrar
// list and call `addMethod()` on the ITypeInfo for each method entry.
//
// Usage:
//   // In a TU that includes logia/AYMethodRegistrarBridge.h:
//   AY_FINALIZE_REGISTRATION_METADATA(MyPlayer)
//   AY_FINALIZE_METHODS(MyPlayer)
//
// Multiple `AY_FINALIZE_METHODS` calls are idempotent — the
// `findMethod` guard inside prevents double registration.
// =====================================================================
#define AY_FINALIZE_METHODS(T) \
    namespace { \
        struct AYT_FinalizeMethods_##T { \
            AYT_FinalizeMethods_##T() { \
                using Type = T; \
                auto* typeInfo = ::ayt::reflect::TypeRegistryImpl::instance().findType<Type>(); \
                if (!typeInfo) return; \
                auto regs = ::ayt::serializer::detail::CollectAndSortRegistrars(#T); \
                for (auto* reg : regs) { \
                    if (!reg->isMethod()) continue; \
                    if (typeInfo->findMethod(reg->name) != nullptr) continue; \
                    typeInfo->addMethod( \
                        static_cast<::ayt::reflect::IMethodInfo*>( \
                            reg->buildMethodInfo())); \
                } \
            } \
        }; \
        static AYT_FinalizeMethods_##T AYT_g_FinalizeMethods_##T; \
    }
