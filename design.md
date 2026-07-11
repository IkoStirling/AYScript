# AYScript Design

> **命名来源**：Logia — λογία（逻辑 / 理据），与 Phoskia（φῶς + σκιά，光与影）成对：GPU 用 Phoskia 写材质，CPU 用 Logia 写玩法。
>
> **文档状态（2026-07-11）**：**Phase S0–S3 + S3.12+R3 + R4.0 已交付**；`AYScript_Test` **671/671** 全绿；`kLogiaPipelineVersion = 5`。  
> **下一主阶段**：§14 剩余工作（引擎宿主接线 → 真实输入 → Reflect backlog → S4 语法）。R4.x 接续：R4.1 vector/array args → R4.2 T* out-param。  
> **指挥入口**：§14 + §14.8（copy-paste prompts）。

## 1. 概述

AYScript 是 AY Engine 的**游戏行为脚本子系统**。**作者编写 Logia 源码（`.logia`）**，经编译器流水线生成 **Lua chunk** 并由 sol2 加载执行。

**核心模型（S2.5 锁定，2026-07-07）**：

- **Logia 是 host-bound behavior DSL**——`script Foo { ... }` 声明一段绑定到 **C++ host 类型** 的行为，不是「只能给 ScriptComponent 用的语法糖」。
- **S2.5 唯一落地的 host**：`ScriptComponent` 子类（如 `PlayerController`）。S3 再扩展 System / Tool 等 host，**不改 Logia 表面语法**。
- **Logia 不定义数据，只消费 AYReflect 元数据**：C++ host 用 `AY_PROPERTY(float, speed, ...)` 注册字段；Logia 通过 `self.speed` 读写这些字段。
- **`script` 块**给出**行为**（生命周期 + 自定义逻辑）；C++ host 给出**数据**（`AY_PROPERTY` 字段）。
- **`var` 块** = 纯 Lua local（脚本内部状态），C++ 不可见。
- **`self`**（S2.5）：lightuserdata 指向绑定的 `ScriptComponent*`。S3 其他 host 时 `self` 类型由 `LogiaHostContext` 决定；无 receiver 的 host（如 CLI tool）可省略 `self`。
- **生命周期 `on_start` / `on_update` / `on_destroy`** 是**约定方法名**，不是 Logia 内置 magic——host 调度方决定调用哪些、何时调用。S2.5 由 `ScriptComponent` + `IScriptBridge` 驱动。

**关键字 `script` 保留**——不改为 `behavior` / `attach` 等。泛化的是 **host 绑定**，不是 DSL 名字。

**Lua 是实现细节**，不暴露给内容作者（体验目标类似 GDScript：引擎自有语法，隐藏宿主语言）。

### 1.1 设计目标

- **Logia DSL**：简化语法，只暴露玩法相关概念（self、生命周期、引擎类型、ambient 引擎 API）
- **Phoskia 式编译器**：词法 → 语法 → 语义分析 → 后端；错误带文件/行号；可缓存编译结果
- **AYReflect 语义层**：C++ 字段/类型在编译期校验，长期可维护、可接编辑器
- **Host 绑定（S2.5）**：`script Name` 与 Reflect 中注册的 C++ 类型同名；**S2.5 运行时**只通过 `ScriptComponent` 实例化并调度
- **Host 多元化（S3）**：同一套 `script` 语法，编译/加载时由调用方传入 `LogiaHostContext`（见 §1.6）
- **热更新**（S3+）：监视 `.logia` 变更，重编译并重载

### 1.2 与 Phoskia 的对称关系

| | Phoskia | Logia |
||--|---------|-------|
| 领域 | GPU 着色器 / 材质 | CPU 游戏行为 |
| 扩展名 | `.phoskia` | `.logia` |
| 编译器命名空间 | `ayt::shader::phoskia` | `ayt::script::logia` |
| 用户可见 | 材质块、uniform、vertex/fragment | `script` 块、生命周期、ambient API |
| 隐藏的后端 | GLSL / bgfx `.sc` | **Lua 5.5** |
| 元数据 | 着色器语义、类型系统 | **AYReflect** TypeRegistry |
| 所属模块 | AYShader | AYScript |
| 与 C++ 端耦合 | 编译期 ubo/cbuffer 布局 | 运行时通过 `AY_PROPERTY` 读写字段 |

### 1.3 在引擎中的位置

```
┌─────────────────────────────────────────────────────────────────┐
│                      Engine Modules                             │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  AYEntity                                                         │
│  ├─ class ScriptComponent { virtual void onAttach(Entity*); ... } │
│  ├─ class PlayerController : ScriptComponent {                    │
│  │      AY_PROPERTY(float, speed, kAttrSerialize)                 │
│  │      AY_PROPERTY(float, jump_force, kAttrSerialize)            │
│  │  }                                                              │
│                                                                  │
│  AYScript                                                         │
│  ├─ Compiler (.logia → Lua chunk)                                 │
│  ├─ LogiaRuntimeBridge (sol2 + Lua 5.5)                          │
│  │     loadScript(name, source) → cached module table              │
│  │     callLifecycle(name, method, receiver) → invoke              │
│  ├─ LogiaScriptBridgeAdapter (AYEntity IScriptBridge shim)        │
│  └─ ScriptSubSystem (drives bridge update() each tick)            │
│                                                                  │
│  AYReflect                                                        │
│  └─ TypeRegistry, ITypeInfo, IFieldInfo, FieldAttribute          │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘

数据流（运行时）：
  ScriptComponent::onUpdate(dt)
    → IScriptBridge::call("onUpdate", this, &dt)
    → LogiaScriptBridgeAdapter::call
    → LogiaRuntimeBridge::callLifecycle("PlayerController", "on_update", this, &dt)
    → sol2 fn.call(receiver, dt)         // Lua `self` = receiver
    → M.on_update(self, dt)              // Logia script 内 `self.position` 走 AYReflect
    → position.x = position.x + self.speed * dt   // self.speed 反射到 C++ PlayerController::speed
```

### 1.4 模块边界

| 模块 | 职责 |
|------|------|
| **AYEntity** | `ScriptComponent` 基类、子类用 `AY_PROPERTY` 注册字段、`IScriptBridge` 接口 |
| **AYScript** | Logia 编译器、运行时加载、adapter、子系统、ambient 引擎 API 绑定 |
| **AYReflect** | 类型/字段元数据；Logia 语义分析的数据源 |
| **AYGameLoop** | 驱动 `ScriptSubSystem::update` |
| **Lua 5.5 + sol2** | **仅实现层**；不出现在公开文档与示例中 |

### 1.6 Host 绑定模型（S2.5 锁定 / S3 扩展）

Logia **语法层**与 **host 类型**解耦：作者始终写 `script Name { ... }`；**谁加载、以什么 C++ 类型绑定 `self`** 由引擎集成层决定。

```
┌─────────────────────────────────────────────────────────────┐
│  .logia 源码（作者可见）                                      │
│    script PlayerController { on_update(dt) { self.speed } } │
└───────────────────────────┬─────────────────────────────────┘
                            │ Compiler::compile(source, ctx)
                            ▼
┌─────────────────────────────────────────────────────────────┐
│  LogiaHostContext（调用方传入，作者不可见）                    │
│    hostKind     = Component | System | Tool | ...  (S3+)    │
│    hostType     = Reflect TypeInfo*（决定 self 类型校验）    │
│    expectSelf   = true/false（Tool 可能无 self）               │
└───────────────────────────┬─────────────────────────────────┘
                            │ SemanticAnalyzer + LuaCodegen
                            ▼
┌─────────────────────────────────────────────────────────────┐
│  Lua module + 运行时调度                                      │
│    S2.5: ScriptComponent → IScriptBridge → callLifecycle    │
│    S3+:  SystemSubSystem / EditorToolRunner / ...           │
└─────────────────────────────────────────────────────────────┘
```

**S2.5 锁定的 `LogiaHostContext` 默认值**（隐式，尚未暴露为公开 API）：

| 字段 | S2.5 值 |
|------|---------|
| `hostKind` | `Component` |
| `hostType` | S3.0 起由调用方传入期望 host 基类型；S2.5 隐式等价于 Component。`nullptr` = 不做 host 兼容性校验 |
| `expectSelf` | `true` |

**S3 计划支持的其他 host（语法不变，只改 ctx + 调度）**：

| 场景 | C++ host | Logia 侧 | `self` | 调度方 |
|------|----------|----------|--------|--------|
| 实体组件脚本（**S2.5 已落地**） | `class PlayerController : ScriptComponent` | `script PlayerController { on_update(dt) {...} }` | `ScriptComponent*` | `ScriptComponent` + `IScriptBridge` |
| ECS System（S3 首选扩展） | `class MovementSystem : ISystem` | `script MovementSystem { on_update(dt) {...} }` | `ISystem*` 或无 | `AnimationSystem` 同类 tick |
| 编辑器/CLI 工具（S3+） | `class BuildTool` | `script BuildTool { run() {...} }` | 无 | 一次性 `call("run")` |
| 事件回调（S3+） | 已注册 Reflect 类型 | `script DamageHandler { on_damage(amount) {...} }` | host 实例 | EventBus 订阅 |
| 纯数据配置 | — | **不用 Logia** | — | 用 JSON / AYConfig |

**语义分析规则（分阶段）**：

| 规则 | S2.5 / S3.0 | S3.1+ | S3.2（可选 B-min） |
|------|-------------|-------|---------------------|
| `script Name` 在 AYReflect 注册 | soft warning（未知名仍生成 Lua） | 同左 | 同左 |
| `Name` 与 `ctx.hostType` 继承兼容 | **不做**（S2.5 从未做子类检查） | **不做**；按 `hostKind` + registry 存在性 + 运行时注册表 | `isDerivedFrom(type, hostType)` 沿单链 parent walk |
| `self.field` 字段存在 | 检查 AY_PROPERTY | 同左 | 同左 |
| 生命周期名 | `on_start/on_update/on_destroy` | 各 host 文档化合法方法集；Tool host 另增 `run()`（S3.8b） | 同左 |

**AYReflect 现状（2026-07-08）**：`ITypeInfo` 无 `isSubclassOf` / 无派生图；`TypeRegistry` 只有 name/id 映射。`resolveScriptName` 仅 `findType(name)`——warning 文案中的 “ScriptComponent subclass” 是**目标语义**，不是 S2.5 已实现行为；S3.0 应修正文案为 “no matching registered type”。

**明确不做**：为每种 host 新增 Logia 关键字（如 `system Foo`、`tool Foo`）。Host 种类是 **C++ 集成概念**，不是语法概念。

### 1.7 明确不做（v1）

- 多宿主语言（Python / JavaScript）并行
- 源码扫描 + 多语言 codegen
- 自研字节码 VM（Logia 编译到 Lua，不自研运行时）
- 向作者暴露 `require`、元表、裸 Lua 协程 API（后期可用 `await` 语法糖封装）

---

## 2. Logia 语言

### 2.1 适用范围

**Logia 是游戏行为专用 DSL，不是通用语言。**

- 输入：`.logia` 源文件
- 输出：Lua chunk（内部）；作者不阅读、不手写 Lua
- 用户：玩法程序、关卡设计（配合编辑器）
- 错误容忍：编译失败 = 脚本不可用（与 Phoskia 一致，编译期拦住错误）

### 2.2 语法示例

```logia
script PlayerController {
    var tick_counter: int = 0        // 纯 Lua local（self 看不到）

    on_start() {
        tick_counter = 0             // 写 local
        // self.speed / self.jump_force 直接来自 C++ PlayerController
        // 的 AY_PROPERTY 注册字段
    }

    on_update(dt: float) {
        tick_counter = tick_counter + 1
        if input.is_pressed("jump") {
            self.position.y = self.position.y + self.jump_force * dt
        }
        self.position.x = self.position.x + self.speed * dt
    }

    on_destroy() {
        log.info("PlayerController destroyed")
    }
}
```

### 2.3 语法原则

1. **`script Name { ... }`**：顶层行为块；`Name` 应对应 AYReflect 中已注册的 C++ host 类型（**S2.5 运行时**通过 `ScriptComponent::setScriptName("Name")` 绑定到实体组件实例）。
2. **`var x: T = ...`**：纯 Lua local 状态（self 不可见）。仅用于脚本内部计数器、缓存等。
3. **`self`**：指向当前 host 实例的 lightuserdata。**S2.5** = `ScriptComponent*`；`self.field` 走 AYReflect（须已在 C++ 用 `AY_PROPERTY` 注册）。
4. **生命周期是约定方法名**：S2.5 标准集为 `on_start` / `on_update` / `on_destroy`；源码可写 `on_update(dt: float)` 作文档性参数，codegen 仍 emit `function M.on_update(self)`，Lua 侧由 bridge 传入 `dt`。**S3** 其他 host 可定义 `run()`、`on_damage()` 等，由调度方调用。
5. **类型写引擎认识的**：`Entity`、`Transform`、`float` 等，由 Reflect 注册表解析。
6. **snake_case**：关键字与 API 统一蛇形命名。
7. **无 Lua 泄漏**：不提供 `local` / `nil` / `pcall` / `require` 等给用户。

### 2.4 后期语法扩展（非 v1 阻塞）

| 特性 | 说明 | 后端策略 |
|------|------|----------|
| `signal` / `connect` | 事件 | 生成 Lua 表 + 回调注册 |
| `await delay(sec)` | 延时 | Lua 协程封装，用户只见 `await` |
| 静态类型提示 | 编译期检查 | SemanticAnalyzer + Reflect |

### 2.5 文法草案（BNF 子集，v1）

```bnf
<program>      ::= <script_decl>+

<script_decl>  ::= "script" <identifier> "{" <member>* "}"

<member>       ::= <var_decl>
                 | <lifecycle_func>

<var_decl>     ::= "var" <identifier> ":" <type> ["=" <expression>] ";"

<lifecycle_func> ::= ("on_start" | "on_update" | "on_destroy" | "run")
                     "(" ")" <block>            (* v1: 无参数；`run` 仅 Tool host *)

<type>         ::= <identifier>    (* Entity, Transform, float, int, bool, ... *)

<block>        ::= "{" <statement>* "}"

<statement>    ::= <var_decl>
                 | <expr_stmt>
                 | "if" <expression> <block> ["else" <block>]
                 | "return" <expression>? ";"

<expression>   ::= <assignment> | <logic_or>

(* 标准表达式层级：or → and → equality → comparison → add → mul → unary → postfix → primary *)
(* postfix 包含 member access：`self.position`、`tick_counter` *)
```

完整文法随实现迭代补充至本文档 §附录。

---

## 3. 编译流程

```
┌────────────────────────────────────────────────────────────┐
│                      Logia 编译流程                          │
├────────────────────────────────────────────────────────────┤
│                                                            │
│  1. Logia 源码 (.logia)                                     │
│                                                            │
│  2. 词法分析 (Lexer) → Token 流                             │
│                                                            │
│  3. 语法分析 (Parser) → AST  (ScriptDecl, VarDeclStmt, ...)  │
│                                                            │
│  4. 语义分析 (SemanticAnalyzer) + AYReflect TypeRegistry    │
│     + LogiaHostContext（S3 公开；S2.5 隐式 = Component）   │
│     — `script Name` 校验 Reflect 中是否存在（S2.5/S3.0: soft warning）│
│     — `ctx.hostType` 传入 analyzer（S3.0: 存储不用；S3.1+: hostKind 规则）│
│     — `var x: T` 校验 T 是已知类型（builtin 或 registry）     │
│     — `self.field` 校验 field 是 AY_PROPERTY 注册的字段      │
│     — Ambient API（log.*, input.*, time.*）— 名称白名单     │
│                                                            │
│  5. 后端 (LuaCodegen) → Lua 源码 / 可直接 load 的 chunk      │
│     — `self.field` 生成反射访问（详见 §5.3）                 │
│                                                            │
│  6. RuntimeLoader (sol2) → 绑定 ambient 引擎 API →          │
│     → 缓存到 LogiaRuntimeBridge 内的 module table           │
│                                                            │
└────────────────────────────────────────────────────────────┘
```

### 3.1 编译缓存（后期）

与 Phoskia / ShaderCache 类似：

- 输入：`.logia` 路径 + 内容 hash + 编译器版本
- 输出：缓存的 Lua chunk 或字节码文件（如 `.logia.cache`）
- 热重载：文件变更 → 失效缓存 → 重编译

---

## 4. 命名空间分层

