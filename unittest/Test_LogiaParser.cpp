// Logia parser unit tests (S0)

#include "AYScript.h"
#include "AYTest.h"

using namespace ayt::script::logia;

namespace {

const char* kPlayerController = R"(
component PlayerController {
    export var speed: float = 5.0
    export var jump_force: float = 8.0

    var transform: Transform

    on_start(entity: Entity) {
        transform = entity.get_component(Transform)
    }

    on_update(dt: float) {
        if input.is_pressed("jump") {
            transform.position.y += jump_force * dt
        }
        transform.position.x += speed * dt
    }

    on_destroy() {
        log.info("PlayerController destroyed")
    }
}
)";

VarDeclStmt* findVarDecl(const ComponentDecl& component, const char* name)
{
    for (const auto& member : component.members) {
        if (auto* varDecl = dynamic_cast<VarDeclStmt*>(member.get())) {
            if (varDecl->name == name) {
                return varDecl;
            }
        }
    }
    return nullptr;
}

LifecycleFuncDecl* findLifecycle(const ComponentDecl& component, LifecycleKind kind)
{
    for (const auto& member : component.members) {
        if (auto* lifecycle = dynamic_cast<LifecycleFuncDecl*>(member.get())) {
            if (lifecycle->kind == kind) {
                return lifecycle;
            }
        }
    }
    return nullptr;
}

} // namespace

TEST_SUITE(LogiaParserTests)

TEST_CASE(parse_player_controller) {
    Compiler compiler;
    const CompileResult result = compiler.compile(kPlayerController);

    CHECK(result.success);
    CHECK(result.errors.empty());
    CHECK(result.program != nullptr);
    CHECK(result.program->components.size() == 1u);

    const ComponentDecl& component = *result.program->components[0];
    CHECK(component.name == "PlayerController");

    VarDeclStmt* speed = findVarDecl(component, "speed");
    CHECK(speed != nullptr);
    CHECK(speed->exported);
    CHECK(speed->typeName == "float");

    VarDeclStmt* transform = findVarDecl(component, "transform");
    CHECK(transform != nullptr);
    CHECK_FALSE(transform->exported);
    CHECK(transform->typeName == "Transform");

    LifecycleFuncDecl* onStart = findLifecycle(component, LifecycleKind::OnStart);
    CHECK(onStart != nullptr);
    CHECK(onStart->params.size() == 1u);
    CHECK(onStart->params[0].name == "entity");
    CHECK(onStart->params[0].typeName == "Entity");
    CHECK_FALSE(onStart->body.empty());

    LifecycleFuncDecl* onUpdate = findLifecycle(component, LifecycleKind::OnUpdate);
    CHECK(onUpdate != nullptr);
    CHECK(onUpdate->params.size() == 1u);
    CHECK(onUpdate->params[0].name == "dt");

    LifecycleFuncDecl* onDestroy = findLifecycle(component, LifecycleKind::OnDestroy);
    CHECK(onDestroy != nullptr);
    CHECK(onDestroy->params.empty());
}

TEST_CASE(parse_if_without_parens) {
    const char* source = R"(
component T {
    on_update(dt: float) {
        if flag {
            x = 1
        }
    }
}
)";
    Compiler compiler;
    const CompileResult result = compiler.compile(source);
    CHECK(result.success);

    const LifecycleFuncDecl* onUpdate =
        findLifecycle(*result.program->components[0], LifecycleKind::OnUpdate);
    CHECK(onUpdate != nullptr);
    CHECK(onUpdate->body.size() == 1u);

    const auto* ifStmt = dynamic_cast<const IfStmt*>(onUpdate->body[0].get());
    CHECK(ifStmt != nullptr);
    CHECK(ifStmt->condition != nullptr);
    CHECK_FALSE(ifStmt->thenBranch.empty());
}

TEST_SUITE_END
