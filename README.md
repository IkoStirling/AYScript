# AYScript

AYScript 是 Logia 脚本语言与运行时模块，提供 Lexer/Parser/语义分析/Lua 代码生成、反射桥、输入桥和 EventBus ambient API。

## 公开接口

```cpp
#include <AYScript.h>
#include <AYScript/ScriptRuntimeModule.h>
#include <AYScript/ScriptRuntimeBridge.h>
#include <AYScript/ScriptSubSystem.h>
#include <AYScript/logia/Logia.h>
```

默认 Client / Server / Editor Host 通过 `ScriptRuntimeModule`
（`AYScript.Runtime`）安装脚本子系统；直接构造或注册 `ScriptSubSystem` 仍可用于独立 Demo
与测试。

## 依赖

- 公开：AYLog、AYGameLoop、AYEntity、AYReflect、AYEventSystem
- 内部：AYIO、sol2、Lua

Logia 语法、宿主接口和跨模块事件边界见 [design.md](design.md)。

测试按 fast/integration 分层，统计使用注册用例数；入口见[统一测试契约](../../AYDocs/testing.md)。