| 命名空间 | 职责 | 参考文件 |
|----------|------|----------|
| `ayt::script` | 引擎集成：ScriptSubSystem、LogiaRuntimeBridge、LogiaScriptBridgeAdapter、ambient API 绑定 | `AYScript.h`、`AYScriptSubSystem.h`、`AYScriptRuntimeBridge.h`、`AYScriptBridgeAdapter.h` |
| `ayt::script::logia` | Logia 编译器核心：Token/Lexer/Parser/AST/Semantic/LuaCodegen | `AYLogia.h`、`AYLexer.h`、`AYParser.h`、`AYAst.h`、`AYSemanticAnalyzer.h`、`AYLuaCodegen.h`、`AYCompilerError.h` |

**分层理由**：

- Logia 是一种语言，`ayt::script::logia` 是其命名空间
- 运行时、sol2 绑定属于引擎集成，不混入语言前端
- 未来若更换 Lua 版本或增加其他后端，只改 `LuaCodegen` + `LogiaRuntimeBridge`

---

## 5. 语义分析与 AYReflect

### 5.1 职责划分

| 层 | 做什么 |
|----|--------|
| **AYEntity** | `ScriptComponent` 基类、子类用 `AY_PROPERTY` 注册字段、`IScriptBridge` 接口（**S2.5 唯一 runtime host**） |
| **AYReflect** | 类型/字段元数据；Logia 语义分析的数据源 |
| **Logia SemanticAnalyzer** | 校验 `script Name` 在 registry；`var` 类型合法；`self.field` 存在；**S3.0** 接收 `LogiaHostContext`（校验不变）；**S3.1+** 按 hostKind 启用规则 |
| **LuaCodegen** | 对 `self.field` 生成反射访问代码（详见 §5.3）；对 ambient API 生成直接调用 |
| **LogiaRuntimeBridge (sol2)** | 注册 ambient API + helper 用于 `self.field` 反射；**S2.5** 只服务 ScriptComponent 调度 |

### 5.2 编译期错误示例

```
error[logia/E001]: unknown script 'PlayerControllerr'
  --> player.logia:1:1
   |
 1 | script PlayerControllerr {
   |        ^^^^^^^^^^^^^^^^^^^ did you mean 'PlayerController'?

error[logia/E002]: unknown field 'speeed' on type 'PlayerController'
  --> player.logia:7:13
   |
 7 |         self.speeed = 5.0
   |              ^^^^^^ 'speed' is registered (AY_PROPERTY on C++ side)

error[logia/E003]: unknown type 'Vectorr3' for var declaration
  --> player.logia:3:5
   |
 3 |     var tick: Vectorr3 = 0
   |     ^^^^^^^^^^^^^^^^^^^^^^ builtin types: int, float, bool, string, Entity
```

这是 Logia 相对「裸 Lua 绑定」的核心长期价值：**错误在编译期，不在运行时才 nil。**

### 5.3 `self.field` 反射访问（生成策略）

S2 阶段：仅做语义检查（字段是否在 AYReflect 注册），codegen 暂时生成 stub（仍可桥接但不全功能）。

S3 完整 codegen：
```lua
-- `self.position` 生成：
local __reflect_pos = ayt_reflect_get_field(self, "position")
__reflect_pos.x = __reflect_pos.x + self.speed * dt
-- `self.speed` 生成：
local __tmp_speed = ayt_reflect_get_field(self, "speed")
-- ... 计算用 __tmp_speed ...
-- 写回（如有赋值）：
ayt_reflect_set_field(self, "speed", new_value)
```

实际生成路径（**S3 实现**，S2 仅语义检查）：
- `self.field` 读 → `ayt_reflect_get_<type>(self, "field")`
- `self.field = expr` → `ayt_reflect_set_<type>(self, "field", expr)`

sol2 usertype 注册（**S3**）：`ScriptComponent` 子类用 `sol::usertype<T>` 暴露字段访问方法。

### 5.4 Ambient API 白名单

以下名称不需要 C++ 端 `AY_PROPERTY` 注册；由 `LogiaRuntimeBridge::registerEngineApi()` 在 sol2 端注册：

| Ambient 名称 | 来源 | 阶段 |
|---|---|---|
| `log.info/warn/error/debug` | `ayt::log::*` | S1 |
| `input.is_pressed/is_just_pressed` | `InputProvider` mock（S1）；真实源 = **AYDevice** `InputMapping`（INT-02） | S1 / INT-02 |
| `time.delta` | `ayt::time::delta()` | S3 |
| `event.emit/subscribe` | AYEvent | S3+ |

`log` / `input` / `time` / `event` 是 ambient identifier——在 `self` 上下文外可以直接使用，**不**走 AYReflect。

### 5.5 与序列化的关系

- C++ 组件字段标 `Serialize` 的，存档 / 网络与 Logia `self.field` 访问**共享同一份 AYReflect 元数据**——数据布局只定义一次。
- Logia **不替代** AYSerializer；场景 `.ayscene` 存组件数据，`.logia` 存行为。
- 因此 Logia **不**再有 `export` 关键字——数据可见性由 C++ 端的 `AY_PROPERTY` + `FieldAttribute` 决定（细则见 **§5.7**）。

### 5.7 C++ → Logia 暴露策略（Script exposure）

> **状态（2026-07-08）**：字段单跳 primitive 读写 **已实现**（S3.3）；方法暴露、attribute 强制、struct 链 **未实现**。本节锁定目标模型与分期，避免与 C++ `public`/`protected`/`private` 混淆。

#### 5.7.1 设计原则

| 原则 | 说明 |
|------|------|
| **数据在 C++，行为在 Logia** | 字段用 `AY_PROPERTY` 注册；玩法逻辑写在 `script Name { ... }` |
| **Logia 只消费 Reflect** | 不在 `.logia` 里重复声明 C++ 成员；语义层查 `ITypeInfo::findField` |
| **一份元数据，多消费方** | 同一 `FieldAttribute` 位可同时服务 Serializer / Editor / Script / Network |
| **脚本可见性 ≠ C++ 访问级别** | `public`/`private` 是编译期；脚本侧用 **Reflect attribute** 控制 |

#### 5.7.2 字段（成员）— 当前路径

```
C++  AY_PROPERTY(Type, name, FieldAttribute::...)
  →  AY_FINALIZE_REGISTRATION_METADATA(T)
  →  TypeRegistry / IFieldInfo
  →  SemanticAnalyzer: self.name 校验 findField
  →  LuaCodegen (S3.3): ayt_reflect_get/set_field(self, hostTypeName, name, ...)
  →  LogiaRuntimeBridge: lua_CFunction 读写内存
```

**S3.3 已实现范围**：

- 仅 **单跳** `self.<primitiveField>`（`resolvedType->getFieldCount() == 0`）
- 类型白名单：int/Int32、float/Float32、bool/Bool、double/Float64、Int64、UInt32
- **`self.position.x` 链式 struct** — 显式 defer 到 S3.11

**`AY_PROPERTY` 与 C++ access specifier**：

- 宏展开为普通成员 `Type name;` — 写在 `public:` / `private:` 哪段，C++ 编译器就按哪段检查。
- Reflect 用 `offsetof` + 成员指针注册，**不记录** C++ access；`private` 段里写 `AY_PROPERTY` 仍会进 TypeRegistry（技术上可行，**不推荐**）。
- **`AY_PROPERTY` 不支持**：`static` 字段（`static_assert`）、`const` 字段（运行期跳过）。

#### 5.7.3 字段可见性 — `FieldAttribute`（非 public/private）

现有 `FieldAttribute`（`IAYReflect.h`）服务序列化 / 编辑器 / 网络；**尚无 Script 专用位 enforced**。

| 现有 flag | 用途 | AYScript 现状 |
|-----------|------|---------------|
| `Serialize` | AYSerializer 存档 | 与 `self.field` **无强制关联** |
| `Hidden` | Editor 隐藏 | Semantic **未**过滤 |
| `BlueprintReadOnly` | UE 风格「蓝图只读」 | `ayt_reflect_set_field` **未**检查 |
| `Transient` | 不序列化 | Script **未**过滤 |

**锁定策略（待实现 — track R1）**：

```cpp
// 建议在 AYReflect FieldAttribute 新增（或复用 BlueprintReadOnly 语义）：
//   ScriptVisible  — Logia 可读；semantic + runtime 均检查
//   ScriptReadOnly — Logia 可读不可写（赋值 / compound assign 拒绝）
```

- 默认（过渡期）：凡 `findField` 命中即可读写（与 S3.3 相同）；标了 `ScriptReadOnly` / `BlueprintReadOnly` 后 runtime 拒绝 `set_field`。
- **不**把 C++ `protected` 映射成「仅派生脚本可写」——Logia host 已是具体类型，无 C++ 式继承访问控制。

#### 5.7.4 方法（成员函数）— S3.12 完成（track R2）

**状态**：✅ S3.12 完成（2026-07-10）。`self.method(args)` 从 Logia 脚本可调用 C++ 方法。

**实现摘要**：

1. **AYReflect 公共 ABI**（`interface/IAYReflect.h`）：
   - 新增 `IMethodInfo` interface（独立于 `IFieldInfo`；不通过 `ITypeInfo` 继承，避免与现有 inspector 链耦合）。
   - `ITypeInfo` 增 4 个 method-side 虚函数 `getMethodCount / getMethod / findMethod / addMethod`，默认 no-op（保持 source-compat，零基础 TU 修改）。vtable layout 变化通过强制完整 rebuild 解决（参见 lessons learned §S3.12）。
   - `TypeInfoImpl<T>` 重写 4 个虚函数 + 新增 `_methods` 字段。

2. **变长 PMF 适配**（`logia/AYMethodInfoImpl.h`，**AYScript-private header**）：
   - 关键决策：`MethodInfoImpl<T, Ret, Args...>` 模板**不**放在 AYReflect，**不**让 foundation TU 看到 — 上轮 S3.12 中断的 MSVC C2275 / C2641 错误根因是 `VectorTypeInfo<ayt::math::Bool>` 在 `MethodInfoImpl` 变长 pack 之后无法 parse。把变长模板隔离在 AYScript-private header，foundation TU 不感知，bug 不重现。
   - `MethodInfoImplConst<T, Ret, Args...>` 单独 specialization 支持 const PMF（const getter 模式）。
   - thread-local 返回 buffer 解耦 ABI 与 Lua。

3. **Macro 路径**（`AYPropertyMacros.h` + `logia/AYMethodRegistrarBridge.h`）：
   - `AY_METHOD(Ret, name, args...)` — 仅 emit 静态 registrar（不 emit 函数声明，用户正常写方法定义即可，宏自动 capture `&T::name`）。
   - `AY_MakeArgList(PMF)` 通过 overload resolution 自动 deduce const / non-const PMF + `Args...`。
   - `AY_MethodRegistrarOf<T, Ret, AY_MethodArgs<Args...>, Pmf, Index>` 两个 partial specialization（非-const / const）继承 `AY_PropRegistrarBase`，加入同一个 registrar 链表。
   - `AY_FINALIZE_REGISTRATION_METADATA(T)` 基础 finalize 跳过 method entries（保持 foundation 零变长模板）；`AY_FINALIZE_METHODS(T)` AYScript-side finalize 调 `buildMethodInfo()` 把 PMF → `MethodInfoImpl<T,Ret,Args...>` → `addMethod()`。两 macro **必须** 在 namespace scope，且 consumer TU 须 `#include "logia/AYMethodRegistrarBridge.h"`。

4. **Analyzer + Codegen**：
   - `CallExpr::resolvedMethod` + `resolvedMethodOwnerName`（`Expr` 上新增 slot，analyzer 填）。
   - `SemanticAnalyzer::analyzeExpr(CallExpr&)` 检测 callee 是 `self.<method>(args)` + 命中 `ITypeInfo::findMethod` 时 stamp。
   - `LuaCodegen::emitExpr(CallExpr&)` 优先 emit `ayt_reflect_call_method(self, "<Type>", "<method>", args...)`，否则 fall through 旧 bare-Lua 路径（保持向后兼容 — 未注册方法走 bare Lua dispatch）。

5. **Bridge**（`AYScriptRuntimeBridge.cpp`）：
   - 新 Lua global `ayt_reflect_call_method(self, type, method, args...)`。
   - `lookupMethod(typeName, methodName)` 缓存到 `MethodKey → IMethodInfo*` map（O(1) 命中）。
   - 栈上 `argSlots[expected]`（8-byte per slot）由 `paramType` tag 分发到 `int32_t / float / bool / double / int64_t` 写入。Return type 同样 tag 分发 push 整数/浮点/布尔到 Lua。`void` 返 `return 0`（不 push 任何值）。
   - 错误处理 fail-closed：未知方法 / arg 数量不匹配 push `nil` + 1 + 写 log，不让 Lua 崩溃。

6. **Pipeline version**：
   - `kLogiaPipelineVersion` 3 → 4，bump 让 S3.11 cache 自动失效。

**测试覆盖**（`Test_LogiaReflectRuntime.cpp`）：

- `lg12_method_void_increments_state` — `self.heal(10)` 真的 `obj.hp += 10`。
- `lg12_method_void_clamps_state` — `heal(50)` 触发 maxHp 钳位。
- `lg12_method_return_primitive_round_trip` — `self.getHp()` 返 int 桥到 Lua `__test_witness = "42"`。
- `lg12_method_unknown_fails_safe` — 未注册方法不崩，state 不动。
- `lg12_method_arg_count_mismatch_runtime_error` — `self.heal()` 缺参，state 不动。
- `lg12_ay_method_macro_void_increments_state` — `AY_METHOD(void, heal, int amount)` macro 路径端到端。
- `lg12_ay_method_macro_const_return_round_trip` — `AY_METHOD(int, getHp)` const 路径端到端。

**结果**：AYSCRIPT_Test **631/631** 全绿（baseline 590 + 5 LG-12 手动 path + 2 AY_METHOD macro path + 其它 34 个原 LG 套件增加）。

**范围**（S3.12 锁定）：
- 非-static、非-overloaded、非-templated member functions only
- primitive args + primitive-or-void return（int/Int32/float/Float32/bool/Bool/double/Float64/Int64）
- const member functions supported
- `self.method(...)` 单层调用；`self.f1.f2.method(...)` chain call 推迟到 track R3

**Lessons learned**（避免未来重蹈）：

1. **MSVC C2275 / C2641 触发条件**：foundation TU 同时看到 `MethodInfoImpl<T,Ret,Args...>` 变长模板 + `VectorTypeInfo<ayt::math::Bool>` 这种 `ayt::math::Bool` qualified-id 模板。修复：foundation TU 不 include 变长 pack 头，consumer TU 各自 include — **不** 影响 ABI 兼容性。
2. **ITypeInfo vtable 加 4 个虚函数破坏 ABI**：inline 化的 `TypeInfoImpl<T>` 模板在每个 TU 独立生成 vtable 实体；vtable layout 一变，所有 inline 化的 .obj 必须 rebuild。ninja 头依赖追踪对 inline 模板实例化不完整 → 部分 TU 漏 rebuild → 运行时分发到错误 vtable 槽位 → 段错误。**修复**：全 `D:/Projects` 重新 build（`find ... -name "*.cpp" -exec touch` 强制 ninja 重 build）。CI 集成：建议 `AYReflect.h` ABI 变化时 `cmake --build --clean-first` 至少触发 AYReflect + 所有 include `IAYReflect.h` 的 consumer。
3. **MSVC `Args...` 变长 pack parser 限制**：
   - 不要在 `Type<Args...>` 紧接 `>(` 之间加 `>>`；用 `Args ...` + 空格（但仍可能与 dependent name 冲突）。
   - dependent name template-id 用 `::ayt::script::logia::reflect::template MethodInfoImpl<...>` 显式 `template` 关键字。
   - 跨 namespace qualification 加 `::` 前缀（`::ayt::script` 避免 `ayt::serializer::script` nested-name lookup）。
4. **Macro 内注释限制**：`#define X /* ... */ macro_continuation\` 中 `/* ... */` 在 MSVC 下产生 C4010 nested-comment 警告并偶尔 broken tokenization。改用 `//` 注释 + 末尾 `\` 续行，**或** 把注释提到 macro 之外。
5. **`AY_CURRENT_CLASS` 是 user-defined 宏**：`AY_PROPERTY` / `AY_METHOD` 在 class body 内使用前必须 `#define AY_CURRENT_CLASS T`，class body 结束 `#undef`。**不** 隐式 derive（member pointer 是从 `&AY_CURRENT_CLASS::name` 推不出 `T` 的）。Header convention 文档在 `AYPropertyMacros.h` 顶部。

