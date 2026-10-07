# AYScript

AYScript 是 Logia 脚本语言与运行时模块，提供 Lexer/Parser/语义分析/Lua 代码生成、反射桥、输入桥和 EventBus ambient API。

## Durable script state

Checked 脚本用 `script Player version 2 { persist score: int = 0 ... }` 显式声明存档版本和字段。
普通 `state` 保持临时状态；任务、订阅、voice/entity/hit/context 令牌应放在临时字段。
`storage.snapshot():string` 生成当前实例的 JSON；`storage.restore(json):bool` 校验并恢复当前实例。
默认 Host 的 `save.write(slot,json)` / `save.read(slot)` / `save.remove(slot)` 使用项目 SaveGameService。
旧数据必须经纯 helper `migrate(from:int,data:table):table` 转换，再按当前 schema 验证。
成功恢复重置临时字段、取消旧任务和订阅，然后运行 `on_restore()`；作者在此重新绑定运行期行为。
失败的校验/迁移不提交字段。hook 失败发生在提交后，通过 storage.error/getLastError 报告。
格式 1 限制 1 MiB、65536 个值、typed array 8 层；标量/向量/typed array 可保存，动态 table/借用对象拒绝。
示例：[persistent_actor.logia](examples/persistent_actor.logia)。C++ 宿主使用 snapshotPersistentState/restorePersistentState。

## Checked Logia

生产新脚本使用 `checkedLogiaHostContext(...)`。旧 Component/System/Tool 调用默认保留兼容语义；
Actor 自动加载、编辑器作者分析和使用新语法的文档采用 checked 语义；旧编辑器模板编译保持兼容。它检查词法作用域、初始化/赋值类型、函数参数与
返回路径、生命周期及 signal/EventBus 回调签名。Actor 的 `actorProperties` 必须来自有效属性声明，
`self.transform.position/scale/rotation` 分别为 vec3/vec3/quat。

| 能力 | 入口及规则 |
|---|---|
| 实例状态 | `state name: type = value`；`var` 仍在同一模块内共享。销毁释放，成功重载保留同名同类型状态；`resetInstanceState` 显式重置。 |
| 受控世界操作 | `bindWorld` 后使用 `entity.*` 令牌；创建、销毁、组件/字段修改排队，`flushWorldCommands` 在迭代边界应用。World 关闭使旧令牌失效。 |
| 模块宿主接口 | `registerHostFunction` 注册标量/vec3 provider；编译与调用均校验。默认 Application/Editor 装配安装基础音频、动画、物理与游戏 UI 函数；独立宿主显式安装。 |
| 异步任务 | `tasks.start(helper, args...)` 类型检查并捕获参数；`await wait_event("alias") into (name: type, ...)` 接收首个标量 payload。后续变量帧恢复，销毁、替换和 shutdown 取消。 |
| 模块与数据 | `import "relative.logia"` 引入 helper；`array<T>` 支持嵌套、length/push/pop，1-based 索引有边界保护；vec3/quat 使用 AYMath 的左手坐标与弧度约定。 |
| 作者工具与发布 | `analyzeLogia` 提供诊断、符号补全和定义引用；错误包含原始文件与调用帧。`buildLogiaPackage` 打包源码依赖闭包，`loadScriptPackage` 校验后加载。 |

可运行示例：[checked_actor.logia](examples/checked_actor.logia)。`on_reload()` 在成功替换并清理旧任务/订阅后，
对已有实例调用，用来恢复订阅和任务；其执行错误作为运行时错误报告，不撤销已提交的替换。

```powershell
ays-logia compile examples/checked_actor.logia --host actor --checked
ays-logia analyze examples/checked_actor.logia --host actor --json
ays-logia package examples/checked_actor.logia --host actor -o out/checked-actor
```

包目录含 `logia-package.json` v2、完整 `.logia` 源码闭包和显式声明资源；可以独立移动，运行时仍编译源码。
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

## 默认引擎函数与项目发布

默认 Client/Server/Editor 的 Script 装配由 AYApplication 安装 42 个 audio/animation/physics/ui/world/gameflow/save 函数。
`world.request(id)` 在 Egress 切换；`gameflow.request(intent)` 排队提交空 payload 意图；
`ui.emit_signal(id)` 驱动 UIFlow 信号或应用命令。请求返回接受状态，实际切换在后续运行时 tick。
示例：[gameplay_flow.logia](examples/gameplay_flow.logia)。
缺少可选服务时 available 返回 false，创建返回 0，修改返回 false；无效参数与过期查询读取报告运行时错误。
`entity.self()` 返回当前 Actor 的 World 令牌；使用 state 保存 voice/hit 令牌，销毁时 stop/release。
`engineLogiaHostContext` 仅提供作者签名；CLI `--engine-host` 开启 checked 并使用同一份签名。

```powershell
ays-logia compile examples/engine_host.logia --host actor --engine-host
ays-logia analyze examples/engine_host.logia --host actor --engine-host --json
```

