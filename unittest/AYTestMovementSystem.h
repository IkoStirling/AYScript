// AYTestMovementSystem.h - test fixture for the System host (LG-04) path.
//
// A minimal ISystem subclass that counts how many times its onUpdate is
// invoked. The Logia `script MovementSystem { on_update() { ... } }` block
// is loaded by the unit test under the same name; ScriptSubSystem
// dispatches the bridge call to this C++ instance per tick.
//
// Why a separate header (and not in examples/): the MovementSystem type
// is test-only — exposing it in examples/ would force examples/ to link
// against AYEntity, which is currently a runtime concern. Keeping the
// fixture next to the tests that consume it avoids that coupling.

#pragma once

#include <IAYEntity.h>
#include <AYWorld.h>

// AYReflect metadata macros for the test fixture's C++ field. Putting
// the finalize macro here (not in AYEntity) keeps AYEntity's public
// header surface clean — MovementSystem is test-only.
#include <AYSerializer/PropertyMacros.h>

#include <cstdint>

namespace ayt::script::test
{

class MovementSystem final : public ayt::entity::ISystem {
public:
#define AY_CURRENT_CLASS MovementSystem
    MovementSystem()
        : _name("MovementSystem"), _updates(0), _lastDt(0.0f) {}

    const char* getName() const override { return _name; }
    void onUpdate(float dt) override {
        ++_updates;
        _lastDt = dt;
    }

    // Test hooks — read by Test_LogiaSystemHost.cpp.
    int  updates() const { return _updates; }
    float lastDt() const { return _lastDt; }

    // AY_PROPERTY field — Logia `self.moveSpeed` resolves against this
    // entry in AYReflect's TypeRegistry. The S3.1 codegen still emits
    // `self.moveSpeed` as a bare Lua member access (usertype binding is
    // S3.x); the AYReflect registration only feeds the analyzer.
    AY_PROPERTY(float, moveSpeed, ayt::reflect::FieldAttribute::None);
#undef AY_CURRENT_CLASS

private:
    const char* _name;
    int   _updates;
    float _lastDt;
};

} // namespace ayt::script::test

// AYReflect registration under the explicit name "MovementSystem" (must
// match the Logia script name and ISystem::getName()). We bypass
// AY_FINALIZE_REGISTRATION_METADATA here because that macro registers
// under the macro arg's last token, and we need the registry entry's
// name to be the un-namespaced "MovementSystem" string.
// Done in Test_LogiaSystemHost.cpp's static-init helper instead.
