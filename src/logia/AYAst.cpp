// AYAst.cpp

#include "logia/AYAst.h"

namespace ayt::script::logia
{

const char* lifecycleKindName(LifecycleKind kind)
{
    switch (kind) {
    case LifecycleKind::OnStart:
        return "on_start";
    case LifecycleKind::OnUpdate:
        return "on_update";
    case LifecycleKind::OnDestroy:
        return "on_destroy";
    }
    return "unknown";
}

} // namespace ayt::script::logia
