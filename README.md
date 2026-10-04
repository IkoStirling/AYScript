# AYScript

AYScript 是 Logia 脚本语言与运行时模块，提供 Lexer/Parser/语义分析/Lua 代码生成、反射桥、输入桥和 EventBus ambient API。

## Checked Logia

生产新脚本使用 `checkedLogiaHostContext(...)`。旧 Component/System/Tool 调用默认保留兼容语义；
Actor 自动加载、编辑器作者分析和使用新语法的文档采用 checked 语义；旧编辑器模板编译保持兼容。它检查词法作用域、初始化/赋值类型、函数参数与
返回路径、生命周期及 signal/EventBus 回调签名。Actor 的 `actorProperties` 必须来自有效属性声明，
`self.transform.position/scale/rotation` 分别为 vec3/vec3/quat。

| 能力 | 入口及规则 |
|---|---|
| 实例状态 | `state name: type = value`；`var` 仍在同一模块内共享。销毁释放，成功重载保留同名同类型状态；`resetInstanceState` 显式重置。 |
| 受控世界操作 | `bindWorld` 后使用 `entity.*` 令牌；创建、销毁、组件/字段修改排队，`flushWorldCommands` 在迭代边界应用。World 关闭使旧令牌失效。 |
| 模块宿主接口 | `registerHostFunction("audio.play", signature, provider)` 等注册标量接口；编译与调用均校验。动画、音频、物理、UI、数值模块由宿主提供具体回调。 |
| 异步任务 | `tasks.start(helper)`，helper 内 `await delay(seconds)` / `await wait_event(alias)`；后续变量帧恢复，销毁、替换和 shutdown 取消。 |
| 模块与数据 | `import "relative.logia"` 引入 helper；`array<T>` 支持嵌套、length/push/pop，1-based 索引有边界保护；vec3/quat 使用 AYMath 的左手坐标与弧度约定。 |
| 作者工具与发布 | `analyzeLogia` 提供诊断、符号补全和定义引用；错误包含原始文件与调用帧。`buildLogiaPackage` 打包源码依赖闭包，`loadScriptPackage` 校验后加载。 |

可运行示例：[checked_actor.logia](examples/checked_actor.logia)。`on_reload()` 在成功替换并清理旧任务/订阅后，
对已有实例调用，用来恢复订阅和任务；其执行错误作为运行时错误报告，不撤销已提交的替换。

```powershell
ays-logia compile examples/checked_actor.logia --host actor --checked
ays-logia analyze examples/checked_actor.logia --host actor --json
ays-logia package examples/checked_actor.logia --host actor -o out/checked-actor
```

包目录含 `logia-package.json` v1 和完整 `.logia` 源码闭包；可以独立移动，运行时仍编译源码。
清单记录宿主/属性 schema、所需 provider 签名、相对文件路径和内容指纹。已有输出目录不会被覆盖。
它不替代项目 Actor/Scene 的 raw/loose 发布规则，也不打包宿主原生代码或外部资源。

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
- 内部：AYIO、AYTime、AYMath、sol2、Lua

Logia 语法、宿主接口和跨模块事件边界见 [design.md](design.md)。

测试按 fast/integration 分层，统计使用注册用例数；入口见[统一测试契约](../../AYDocs/testing.md)。