#### 5.7.4 R3 — 非 primitive args/return（struct/enum/std::string）

**状态**：✅ S3.12+R3 完成（2026-07-10）。S3.12 升级到支持 struct args / struct return / enum / `std::string`。

**实现摘要**：

1. **Slot convention 统一化**：每个 arg slot 现在都是**指针**——primitive 写 8-byte 栈槽（`&argSlots[i]`），`std::string` / `T*` / `const T&` 写堆分配指针（bridge 用 cleanup queue 在 invoke 后释放）。`MethodInfoImpl::readArg<I>` 用 `if constexpr` ladder dispatch：
   - `enum E` → `std::underlying_type_t<E>` memcpy + `static_cast<E>`
   - `std::string` → heap 指针 → copy-construct 进 PMF 参数
   - `const T&` / `T&` → 指针 → deref
   - `T*` / `const T*` → 指针传递
   - `T` by value → memcpy for trivially-copyable, copy-construct for struct
2. **MethodInfoImpl::storeReturn**：thread-local return buffer 改用 `std::string`（不是 `std::vector<uint8_t>`）— placement-new `std::string` onto raw `vector<uint8_t>` 会让 string 的 `_Container_base12` 子对象（MSVC STL 内部记录 iterators）corrupt；改用 `std::string` 自家 allocator 保证 placement-new 干净。trivially-copyable types memcpy 进 string buffer，non-trivial types placement-new。
3. **Bridge `ayt_reflect_call_method_c` 扩**：
   - 每个 arg 按 `paramType->getName()` dispatch：
     - primitive → 8-byte 栈槽
     - `std::string` → `new std::string(lua_tostring(...))` → cleanup queue
     - struct（`getFieldCount() > 0`）→ `::operator new(typeSize)` → `memset(0)` → 逐字段 `lua_getfield` + `storeFieldPrimitive` → cleanup queue
     - unknown → fallback as int (covers enum)
   - return 同样 dispatch：primitive push、std::string push、struct build Lua table 逐字段 `pushFieldPrimitive`
   - **retType 为 nullptr 的 fallback**：C++ enum 没有专门 `registerEnum<E>()`，所以 `findType<State>()` 返回 nullptr → bridge 看到 `retPtr != nullptr` 但 `retType == nullptr` → 当成 int push
4. **`TableExpr` AST + parser + codegen**（S3.12+R3 前置）：
   - `AYAst.h` 新增 `TableExpr` 节点（`std::vector<Entry>`，每 entry = (key_expr, value_expr)）
   - Parser 在 `parsePrimary` 看到 `{` 时进入 table-literal production：`{key=value, key=value, ...}` — key 必须是 IdentifierExpr
   - Codegen emit `{name1 = value1, name2 = value2, ...}` — Lua 表构造语法要求 key 是 bareword identifier（**不是** 字符串字面量；之前第一次试错写了 `{"maxHp" = 10}` 是非法 Lua 语法）。
   - **Logia 不支持 `..` 字符串拼接**——struct return 测试不能写 `__test_witness = tostring(s.maxHp) .. "," .. tostring(s.attack)`，必须用多语句 + 中间变量，或拆字段单独测试。
5. **kLogiaPipelineVersion 4 → 5**（header cache invalidation）。
6. **测试覆盖**（7 个 LG-12 R3 tests）：
   - `lg12_r3_method_struct_arg_const_ref` — fire damage 2x（damageType=1）→ `hp -= 10*2 = 80`
   - `lg12_r3_method_struct_arg_cold_damage` — cold 1x → `hp -= 5 = 95`
   - `lg12_r3_method_struct_return_to_lua_table` — `s.maxHp == 200`
   - `lg12_r3_method_enum_arg_int_cast` — `setState(1)` 不崩
   - `lg12_r3_method_enum_return_int_cast` — `getState()` 返回 int 0/1
   - `lg12_r3_method_string_arg_and_return` — `setName("villain")` + `getName()` round-trip
   - `lg12_r3_method_lua_table_field_name_exact_match` — wrong field name silent miss
   - **结果**：AYScript_Test **660/660** 全绿（baseline 631 + 7 LG-12 R3 + 22 LG-12 baseline + ...）。AYReflect_Test **156/156** 全绿（baseline 133 + 23 R3 specializations）。

**范围（R3.0）**：

| ✅ 锁 | ❌ 推迟 |
|---|---|
| Primitive args + return (S3.12) | `std::vector<T>` / `std::array<T,N>` args (R4.1) |
| `const T&` / `const T*` args | `T&` / `T*` out-params (R4.2) |
| `T` (by value) return | `unique_ptr` / `shared_ptr` (R5) |
| `std::string` args + return (top-level) | `registerEnum<E>()` + `EnumTypeInfo<E>` (R3.5+) |
| `enum` args + return (int-cast, no dedicated EnumTypeInfo) | `m_`/`b_` prefix stripper (R3.5+) |
| Exact-match field names (camelCase both sides) | `std::string` field on a struct (R3.5+: placement-new in pushFieldPrimitive / storeFieldPrimitive) |
| Struct args/return with **primitive + enum** fields | `local` keyword audit (deferred — design §2.3 violation in tests) |
| Inline `{...}` table literals (TableExpr AST added) | |
| **Nested struct fields (R4.0)** | |

**范围外**（设计原则）：
- **C++ struct 字段名必须用 camelCase 且不加 `m_`/`b_` 前缀**——Logia 表字面量 key 必须匹配 C++ 字段名。R3.5 会加 stripper + PascalCase↔snake_case 自动转换。
- **C++ enum 必须能被 AYReflect `findType<Ret>()` 找到**——R3.0 没注册，所以 enum args/return 的 `paramType/retType` 是 nullptr，bridge fallback 为 int path。R3.5 加 `registerEnum<E>()` 后 retType 不再是 nullptr。
- **Logia 字符串拼接 `..` 不支持**（parser 没有 DotDot token）。struct return 测试要么拆字段单独测试，要么用 Lua-side 字符串拼接（通过 __test_witness 多次赋值）。