原生 provider 支持标量与有限 vec3。独立 bridge 仍需显式安装 provider；元数据不授予运行权限。
正常 ProjectBuild 自动收集、校验并以 raw/loose 发布 import 闭包。详见
[默认接口与发布契约](../../AYDocs/LOGIA-CHECKED-CAPABILITIES.md#4-默认引擎宿主函数)。

## 异步事件工作流

[async_events.logia](examples/async_events.logia) 展示同步事件 handler 向任务传参和类型化 payload 等待。
`into` 声明当前词法块内的局部变量；位置、数量和类型必须与已知 alias 的签名完全匹配。
Scene alias 的借用指针只允许同步 `event.subscribe` 消费。无 `into` 的等待继续仅用作通知。
task 参数声明为 Scene/Entity 或其嵌套数组时拒绝编译；Entity 的稳定 World 令牌使用 int。
table 内容由调用者保持有效，不能通过表跨帧保存借用指针。
等待不缓存订阅建立前的事件；同一等待取第一个到达事件，其余事件丢弃，不提供事件队列或超时。
标量参数按启动时值传递，array/table/vector 保持引用语义。helper 必须返回 void；含 await 的 helper
仍不能普通调用或直接作为事件 handler。每帧最多恢复 256 个任务，轮转 ID 防止 1024 上限内的饥饿。

```powershell
ays-logia compile examples/async_events.logia --host actor --checked
ays-logia analyze examples/async_events.logia --host actor --json
# 默认模型测试受限于 32 轮；显式长时验收直接运行可执行文件，避免 CTest 的 120 秒超时。
$env:AY_LOGIA_ASYNC_STRESS_SECONDS = '3600'
$env:AY_LOGIA_ASYNC_STRESS_SEED = '20261007'
out/build/windows-debug-vs2026-insider/AYRuntime/AYScript/unittest/AYScript_AsyncStressTests.exe
Remove-Item Env:AY_LOGIA_ASYNC_STRESS_SECONDS, Env:AY_LOGIA_ASYNC_STRESS_SEED
```

压力测试逐操作比较原生模型与 Logia 的状态，交错延时、首事件、实例释放、失败/成功重载和重启。
它接受 0–18000 秒，0 为默认 32 轮；失败输出 seed/round/step 便于确定性重放。
Windows Debug 另用 CRT 堆快照检查 256 次编译/运行时重建后的存活原生分配，覆盖重复注册
Transform 字段元数据导致的泄漏；长时测试的 Lua 预算不能替代这项原生堆检查。
MSVC AddressSanitizer 验收时，匹配编译器版本的 `clang_rt.asan_dynamic-x86_64.dll` 须能被
测试程序和 CLI 定位；运行库未装载的启动超时不属于脚本测试通过。

## Explicit resources

Checked 脚本以 `asset sound:string="audio/start.ayaudio"` 声明 immutable 的 asset-root-relative 路径。
编译器/CLI analysis 记录 resourceDependencies；import 模块也可导出 asset 常量。
普通字符串保持动态语义，不自动收集。运行时允许 DB/pak 虚拟资源；路径以 AYIO PathSafety 检查根边界。
脚本包 v2 原样复制声明资源并记录内容指纹，读取器兼容无资源的 v1；单资源 64 MiB，总资源 256 MiB，最多 128 项。
缺失资源、指纹不符、路径越界、清单冲突拒绝发布/加载。包只复制资源，不替代 cook；目标 Host 仍须有对应 loader。
ProjectBuild 的 resourceDependencies 独立记录资源边，使用项目 transform/storage 规则，排除/缺失引用为错误。
执行前重新检查依赖集合，变更须重新规划；失败不替换上一份输出。示例：examples/resource_refs.logia。

### Source debugger

Owner-thread hosts use startDebugActorLifecycle, setDebugBreakpoints, resumeDebug
and debugSnapshot for real coroutine pause/Continue/Into/Over/Out. Snapshots copy
frames, bounded local values, receiver state and inspectTasks wait information.
Quiesce normal lifecycle/task/event dispatch, keep the Actor alive, then stopDebug
or releaseInstance before destruction. Load/reload/shutdown stop the session.
Async helpers run in the normal scheduler, not the debug lifecycle coroutine.
See unittest/Test_LogiaDebug.cpp and the AYEditor Actor preview toolbar.

### Reusable behaviors

Checked import modules may declare helpers, immutable assets, state/persist fields,
signals and lifecycle hooks. They share one checked declaration namespace: choose
behavior-specific exported names; collisions fail rather than override. Mutable
script-block var exports are rejected. Each canonical file is included once in a
depth-first source traversal; start/update/reload/restore hooks run in that order
and destroy runs in reverse. Each hook retains independent return/local scope.
The entry owns the persistent version/migrate policy; imported version headers
above 1 are rejected. Actor inheritance still replaces a script, while imports
compose behavior explicitly. See examples/composed_actor.logia and behaviors/health.logia.