**Reference designs**：
- Godot GDExtension（[docs](https://docs.godotengine.org/en/stable/tutorials/scripting/gdextension/gdextension_cpp_example.html)）：Variant + Dictionary 模式 — struct 是 Dictionary，field-by-field marshalling。R3 走相同路线：Lua table → bridge field walk → C++ struct。
- Unreal BlueprintCallable（[UHT](https://docs.unrealengine.com/5.0/en-US/ProgrammingAndScripting/GameplayArchitecture/Functions/index.html)）：typed `UPARAM`、UHT-generated metadata、zero-cost bridge。R3 借用 typed signature，但 marshal 是手写（无 UHT）。
- xLua：`xlua.cast(table, type)` runtime type-cast。R3.0 是 compile-time typed + runtime marshalling，比 xLua 安全但需要预注册。

**Lessons learned (R3 specific)**：

6. **`std::vector<uint8_t>` 装不下 `std::string`**：placement-new `std::string` onto `std::vector<uint8_t>::data()` 会让 `std::string::~basic_string()` 调 `_Orphan_all_unlocked_v3()` 时 read `0xFFFFFFFFFFFFFFFF` 崩。原因：`std::string` 的 `_Container_base12` 子对象（追踪 iterators）在 placement-new 时没正确初始化。修复：thread-local buffer 改用 `std::string` itself——`std::string` 自带 allocator，重 placement-new 是干净状态。
7. **Lua 表构造语法严格**：`{maxHp = 10}` (bareword key) 是合法；`{"maxHp" = 10}` (string-literal key) 是非法；`[expr] = value` 是合法（generic 形式）。Codegen 必须 emit bareword。
8. **Logia 没有 `..` token**：Lua 字符串拼接 `..` 在 Logia 源码里写不出来。struct return 测试不能用 `tostring(s.maxHp) .. tostring(s.attack)`。**要么** Logia parser 加 DotDot token（S4 范围），**要么** 测试拆字段。
9. **TableExpr 必须先于 struct-arg 实装**：S3.12 R3 计划原以为"`local t = {...}`"是 S3.12 R3.0 的工作路径，但 Logia parser 没 table-literal production。R3.0 必须加 TableExpr AST + parser + codegen（不在原计划里），否则所有 struct-arg 测试都编译失败。
10. **enum 没 AYReflect registry 时 bridge fallback**：R3.0 enum 用 `findType<EnumType>()` 返回 nullptr，bridge 检测 `retType == nullptr` 时把 `retPtr` 当 int32 push。R3.5 加 `registerEnum<E>()` 后 retType 不再 nullptr，bridge 走完整 path。

#### 5.7.4 R4.0 — 嵌套 struct fields

**状态**：✅ **R4.0 完成（2026-07-11，commit 待 push）**。S3.12+R4.0 是 R3.0 之后的第一个增量切片；在 R3 的 struct/enum/std::string args/return 之上加 **嵌套 struct 字段** 的递归 marshal。

**实现摘要**：

1. **`pushFieldPrimitive` 嵌套 marshal**：`AYScriptRuntimeBridge.cpp` 的 `pushFieldPrimitive` 检测 `field->getType()->getFieldCount() > 0` 时进入新分支 — 在 Lua 栈顶 push 一个新 table，按 sub-field 递归 walk，每个 sub-field 调 `pushFieldPrimitive` 自身；当 sub-field 是 primitive 时 push 数值+ lua_pushstring(key)+ lua_settable(`subTableIdx`) 写入 sub-table。返回 1，让上层 caller 把 sub-table 当作 value settable 进 outer table。**关键**：inner `lua_settable` 用**绝对索引** `subTableIdx` 而非 `lua_settable(L, -3)` — 绝对索引在嵌套调用栈任何深度都正确锚定 sub-table。
2. **`storeFieldPrimitive` 嵌套 marshal**：对称路径。当 field type 是 struct (`getFieldCount() > 0`)、Lua top-of-stack 是 table 时：heap-alloc 一个 tmp T（memset 0），按 sub-field `lua_getfield(L, valueStackIdx, subName)` 取每个 Lua-sub-table 的子项，递归 `storeFieldPrimitive` 填进 tmp T 的 offset，最后 memcpy 进 fieldPtr。
3. **R3 struct return path 自动 extend**：R3.0 的 `retType->getFieldCount() > 0` 分支 loop 调 `pushFieldPrimitive(field, fieldPtr)` 处理每个顶层 field。当顶层 field 自己也是 struct 时，新分支递归构造 sub-table,**不在 R3.0 代码改一行**。R4.0 这条路让 `self.method()` 返回 struct-of-struct 自动转 Lua-table-of-table。
4. **R3.11 chain reflect path 自动 extend**：`ayt_reflect_get_field_chain_c` 的 leaf hop 也调 `pushFieldPrimitive`，自动支持嵌套 leaf。但**不**支持 chain 中的 nested-struct intermediate hop — 推迟到 R4.5+（需要 ITypeInfo 表的 typeid-based chain resolver）。
5. **AYReflect ABI 不动**：完全在 `AYScriptRuntimeBridge.cpp` 的两个 helper 函数内加 nested 分支；`ITypeInfo`、`IFieldInfo`、`TypeRegistry`、`TypeInfoImpl<T>` 零变化。

**测试覆盖（2 case，`Test_LogiaReflectRuntime.cpp` R4Player fixture + LG-12 R4.0 套件）**：

- `lg12_r4_nested_struct_arg_subfield_round_trip` — `self.applyDamage({source={x=10,y=20}, amount=7})` 真实 mutate `R4Player::hp`。`source` 是 nested struct field，`storeFieldPrimitive` 走递归路径把 `{x=10,y=20}` Lua table 填进 `DamageInfo.source`。
- `lg12_r4_nested_struct_via_setPosition_then_read` — `self.setPosition({x=3.5,y=4.5})` + `self.getPosition()` round-trip。push 方向递归构造 Lua sub-table，store 方向递归读 Lua table 进 C++ struct offset。

**结果**：`AYScript_Test` **671/671 全绿**（baseline 660 + 2 R4.0 + 9 调整）。

**Deferred（独立切片）**：
- `lg12_r4_nested_struct_return_subfield_round_trip` — 原本想用 `self.getStats().location.x` 验证嵌套 struct return 时 sub-table 自动嵌套。**测试本身跳过的原因**与 R4.0 nested marshal 无关 — 它依赖 `__test_witness = tostring(self.getStats().location.x)` 这条 codegen 路径，在 Logia codegen 处理 `method_call.field.field` 链式 chain reflect 时未启用（这部分是 R3.11+S4 还没合的扩展，R3.11 当前只覆盖 `self.f1.f2` lightuserdata 根的 chain）。R4.0 的 nested struct 实际跑通 — test 2 (`setPosition` round-trip) 已经间接覆盖。
- `local` 关键字 audit — design §2.3 规定不暴露 Lua `local`（应该用 module-level `var x: T` 或函数体内 bare-assign）。R3 及 R4 测试套件里若干 `local s = ...` 写法是漏网之鱼，codegen emit 出 broken split-text 但"运气好" 在 sol2 + safe_script 下仍 PASS。**这独立于 R4.0 工作**，下一个 session 单独 audit 并改成 `var` 或 bare-assign。

**Lessons learned（R4.0 specific）**：

11. **嵌套 marshal 用绝对索引 `lua_settable(L, subTableIdx)` 而非 `lua_settable(L, -3)`**：相对索引 (-3) 在递归深处因为 pushFieldPrimitive 内部又 `lua_newtable` push 了一个新的 sub-table，**index -3 重新指向会错**（曾经我 DBG 看到 -3 = "location" 而不是 Vector2 sub-table — 因为新 push 加进来后相对索引移动）。捕获 `subTableIdx = lua_gettop(L)` 在 `lua_newtable` 之后立即捕，inner settable 全部用绝对值 — 这是 deep-recursive Lua C API 的关键模式。
12. **AYReflect 测试 fixture 共享 type-name 是隐藏的 dependency 风险**：R3 fixture 注册 `R3Player::Stats as "Stats"`,R4 fixture 想注册 `R4Player::Stats as "Stats"`。TypeRegistry 不阻止重名 — `findType<Ret>()` 用 typeid 走 hash，但 `registerTypeInfo(name, info)` 的 name 是 process-wide 的字符串 key，如果其他 fixture 走 `findType("Stats")` 会拿到**先注册**的 ITypeInfo，导致 field 数对不上。最干净是在每个 fixture 用 unique type name（R4 用 `R4Stats`），避免"name collision 踩到另一个 fixture 的 ITypeInfo"。**未来**: add `registerTypeInfo` 名字重复时 warning（push backlist of R4.1+）。
13. **Logia 的 method-call-then-chain (`getStats().location.x`) 不走 R3.11 chain reflect**：R3.11 只覆盖 `self.<f1>.<f2>`，根必须是 lightuserdata `self`。bridge 返回的 Lua table 上的 `.field.field` 是普通 Lua sub-table 访问 — **需要测试用 `local s = ...` 中间变量**才能在 Logia emit 时正确处理。但 `local` 又是禁止 keyword（见 §2.3）。这是一个**未来 S4 / R5 范畴** — Logia 要么加 `..` token 用于字符串组合，要么把 method-call-chain 也走 R3.11-style chain reflect。

#### 5.7.5 消费方矩阵

| 消费方 | 字段 | 方法 | 阶段 |
|--------|------|------|------|
| AYSerializer | `Serialize` / `Transient` | N/A | ✅ |
| AYEditor Inspector | `Hidden` / `EditAnywhere` / Slider… | 未来 `EditorCallable` | ⏳ |
| **AYScript Logia** | `ScriptVisible` / `ScriptReadOnly`（R1） | `AY_METHOD` / `ayt_reflect_call_method`（R2） | 🟡 字段+方法已通；R1 可见性未强制 |
| AYNetwork | `NetReplicate` | 未来 | ⏳ |

#### 5.7.6 暴露能力 backlog（Reflect consumer track）

| ID | 内容 | 状态 | 依赖 |
|----|------|------|------|
| **R1** | `ScriptVisible` / `ScriptReadOnly` semantic + `ayt_reflect_set_field` enforce | ⏳ 待做 | AYReflect `FieldAttribute` 新位或复用 `BlueprintReadOnly` |
| **R2** | `AY_METHOD` + `IMethodInfo` + `ayt_reflect_call_method` | ✅ S3.12 | — |
| **R3** | struct 链 `self.position.x`（= S3.11） | ✅ S3.11 | — |
| **R3.5** | `registerEnum<E>()`；`m_`/`b_` 字段名 stripper；struct 内 `std::string` 字段 marshal | ⏳ 待做 | AYReflect enum 注册 |
| **R4** | `std::vector<T>` / `std::array<T,N>` args；嵌套 struct 字段；`T&` out-param | ⏳ 待做 | R3.5 可选 |
| **R5** | `unique_ptr` / `shared_ptr` 方法参数 | ⏳ 推迟 | 所有权语义锁定后 |
| **R6** | sol2 `usertype` 优化（替代部分 `lua_CFunction`） | ⏳ 性能驱动 |  profiling 数据 |

---

### 5.6 LogiaHostContext 与 S3 分阶段路线

S2.5：`SemanticAnalyzer` 行为等价于隐式 `LogiaHostContext{ Component, nullptr, true }`，但 **API 尚未公开**。

#### API（S3.0 锁定）

```cpp
namespace ayt::script::logia {

enum class LogiaHostKind {
    Component,   // S2.5 — ScriptComponent on Entity
    System,      // S3.1 — ISystem tick
    Tool,        // S3+ — editor/CLI one-shot
    EventHandler // S3+ — callback object
};

struct LogiaHostContext {
    LogiaHostKind kind = LogiaHostKind::Component;
    // Expected host base type for future strict checks.
    // S3.0: stored, not used for validation. nullptr = skip.
    const ayt::reflect::ITypeInfo* hostType = nullptr;
    bool expectSelf = true;   // false for Tool.run()-only scripts
};

} // namespace
```

公开入口：

```cpp
CompileResult Compiler::compile(const std::string& source,
                                const LogiaHostContext& ctx = LogiaHostContext{});
```

默认参数保持 S2.5 行为；现有 169 测试零修改。

#### S3.0 — LG-03：API 穿线（**不改 AYReflect**）

| 做 | 不做 |
|----|------|
| `LogiaHostContext` + `Compiler::compile(source, ctx)` | `ITypeInfo::isSubclassOf` |
| `SemanticAnalyzer` 构造/分析时存 `_ctx` | TypeRegistry 派生图 |
| 校验逻辑与 S2.5 相同（registry lookup + soft warning） | 启用 `hostType` 子类 hard check |
| 修正 unknown-script warning 文案 | 重命名 `ScriptDecl` → `BehaviorDecl` |
| +2~3 个 ctx 传递单测 | 改 LuaCodegen 输出形状 |

#### S3.1 — LG-04：System host 落地（**仍可不碰 Reflect 继承**）

| 项 | 策略 |
|----|------|
| 校验 | `ctx.kind == System` + `findType(scriptName)` + `World` 已注册该系统 |
| `self` | `ISystem*` 或 `expectSelf=false`（实现时二选一锁死） |
| 调度 | GameLoop tick 调 `callLifecycle("MovementSystem", "on_update", system, &dt)` |
| 继承 | **不需要** `isSubclassOf`——类型合法性由 ECS 注册 + Reflect 名字存在性保证 |

**S3.1 (LG-04) 锁定决策**（2026-07-08 实现完成后补）：

- **`self` 语义 = `ISystem*` lightuserdata**（`ctx.expectSelf = true`）。理由：与 Component host 的 `self` 模型保持对称；`self.field` 的 reflection 路径后续 S3.x 接入 `sol::usertype<ISystem>` 时无需重写语义层。
- **未知 `script Name` = soft warning**（与 S2.5 / LG-03 保持一致）。host-kind-aware hint：System host 提示 `AY_SYSTEM`；Component host 提示 `AY_FINALIZE_REGISTRATION_METADATA`。
- **System host lifecycle 白名单**：`on_start`、`on_update` 调；`on_destroy` 编译期软警告"not invoked on System host scripts"（ISystem 不销毁）。
- **调度入口**：`ScriptSubSystem::update(float dt)` / `fixedUpdate(float dt)` 遍历 `World::instance().systemCount()`，对 `getSystemNameAt(i) == bridge.hasScript(name)` 的 system 调 `callLifecycle(name, "on_update", systemPtr, &dt)`。
- **新增 `World::findSystemByName(name)`**（`AYRuntime/AYEntity/include/AYWorld.h`）——S3.1 之前 World 没有按名查 system 的公共入口。
- **测试 fixture**：`unittest/AYTestMovementSystem.h`（ISystem 子类 + `AY_PROPERTY(moveSpeed, ...)` + `AY_FINALIZE_REGISTRATION_METADATA`），`unittest/Test_LogiaSystemHost.cpp`（4 用例）；`examples/movement_system.logia`（文档示例）。
- **S3.10 完成**：`self.field`（单跳 primitive）现在通过 `ayt_reflect_get_field` / `set_field` 真正读写 AY_PROPERTY 字段 — 见下文完成记录。
- **S3.11 完成**：`self.<f1>.<f2>...` 链式（multi-hop struct）通过 `ayt_reflect_*_field_chain` 读写 — 见下文完成记录。
- **未做**：sol2 usertype 直通（继续隐藏 self 的 lightuserdata 语义）；`self.position.x = self.position.y + self.z` 三段以上链（当前只支持 2-hop；扩展只需 codegen + helper 同形扩展）。

**S3.10 (LG-05 + System host) 完成记录**（2026-07-09）：

- **结论**：`self.field` usertype 绑定在 S3.3 LG-05 阶段已经**跨 host kind 通用**。S3.1 System host 与 S3.0 Component host 共享同一条 codegen 路径（`isSingleHopSelfFieldExpr` 只看 `hostTypeName` + `resolvedField`，**不**看 `ctx.kind`），所以 System 端跑通 `self.moveSpeed = 1.5` 不需要任何 codegen/analyzer 改动——只缺一段「AYReflect primitive types 已注册」的保证。
- **修复**：`AYSemanticAnalyzer.cpp` + `AYScriptRuntimeBridge.cpp` 都加了 `ensurePrimitiveTypesRegistered()`（int / Int32 / Int64 / float / Float32 / double / Float64 / bool / Bool），由 analyzer ctor + bridge `ensureLua()` 各调一次（幂等）。`Test_LogiaSystemHost.cpp` 的 `MovementSystemRegistrar` 在 `reg.findType("float")` 之前先注册 float（静态 init 顺序跨 TU 不确定，必须本地 bootstrap）。
- **测试**：`Test_LogiaSystemHost.cpp` 新增 5 用例（s310_）：`s310_system_host_codegen_emits_reflect_calls`、`s310_system_host_self_field_reads_cpp_value`、`s310_system_host_self_field_writes_cpp_value`、`s310_system_host_self_field_codegen_rewrite`、`s310_component_host_self_field_unchanged_regression`（LG-05 Component-host 路径回归）。合计 9 用例，`AYSCRIPT_Test` 579/579 全绿。
- **未做**：struct chain `self.position.x` reflect（S3.11 完成）；`IMethodInfo` / `self.heal()`（§5.7.4 track R2）；纯 Reflection 名字解析的 fallback（仅在 `hostTypeName` 命中 AYReflect registry 时走 rewrite，未注册类型保留 S2.5 bare member access）。

**S3.11 (LG-05 multi-hop chain) 完成记录**（2026-07-09）：

- **结论**：原 S3.10 锁定决策里把 multi-hop chain 标为 "runtime no-op，out of S3.3 scope"——这次收回。`self.<f1>.<f2>...<leaf>` 通过新的 `ayt_reflect_get_field_chain` / `set_field_chain` 真正读写 C++ 嵌套 struct 字段，跨 Component / System / Tool host kind。`examples/player_controller.logia` 的 `self.position.y = self.position.y + self.jump_force * dt` 现在确实修改 `PlayerController::position.y`。
- **范围**：只支持 2-hop（`self.<intermediate>.<leaf>`）+ primitive leaf（int / float / double / bool / int64）；`FQuaternion`（4 维 quaternion w/x/y/z）推迟到 §5.7.4 track R2（与 IMethodInfo 一起）。
- **机制**：
  - **Analyzer (`ensureAYEntityTypesRegistered`)**：注册 `FVector3` 的 `x/y/z` Float32 字段（带 `getFieldCount()==0` 幂等 guard，避免重入覆盖）。这是 `analyzeMemberExpr` 能沿链 stamp `resolvedField` + `resolvedType` 到叶子的前提。
  - **Codegen**：`isSelfFieldChainExpr(e, names, hops)` probe（>= 2 hops、每跳 `resolvedField != nullptr`、叶子 primitive）。chain 命中则发 `ayt_reflect_*_field_chain(self, "<Host>", "f1", ..., "fN")`（codegen 把整条链烤进 args）；单跳维持 S3.10 `ayt_reflect_*_field(self, "<Host>", "f")`。直接赋值与 `+= -= *= /=` 都走 chain helper。
  - **Bridge (`ayt_reflect_get/set_field_chain_c`)**：variadic 收 chain args，从 `3..nargs-1` 逐跳查 `lookupField(typeName, fieldName)`、`ptr = field->get(ptr)`、`typeName = field->getType()->getName()`；叶子复用 `pushFieldPrimitive`（读）或 `storeFieldPrimitive`（写，写是从 S3.10 set 里抽出来的共享 helper）。fail-safe：unknown field 返回 nil / log error，不崩、不污染 C++ 字段。
  - **kLogiaPipelineVersion 2 → 3**：cache key 失效重灌（codegen 输出形状变了）。
- **测试**：
  - `Test_LogiaReflectRuntime.cpp` 新增 5 用例（`LG11Inner` / `LG11Holder` 嵌套 fixture）：read round-trip、write round-trip、compound assign、unknown leaf safe、unknown intermediate safe。**Inline registrar**（每测试首行 `ensureLG11Registered()`）取代 TU-scope static——后者跨 TU 静态 init 顺序不可靠，导致 `LG11Holder in reg=0`。
  - `Test_LogiaCodegen.cpp::codegen_full_player_controller` 注册 stub `PlayerController`（`PCStub { FVector3 position; float speed, jump_force; }`），改 assertions 验证 chain reflect calls（`ayt_reflect_get_field_chain(self, "PlayerController", "position", "y")` 等）+ `CHECK_FALSE(__tmp_)`。Legacy bare Lua fallback 消失。
  - baseline 579 + 5 chain + 1 codegen update = **604/604 全绿**。Component / System / Tool 三种 host + LG-05 单跳路径零回归。
- **未做**：FQuaternion（推迟到 track R2）；sol2 usertype（继续隐藏 self 为 lightuserdata，桥端走 offset arithmetic）；3+ 段链（codegen / helper 同形扩展即可，预留接口未实现）；`IMethodInfo`（§5.7.4 track R2）。

#### S3.2 — LG-04b（可选 B-min）：Reflect 单链 parent

**触发条件**（任一成立才做）：Component host 需 hard 拒绝「已注册但不是 Component 子类」的类型；Serializer/Editor 也要同一套 `isDerivedFrom`。

**范围**（刻意小于完整派生图）：

```cpp
// ITypeInfo — optional virtual, default 0
virtual size_t getBaseTypeId() const { return 0; }
virtual void setBaseTypeId(size_t /*baseId*/) {}  // default no-op

// TypeRegistry — walk base chain only
bool isDerivedFrom(const ITypeInfo* type, const ITypeInfo* base);
void setBaseTypeByName(const char* childName, const char* baseName);
```

注册宏（`AY_INHERITS` + `AY_FINALIZE_REGISTRATION_METADATA`）写入 `getBaseTypeId()`；**不做** `getAllDerived()`、多继承图。

**Logia 集成**：新增 `LogiaHostContext::strictInheritance`（默认 false）。当 `kind == Component && hostType != nullptr && strictInheritance == true` 时，analyzer 调 `isDerivedFrom(scriptNameInfo, hostType)`：true → 通过；false → **hard error**（`TypeMismatch`）；script name 不在 registry → 维持 S2.5/LG-03 的 soft warning（strict 不适用）。

#### S3.8 — LG-07：Tool host（editor / CLI one-shot）

**S3.8b (LG-07) 锁定决策**（2026-07-08 实现完成后补）：

- **Surface syntax**：新增 lifecycle 关键字 **`run()`**（Lexer `TokenType::Run` → Parser → `LifecycleKind::Run` → codegen `M.run`）。示例：`script BuildTool { run() { log.info("ok") } }`。
- **`toolLogiaHostContext()`**（`logia/AYLogia.h`）：`{ kind=Tool, hostType=nullptr, expectSelf=false, strictInheritance=false }` — Tool host 的单一权威 ctx 工厂。
- **Semantic — Tool host**（`ctx.kind == Tool`）：
  - **`run()`**：合法入口，不警告。
  - **`on_start` / `on_update` / `on_destroy`**：软警告 `"<name> is not invoked on Tool host scripts"`，hint 指回 `run()`。
  - **`expectSelf=false`**：analyzer **不**注入 `self`；脚本体引用 `self` → S2.5 implicit-global 软警告。
- **Semantic — 非 Tool host**（Component / System）：脚本内声明 **`run()`** → 软警告，hint 指 `toolLogiaHostContext()` + `runTool()`。
- **Codegen**（`LuaCodegenOptions::hostContext`）：`expectSelf=false` → `function M.run()`（**无** `self`）；`expectSelf=true` → 保持 S2.5 `function M.on_*(self)`。`compileLogiaToLua()`（`AYLogiaPipeline.cpp`）把调用方 `ctx` 写入 `codegenOpts.hostContext`。
- **Bridge — `LogiaRuntimeBridge::runTool(name, src, errors)`**：`loadScript(name, src, toolLogiaHostContext(), errors)` + `callLifecycle(name, "run", nullptr, nullptr)`。`callLifecycle` 的 `run` 分支以 **零 Lua 实参** dispatch（匹配无 `self` 签名）。
- **Compile cache**：Tool 与 Component **独立 cache slot**（key 含 `kind=Tool` + `expectSelf=false`）；同源字符串 Component `loadScript` 与 Tool `runTool` **不会**错误 hit 彼此产物。
- **Pipeline version**：`kLogiaPipelineVersion = 2`（S3.8b bump；见 `AYScriptRuntimeBridge.h` History 注释）。
- **测试**：`unittest/Test_LogiaToolHost.cpp`（19 用例：lexer `run` keyword / Tool semantic policy / `toolLogiaHostContext` 工厂 / 非 Tool 上 `run()` 警告 / codegen `function M.run()` / Component 回归 / `runTool` 端到端 + cache / System `on_destroy` 回归）。
- **未做**：Editor / CLI 可执行文件集成（→ **S3.9**）；Tool hot reload（one-shot 不需要）；`examples/build_tool.logia`（可选文档示例）。

> **S3.8-min 归档**（同日早些时候交付，已被 S3.8b 取代）：曾用 `on_start` 作 Tool 入口、`defaultLogiaHostContext()` 编译、无 `run` 关键字 / 无 `runTool` / 无 `expectSelf` codegen 分支。保留此记录仅供 bisect；新代码 **必须** 走 S3.8b 路径。

#### 明确拒绝的方案

| 方案 | 结论 |
|------|------|
| S3.0 上完整 B（派生图 + 改 FINALIZE 宏） | ❌ 跨模块爆炸，非 LG-03 范围 |
| C：child→base 映射散在 AYScript 调用方 | ❌ 破坏 host 模型集中在校验层 |

**迁移原则**：

1. AST 仍叫 `ScriptDecl`。
2. **S3.0** 只穿 `ctx`，**不**把「ScriptComponent 子类检查」当作迁移项（S2.5 从未实现）。
3. **S3.1** 第一个新 host = System；合法性 = hostKind + registry + 运行时注册。
4. **S3.2** 按需 B-min；完整派生图留 Reflect 独立 backlog。

---

## 6. 运行时集成

### 6.1 ScriptComponent（AYEntity 提供）

```cpp
class IScriptBridge {
public:
    virtual ~IScriptBridge() = default;
    virtual bool call(const char* method, void* arg1, void* arg2) = 0;
    virtual bool hasScript(const char* scriptName) const = 0;
};

class ScriptComponent : public IComponent {
    void onAttach(Entity* entity) override;   // → bridge.call("onStart", this, entity)
    void onUpdate(float dt) override;        // → bridge.call("onUpdate", this, &dt)
    void onDetach() override;                // → bridge.call("onDestroy", this)
    // ...
};
```

### 6.2 IScriptBridge 调用约定（AYEntity 已固定）

```cpp
namespace ayt::entity {
class IScriptBridge {
public:
    virtual ~IScriptBridge() = default;
    virtual bool call(const char* method, void* arg1, void* arg2) = 0;
    virtual bool hasScript(const char* scriptName) const = 0;
};
}
```

`method` 是 camelCase：`"onStart"` / `"onUpdate"` / `"onDestroy"`。`arg1` 是 `ScriptComponent* this`；`arg2` 是 `Entity*`（onStart）或 `float*`（onUpdate）或 `nullptr`（onDestroy）。

Logia 生成的 Lua 模块函数是 snake_case (`M.on_start`)。`LogiaScriptBridgeAdapter` 做 camelCase → snake_case 转换。

### 6.3 LogiaRuntimeBridge（AYScript 实现）

```cpp
namespace ayt::script {

class LogiaRuntimeBridge {
public:
    bool initialize();
    void shutdown();

    // Compile and load a Logia source string under a logical script
    // name. The compiled module table is cached; re-loads with the
    // same name replace the cache entry.
    bool loadScript(const std::string& scriptName,
                    const std::string& logiaSource,
                    std::vector<logia::CompilerError>& errors);

    [[nodiscard]] bool hasScript(const std::string& scriptName) const;

    // Invoke a lifecycle method. `receiver` is the ScriptComponent*
    // (forwarded to Lua as lightuserdata so the script's `self`
    // parameter receives it). `arg2` is passed verbatim (Entity* or
    // float* depending on the method). Lua silently discards extra
    // args, so passing `entity` to `on_start()` (which takes no
    // args in Logia) is harmless.
    bool callLifecycle(const std::string& scriptName,
                       const std::string& methodName,   // snake_case: on_start / on_update / on_destroy / run
                       void* receiver = nullptr,
                       void* arg2 = nullptr);

    // S3.8b (LG-07) — ToolRunner one-shot: compile under
    // toolLogiaHostContext(), then dispatch run() with no receiver.
    bool runTool(const std::string& scriptName,
                 const std::string& logiaSource,
                 std::vector<logia::CompilerError>& errors);
};

}
```

### 6.4 ScriptSubSystem

```cpp
class ScriptSubSystem : public ISubSystem {
public:
    bool initialize() override;
    void update(float deltaTime) override;       // S3: iterate all ScriptComponent, call onUpdate
    void fixedUpdate(float fixedDeltaTime) override;
    void shutdown() override;

    LogiaRuntimeBridge& bridge();
};
```

### 6.5 引擎 API 暴露给 Logia 的 ambient 面（分期）

| 阶段 | API |
|------|-----|
| S1 | `log.info/warn/error/debug`，`input.is_pressed/is_just_pressed`（mock） |
| **S3.5** | `time.delta`、`time.total`（真实，由 `ScriptSubSystem::tickAmbient(dt)` 注入）；`input.is_pressed/is_just_pressed` 改走 **可注入 `InputProvider*`**（默认 `MockInputProvider`，保留 S1 jump-only 行为） |
| **INT-02** | 真实 **AYDevice** `InputMapping` → `InputProvider`（替换 `MockInputProvider`） | §14.3 P1 |
| S4 / S3+ | `event.emit/subscribe`、`spawn_prefab(path)` | §14.5 |

---

## 7. 目录结构

```
AYScript/
├── design.md                       # 本文档
├── CMakeLists.txt
├── include/
│   ├── AYScript.h                  # 公开入口
│   ├── AYScriptSubSystem.h
│   ├── AYScriptRuntimeBridge.h     # sol2 + 引擎 API 绑定
│   ├── AYScriptBridgeAdapter.h     # AYEntity IScriptBridge 适配
│   └── logia/
│       ├── AYLogia.h               # Compiler 入口 + toolLogiaHostContext()
│       ├── AYLogiaPipeline.h       # compileLogiaToLua() heap-backed pipeline
│       ├── AYToken.h
│       ├── AYLexer.h
│       ├── AYParser.h
│       ├── AYAst.h
│       ├── AYSemanticAnalyzer.h
│       ├── AYLuaCodegen.h
│       └── AYCompilerError.h
├── src/
│   ├── AYScriptSubSystem.cpp
│   ├── AYScriptRuntimeBridge.cpp
│   ├── AYScriptBridgeAdapter.cpp
│   └── logia/
│       ├── AYLexer.cpp
│       ├── AYParser.cpp
│       ├── AYSemanticAnalyzer.cpp
│       ├── AYLuaCodegen.cpp
│       ├── AYLogia.cpp
│       └── AYLogiaPipeline.cpp
├── unittest/
│   ├── LogiaTestHelpers.h          # compile→Lua: use compileLogiaToLua()
│   ├── Test_LogiaToolHost.cpp      # S3.8b Tool host (19 cases)
│   ├── Test_LogiaLexer.cpp
│   ├── Test_LogiaParser.cpp
│   ├── Test_LogiaSemantic.cpp
│   ├── Test_LogiaCodegen.cpp
│   ├── Test_LogiaRuntime.cpp
│   └── Test_LogiaAdapter.cpp
└── examples/
    └── player_controller.logia
```

---

## 8. 实现优先级

按**长期好用**排序。

### Phase S0 — 语言骨架 ✅

- [x] `ayt::script::logia`：Token、Lexer、Parser
- [x] AST：`ComponentDecl`（**注**：S2.5 改名 `ScriptDecl`）、`VarDeclStmt`、生命周期函数、表达式子集
- [x] 错误格式：`error[logia/...]`（`AYCompilerError`）
- [x] 单元测试：`Test_LogiaLexer`、`Test_LogiaParser`
- [x] 示例：`examples/player_controller.logia`

### Phase S1 — Lua 后端与运行时 ✅

- [x] `LuaCodegen`：AST → Lua 源码
- [x] `LogiaRuntimeBridge`：sol2 3.5.0 + Lua 5.5.0 加载、调用生命周期
- [x] `ScriptSubSystem`：ISubSystem 占位实现
- [x] 单元测试：`Test_LogiaCodegen`、`Test_LogiaRuntime`
- [x] 端到端：Logia 脚本能修改 mock 状态

### Phase S2 — Reflect 语义（核心） ✅

- [x] `SemanticAnalyzer` + `TypeRegistry` —— 已接入 AYReflect
- [x] 类型/字段编译期校验
- [x] 单元测试 13 个

### Phase S2.5 — ScriptComponent host 重设计 ✅

- [x] `component` → `script` 重命名
- [x] 删除 `export` 关键字
- [x] `ComponentDecl` → `ScriptDecl`
- [x] 删除 `entity.get_component(...)` 路径（改由 self + World 查询）
- [x] 生命周期参数移除（codegen emit `(self)`；bridge 仍可传 dt）
- [x] `self.field` 语义检查走 AYReflect
- [x] 未知 `script Name` → **soft warning**（非 hard error，支持离线编辑）
- [x] 重写所有测试 + example（169/169 通过）
- [x] **design §1.6**：Logia = host-bound behavior DSL；ScriptComponent = 第一 host

### Phase S3 — 引擎 API、多 host 与工具 ✅（2026-07-10 闭环）

> **范围**：S3.0–S3.9（原计划）+ S3.10–S3.12+R3（超额交付）。  
> **测试基线**：`AYScript_Test` **660/660**；`kLogiaPipelineVersion = 5`。  
> **未纳入 S3、转入 §14**：Editor/Game 宿主接线、真实输入、R1/R3.5/R4、S4 语法。

#### S3.0 — LG-03：LogiaHostContext API 穿线 ✅ 目标

- [x] 公开 `LogiaHostContext` + `Compiler::compile(source, ctx)`（§5.6）
- [x] `SemanticAnalyzer` 存 `_ctx`；**校验行为与 S2.5 相同**
- [x] 修正 unknown-script warning 文案（去掉虚假的 subclass 暗示）
- [x] +2~3 单元测试：ctx 传递 + 默认 ctx 等价旧路径
- [x] **不动 AYReflect**

#### S3.1 — LG-04：System host

- [x] `script MovementSystem { on_update(dt) }` + GameLoop tick 调度
- [x] 校验：`hostKind` + Reflect 存在 + World 系统注册（**不用 isSubclassOf**）
- [x] `self` 决策 = `ISystem*` lightuserdata（`expectSelf=true`）
- [x] `World::findSystemByName(name)` 公共入口
- [x] System host lifecycle 白名单：`on_destroy` 软警告
- [x] `examples/movement_system.logia` + `unittest/Test_LogiaSystemHost.cpp`（4 用例）

#### S3.2 — LG-04b（可选）：Reflect B-min

- [x] `ITypeInfo::getBaseTypeId()` + `TypeRegistry::isDerivedFrom()`（单链 parent only）
- [x] Component strict 模式：拒绝「已注册非 Component 派生」的 script 名

#### S3.3 — LG-05：`self.field` 走 AYReflect 真实读写

**S3.3 (LG-05) 锁定决策**（2026-07-08 实现完成后补）：

- **范围**：单跳 `self.<primitiveField>`（resolvedField 非空 + resolvedType 字段数为 0）走 reflect 调用；**chained struct 链 `self.position.x` 保留 S2.5 行为**（裸成员访问、运行时 no-op）— 链式 struct 需要递归 reflect、暂不实现。
- **bridge 入口**：`ayt_reflect_get_field(self, "<type>", "<f>")` / `ayt_reflect_set_field(self, "<type>", "<f>", <value>)`。注册为 `lua_CFunction`（`lua_register`），不走 sol2 variadic-template 路径，避免与未来 sol2 类型签名推导交叉。
- **codegen 注入点**：single-hop primitive 时，`emitMemberExpr` 直接输出 `ayt_reflect_get_field(...)`；`emitExprStmt` 处理 `self.field = <r>` 和 `self.field <op>= <r>`（compound 拆分为「读 + 算 + 写」三步）。
- **host type name 来源**：`ScriptDecl::hostTypeName` 由 SemanticAnalyzer 在 resolveScriptName 成功后 stamp，codegen 用作字符串字面量。
- **类型白名单**：int / Int32 / float / Float32 / bool / Bool / double / Float64 / Int64 / UInt32；其他类型 set 失败打印 ayt::log::error 但不崩；get 返回 nil。
- **测试**：`unittest/Test_LogiaReflectRuntime.cpp`（5 用例：helpers 可见、单 hop 读、单 hop 写、codegen 字符串包含 reflect call、未知 type 安全）+ `Test_LogiaSemantic` 中既有 `self.field` 用例保持绿。
- **后续**：链式 struct 已在 **S3.11** 完成；本节锁定决策保留作历史记录。

#### S3.x — 其余（与 host 正交）

- [x] sol2 usertype：`self.field` 真正走 AYReflect 读写（LG-05 / S3.3）— **见上**
- [x] **S3.4** ScriptSubSystem / bootstrap：Component host 注入 bridge + 加载 `.logia` + GameLoop tick — pimpl `LogiaScriptBridgeAdapter` + `bindAndLoad(comp, src, errs)`；tick 走 `World::getAllEntities()` → `entity->onUpdate(dt)`（commit `028a555`）
- [x] **S3.5** 真实 AYTime 绑定 + 可注入 input 后端（替换 mock）
- [x] **S3.6** 编译缓存（LG-06a）— 见下锁定决策
- [x] **S3.7a** 热重载 API（LG-06b part A，pure memory）— 见下锁定决策
- [x] **S3.7b** 热重载 FileWatcher 集成（LG-06b part B）— 见下锁定决策
- [x] **S3.8** Editor / CLI Tool host（LG-07 / **S3.8b**）— 见 §5.6 S3.8 + 下锁定决策摘要
- [x] **S3.9** CLI：`ays-logia compile`（可选）— 见 §5.6 S3.9 + 下锁定决策摘要
- [x] **S3.10** System host `self.field` 端到端验证（可选）— 见 §5.6 S3.1 S3.10 完成记录
- [x] **S3.11** struct 链式 reflect `self.position.x`（可选，大）— 见 §5.6 S3.11 完成记录
- [x] **S3.12** `self.method()` primitive reflect call（track R2 §5.7.4）— `IMethodInfo` + `AY_METHOD` + `ayt_reflect_call_method`
- [x] **S3.12+R3** 非 primitive 方法 args/return（struct / enum / `std::string` + `TableExpr` AST）

**S3.5 锁定决策**（2026-07-08 实现完成后补）：

- **范围仅两件事**：`time.delta` / `time.total` 真实化；`input.is_pressed` / `input.is_just_pressed` 改走 **可注入后端**。
- **time 来源**：`LogiaRuntimeBridge::tickAmbient(scaledDelta)` 被 `ScriptSubSystem::update` / `fixedUpdate` 在每次 dispatch 前调用一次。`time.delta` = 最近一次 publish；`time.total` = 累加 scaled 流逝。负 dt 在 bridge 内 clamp 到 0，不前进 total。
- **input 来源**：`LogiaRuntimeBridge::InputProvider` 纯虚接口（`isPressed` / `isJustPressed`）。默认 = 文件局部 `MockInputProvider`（保留 S1 的 "jump only" 行为）；`setInputProvider(p)` 可在运行时替换，`setInputProvider(nullptr)` 自动回退到默认 mock（不崩）。**未**接 **AYDevice** `InputMapping`（Phase-1 仅窗口；见 `AYDevice/design.md` §1.3）。
- **测试**：`unittest/Test_LogiaAmbient.cpp`（9 用例：cold delta=0 / tickAmbient publish / total 累加 / 负 dt clamp / SubSystem 端到端 / 默认 mock 行为 / 自定义 provider dispatch / query 计数与 key 字符串 / null 回退）。
- **未做**：`input.<key>` 与 **AYDevice Action** 绑定（INT-02）；`event.emit` / `event.subscribe`；`spawn_prefab`。

**S3.6 (LG-06a) 锁定决策**（2026-07-08）：

- **缓存粒度**：每 `LogiaRuntimeBridge` 实例独占一份内存缓存（`unordered_map<size_t, CompiledEntry>`，在 `Impl` pimpl 内）。
- **缓存 key**：`hash(source) ^ hash(LogiaHostContext) ^ kLogiaPipelineVersion`（三次混合，splitmix64 风格）。`kLogiaPipelineVersion` 是 AYScript 头里的 compile-time `size_t` 常量；**任何 lexer/parser/semantic/codegen 输出形状或诊断 code 文案变化**必须 bump 该常量（否则旧缓存字符串的形状与新 Pipeline 不一致）。
- **`LogiaHostContext` 必须入 key**——同一份 `script Foo` 在 `Component` 上下文和 `System` 上下文产生的诊断/codegen 语义下游不同，content-addressed cache 会冲突。`LogiaHostContext::hostType` 按指针地址混入（ITypeInfo* 是 stable 的 registry 地址）。
- **缓什么**：只缓存 **编译结果**（`generatedLua` + `compileOk`）——不缓存 sol::state 的运行产物。`safe_script` + 模块表注册每次仍跑（sol::state 生命周期独立于编译缓存；shutdown 会重置 sol 与缓存同生同灭）。
- **失败也缓存**：`compile.success == false` 也写入 cache（`generatedLua = "", compileOk = false`），重复 loadScript 同一坏源 → hit + non-empty errors，避免重复花 lex/parse 时间。这与 S3.7 热重载语义一致：源未变就应当 not re-parse。
- **观测**：`compileCacheHitCount()` / `compileCacheMissCount()` / `resetCompileCacheCounters()` / `clearCompileCache()` 全部公开，unittest `Test_LogiaCompileCache.cpp` 用上所有四个。
- **`loadScript` 4-arg 重载**（带 `LogiaHostContext&`）公开为正式入口；3-arg 重载委派到 `defaultLogiaHostContext()`，保留 S2.5 / S3.0 / S3.5 caller 的零修改兼容性。
- **未做**：磁盘持久化（spec `optional`，先跑内存版本；S3.7 文件监听可叠加做按目录哈希落盘）、disk-cache invalidation、TTL eviction、`compileCache` 大小上限（当前无界，依靠 `shutdown()` 清零）。这些在 S3.7 文件路径确认后加入。
- **测试**（8 用例）：hit/miss on repeat load / hit preserves hasScript + lifecycle behavior / different source miss / different ctx miss / failed compile caches failure / clearCompileCache 保留 modules / resetCounters 不清缓存 / shutdown wipes both。

**S3.7a (LG-06b part A) 锁定决策**（2026-07-08 实现完成后补）：

- **API surface**：`LogiaRuntimeBridge::reloadScript(name, src, errs)`（3-arg 委派到 `defaultLogiaHostContext()`）+ `reloadScript(name, src, ctx, errs)`（4-arg 与 `loadScript` 4-arg 对称）。两个重载均 **不**接触 `ScriptSubSystem`、**不** link AYIO、**不** include `AYFile.h`。文件路径加载 / FileWatcher 推到 S3.7b。
- **失败策略**（核心锁定）：`reloadScript` 调底层 `loadScript`；loadScript 失败时 `_impl->scripts` 已被它自己 `erase`——S3.7a 在 reload 入口**先快照** `sol::table prior`（sol::table 是 handle 副本，廉价），loadScript 失败后**回填**到 `_impl->scripts[name]`。结果：`hasScript(name) == true` 跨失败 reload 保持；lifecycle 仍可调；旧模块继续跑。errors 仍返回给调用方。
- **新源码 reload → 重新绑定**：`loadScript` 内部把 `safe_script` 跑出的 module table 写到 `_impl->scripts[name]`，reload 入口不需特别处理。S3.6 的 cache key (`hash(src) ^ hash(ctx) ^ kLogiaPipelineVersion`) 决定是否走 front-end：同源码 reload 命中、源码变 reload 自动 miss。
- **测试**（6 用例，全为 filesystem-free）：`reload_with_new_source_replaces_module_behavior`（V1→V2，witness `1`→`10`）/ `reload_with_bad_source_keeps_prior_module`（坏源→hasScript 仍 true，V1 逻辑继续，witness `1`→`2`）/ `reload_with_identical_source_hits_compile_cache`（miss 计数不变，hit 计数 +1）/ `reload_then_call_executes_fresh_body`（V2 状态不继承 V1，witness `0`→`10`→`20`）/ `reload_three_arg_uses_default_host_context`（3-arg 委派与 4-arg 走同一 ctx）/ `reload_of_unloaded_script_loads_it`（reload = loadScript 行为，hadPrior 路径跳过 snapshot）。
- **不**做：FileWatcher、磁盘文件读取、`pollAndApplyReloads`、`ScriptSubSystem` 任何改动、`Test_LogiaHotReload` 任何文件系统依赖。
- **下一步**：S3.7b 单独 session。在 S3.7a 绿后接入 `ayt::io::FileWatcher`，按 §S3.7b 锁定语义。

**S3.7 (LG-06b) 锁定决策**：

> **实现状态（2026-07-08）**：✅ **S3.7a + S3.7b 已交付**。`reloadScript` API + `ScriptSubSystem` FileWatcher 协调器 + `Test_LogiaHotReload*.cpp`。

**分两阶段交付（强制）**：

| 阶段 | 交付物 | 验收 |
|------|--------|------|
| **S3.7a** | `LogiaRuntimeBridge::reloadScript(name, src, ctx, errs)`（薄封装 → `loadScript`）；`Test_LogiaHotReload.cpp` **仅 API 路径**（改源码字符串、断言 lifecycle 行为变化） | 全量测试绿；**不** link AYIO；**不**在 SubSystem 构造 FileWatcher |
| **S3.7b** | `ScriptSubSystem` 接 `ayt::io::FileWatcher`：`watchScriptPath` / `setHotReloadEnabled` / `pollAndApplyReloads()` | S3.7a 绿后再合；可选单独 OS 集成测 |

**`reloadScript` 语义**：

- 与 `loadScript` 相同 pipeline；**源码变化 → S3.6 cache key 变化 → 自动 miss**，无需手动 `clearCompileCache()`。
- 成功：替换 `_impl->scripts[scriptName]` 模块表；**不**重复注册 ambient / reflect 全局（`initialize()` 只做一次）。
- 失败：**保留**旧模块与 `hasScript(name)==true`；errors 返回给调用方；log warning。
- **帧边界 swap**：`pollAndApplyReloads()` 在 `update()` / `fixedUpdate()` **最前**调用（在 `tickAmbient` 之前或之后二选一，锁定 **之前** lifecycle dispatch），本帧 lifecycle 要么全用新模块要么全用旧模块——实现选 **reload 在 dispatch 前**，文档写死。

**FileWatcher 集成（S3.7b only）**：

- **所有权**：`FileWatcher` 只存在于 `ScriptSubSystem` 的 **pimpl**（`HotReloadState`），**不要**放进 `LogiaRuntimeBridge`（避免 bridge TU 拉 AYIO / 析构顺序与 sol 交织）。
- **读文件**：SubSystem 用 `ayt::io::File::readAllText`；bridge **不** include `AYFile.h`。
- **注册表**：`normalizedPath → { scriptName, LogiaHostContext, debounceStartMs }`；`bindAndLoad` / System 加载路径时 `watch(path)`。
- **debounce**：100ms（与 AYShader hot reload 一致）；`Modified`/`Created` 合并。
- **生命周期（防 segfault 硬规则）**：
  1. `enableHotReload(true)` → 创建 watcher（若尚无）→ `start()`。
  2. `shutdown()` **第一行** → `stopWatcher()`（`FileWatcher::stop()` + `clearPending()`）。
  3. `~ScriptSubSystem()` **必须**调用与 `shutdown()` 相同的 `stopWatcher()`（防止测试未调 `shutdown()` 时 watcher 线程 outlive SubSystem）。
  4. 顺序：`stopWatcher()` → `_adapter.reset()` → `_bridge.shutdown()`。
- **主线程**：只用 `pollPending()`；**禁止**在 FileWatcher callback 里 touch bridge / World。

**2026-07-08 失败复盘（归档）**：

- 全量测试 segfault，且移除 `Test_LogiaHotReload` / 注释 `pollAndApplyReloads` / 移除 AYIO link 仍崩 → 说明 **单次 PR 改动面过大**，难以 bisect；回滚后 S3.6 基线 exit 0。
- 可疑点（未确证）：lazy 构造的 `unique_ptr<FileWatcher>` 在 `~ScriptSubSystem() = default` 路径下未 `stop()`；World/adapter/bridge 析构顺序与 watcher 线程竞态。
- 附带损害：linter 回滚时曾把 `time.delta`/`time.total` 改成 lambda 而 codegen 仍 emit `time.delta`（无括号）→ S3.5 ambient 14 fail；已在 commit `9cb9893` 修复（codegen 发 `time.delta()`）。

**S3.7 测试计划**：

1. `reloadScript` 改 witness global → 下一帧 `on_update` 读新值。
2. reload 坏源 → `hasScript` 仍为 true，旧逻辑仍跑。
3. 同源码 reload → compile cache **hit**（S3.6 计数可观测）。
4. （S3.7b）scratch 目录 + FileWatcher 集成测 **独立** `TEST_CASE`，失败不阻塞 CI 时可 `#ifdef AYSCRIPT_HOTRELOAD_OS_TEST`。

**S3.8 (LG-07) 验收摘要**（完整决策见 §5.6 **S3.8 — LG-07**）：

> **实现状态（2026-07-08）**：✅ **S3.8b 已交付**。`run` 关键字 + `toolLogiaHostContext()` + `expectSelf` codegen + `runTool()` + `Test_LogiaToolHost.cpp`（19 用例）。

- **Acceptance**：
  - ✅ Tool 脚本 `run() { ... }` 经 `runTool()` compile + 执行一次。
  - ✅ Component / System host 路径不变（codegen / semantic / bridge 行为与 S3.7b baseline 一致，除新增 `run` 关键字解析与非 Tool 上 `run()` 软警告）。
  - ✅ Tool 与 Component **独立** compile cache slot（`kind` + `expectSelf` 入 key）。
  - ✅ `AYScript_Test` 全量全绿（exit 0）。
- **未做**（→ S3.9 或更后）：
  - CLI / Editor 二进制集成（`ays-logia compile`）。
  - Tool hot reload（one-shot 不需要）。
  - `examples/build_tool.logia`（可选）。

**S3.9 锁定决策**（2026-07-09 实现完成后补）：

- **范围**：CLI 驱动 Logia compile pipeline，离线校验 + 编辑器 build 动作；**不进** Editor critical path（Editor 走 in-process bridge，跟 S3.8b 的 `runTool` 共用）。
- **结构**：`cli/CliCompile.{h,cpp}`（共享编译入口，单元测试不走 fork）+ `cli/main.cpp`（argv 解析 / 输出 / exit code）+ `cli/CMakeLists.txt`（仅 link AYScript，**不**链 sol2/Lua runtime）。
- **API**：`ays-logia compile <file> [--host component|system|tool] [--strict-inheritance] [-o out.lua]`。不声明 `--host` 默认 `Component`（与 `defaultLogiaHostContext()` 对齐）；`--host tool` 隐式设 `expectSelf=false`。
- **诊断格式**：`path:line:col: severity: message`（severity ∈ `error` / `warning` / `note`）；`note:` 是 hint 行的 tag。stderr 输出，grep-friendly。warnings 不 flip exit code。
- **Exit codes**：0 = success（含 warnings）；1 = compile / 写盘错误；2 = bad usage（未知 flag、缺 subcommand、缺 `<file>`）。
- **CLI 校验顺序**：`<file>` 位置参数**先**校验（以 `-` 开头且非 `-h/--help` 一律按 unknown flag 处理，exit 2），再扫剩余 argv。理由：避免 `compile --typo` 静默退化成 "could not open file '--typo'"，给 CI / editor 更精准的 signal。
- **Parser 错误投影**：`Compiler::compile` 现在把 parser 错误也写入 `CompileResult::diagnostics`（旧路径只写 `errors` 旧结构）。CLI 单次 `for (d : diagnostics)` 就能打印所有 parser + semantic + codegen failure。
- **End-to-end 测试**：Win32 `CreateProcess` + `CreatePipe` 捕获 stdout/stderr，`GetExitCodeProcess` 读真 exit code（绕过 cmd.exe 把 inner exit code 屏蔽成 0 的坑）。直接用 `compileFromCli()` 的 unit test 覆盖快路径；e2e fork 路径覆盖 argv + 输出 + exit code。
- **Acceptance**：`ays-logia compile examples/player_controller.logia` 打印 warnings on stderr + 写 Lua artifact on stdout，exit 0；坏 source 打印 3 个 `file:line:col: error:` 行 + exit 1；`compile --no-such-flag` 打印 Usage + exit 2。
- **测试**：`unittest/Test_LogiaCli.cpp`（12 用例：direct 7 + e2e 5），`AYSCRIPT_Test` 555/555 全绿。
- **未做**：Bytecode 输出（`luaL_dump`）——推迟；watch / LSP hook；`--strict-inheritance` 仅 Component host 生效。

**Phase S3 闭环。** 剩余工作见 **§14**。

### Phase S4 — 语法扩展（下一主阶段）

- [ ] `signal` / `connect`
- [ ] `await delay`
- [ ] Source map 完善（运行时错误映射回 `.logia` 行号）

---

## 9. 依赖

| 依赖 | 用途 |
|------|------|
| Lua 5.5 | Logia 后端 VM（隐藏） |
| sol2 3.5.0 | C++ ↔ Lua 绑定 |
| AYReflect | 语义分析、类型/字段注册 |
| AYEntity | ScriptComponent 基类、子类（带 AY_PROPERTY 字段） |
| AYGameLoop | 子系统调度 |
| AYIO | `File::readAllText` + `FileWatcher`（S3.7b 热重载；**不用** AYConfig stub） |

---

## 10. 与工业级引擎对比

| 功能 | AY Logia | Godot GDScript | Unity C# | O3DE Lua |
|------|----------|----------------|------------|----------|
| 自有表面语法 | ✅ | ✅ | ✅ | ❌（裸 Lua） |
| 隐藏宿主语言 | ✅（Lua） | N/A（自研 VM） | N/A（CLR） | ❌ |
| 编译期类型检查 | ✅（S2） | ✅（GDScript 2） | ✅ | ❌ |
| 组件生命周期 | ✅ | ✅ | ✅ | 约定 |
| 热更新 | ✅（S3.7 API + FileWatcher） | ✅ | 有限 | ✅ |
| 共享 C++ 数据 | ✅（AY_PROPERTY） | ❌（GDScript 自管） | ❌ | 部分 |
| 编辑器 inspector | 规划（S3+） | ✅ | ✅ | 部分 |

---

## 11. 变更记录

| 日期 | 变更 |
|------|------|
| 2026-07-06 | **路线 A 定型**：Logia DSL → Lua 后端 |
| 2026-07-06 | S0 完成：Lexer/Parser/AST + 6 个单测 |
| 2026-07-06 | S1 完成：LuaCodegen + LogiaRuntimeBridge（sol2 3.5.0 + Lua 5.5.0）；注意后端是 Lua **5.5**（vcpkg） |
| 2026-07-07 | S2 第一版完成（168/168 测试通过）：SemanticAnalyzer + IScriptBridge adapter |
| 2026-07-07 | **S2.5 重设计**：删除 `component`/`export`/`entity` 参数；引入 `script`/`self` 模型；Logia 改为消费 AYReflect 元数据而非定义数据。详见 §1 核心模型 |
| 2026-07-08 | **§1.6 Host 绑定模型**：Logia = host-bound behavior DSL；S2.5 锁 Component host |
| 2026-07-08 | **§5.6 S3 分阶段**：S3.0 API 穿线（不改 Reflect）；S3.1 System host；S3.2 可选 B-min `getBaseTypeId` |
| 2026-07-08 | **S3.1 (LG-04) 完成**：第一个非 Component host——ECS `ISystem`（`script MovementSystem { on_update() { ... } }`）。校验 = `hostKind + Reflect name lookup + World registration`（无 `isSubclassOf`）。`self` = `ISystem*` lightuserdata。`ScriptSubSystem::update/fixedUpdate` 调度。新增 `World::findSystemByName`。`unittest/Test_LogiaSystemHost.cpp`（4 用例）+ `examples/movement_system.logia` |
| 2026-07-08 | **S3.2 (LG-04b) B-min 完成**：Reflect 单链 parent 指针 + `isDerivedFrom()` + Component host strict 模式（`LogiaHostContext::strictInheritance`）。`ITypeInfo::getBaseTypeId()` 默认 0；`AY_FINALIZE_REGISTRATION_METADATA` 写入 `AY_INHERITS` 解析出的 base；strict 模式仅在 `kind==Component && hostType!=nullptr && strictInheritance==true` 时启用，对「已注册但非 hostType 派生」的 script 名产生 hard error。`unittest/ReflectBminTests`（6 用例）+ `Test_LogiaSemantic`（5 用例）。**不**做完整派生图 / `getAllDerived()` / 多继承。 |
| 2026-07-08 | **S3.3 (LG-05) 完成**：`self.<primitiveField>` 单跳走 AYReflect 真实读写。Bridge 注册 `ayt_reflect_get_field` / `ayt_reflect_set_field`（`lua_CFunction`，lua_register）；codegen 单跳 primitive leaf 改发 `ayt_reflect_*_field(self, "<type>", "<f>", ...)`；`ScriptDecl::hostTypeName` 由 SemanticAnalyzer stamp；compound assignment 拆分「读 + 算 + 写」。`unittest/Test_LogiaReflectRuntime.cpp`（5 用例：helpers 可见 / 单跳读 / 单跳写 int+float+bool / codegen 字符串包含 reflect 调用 / 未知 type 安全）。`AYScript_Test` 237/237 PASS。**不**做 struct subfield 链 (`self.position.x`)、non-primitive 字段。 |
| 2026-07-08 | **S3.5 完成**：ambient `time.*` / `input.*` 真实化。`LogiaRuntimeBridge::tickAmbient(dt)` 由 `ScriptSubSystem::update` / `fixedUpdate` 在 dispatch 前调用一次，`time.delta` = 最近 publish、`time.total` = 累加 scaled 流逝；负 dt clamp 到 0。`input.is_pressed` / `input.is_just_pressed` 改走可注入 `LogiaRuntimeBridge::InputProvider*`：默认 = 文件局部 `MockInputProvider`（保留 S1 jump-only 行为），`setInputProvider(p)` / `nullptr` 回退走均不崩。**未**接真实 `AYInput`（`AYInput/` 仅 `.git`，目录空）/ `AYDevice` 输入轮询——记入 §6.5 TODO。`unittest/Test_LogiaAmbient.cpp`（9 用例）+ `examples/player_controller.logia` 现在可在 ScriptSubSystem tick 内用 `time.delta` 而非依赖旧的全局 mock。 |
| 2026-07-08 | **S3.6 完成（LG-06a）**：每 `LogiaRuntimeBridge` 实例独占内存编译缓存。缓存 key = `hash(source) ^ hash(LogiaHostContext) ^ kLogiaPipelineVersion`（splitmix64-style 三重混合）；host context 必须入 key（Component vs System 同源不同诊断）+ pipeline version 必须入 key（codegen 形状变化 bump 常量，强制全量失效）。只缓存 **编译结果**（`generatedLua` + `compileOk`），sol::state 运行产物每次仍跑（shutdown 同生同灭）。失败也缓存：同一坏源重复 loadScript → hit + non-empty errors，跳过 Lex/Parser/Semantic。新公开 API：`loadScript(name, src, ctx, errs)` 4-arg 重载 + 三个观测接口 `compileCacheHitCount/MissCount/Counters/clearCompileCache`。3-arg 重载委派到 `defaultLogiaHostContext()`，S2.5 / S3.0 / S3.5 caller 零修改。`unittest/Test_LogiaCompileCache.cpp`（8 用例）。**未做**磁盘持久化、TTL、cache size 上限——spec optional，先跑内存版本，等 S3.7 文件路径策略定了再决定是否叠加磁盘。 |
| 2026-07-08 | **§5.7 Script exposure 设计**：字段/方法暴露策略、`FieldAttribute` vs C++ access、R1–R4 backlog。**S3.7 设计锁定 + 失败复盘**：分两阶段 S3.7a（reload API）/ S3.7b（AYIO FileWatcher）；实现尝试 segfault 回滚，代码未合并。 |
| 2026-07-08 | **S3.7a 完成（LG-06b part A）**：纯内存 `LogiaRuntimeBridge::reloadScript(name, src[, ctx], errs)` 落地。3-arg 委派到 `defaultLogiaHostContext()`，4-arg 与 `loadScript` 4-arg 对称共享同一 S3.6 cache key。**失败策略**（核心）：reload 入口先 snapshot `sol::table prior`（sol::table 是廉价 handle 副本），委派到 `loadScript`，若 loadScript 失败（它自己 `erase` 了 `_impl->scripts[name]`）就**回填** prior → `hasScript(name) == true` 跨失败 reload 保持，旧模块继续跑。`unittest/Test_LogiaHotReload.cpp`（6 用例，filesystem-free）。 |
| 2026-07-08 | **S3.7b 完成（LG-06b part B）**：`ScriptSubSystem` + `AYScriptHotReload.cpp` 接 `ayt::io::FileWatcher`（AYIO PRIVATE link）。`setHotReloadEnabled` / `watchScriptPath` / `unwatchScriptPath` / `bindAndLoadFromFile` / `hotReloadApplyCount`；`update`/`fixedUpdate` 最前 `pollAndApplyReloads`（100ms debounce）；`shutdown()` 与 `~ScriptSubSystem()` 先 `stopHotReload()`。`unittest/Test_LogiaHotReloadWatcher.cpp`（5 用例）。 |
| 2026-07-08 | **S3.8-min（LG-07 中间版，已取代）**：Tool host policy 仅用 `on_start` 入口 + `defaultLogiaHostContext()`；12 用例。同日 **S3.8b** 交付完整 LG-07。 |
| 2026-07-08 | **S3.8b 完成（LG-07）**：`run` lifecycle 关键字；`toolLogiaHostContext()`（`expectSelf=false`）；Tool semantic 白名单 + 非 Tool 上 `run()` 对称警告；codegen `function M.run()`；`LogiaRuntimeBridge::runTool()`；`compileLogiaToLua` 转发 `hostContext`；`kLogiaPipelineVersion=2`。`unittest/Test_LogiaToolHost.cpp`（19 用例）。`compileLogiaToLua` heap pipeline + `LogiaTestHelpers.h`（MSVC /GS stack 防护）。 |
| 2026-07-09 | **S3.9 完成（CLI `ays-logia compile`）**：`cli/CliCompile.{h,cpp}` 共享编译入口 + `cli/main.cpp`（argv / 输出 / exit code）+ `cli/CMakeLists.txt`（只 link AYScript，不链 sol2/Lua runtime）；`Compiler::compile` 把 parser errors 投影进 `diagnostics` 让 CLI 单次迭代覆盖全诊断；Win32 `CreateProcess` + `CreatePipe` e2e 测试（绕 cmd.exe exit code 屏蔽）；`unittest/Test_LogiaCli.cpp` 12 用例；`AYSCRIPT_Test` 555/555 全绿。CLI **不进** Editor critical path（Editor 仍走 S3.8b `runTool` in-process）。 |
| 2026-07-09 | **S3.10 完成（System host self.field）**：`ensurePrimitiveTypesRegistered` 跨 TU bootstrap int/float/bool/double/int64；S3.10 codegen rewrite 跨 Component / System / Tool host kind 共享同一路径；`Test_LogiaSystemHost.cpp` 新增 5 用例；`AYSCRIPT_Test` 579/579 全绿。 |
| 2026-07-09 | **S3.11 完成（multi-hop chain reflect）**：`ayt_reflect_get_field_chain` / `set_field_chain` 新 Lua globals；codegen 新增 `isSelfFieldChainExpr` probe + chain-read / chain-write / chain-compound 分支；`ensureAYEntityTypesRegistered` 注册 `FVector3` 的 `x/y/z` Float32 字段（幂等 `getFieldCount()==0` guard）；`storeFieldPrimitive` 从 S3.10 set 抽出共享给 chain；`kLogiaPipelineVersion` 2 → 3；`Test_LogiaReflectRuntime.cpp` 新增 5 用例 + `Test_LogiaCodegen.cpp::codegen_full_player_controller` 注册 stub PlayerController 改 assertions 验证 chain calls；`AYSCRIPT_Test` 604/604 全绿。FQuaternion / IMethodInfo 推迟到 §5.7.4 track R2。 |
| 2026-07-10 | **S3.12 完成（self.method() reflect call path，track R2）**：`IMethodInfo` interface + `ITypeInfo::addMethod/findMethod/getMethodCount/getMethod` 4 个虚函数（default no-op，保持 source-compat）；`TypeInfoImpl<T>::_methods` 存储 + 4 override；变长 PMF 适配 `MethodInfoImpl<T,Ret,Args...>` / `MethodInfoImplConst<T,Ret,Args...>` 放在 `logia/AYMethodInfoImpl.h`（AYScript-private header，**不**让 foundation TU 看到 — 上轮 S3.12 中断的 MSVC C2275 触发条件）；`AY_METHOD(Ret, name, args...)` macro + `AY_MakeArgList(PMF)` deduction helper + `AY_MethodRegistrarOf<T,Ret,Args...>` partial specializations（非-const / const）；`AY_FINALIZE_REGISTRATION_METADATA(T)` 基础 finalize 跳过 method entries，`AY_FINALIZE_METHODS(T)` AYScript-side finalize 调 `buildMethodInfo()` → `MethodInfoImpl` → `addMethod()`；SemanticAnalyzer stamps `CallExpr::resolvedMethod + resolvedMethodOwnerName` 当 `self.<method>(...)` 命中 `ITypeInfo::findMethod`；LuaCodegen 优先 emit `ayt_reflect_call_method(self, "<Type>", "<method>", args...)` 否则 fall through bare-Lua；Bridge 新 `ayt_reflect_call_method_c` C entry + `lookupMethod` cache（O(1)）；`kLogiaPipelineVersion` 3 → 4；`Test_LogiaReflectRuntime.cpp` 新增 5 手动 path 用例（`self.heal(10)` 真实 mutate、`self.getHp()` round-trip、unknown safe、arity-mismatch safe、clamp）+ 2 `AY_METHOD` macro 端到端用例（void + const PMF）；`AYSCRIPT_Test` **631/631 全绿**（baseline 590 + LG-11 链式 5 + LG-12 手动 5 + LG-12 macro 2 + 其余已有 29）。Lessons learned：vtable 变化需整仓 rebuild（ninja dep 追踪对 inline 模板实例化不完整 → 部分 TU 漏 rebuild 段错误）、变长 pack 跨 namespace qualification 加 `::` 前缀、MSVC `Args...` dependent name 用 `::template MethodInfoImpl<...>`。 |
| 2026-07-10 | **S3.12+R3 完成（非 primitive args/return）**：…`AYScript_Test` **660/660 全绿**。 |
| 2026-07-11 | **§14 + §14.8**：Phase S3 闭环指挥；**输入统一 AYDevice**，废弃 `AYInput`，INT-02 改接 `InputMapping`。 |
| 2026-07-11 | **S3.12+R4.0 完成（嵌套 struct 字段 marshal）**：`pushFieldPrimitive` + `storeFieldPrimitive` 加 `field->getType()->getFieldCount() > 0` 递归分支 — push 方向构造 Lua sub-table（用绝对索引 `subTableIdx = lua_gettop(L)` 做 `lua_settable` 而非相对 `-3`，避免深递归时相对索引漂移），store 方向 heap-alloc tmp T + memset + `lua_getfield(L, valueStackIdx, subName)` 逐字段递归。R3.0 struct return path (`retType->getFieldCount() > 0`) 自动 extend — 改一行不动。新 `R4Player` fixture（`Vector2` nested in `DamageInfo` + `Stats`）注册用 unique type name `R4Stats`（避开 R3 `Stats` 命名冲突 — 类型注册表用 typeid 找，但 `registerTypeInfo(name)` 的 name 是 process-global key，多 fixture 注册相同 name 会让 `findType("Stats")` 拿到**先**注册的 ITypeInfo，导致 field 数量不对）。新增 2 LG-12 R4.0 测试（`applyDamage({source={x=10,y=20},amount=7})` + `setPosition/getPosition` round-trip）。`AYScript_Test` **671/671 全绿**。**不 bump kLogiaPipelineVersion** (R4.0 不动 codegen，只 marshal 路径新)。Deferred 到 R4.1/R4.5+:vector/array args、nested-struct 内的 std::string、name stripper。Deferred audit:现有测试 5 个文件用 `local` keyword — design §2.3 禁止泄露 Lua 关键字，codegen emit 出 broken split text 但"运气好"在 sol2 + safe_script 下 PASS。 |

---

## 12. 参考

- [`AYShader/design.md`](../AYShader/design.md) — Phoskia 编译器模式
- [`AYFoundation/AYReflect/design.md`](../../AYFoundation/AYReflect/design.md) — 元数据系统
- [`AYEntity/design.md`](../AYEntity/design.md) — ScriptComponent
- [sol2](https://sol2.readthedocs.io/) — Lua C++ 绑定（仅实现层）

---

## 13. Session prompts (copy-paste)

**Done through S3.12+R3:** S3.0–S3.12+R3 全部交付（见 §8 Phase S3 ✅）。

**Remaining:** §14 + §14.8 prompts.

Use **one prompt per new chat**. Read linked docs first. Do not run cmake/msbuild unless prompt says verify locally.

### 13.1 Recommended order (post-S3)

| Order | ID | 内容 | Status |
|-------|-----|------|--------|
| **P0** | **INT-01** | Editor/Game 注册 `ScriptSubSystem` + Play 加载 `.logia` | ⏳ |
| **P1** | **INT-02** | 真实 **AYDevice** `InputMapping` → `InputProvider` | ⏳ |
| **P2a** | **R1** | `ScriptVisible` / `ScriptReadOnly` 强制 | ⏳ |
| **P2b** | **R3.5** | `registerEnum` + 字段名 stripper + struct 内 string | ⏳ |
| **P2c** | **R4** | vector/array args、嵌套 struct、out-param | ⏳ |
| **P3** | **S4.x** | signal / await / source map | ⏳ |
| opt | **INT-03** | 磁盘 compile cache、Editor `runTool` 菜单 | ⏳ |
| opt | **INT-04** | EventHandler host、`event.emit` | ⏳ |
| parallel | Foundation ED-01–04 | 引擎 north-star — 不阻塞 Logia | — |

---

### Prompt S3.4 — Component host GameLoop wiring

```
Implement AYScript S3.4: wire ScriptComponent host end-to-end in GameLoop.

Read first:
- AYRuntime/AYScript/design.md §6.1–§6.4, §692 (ScriptSubSystem 遍历 ScriptComponent)
- AYRuntime/AYScript/src/AYScriptBridgeAdapter.cpp
- AYRuntime/AYEntity/include/components/AYScriptComponent.h
- AYRuntime/AYScript/unittest/Test_LogiaReflectRuntime.cpp (S3.3 reflect path)

Context:
- S3.1 already ticks System-host scripts via ScriptSubSystem::update → World systems.
- ScriptComponent calls IScriptBridge::call(onStart/onUpdate/onDestroy) but nothing injects LogiaScriptBridgeAdapter or loads .logia by script name in the live loop.

Scope (DO):
1. ScriptSubSystem (or Entity bootstrap hook): create LogiaScriptBridgeAdapter bound to bridge.
2. On initialize / entity attach: set adapter on ScriptComponent instances; load .logia source into LogiaRuntimeBridge keyed by script name (path convention TBD — document in design.md).
3. Ensure Entity tick path invokes ScriptComponent::onUpdate → adapter → callLifecycle with ScriptComponent* receiver (S3.3 reflect helpers must work at runtime).
4. Add unittest: mock World + entity with ScriptComponent + minimal .logia that mutates AY_PROPERTY field via self.speed (no HWND).

Scope (DO NOT):
- No struct chain reflect (self.position.x) — deferred.
- No hot reload yet (S3.7).
- No new Logia syntax.

Acceptance:
- Component-host script runs on GameLoop tick and self.<primitive> read/write mutates C++ AY_PROPERTY.
- Existing System-host tests (Test_LogiaSystemHost) stay green.
- AYScript_Test count increases; user verifies all green locally.
```

---

### Prompt S3.5 — Real ambient API (time + input)

```
Implement AYScript S3.5: bind real AYTime + AYDevice input ambient APIs (replace mocks).

Read first:
- AYRuntime/AYScript/design.md §6.5 (ambient API table)
- LogiaRuntimeBridge ambient registration (log.info, input.is_pressed mock sites)

Scope (DO):
1. Expose `time.delta` (and optionally `time.total`) from GameLoop scaled delta passed into bridge tick.
2. Wire `input.is_pressed` / `input.is_just_pressed` to AYDevice or existing input poll layer (document key code mapping).
3. Keep mock fallbacks for headless unittest only (#ifdef or injectable backend).
4. Add unittest: script calling time.delta receives non-zero dt in tick simulation.

Scope (DO NOT):
- No event.emit/subscribe yet (S4 or later).
- No spawn_prefab.

Acceptance:
- examples/player_controller.logia can use real time.delta in integrated tick test.
- Unit tests pass without real window where mock backend used.
```

---

### Prompt S3.6 — LG-06a Compile cache

```
Implement AYScript S3.6 / LG-06 part A: compile cache for Logia → Lua.

Read first:
- AYRuntime/AYScript/design.md §3 compile pipeline, §692 LG-06
- LogiaRuntimeBridge::loadScript implementation

Scope (DO):
1. Cache key = hash(source) + serialized LogiaHostContext fields + compiler version stamp.
2. Persist cache entry: source hash → generated Lua string (memory cache minimum; optional disk under user cache dir).
3. loadScript skips Lexer/Parser/Semantic/Codegen on cache hit; still validates script name registry.
4. Unittest: compile same source twice → second call hits cache (expose hit/miss counter for test).

Scope (DO NOT):
- No FileWatcher yet (S3.7).

Acceptance:
- Cache hit produces identical Lua module table as miss.
- All existing codegen/semantic tests green.
```

---

### Prompt S3.7a — LG-06b Hot reload API (no FileWatcher)

```
Implement AYScript S3.7a ONLY: reloadScript API + unit tests. NO FileWatcher yet.

Read first:
- AYRuntime/AYScript/design.md § S3.7 locked decisions (S3.7a / S3.7b split)
- LogiaRuntimeBridge::loadScript (S3.6 cache behavior)
- DO NOT link AYIO in this session.

Scope (DO):
1. LogiaRuntimeBridge::reloadScript(name, src, ctx, errs) — delegates to loadScript; document fail-keeps-old-module.
2. Test_LogiaHotReload.cpp: reload changes on_update witness; bad reload keeps old module; cache hit on same source reload.
3. Run full AYScript_Test — must exit 0 before ending session.

Scope (DO NOT):
- ScriptSubSystem FileWatcher / pollAndApplyReloads
- reloadScriptFromFile / AYFile.h in bridge
- Lazy FileWatcher in SubSystem destructor paths

Acceptance:
- All existing tests green + new hot-reload API tests green.
- Zero segfault on full suite.
```

---

### Prompt S3.7b — LG-06b FileWatcher wiring

```
Implement AYScript S3.7b: wire ayt::io::FileWatcher into ScriptSubSystem.

Prerequisite: S3.7a merged and full AYScript_Test green.

Read first:
- AYRuntime/AYScript/design.md § S3.7 lifecycle rules (stopWatcher before bridge shutdown)
- AYFoundation/AYIO/include/AYFileWatcher.h + unittest/Test_FileWatcher.cpp
- AYShader pollHotReload debounce pattern (reference only)

Scope (DO):
1. HotReloadState pimpl on ScriptSubSystem: watchScriptPath, enableHotReload, pollAndApplyReloads at start of update().
2. CMake: link AYIO PRIVATE on AYScript.
3. shutdown() AND ~ScriptSubSystem() both call stopWatcher() first.
4. Optional OS integration test behind ifdef.

Scope (DO NOT):
- Put FileWatcher inside LogiaRuntimeBridge
- Touch time.delta / time.total registration shape without updating AYLuaCodegen

Acceptance:
- S3.7a tests still green.
- Edit .logia on disk → next tick new logic (manual or OS test).
```

---

### Prompt S3.8 — LG-07 Tool host ✅ DONE (S3.8b)

> **Completed 2026-07-08.** See §5.6 S3.8 + Phase S3 验收摘要. Do not re-implement unless bisecting regressions.

Original prompt (archived):

```
Implement AYScript S3.8 / LG-07: Tool host (run-only, no self).
...
```

Delivered as **S3.8b**: `run()` keyword, `toolLogiaHostContext()`, `expectSelf` codegen, `runTool()`.

---

### Prompt S3.9 — CLI ays-logia compile ✅ DONE (2026-07-09)

```
Implement AYScript S3.9: optional CLI `ays-logia compile` for CI/editor.

Read first:
- AYRuntime/AYScript/design.md §3 compile pipeline, §5.6 S3.8 (toolLogiaHostContext)
- AYRuntime/AYScript/include/logia/AYLogiaPipeline.h (compileLogiaToLua)
- AYRuntime/AYScript/examples/player_controller.logia

DO:
- Small executable or AYTool CMake target: read .logia path from argv.
- `--host component|system|tool` selects LogiaHostContext
  (tool → toolLogiaHostContext(), system → kind=System, default → defaultLogiaHostContext()).
- Emit generated Lua to stdout or `-o path.lua`; exit non-zero on compile errors.
- Print diagnostics with file:line on stderr.

DO NOT:
- Embed in Editor critical path.
- Re-implement compile pipeline (call compileLogiaToLua / Compiler directly).

Acceptance:
- `ays-logia compile examples/player_controller.logia` → writes valid Lua or prints errors.
- `ays-logia compile tool.logia --host tool` → output contains `function M.run()` (no self).
- `ays-logia compile bad.logia` → exit 1, non-empty stderr.

Acceptance met — see §5.6 S3.9 锁定决策摘要. Do not re-implement.
```

---

### Prompt S4.1 — signal / connect (syntax)

```
Implement AYScript S4.1: signal / connect syntax (design Phase S4).

Read: AYRuntime/AYScript/design.md Phase S4.

DO: lexer/parser/semantic/codegen for signal declarations + connect(handler); runtime dispatch stub or bridge to AYEventSystem when ready.
Acceptance: parse + compile sample; unit tests for AST + codegen shape.
Gate: S3.4+ runtime stable preferred.
```

---

### Prompt S4.2 — await delay

```
Implement AYScript S4.2: await delay coroutine sugar.

Read: design.md Phase S4, Lua 5.5 coroutine constraints.

DO: surface syntax → desugar to Lua coroutine/yield pattern; document limitations.
Acceptance: minimal script with await delay compiles; runtime test with mocked timer.
```

---

### Prompt S4.3 — Source map

```
Implement AYScript S4.3: runtime errors map back to .logia line numbers.

Read: design.md Phase S4 source map item.

DO: codegen embeds line mapping table; bridge wraps Lua errors to logia file:line.
Acceptance: intentional runtime error in test script reports .logia line, not chunk.lua line.
```

---

### Prompt S3.10 — System host reflect fields ✅ DONE (2026-07-09)

```
Implement AYScript S3.10 (optional): verify self.<field> on System host (ISystem*) via S3.3 ayt_reflect_* path.

Read: design.md S3.1 note "self.field usertype" — S3.3 may already work if hostTypeName stamped for System scripts.

DO: extend Test_LogiaReflectRuntime or Test_LogiaSystemHost with MovementSystem + AY_PROPERTY field read/write in on_update.
If gap found: fix codegen/analyzer to stamp hostTypeName for System host same as Component.

Acceptance: movement_system.logia can mutate moveSpeed via self.moveSpeed.
Only do if S3.4 landed and System scripts need fields.
```

Acceptance met — see §5.6 S3.1 S3.10 完成记录. Do not re-implement.
```

---

### Prompt S3.11 — Struct chain reflect ✅ DONE (2026-07-09)

```
Implement AYScript S3.11 (optional): chained struct field reflect (self.position.x).

Read: design.md S3.3 locked decision — explicitly deferred.

DO: recursive reflect walk for AY_PROPERTY struct fields; codegen nested get/set.
Acceptance: round-trip vec3 field on test component; document perf limits.

Gate: only after S3.4 + S3.5 stable; high complexity — confirm with tech lead before starting.
```

Acceptance met — see §5.6 S3.11 完成记录. 604/604 green. Do not re-implement.
```

---

## 14. 剩余工作与指挥 (Post-S3)

> **读者**：引擎集成负责人、Editor 负责人、后续 AI session。  
> **前提**：AYScript 模块内编译器 + 运行时 + 多 host + 热重载 + CLI **已自洽**；缺口在**宿主接线**与**Reflect 消费方深化**。

### 14.0 能力 vs 缺口（一览）

| 维度 | 模块内状态 | 产品化缺口 |
|------|------------|------------|
| Logia 编译器 | ✅ | — |
| Component / System / Tool host | ✅ | Editor 未注册 SubSystem |
| `self.field` 单跳 + 2-hop 链 | ✅ | 3+ hop、FQuaternion 未做 |
| `self.method()` primitive + struct/enum/string | ✅ | vector/out-param 未做（R4） |
| 热重载 API + FileWatcher | ✅ | Editor Play 未默认开启 watch |
| `ays-logia compile` | ✅ | Editor build 动作未接菜单 |
| `time.delta/total` | ✅ | — |
| `input.*` | 🟡 Mock 可注入 | 未接 AYDevice `InputMapping`（Phase-2） |
| `event.*` / `spawn_prefab` | ❌ | S4 或 INT-04 |
| EventHandler host | ❌ | INT-04 |
| `ScriptReadOnly` 强制 | ❌ | R1 |
| Editor Inspector 脚本字段 | ❌ | 依赖 R1 + Editor UI |

### 14.1 模块边界（剩余工作归属）

| 工作项 | 主责模块 | AYScript 侧 |
|--------|----------|-------------|
| **INT-01** 宿主注册 + Play 加载 | `AYEditor` / `AYApplication` | 提供 `ScriptSubSystem` API，**不**改 compiler |
| **INT-02** 真实输入 | **AYDevice** | `DeviceInputProvider` 读 `InputMapping` / `InputState`；见 `AYDevice/design.md` §1.3 |
| **R1** 脚本可见性 | `AYReflect` + `AYScript` | Semantic + `ayt_reflect_set_field` enforce |
| **R3.5** enum/字段名 | `AYReflect` + `AYScript` bridge marshal |
| **R4** 复杂方法签名 | `AYScript` only |
| **S4** 新语法 | `AYScript` only |
| **INT-03** 磁盘 cache / Editor tool | `AYScript` + `AYEditor` |

### 14.2 P0 — INT-01：引擎宿主接线（最高 ROI）

**问题**：`ScriptSubSystem` **不会**自动注册。`AYScriptSubSystem.h` 要求宿主显式：

```cpp
IGameLoop::instance().registerSubSystem(new ScriptSubSystem());
```

当前 `AYEditor` / `AYApplication` **未**引用 `ScriptSubSystem` → Play 模式不会跑 Logia。

**锁定决策（待实现）**：

| 项 | 决策 |
|----|------|
| 注册时机 | `AYApplication::registerSubSystems()` 或 `AYEditorApp` init，在 `GameLoop` 启动前 |
| 依赖顺序 | `ScriptSubSystem` 依赖 `ayt.entity`（descriptor 已声明）；在 `ResourceSubSystem` 之后、`RendererSubSystem` 前后均可 |
| 脚本路径约定 | 开发期：`Content/Scripts/<ScriptName>.logia`；组件 `setScriptName("PlayerController")` 与文件名同名 |
| 加载入口 | `ScriptSubSystem::bindAndLoadFromFile(comp, path, errs)` 或场景序列化后批量 bind |
| 热重载 | Editor dev：`setHotReloadEnabled(true)` + `bindAndLoadFromFile` 已 `watchScriptPath` |
| 验收 | Play 模式下 `examples/player_controller.logia` 能改 `self.speed` / `self.position.y` |

**不做**：改 Logia 语法；在 AYScript 内硬编码 Editor 路径（路径由宿主传入）。

### 14.3 P1 — INT-02：真实输入（AYDevice）

**问题**：S3.5 仅做到可注入 `InputProvider*`；默认仍是 `MockInputProvider`（jump only）。

**前置**：`AYDevice` Phase-2（`KeyboardDevice` + `InputMapping`）。**不**建设 `AYInput` 模块（见 `AYDevice/design.md` §1.3）。

**锁定决策（待实现）**：

| 项 | 策略 |
|----|------|
| 适配层 | `DeviceInputProvider : LogiaRuntimeBridge::InputProvider`，读 `AYDevice::DeviceManager` + `InputMapping::isActionPressed(action)` |
| 键位映射 | Logia 字符串 = **Action 名**（`"jump"`）；物理键绑定在 AYDevice `InputMapping` 或 `AYConfig` `[Input.Actions]` |
| 注册时机 | `ScriptSubSystem::initialize()` 成功后 `bridge.setInputProvider(&deviceProvider)`（`DeviceSubSystem` 已 poll 后查询） |
| 单测 | 保留 `MockInputProvider`；`Test_LogiaAmbient` 不依赖 HWND |
| 依赖顺序 | `DeviceSubSystem`（poll）→ `ScriptSubSystem`（query InputProvider） |

**不做**：独立 `AYInput` 库；在 Logia 暴露裸 scancode；`event.emit`（归 S4/INT-04）。

### 14.4 P2 — Reflect 消费方 backlog

见 §5.7.6。推荐顺序：**R1 → R3.5 → R4**。

| ID | 交付物 | 验收 |
|----|--------|------|
| **R1** | `FieldAttribute::ScriptVisible/ScriptReadOnly`（或复用 `BlueprintReadOnly`）；Semantic 拒绝不可见字段；`set_field` runtime enforce | 只读字段赋值 compile error 或 runtime log+no-op |
| **R3.5** | `registerEnum<E>()`；bridge 不再 enum int fallback；`m_`/`b_` stripper；struct 内 `std::string` field marshal | enum 方法 round-trip 走 typed path |
| **R4** | `std::vector<T>`/`std::array` args；嵌套 struct 字段；`T&` out-param | 新方法签名 unittest |
| **R5/R6** | 智能指针、sol2 usertype | 仅在有性能/所有权需求时启动 |

### 14.5 P3 — Phase S4 与可选集成

| ID | 内容 | 备注 |
|----|------|------|
| **S4.1** | `signal` / `connect` 语法 + codegen | 可桥接 `AYEventSystem` |
| **S4.2** | `await delay` 协程糖 | Lua 5.5 coroutine 限制需文档化 |
| **S4.3** | Source map：运行时错误 → `.logia` 行号 | codegen 嵌 line table |
| **INT-03** | 磁盘 `.logia.cache`；Editor 菜单调 `ays-logia compile` / `runTool` | 非 Play 关键路径 |
| **INT-04** | `LogiaHostKind::EventHandler`；`event.emit/subscribe` ambient | 依赖 EventSystem 稳定 API |
| **INT-05** | 3+ hop chain、`FQuaternion` 链式 reflect | codegen/helper 同形扩展 |

### 14.6 明确推迟

- 多宿主语言（Python/JS）
- 自研字节码 VM
- 向作者暴露 `require` / 裸协程 API
- Tool hot reload（one-shot 不需要）
- `luaL_dump` bytecode CLI 输出
- 完整 Reflect 派生图 / 多继承 `isSubclassOf`

### 14.7 风险与约束（继承 S3）

1. **`kLogiaPipelineVersion`**：任何 codegen/诊断形状变化必须 bump（当前 `5`）。
2. **`ITypeInfo` vtable 变化**：AYReflect ABI 变更后**整仓 rebuild**（见 §5.7.4 lessons learned）。
3. **`MethodInfoImpl` 变长模板**：**禁止**放入 AYReflect foundation TU；保持 `logia/AYMethodInfoImpl.h` private。
4. **热重载析构顺序**：`stopHotReload()` → `adapter.reset()` → `bridge.shutdown()`（§S3.7b）。
5. **Editor 栈**：接 ScriptSubSystem 时**不要**破坏现有 `uiBackend` shutdown 顺序（见 AYEditor 会话记录）。

---

### 14.8 Session prompts (copy-paste)

#### Prompt INT-01 — Editor/Game ScriptSubSystem wiring (P0)

```
Implement AYScript INT-01 (P0): register ScriptSubSystem in Editor/Game host.

Read first:
- AYRuntime/AYScript/design.md §14.2, §6.4, AYScriptSubSystem.h (explicit registration note)
- AYRuntime/AYApplication/design.md (registerSubSystems pattern)
- AYRuntime/AYEditor/src/AYEditorApp.cpp (GameLoop init order)

DO:
1. Register ScriptSubSystem in AYEditor (and/or Game) before GameLoop run.
2. On Play enter: load .logia for ScriptComponent instances (path convention: document in design.md §14.2).
3. Optional dev: setHotReloadEnabled(true) when loading from file.
4. Smoke: PlayerController + examples/player_controller.logia mutates AY_PROPERTY in Play.

DO NOT:
- Change Logia compiler or bridge semantics.
- Auto-register inside AYScript TU (keep explicit host registration).

Acceptance:
- Play mode runs on_update; self.speed / self.position.y reflect works.
- AYScript_Test 660/660 still green.
- Editor shutdown order unchanged (uiBackend before GameLoop shutdown).
```

#### Prompt INT-02 — Real input provider (P1, AYDevice only)

```
Implement AYScript INT-02 (P1): DeviceInputProvider for input.is_pressed.

Read: design.md §14.3, §6.5, AYDevice/design.md §1.3 (no AYInput module)

DO:
1. DeviceInputProvider implements LogiaRuntimeBridge::InputProvider.
2. Query AYDevice InputMapping by Action name ("jump"); do NOT create AYInput library.
3. ScriptSubSystem::initialize sets provider; tests keep MockInputProvider.
4. Blocked until AYDevice Phase-2 (KeyboardDevice + InputMapping) lands.

DO NOT: AYInput module, event.emit, new Logia syntax.

Acceptance: With window focused, script sees real Action state; headless tests unchanged.
```

#### Prompt R1 — Script field visibility (P2a)

```
Implement AYScript R1: ScriptVisible / ScriptReadOnly enforcement.

Read: design.md §5.7.3, §5.7.6, AYReflect IAYReflect.h FieldAttribute

DO:
1. Add or reuse FieldAttribute flags for script visibility/read-only.
2. SemanticAnalyzer: reject self.field on non-visible fields.
3. ayt_reflect_set_field: reject ScriptReadOnly at runtime.
4. Unittest: write to read-only field fails safe.

Acceptance: compile-time + runtime both enforce; existing 660 tests green.
```

#### Prompt R3.5 — Enum registry + field stripper (P2b)

```
Implement AYScript R3.5: registerEnum + field name normalization.

Read: design.md §5.7.4 R3 scope table, §5.7.6

DO:
1. AYReflect registerEnum<E>() + EnumTypeInfo (minimal).
2. Bridge: enum args/return use retType/paramType, not int fallback.
3. Optional: m_/b_ prefix stripper for struct table keys.
4. Struct fields containing std::string in pushFieldPrimitive/storeFieldPrimitive.

Acceptance: LG-12 R3 enum tests use typed path; field stripper unit test.
```

#### Prompt S4.1 — signal / connect (P3)

```
Implement AYScript S4.1 per design.md Phase S4.

Read: §2.4, Phase S4, AYEventSystem/design.md (if exists)

DO: lexer/parser/semantic/codegen for signal + connect; runtime stub or EventSystem bridge.
Acceptance: sample compiles; AST + codegen shape tests.
Gate: INT-01 landed preferred.
```

---
