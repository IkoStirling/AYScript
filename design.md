# AYScript Design

> **命名来源**：Logia — λογία（逻辑 / 理据），与 Phoskia（φῶς + σκιά，光与影）成对：GPU 用 Phoskia 写材质，CPU 用 Logia 写玩法。
>
> **文档状态（2026-07-15）**：**Phase S0–S3 + S3.12+R3 + R4.0 + R4.1 + R4.1b + R4.1c + R4.1d + R4.2 + R4.2b + R5.0 + R5.0.1 + R5.1 + R5.2-A + R5.2-B + R5.2-C + R5.2-D + R5.2-E + R5.2-H + S5 ED-02 + S5 ED-03 已交付**；`AYScript_Test` **1066/1066** 全绿；`kLogiaPipelineVersion = 23`。详见 §5.7.4.x+3 R5.2-A → R5.2-H 静态类型校验系列 + §5.7.4.x+4 S5 ED-02 source-location stamping + §5.7.4.x+5 S5 ED-03 Lua runtime panic → Logia source line translation。
> **下一主阶段**：§14 剩余工作（引擎宿主接线 → 真实输入 → Reflect backlog → S4 语法）。R5.2 待选：R5.2-F (negative step) / R5.2-G (var step + runtime defense) / R5.2-I (multi-hop chain 收紧)。
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
4. **生命周期是约定方法名**：S2.5 标准集为 `on_start` / `on_update` / `on_destroy`；源码可写 `on_update(dt: float)`（参数被当真传入 Lua body，codegen emit `function M.on_update(self, dt)`；bridge 在 `callLifecycle` 同步把 `dt` 推到对应位置）。**S3** 其他 host 可定义 `run()`、`on_damage()` 等，由调度方调用。**2026-07-11 audit 调整**：原先的"documentary param"措辞已过时，跟 `emitLifecycleFunc` line 151-154 + `callLifecycle` line 1334 实情不符。玩家 controller 示例（`self.jump_force * dt`）真正在用了 dt。
5. **类型写引擎认识的**：`Entity`、`Transform`、`float` 等，由 Reflect 注册表解析。
6. **snake_case**：关键字与 API 统一蛇形命名。
7. **无 Lua 泄漏**：不提供 `local` / `nil` / `pcall` / `require` 等给用户。**2026-07-11 更新**：彻底硬拒会破坏 R3 / R4 测试的兼容性，所以 `local`/`nil` 暂保留为 Identifier（纯字面 emit），SemanticAnalyzer 改 emit 一条 soft warning（`LuaKeywordLeak`）把 leak 暴露给读者。R5+ 再考虑硬拒。`function` 已升级为 Logia keyword（script-block scope helper）。

### 2.4 后期语法扩展（非 v1 阻塞）

| 特性 | 说明 | 后端策略 |
|------|------|----------|
| `while (cond) { body }` | 条件循环 | `while cond do ... end`（R5.0，2026-07-13 ✅ 落地） |
| `for (var i : N) { body }` | 计数循环 | `for i = 1, N do ... end`（R5.0，2026-07-13 ✅ 落地） |
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
                 | "if" ["("] <bool_expr> [")"] <block> ["else" <block>]                                  (* R5.0.1: parens optional; R5.2-H: cond must be bool *)
                 | "return" <expression>? ";"
                 | "while" ["("] <bool_expr> [")"] <block>                                              (* R5.0.1: parens optional; R5.2-H: cond must be bool *)
                 | "for" ["("] "var" <identifier> ":" <int_expr>
                                  ("," <int_expr>)?              (* R5.0.1: range form start,end *)
                                  ("," <int_const_expr>)?        (* R5.2-D: optional positive-int-literal step (range form only) *)
                                  [")"] <block>                 (* R5.0.1: parens optional *)
                 | "break"                                                                          (* R5.1: must be inside a loop *)
                 | "continue"                                                                       (* R5.1: must be inside a loop *)
                 | "::" <identifier> "::"                                                            (* R5.2-B: loop-scoped label; body-visible to break :L *)

<expression>   ::= <assignment> | <logic_or>

(* 标准表达式层级：or → and → equality → comparison → add → mul → unary → postfix → primary *)
(* postfix 包含 member access：`self.position`、`tick_counter` *)

(* R5.2-H (2026-07-14): if / while conditions MUST reduce to bool.
   Logia has no implicit truthiness — `if (n)` where `n: int` is a
   hard error. Accepted bool_expr shapes (see R5.2-H for full matrix):
     - bool literal, bool-typed identifier / self.field / self.method()
     - comparison ops {==, !=, <, <=, >, >=} over two primitive leaves
     - logical ops {&&, ||} over two bool leaves
     - `!` (Bang) on a bool leaf
   Carve-outs (R5.2-H.b): for-loop counter identifiers count as int leaves;
   ambient-receiver CallExpr (`input.is_pressed(...)`) counts as bool. *)
<bool_expr>     ::= <literal_bool>
                  | <identifier_bool>
                  | <self_field_bool>
                  | <self_method_bool>
                  | "!" <bool_expr>
                  | <bool_expr> ("&&" | "||") <bool_expr>
                  | <int_expr> ("==" | "!=" | "<" | "<=" | ">" | ">=") <int_expr>
                  | <float_expr> ("==" | "!=" | "<" | "<=" | ">" | ">=") <float_expr>
                  | <string_expr> ("==" | "!=") <string_expr>
                  | <ambient_call>                                       (* R5.2-H.b: input.is_pressed(...) etc. *)

(* R5.2-C / R5.2-E: for-loop bound must statically reduce to int.
   Accepted: int literal, int-typed identifier, int-typed self.field /
   self.method(), arithmetic ops {+, -, *, /, %} over int-typed leaves.
   Rejected: int + float (no implicit promotion), string / bool leaves.
   Short form `for (var i : N)` accepts the same set minus arithmetic. *)
<int_expr>      ::= <int_literal>
                  | <identifier_int>
                  | <self_field_int>
                  | <self_method_int>
                  | <int_expr> ("+" | "-" | "*" | "/" | "%") <int_expr>
                  | "(" <int_expr> ")"                                   (* R5.2-E: (n + 1) - 1 *)

(* R5.2-D: for-loop step is STRICTER than bound — must be a literal
   int OR a constant-folded expression of int literals. Variables
   (e.g. `n - 1` where `n: int`) are REJECTED. Rationale: codegen
   cannot enforce `step != 0` without injecting a runtime check,
   which violates the "static-only" principle. See R5.2-G for
   future "non-const step" slice. *)
<int_const_expr> ::= <int_literal>
                   | <int_const_expr> ("+" | "-" | "*" | "/" | "%") <int_const_expr>
                   | "(" <int_const_expr> ")"
                   | "-" <int_const_expr>                               (* R5.2-D: unary negation *)
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

现有 `FieldAttribute`（`ayreflect/IReflect.h`）服务序列化 / 编辑器 / 网络；**尚无 Script 专用位 enforced**。

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

1. **AYReflect 公共 ABI**（`interface/ayreflect/IReflect.h`）：
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
2. **ITypeInfo vtable 加 4 个虚函数破坏 ABI**：inline 化的 `TypeInfoImpl<T>` 模板在每个 TU 独立生成 vtable 实体；vtable layout 一变，所有 inline 化的 .obj 必须 rebuild。ninja 头依赖追踪对 inline 模板实例化不完整 → 部分 TU 漏 rebuild → 运行时分发到错误 vtable 槽位 → 段错误。**修复**：全 `D:/Projects` 重新 build（`find ... -name "*.cpp" -exec touch` 强制 ninja 重 build）。CI 集成：建议 `AYReflect.h` ABI 变化时 `cmake --build --clean-first` 至少触发 AYReflect + 所有 include `ayreflect/IReflect.h` 的 consumer。
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

#### 5.7.4 R4.1 — std::vector / std::array 方法参数与返回值

**状态**：✅ **R4.1 完成（2026-07-13，commit 待 push）**。R3.0 + R4.0 之后，method 边界只剩 homogeneous 容器 (`std::vector<T>` / `std::array<T, N>`) 没有覆盖。R4.1 把容器作为 method 参数与返回值打通,Element 类型限制为 primitive (int / float) — 结构体 / std::string 元素推到 R4.1b。

**实现摘要**：

1. **AYReflect 新增 `ArrayTypeInfo<T, N>`**(镜像 `VectorTypeInfo<T>`)：`AYFoundation/AYReflect/AYReflect.h` 新增模板,实现 `IContainerTypeInfo` 接口的所有方法;`getContainerSize()` 返回 compile-time N;`resize()` / `pushBack()` 是 no-op stubs(因为 std::array 不能 grow)。配套新增 `registerArrayType<T, N>(name)` helper。
2. **`IContainerTypeInfo` 加 `isFixedSize()` virtual** (R4.1):默认 `false`(对 `VectorTypeInfo` 保持源码兼容),`ArrayTypeInfo` override 为 `true`。这是 bridge 区分 std::vector vs std::array 的唯一信号 — 避免 name-string sniffing。**vtable ABI 变化**,按 memory note 3 触发整仓 rebuild。
3. **`MethodInfoImpl::readArg` 加容器分支**:`detail::is_std_vector_v` / `detail::is_std_array_v` SFINAE traits 检测 `std::vector<T>` / `std::array<T, N>` 参数类型;slot 持有 heap-allocated 容器指针,readArg copy-construct 到 PMF 参数(const T& 自然 bind 到 returned 局部)。`MethodInfoImplConst` 同步扩展。
4. **bridge arg-marshal 新增 `IContainerTypeInfo` 分支** (`ayt_reflect_call_method_c`):dynamic_cast 后,`lua_len()` 取元素数 N,`lua_rawgeti` 逐元素读 int / float。`isFixedSize()` 决定 `resize + push_back` (vector) vs `clamp + memset + memcpy` (array) 两条路径。`getContainerSize(nullptr)` 在 ArrayTypeInfo 上返回 compile-time N,作为 cap。
5. **bridge return-marshal 新增 `IContainerTypeInfo` 分支**:invoke 完检查 retType,`getContainerSize(retPtr)` 取 N,`getElementAt(retPtr, i)` 拿每个元素指针,逐元素 push 到新 Lua table。1-indexed。
6. **Logia TableExpr 解析加 positional 形式**:`include/logia/AYParser.h` + `src/logia/AYParser.cpp` 的 `parsePrimary` 在 `{` 之后 peek `isStartOfKeyedEntry()` (Identifier+`=` 紧邻)— 是则走原有 keyed 路径,否则走新 positional 路径 `do { entry.key=nullptr; entry.value=parseExpression(); } while(comma)`。`TableExpr::Entry::key` 字段早就是 nullable(预留 for R3.5+),本切片落实。Codegen 的 `e.key==nullptr` 分支已经存在,直接 emit `value`(`{1, 2, 3}` → 1-indexed Lua table,正好对得上 vector/array 的 `lua_rawgeti` 1-indexed 索引)。

**测试覆盖（6 cases,`Test_LogiaReflectRuntime.cpp` R4_1Player fixture + LG-12 R4.1 套件）**：

| # | 名称 | 验证点 |
|---|---|---|
| 1 | `lg12_r41_vector_int_arg_sum` | `self.sumVec({1,2,3,4,5})` 返回 15 |
| 2 | `lg12_r41_vector_int_return_to_lua_table` | `self.getVec()` 返回 Lua table,`#v == 5`,`v[3] == 3` |
| 3 | `lg12_r41_vector_int_arg_shorter_than_expected` | `self.first3({1,2})` → C++ 收到 2-element vector,sum = 3 |
| 4 | `lg12_r41_array_float4_arg_sums_to_hp` | `self.applyFloatArr({1.5,2.5,3.5,4.5})` → `hp = 12` |
| 5 | `lg12_r41_array_int3_return` | `self.getArr3()` → Lua table,`#a == 3`,`a[2] == 20` |
| 6 | `lg12_r41_empty_vector_returns_minus_one` | `self.first({})` → C++ 收到空 vector,返回 -1 |

**结果**:`AYScript_Test` **752/752 全绿**(baseline 742 + R4.1 6 cases + 1 sub-case + 已有 3 个 dump-case 集成用例)。

**kLogiaPipelineVersion 9 → 10**:bridge runtime marshal 路径新增(不影响 codegen emit shape,但 compile cache 强制失效)。

**范围**：

| ✅ R4.1 ships | ❌ Deferred |
|---|---|
| `std::vector<T>` arg + return (T = int / float) | `std::vector<T>` T = struct / std::string (R4.1b) |
| `std::array<T, N>` arg + return (T = int / float) | `std::vector<bool>` (bit-packed, R4.1c) |
| Positional TableExpr `{v1, v2, v3}` (parser + codegen) | `std::vector<std::vector<T>>` (R4.1d) |
| `IContainerTypeInfo::isFixedSize()` virtual | `T&` / `T*` out-param (R4.2) |

#### 5.7.4 R4.1b — container element types: struct / std::string

**状态**：✅ **R4.1b 完成（2026-07-13,commit 待 push）**。R4.1 留下 3 个未完成 case —— `std::vector<MyStruct>` / `std::vector<std::string>` / `std::array<MyStruct,N>` 都没在 method 边界打通。R4.1b 把这 3 条 closure,`std::array<std::string,N>` 推 R4.1c(placement-new N strings + per-element dtor walk,非平凡)。

**实现摘要**：

1. **bridge 容器 arg-marshal 加四-way 分支** (`ayt_reflect_call_method_c`)：原本两-way (int/float) 拆成 int / float / struct / std::string 四-way。`isFixedSize` 决定 array vs vector 路径,element 类型决定 fill 路径:
   - **array<primitive>**:raw-byte buffer + memcpy(原 R4.1 路径,保留)
   - **array<MyStruct,N>** (R4.1b 新):`::operator new(cap * sizeof(MyStruct))` + memset 0 + 每元素 `storeFieldPrimitive` 递归填字段。Cleanup:`::operator delete` (struct trivial dtor 时无操作,memset 0 init 状态有效)
   - **vector<primitive>**:heap-alloc `std::vector<int/float>` + `push_back`(原 R4.1 路径)
   - **vector<std::string>** (R4.1b 新):`new std::vector<std::string>` + `emplace_back(lua_tostring)`。`~vector<std::string>` 销毁每个 string
   - **vector<MyStruct>** (R4.1b 新):`new std::vector<uint8_t>` resized to `luaLen * sizeof(MyStruct)`,memset 0 + 每元素 `storeFieldPrimitive` 填字段。slot pointer reinterpret as `vector<MyStruct>*` 在 `MethodInfoImpl::readArg`(`ArgT(*vp)` 路径)。Cleanup:`delete vector<uint8_t>` — **仅限于 trivially-destructible + trivially-copyable T**(同 R3 struct-arg 限制)
   - **array<std::string,N>**:warn + nullptr(推 R4.1c)

2. **bridge 容器 return-marshal 同样扩四-way**：读路径只看 `getElementAt(k)`,不需要 fill,所以 type-safe:
   - **primitive**:memcpy + lua_pushinteger/number
   - **std::string**:`lua_pushlstring(sp->data(), sp->size())`
   - **struct**:`lua_newtable` + 每个 field 走 `pushFieldPrimitive` 递归(R4.0 已支持 nested struct field,这里 free reuse) + sub-table 由外层 `lua_rawseti` 设到 outer table
   - 1-indexed Lua table(同 R4.1 — 与 Logia positional TableExpr emit shape 对齐)

3. **`MethodInfoImpl` / `MethodInfoImplConst` 不动**:`is_std_vector_v` / `is_std_array_v` traits (R4.2 已 strip ref/const) 和 `readArg` slot convention `*vp` copy-construct 任意 T — 对 MyStruct / std::string 元素 T 自动 work,无需 MethodInfoImpl 改一行。

4. **kLogiaPipelineVersion 11 → 12**:bridge runtime marshal 路径新增(不影响 codegen emit shape,但 compile cache 强制失效)。

5. **vector<MyStruct> 实现细节 — 为什么用 `vector<uint8_t>` reinterpret**:bridge 编译时只见 `ITypeInfo*`,不知道 T。要 typed `new std::vector<MyStruct>()` 需 typed trampoline(走 `IMethodInfo` virtual 扩展,ABI 变化)。`vector<uint8_t>` reinterpret 路径虽然技术上是 UB(对 non-trivial dtor),但对 trivially-destructible T 现实安全,与 R3 struct-arg 限制一致。Future Path A(typed trampoline)可消除此限制,需另开 slice。

**测试覆盖（6 cases,`Test_LogiaReflectRuntime.cpp` R4_1bPlayer fixture + LG-12 R4.1b 套件）**：

| # | 名称 | 验证点 |
|---|---|---|
| 1 | `lg12_r41b_vector_struct_arg` | `self.sumPoints({{x=1,y=2},{x=3,y=4}})` → 2*1000+(1+2+3+4) = 2010 |
| 2 | `lg12_r41b_vector_struct_return` | `self.getPoints()` 写 lastFirstX/Y/lastSecondX/Y 到 self,* Logia 读 → first_x(10) + second_y(40)*100 = 4010 |
| 3 | `lg12_r41b_vector_string_arg` | `self.joinStrings({"hello","world"})` → 2*100+(5+5) = 210 |
| 4 | `lg12_r41b_vector_string_return` | `self.getStrings()` 写 first/second string lengths 到 self.lastFirstX/lastSecondX,Logia 读 → 3 + 3*10 = 33 |
| 5 | `lg12_r41b_array_struct_arg` | `self.sumArray({{x=1,y=2},{x=3,y=4}})` → 1+2+3+4 = 10 + self.lastPoint == (4,6) |
| 6 | `lg12_r41b_vector_struct_single_element` | `self.onePoint({{x=42,y=0}})` → lastSum=42, lastLen=1, witness = 42+100 = 142 |

**结果**：`AYScript_Test` **824/824 全绿**(baseline 778 + R4.1b 6 tests + 已有 40 个 dump/reflect/adapter/tool 集成用例)。

**范围**：

| ✅ R4.1b ships | ❌ Deferred |
|---|---|
| `std::vector<MyStruct>` arg + return(trivially-destructible T) | `std::array<std::string, N>` arg/return (R4.1c,placement-new) |
| `std::vector<std::string>` arg + return | `std::vector<non-trivial struct>`(typed trampoline ABI 变化,推 Path A slice) |
| `std::array<MyStruct, N>` arg (N=2 in fixture;一般 N 工作) | `std::vector<bool>` (R4.1c,bit-packed) |
| 容器 return sub-table 走 pushFieldPrimitive 递归(R4.0 reuse) | `std::vector<std::vector<T>>` nested container (R4.1d) |

**Lessons learned (R4.1b specific)**：

26. **vector<struct> 用 `vector<uint8_t>` byte buffer 是 pragmatic UB 妥协**:bridge 见 `ITypeInfo*` 无法 typed `new std::vector<MyStruct>()`,Path A 需 `IMethodInfo` vtable 扩 typed trampoline。R4.1b 选 Path B(`vector<uint8_t>` resize + reinterpret at readArg)以避免 ABI 变化,document 限制(trivially-destructible T only)—— 与 R3 struct-arg 已存在的限制对齐。Lesson:**ITypeInfo-driven bridge 在 typed instantiation 是隐藏 bottleneck,每个非-trivial element kind 都要决定走 ABI 变化 vs runtime UB**。
27. **容器 return 走 side-effect pattern + self.* readback**:与 R4.2 out-param 同样的 Lua 限制(`var pts = self.getPoints(); pts[1].x`)是 codegen 跨 method-call-chain 的限制,不是 bridge 限制。Fixture 用 `self.lastFirstX/Y` 等显式 side-effect field,Logia 读 self.*。这与 R3+ 的 `__test_witness = tostring(self.foo)` 模式一致。
28. **registerVectorType<std::string>() 需先注册 std::string**:bridge 的 `ensureBuiltinTypesRegistered` 只注册 int/float/bool/double/Int64,**不注册 std::string**。`registerVectorType<T>` 内部 `findType<T>()`,对 `T = std::string` 必须先 `registerType("std::string", ...)`。R3 fixture 的 DamageInfo 路径有 std::string field 同款限制,R4.1b 走独立的 std::string-as-element 路径需要在 fixture 里多加一行注册。Lesson:**测试 fixture 检查 `findType<T>()` 不是 native bool/int/float 时显式注册**,不要假设 bridge 帮你做了。
29. **struct as byte buffer 的 memset 0 + leave-alone-on-destroy 模式**:`std::array<MyStruct, N>` 的 `::operator new(cap * typeSize) + memset 0 + per-element storeFieldPrimitive + ::operator delete` 不需要 explicit `~MyStruct()` 调用,因为 trivial dtor 不做操作。这与 R3 struct-arg 的 `memset(mem, 0, typeSize)` 模式一致。

#### 5.7.4 R4.1c — `std::array<std::string, N>` arg placement-new

**状态**:✅ **R4.1c 完成(2026-07-13,commit 待 push)**。R4.1b 留了一个 deferred branch:`isFixedSize && elemIsString` 在 container arg-marshal 里只 warn + nullptr,PMF 收到 default-constructed `std::array<std::string,N>`(可能 UB)。R4.1c 把这个最后一块 closure。

**实施摘要**:

1. **bridge arg-marshal 的 `isFixedSize && elemIsString` 分支 placement-new path**:`::operator new(cap * sizeof(std::string))` alloc 字节块,显式 placement-new `std::string()` 在所有 cap slots(未填 slots 留空 string,匹配 array<int,N> 的 zero-fill convention)。Lua `luaLen` slots 用 `lua_rawgeti(L, stackIdx, k+1)` + `lua_tostring` + copy-assign(`*(strPtr+k) = std::string(s ? s : "")`)。Cleanup lambda:**显式 `(static_cast<std::string*>(p)+k)->~basic_string()` walk N 次,然后 `::operator delete(p)`** — 不能像 `vector<std::string>` 那样 `delete vector*` 让 `~vector` 顺带 destroy(那是 vector 的内置机制,raw 字节块没有)。
2. **关键支撑改动:`HeapSlot::deleter` 从 `void(*)(void*)` 升级 `std::function<void(void*)>`**:早期用 raw C fn ptr 是为了避开 `std::function` heap alloc + 把 destructor 类型擦除绑到 heap pointer 实际类型(见 R4.1b 注释 lines 686-694)。但 raw fn ptr **不接受 lambda capture** — R4.1c 的 placement-new cleanup 需要 capture `cap` 走 N 次 `~basic_string()` walk,被迫升 `std::function`。所有现有 stateless lambda / raw fn ptr 调用方隐式转换兼容,**call sites 一行不动**。Caller 的 `registerCleanup(mem, [cap](void* p){...})` 编译通过。
3. **return-side `std::array<std::string, N>` 不需要改**:R4.1b 的 `elemIsString → getElementAt → lua_pushlstring` 已经覆盖(`ArrayTypeInfo::getElementAt` 给的也是 `std::string*`,与 vector 路径同款 — `IContainerTypeInfo` 的 vtable ABI 没变,只是用 typed instance 指针)。

**测试覆盖(1 case,`Test_LogiaReflectRuntime.cpp` R4_1bPlayer 复用 + LG-13 R4.1c 套件)**:

| # | 名称 | 验证点 |
|---|---|---|
| 1 | `lg13_r41c_array_string_arg` | `self.joinArray({"hello","world"})` → 2*100+(5+5) = 210 + self.lastStrA='h' + self.lastStrB='w' |

**结果**:`AYScript_Test` **830/830 全绿**(baseline 824 + R4.1c 1 case + 已有 5 个 reflect/dump 集成用例 — 总数比 R4.1b baseline 多 6 是因为 R4.1b 后又累加了其他 reflect/adapter fixture cases)。

**kLogiaPipelineVersion 12 → 13**:bridge runtime arg-marshal path 再次变化,缓存强制失效。

**范围**:

| ✅ R4.1c ships | ❌ Deferred |
|---|---|
| `std::array<std::string, N>` arg(placement-new N strings + per-element dtor walk) | `std::vector<bool>` (R4.1d,bit-packed) |
| `std::array<std::string, N>` return(R4.1b 已经覆盖) | `std::vector<non-trivial struct>`(typed trampoline Path A) |
| `HeapSlot::deleter` 升 `std::function` 支持 per-call capture | nested container elements(`vector<vector<...>>`) |

**Lessons learned (R4.1c specific)**:

30. **`HeapSlot::deleter` 从 raw C fn ptr 升 `std::function` 是 per-call capture 的必要解**:raw fn ptr 早期是 premature optimization(避开 `std::function` heap alloc + 锁定 destructor 类型擦除) — 但 R4.1c placement-new N strings 的 cleanup lambda 需要 capture `cap` 才能走 N 次 `~basic_string()` walk,被迫升 `std::function`。Lesson:**lifetime-extending 容器**(cleanup / hook / observer 队列)**早期 raw fn ptr 是 over-optimization** — `std::function` 的开销(heap alloc + virtual dispatch)在 cleanup 这种每-call 几次的场景下完全可忽略。换成 capture-friendly 后所有 stateless lambda + raw fn ptr 自动转换兼容,后续 R-n 任意 capture 都不用再设计 cleanup 容器。

**Lessons learned (R4.1 specific)**:

**Lessons learned (R4.1 specific)**：

17. **MethodInfoImpl 的 if-constexpr ladder 是 bridge 协议扩展点**:R3.0 加了 enum / std::string / lvalue-ref / pointer 分支,R4.1 加 std::vector / std::array。每个新分支只需要在 readArg 加一个 if constexpr,bridge arg-side 加一个 dynamic_cast 分支,无需修改 PMF 本身。MethodInfoImpl 作为"slot 派发器"的设计在 R4.1 完整闭环。
18. **Positional TableExpr 是 R4.1 的隐藏前提**:`{1, 2, 3}` 之前是 parser hard-reject (case 04 / emit_dump 04)。R4.1 必须扩 parser,但**改动面小**:`TableExpr::Entry::key` 早是 nullable(预留 for R3.5+),codegen 早处理 `key==nullptr` — 只需要 parsePrimary 加 peek + 选择两条路径。Lesson:AST 节点预留 nullable 字段降低了后续切片成本。
19. **ITypeInfo vtable 加虚函数触发整仓 rebuild**:`isFixedSize()` 是一行 override,但 AYReflect ABI 变化按 memory note 3 必须 `--clean-first` 或 `find -name '*.cpp' -exec touch`。`build_ayscript.bat` 第一次 build 后链接 AYReflect.lib,后续 cpp TU 通过 dep 链 rebuild — 验证通过。
20. **`__test_witness = tostring(s)` 走 `getLuaGlobalString` 不是 `tryGetLuaGlobalNumber`**:Lua `tostring(15)` 是 string,`tryGetLuaGlobalNumber` 只对 number return true。R3 case 12 用 `getLuaGlobalString` 验证 witness,R4.1 case 1/3/5/6 同样用 string 比较(避开 tostring-vs-number 类型误读)。case 4 (applyFloatArr → hp 写入) 用 `getLuaGlobalString("12")` 验证 C++ 写入 — 这里 C++ field 是 int,Logia 读 `self.hp` 是 number,但 `tostring(self.hp)` 走 string 通道才安全。

#### 5.7.4 R4.2 — T& / T* out-param (write-back)

**状态**：✅ **R4.2 完成（2026-07-13，commit 待 push）**。R3.0 加了 `const T&` / `const T*` 作为 input-only args;R4.2 把非 const 的 `T&` / `T*` 打通为 out-param(写回)语义 — bridge 分配 fresh T(seeded from Lua),PMF 写入,bridge 读回 Lua。这是 C++ 经典 out-param idiom(`int fillInt(int& out)`)的 Logia 入口。

**实现摘要**：

1. **AYReflect 加 `IMethodInfo::getParamIsOut(size_t)` virtual**:`AYFoundation/AYReflect/interface/ayreflect/IReflect.h` 新增 `virtual bool getParamIsOut(size_t) const { return false; }` 默认 false(对 input-only IMethodImpls 源码兼容)。**vtable ABI 变化** — 触发整仓 rebuild(同 R4.1 的 `isFixedSize()`)。`MethodInfoImpl` 与 `MethodInfoImplConst` 各自实现 `paramIsOutAtImpl` + `paramIsOutForIndex<I>`,后者用 `static_assert` 风格:`is_lvalue_reference_v<ArgT> && !is_const_v<remove_reference_t<ArgT>>` 判 `T&` out-param,`is_pointer_v<ArgT> && !is_const_v<remove_pointer_t<ArgT>>` 判 `T*` out-param,其它一律 false。
2. **`is_out_param_v` SFINAE trait**:`include/logia/AYMethodInfoImpl.h` `detail` 命名空间新增 `is_out_param<ArgT, EnableT>`,用 `std::enable_if_t` 区分 const ref/ptr(输入)与非 const ref/ptr(输出)。这是关键 — `const T&` 与 `T&` 在 C++ 是不同的 type,需要 SFINAE 区分。
3. **`readArg` 拆成 `read<I>` + `readArg<I>` + `readOutArg<I>`**:`read<I>` 是 `decltype(auto)` 包装,用 `if constexpr (is_out_param_v<ArgT>)` 选择两条路径:
   - **`readOutArg<I>`(R4.2 新增)**:返回 `T&` 或 `T*` 指向 bridge 的 heap slot;PMF 写入,bridge 在 post-invoke 写回。
   - **`readArg<I>`(R3.0 + R4.1 既有)**:返回 by value;PMF 取 const ref 绑到 returned local(对 input-only 类型,行为不变)。
4. **`is_std_vector_v` / `is_std_array_v` trait 必须 strip ref/const**:`ArgT` 可能是 `const std::vector<int>&` (R4.1 input) 或 `std::vector<int>&` (R4.2 out-param)。原 R4.1 trait 只匹配 `std::vector<T,A>`,对 `const std::vector<int>&` 不 match → 落到 lvalue-ref 分支 → 走 `*static_cast<const ArgT*>(slot)` 把 `ArgT = const std::vector<int>&` 当 reference type 处理 → C3536 `vp` 未初始化 + C2100 deref 失败。**R4.2 fix**:`is_std_vector<X> = is_std_vector<remove_cv_t<remove_reference_t<X>>>::value`,把 ref/const 剥掉再 match。这是 R4.2 唯一改动的 R4.1 trait 代码,1 行核心 fix。
5. **bridge arg-marshal 加 out-param 分支**:`src/AYScriptRuntimeBridge.cpp` 在 fallback `else` 内 `if (method->getParamIsOut(i) && paramType)` 检测。int/float/struct 三种类型支持:heap-alloc T,seed from Lua 初始值,register cleanup,`argPtrs[i] = heapPtr`(struct) 或 `vec->push_back` 形式(int/float)。std::string out-param 推 R4.2b。
6. **bridge post-invoke write-back loop**:invoke 完扫描所有 out-param,对 struct 类型用 `pushFieldPrimitive` 递归构造新 Lua table 覆盖原 arg slot。**但 Lua local variable 不会 auto-rebind**(Lua 限制) — Logia 用户读 out-param 走 C++ 写到 self.foo + Logia 读 self.foo 的 side-effect pattern(标准 Lua idiom)。fixture 统一走 self.lastResult / self.lastFloat / self.lastPoint。

**测试覆盖（5 cases,`Test_LogiaReflectRuntime.cpp` R4_2Player fixture + LG-12 R4.2 套件）**：

| # | 名称 | 验证点 |
|---|---|---|
| 1 | `lg12_r42_int_out_param` | `self.fillInt(x)` → C++ 写 42,Logia 读 self.lastResult = 42 |
| 2 | `lg12_r42_int_in_out_param` | `var seed = 10; self.addFive(seed)` → C++ 加 5,lastResult = 15 |
| 3 | `lg12_r42_float_out_param` | `self.fillFloat(f)` → C++ 写 2.0,lastFloat = 2.0 |
| 4 | `lg12_r42_int_ptr_out_param` | `self.fillViaPtr(p)` → C++ 通过 `*out = 99` 写,lastResult = 99 |
| 5 | `lg12_r42_struct_out_param` | `self.mutatePoint({x=1, y=2})` → C++ 改 lastPoint = (11, 22),witness = 1122 |

**结果**：`AYScript_Test` **778/778 全绿**(R4.1 752 + R4.2 5 cases + 已有 21 个 dump/reflect 集成用例)。

**kLogiaPipelineVersion 10 → 11**:bridge runtime marshal 路径新增 + IMethodInfo vtable 变化,缓存强制失效。

**范围**：

| ✅ R4.2 ships | ❌ Deferred |
|---|---|
| `T&` out-param (int / float / struct) | `std::string` out-param (R4.2b,placement-new) |
| `T*` out-param (非 null) | nested-struct out-param(R4.1 已经有,但 R4.2 fixture 走 single-level) |
| Lua side 通过 self.* 读回 out-param(标准 Lua idiom) | "Rebind local var"语义(需要 codegen-level patch,推 S4) |

**Lessons learned (R4.2 specific)**：

21. **`is_std_vector_v` / `is_std_array_v` trait 必须 strip ref/const**:R4.1 写时只考虑 `std::vector<T,A>` exact match,R4.2 让 `const std::vector<int>&` 也走 vector 分支后,发现 trait 不 match(因为 partial spec 看不到 ref) → 落到 lvalue-ref 分支 → C3536 / C2100 编译错误。**Lesson:为 R-n+1 写 trait 时,先想 R-n 引入的 ref 修饰,提前 strip 一次**。这是 R4.1 trait 的"小裂缝"被 R4.2 放大。
22. **MSVC `decltype(auto)` + `if constexpr` + 不同 ref/value 路径会触发 C3487**:单一 `if constexpr` 内的两个 return 语句推导类型不同(即使一边的分支被 discarded,MSVC 仍做 type consistency check)。**Fix**:把不同 ref/value 路径拆到独立函数,`read<I>` 用 `if constexpr` 选择调用,每个函数自身 deduced return type 一致。`readOutArg` 进一步拆成 `T&` overload + `T*` overload(用 `enable_if_t` 区分)以避开 C3487 持续触发。
23. **`std::tuple<Args...>{read<I>(args)...}` vs `std::apply` 不必要**:R4.2 一开始用 tuple + apply 试图"lvalue ref 在 lambda 转发中保留",实测 direct pack expansion `(self->*_pmf)(read<I>(args)...)` 就够了 — readOutArg 返回 `T&` / `T*` 已经是 lvalue,pack expansion 保留 ref 类别。tuple + apply 是过度设计,多了一层 + 调试更难。Lesson:**先试最直白的写法**(pack expansion),不行再加 std::apply。
24. **Lua 浮点数 round-trip 不精确**:`3.14f` stored as double = `3.1400001049041748`,`tostring` 出 long form。R4.2 fixture 改用 `2.0f` 让 tostring 出 "2.0" 这种短形式。R4.1 float test (`1.5+2.5+3.5+4.5 = 12` then `static_cast<int>(s)`) 巧妙地避开了这个问题。
25. **Lua local variable 不能从 C 端 rebind**:out-param 写回后,Logia 用户的 `var x` 仍指向旧 value(Lua 限制,跟 const-correctness 类似)。Fixture 用 self.* side-effect pattern:PMF 写 `*x = 42; lastResult = 42;`,Logia 读 `self.lastResult`。这与 C++ 的 `__out_int` 风格宏 / 第三方 out-param 包装的常见做法一致,用户适应后是干净的。

#### 5.7.4 R-Audit — 语法审计修复（2026-07-11）

**状态**：✅ 完成（commit 待 push）。三个并发 defect 经 plan-mode agent 复核根因后一并修复。

**实施摘要**：

1. **新增 `function` keyword + script-block helper**：
   - `AYToken.h` 新增 `TokenType::Function`；`AYLexer.cpp` 把 `"function"` 加入 keywords 表（`local` 仍由设计保留为 Identifier）。
   - `AYAst.h` 新增 `FunctionDeclStmt`（`name` + `params` + `body`）。
   - `AYParser.cpp` 新增 `parseFunctionDeclStmt()` 并在 `parseMember()` 内部 dispatch（**只**在 script-block 上下文里识别），同时 `parseStatement` 看到 `function` 就 hard-error（"function declarations only allowed as script members"）。
   - `AYLuaCodegen.cpp` 新增 `emitFunctionDecl()` —— 顶层 `function NAME(...) ... end`，无 `M.` 前缀，让 lifecycle body 通过裸名 `doubleIt(21)` 调用；同事在 `emitStmt` 加防御（防止畸形 AST 落到这里也继续走对路径）。

2. **`local` keyword leak → soft warning**：
   - `AYCompilerError.h` 新增 `ErrorCode::LuaKeywordLeak`。
   - `AYSemanticAnalyzer.cpp::analyzeStmt()` 检查 `ExprStmt(IdentifierExpr("local|nil|function|..."))` 形态，emit 一条 `DiagnosticSeverity::Warning` 级别的诊断，message + hint 指引改用 `var` 或 function 块级 helper；**不** fail compile。
   - 设计原因：保留 R3.0 + R3 + R4 现有 15+ 个测试用例的兼容性（这些用例全部靠 `local s = self.method()` 形式），soft warning 把"违规还在跑"的状态透明给日志读者。

3. **`on_update(dt)` test gap 修复**：
   - `Test_LogiaAdapter.cpp::adapter_maps_onUpdate_to_on_update_passes_dt` 由原来只 witness `"on_update_called"` 升级成 numeric-equality 检查 `__test_dt` 全局（用 `bridge.tryGetLuaGlobalNumber` 读 2.5f 值）。
   - 同步清理 `parseLifecycleFunc` line 109-112 的 stale comment（旧注释说 "codegen ignores the params" —— 实际现在 `emitLifecycleFunc` 已经把 `func.params` 在 line 151-154 透传到 Lua signature，跟着 bridge `callLifecycle` 在 line 1334 推 `dt`）。

**kLogiaPipelineVersion 5 → 6**：(因 codegen emit 形状在 `function NAME(...)` 路径下加新形态，且 diagnostic surface 新增 `LuaKeywordLeak` 警示，缓存强制失效)。

**测试覆盖**（`Test_LogiaEmitDump.cpp` 13 cases —— 升级为 acceptance suite，file header comment 已重写说明用途）：

| # | Logia source | Expect success | 验证点 |
|---|---|---|---|
| 01 | `local s = self.method()` | true | emit broken shape（documented） + LuaKeywordLeak 警告 |
| 02 | multiple `local`s | true | 同上多行 |
| 03 | `var tick: int = 0` | true | emit 干净的 `local tick = 0` |
| 04 | `var tick = 0` (no type) | **false** | parser hard-reject（defer type inference）|
| 05 | `if x == nil { }` | true | 干净的 `if (x == nil) then ... end` |
| 06 | `if then/end Lua form` | **false** | brace-only 设计 |
| 07 | `self.f1.f2.f3` 3-hop | true | bare Lua emit（runtime no-op，defer 3-hop chain） |
| 08 | `..` concat | **false** | 无 DotDot token |
| 09 | `function foo() { ... }` script-block | **true** (audit 升级) | emit 顶层 `function foo()` |
| 10 | `local function m() { }` | **false** | `local` 没 keyword |
| 11 | `function doubleIt + on_start call` | true | emit 顶层 helper + on_start 用对名字 |
| 12 | `function` inside `on_update` | false | parser 明确 error |
| 13 | `local s = ...` warning | true | LuaKeywordLeak diagnostic surfaced |

**结果**：`AYScript_Test` **692/692 全绿**（baseline 671 + R3 已 ship 9 增量 + audit 21 cases = 692）。AYReflect 完全不动。

**Lessons learned (R-Audit specific)**：

14. **审计时务必 trace emit 不只 trace source**：plan-mode agent 把"emit 哪一行 broken"和"哪个 parser 步骤错"切开；Class A `local\nname = expr` 是 **parser** 把 `local` 当 identifier 产生两个 statement，不是 codegen 的 indent 错。R3 测试全部因为"sol2 safe_script 偶然 parse pass"继续 work —— 测试绿不代表真对。
15. **diagnostic 与 parse success 是解耦的**：soft warning 让 `local s = ...` 仍然 compile-success=true，但 diagnostics 里冒一条 LuaKeywordLeak。这样 (a) 不破坏 15 个 R3 测试的 happy path，(b) CLI 用户 `ays-logia compile foo.logia` 能看到一行提示。
16. **`function` 块的语义边界必须在 parser enforce**：codegen 总是可以 emit 出格式合法的 Lua（`function foo() ... end` 是合法顶层）；所以 guard 一定要在 parser 拒绝"script-block scope 之外"，否则会产生看起来 work 但语义错位的 helper（被嵌套在 on_update 里会被 hoist 成全局 module 范围 fn，绕过设计意图）。用 `parseMember` 而**非** `parseStatement` 作为 function 的入口是实现要点。

#### 5.7.4.x R5.0 — 循环语句（while / for）（2026-07-13）

**状态**：✅ 完成。`while (cond) { body }` 与 `for (var i : N) { body }` 在 R5.0 落地为 Logia 一级语法；Lua 5.5 原生支持对应 `while ... do ... end` 和 `for i = 1, N do ... end`，codegen 直 emit，零运行时 helper。

**为什么 R5.0 现在才做**：R3 / R3.5 / R4 的反射链路（self.field chain / IMethodInfo / struct return）之前是优先级，循环控制这种"控制流" 在用户脚本里大量出现但又有纯 Lua 兜底（`for k,v in pairs(t) do ... end`）。R5.0 把循环提到第一类语法是为了：(a) 让 R4.1 vector/array 参数的样例代码（数组求和、容器遍历）能写得出来；(b) 给后续 R5.0.1 `break` / `continue` / C-style `for` / range `for` 打基础。

**语法与实现**：

- **Lexer**：`include/logia/AYToken.h` 新增 `TokenType::While` / `TokenType::For`；`src/logia/AYLexer.cpp` 把 `"while"` / `"for"` 加入 keywords 表，从此作为保留字（`ExprStmt(IdentifierExpr("while"))` 路径关闭）。
- **AST**：`include/logia/AYAst.h` 新增 `WhileStmt(condition, body)` 和 `ForStmt(counterName, bound, body)`。`bound` 是 `ExprPtr`，可以是任意 Logia 表达式（literal / identifier / function call / 算术）。
- **Parser**：`src/logia/AYParser.cpp` 新增 `parseWhileStmt()` / `parseForStmt()`，dispatch 加在 `parseStatement()` 的 `If` 之后、`Var` 之前。
  - `parseWhileStmt()` 显式 consume `(` + `parseExpression()` + `)` + `{` + `parseBlockBody()` + `}`，仿 `parseIfStmt` 形态。
  - `parseForStmt()` 显式 consume `(` + `var` + `Identifier` + `:` + `parseExpression()` + `)` + `{` + `parseBlockBody()` + `}`。**关键不变量**：这一路径**不**复用 `parseVarDecl`，否则 `for (var i : 10)` 里 `10` 会被 `parseVarDecl` 的 `consumeIdentifier("Expected variable type")` 当 type name 报 unknown-type。`for` header 是专用 counter-var pattern，仅在 `for(...)` 上下文里 parse。
- **Codegen**：`src/logia/AYLuaCodegen.cpp` 新增 `emitWhileStmt()` / `emitForStmt()`，在 `emitStmt()` dispatch 的 `IfStmt` 后插入。
  - `emitWhileStmt` emit `while <cond> do\n...end\n`，`cond` 经 `emitExpr`（返回 string）拼装，BinaryExpr 自动带括号。
  - `emitForStmt` emit `for <counter> = 1, <bound> do\n...end\n`。**关键不变量**：emit 出来的 Lua `for` 不带 `var` 也不带 `:` —— Logia 层的 `var i : N` 解析后只剩 `counterName` 和 `bound` 两个语义字段，Lua 的 `for` 本身隐式声明 counter。start hardcoded = 1（Lua numeric-for 默认 step=+1，无需 emit step 参数）。
- **Analyzer**：`src/logia/AYSemanticAnalyzer.cpp` 新增 `analyzeWhileStmt()` / `analyzeForStmt()`，在 `analyzeStmt()` dispatch 的 `IfStmt` 分支后插入。
  - **counter 不进 `_scope`**：Lua `for i = 1, N do ... end` 已是 loop-local；如果把 `i` 也塞进 analyzer 的 `_scope`，循环外的 `i = 5` 在 analyzer 视角下"看似声明过"但 runtime 是 implicit global —— analyzer/runtime 视图会失配。直接 walk 子节点、不动 scope，是 analyzer/runtime 视图一致的最稳路径。
  - 不做 bound type-check（S2.5 analyzer 不对一般表达式 stamp 类型，只有 `self.<field>` 走 LG-05 路径；expression-type inference 超 R5.0 范围）。
- **`synchronize()` recovery token**：`src/logia/AYParser.cpp::synchronize()` case 列表加 `While` / `For`，loop body 内 parse error 能在下一个 loop 边界恢复。

**kLogiaPipelineVersion 6 → 7**：codegen emit 形状新增 `while ... do ... end` 和 `for ... = 1, ... do ... end` 两种，缓存强制失效。

**测试覆盖**（`Test_LogiaEmitDump.cpp` 16 cases —— 13 audit + 3 R5.0）：

| # | Logia source | Expect success | 验证点 |
|---|---|---|---|
| 14 | `while count < 5 { ... }` 在 script-block helper 里 | true | emit `while (count < 5) do ... end` |
| 15 | `for var i : 10 { ... }` 在 script-block helper 里 | true | emit `for i = 1, 10 do ... end`（**不**带 `var` / `:`）|
| 16 | 嵌套 `for var i : 3 { for var j : 3 { ... } }` | true | emit 两个 `for ... do ... end` 嵌套；inner `j` shadow outer `i` 由 Lua 保证 |

**结果**：`AYScript_Test` **705/705 全绿**（baseline 692 + R5.0 3 cases + 已有的 10 个漏计入的 dump-case 集成用例）。

**Defer to R5.0.1 / R4.5+**：

| 不做 | 原因 | 计划 |
|---|---|---|
| `break` / `continue` | R5.0 锁 brace-only 控制流最简子集；break-label 跳转会膨胀设计面 | R5.0.1 |
| C 风格 `for (i = 1; i <= N; i++)` | Logia 没 `++` 也没 `;`-list inside paren | R5.0.1 |
| Range `for (i in 1..N)` | Logia 没 `..` token（emit_dump 08 明确 hard-reject） | R4.5+ |
| Bound 类型校验 | 需要 expression-type inference（S2.5 不支持） | R5.0.1+ |

**Lessons learned (R5.0 specific)**：

17. **`for (var i : N)` 的 var-vs-type 歧义由 parser 显式处理而非 grammar 升级**：原本可以引入 `for x in 1..N` range syntax 绕开，但 `..` token 还没引入（emit_dump 08 显式 reject）。走 `var i : N` 路径时，`parseForStmt` 必须显式 consume `var Identifier Colon Expr` 并**不**复用 `parseVarDecl`，否则 `10` 会被当 type name。这是 "grammar 复用陷阱" 的典型 —— 看着像 typed-decl，实际是 counter-var 模式。
18. **counter scope 必须由 Lua 提供而非 analyzer 提供**：把 `i` 塞进 `_scope` 看似合理，但会与 Lua 的 `for` 循环-local 语义打架（loop 外的 `i = 5` 会变成 implicit global 而不是 "修改旧 counter"）。统一原则：**Logia 生成的代码里 counter 走 Lua loop-local，analyzer 也不假装拥有这个 counter**。
19. **`while` 的 emit 用 `do/end` 而非 brace**：Lua 的 `while cond do ... end` 关键字与 Logia 的 brace-only 不冲突 —— brace-only 是 Logia 用户视角的语法决策；emit 出来的 Lua 用 Lua 原生 `do/end` 是 codegen 自由度的体现（与 `if` 的 `then/end` 路径相同）。两者不必统一，brace-only 是 source-level 的，do/end 是 target-level 的。
20. **`function` / `while` / `for` 三个 keyword 的 scope 策略可以不同**：`function` 必须 script-block-only（parser 强制），`while` / `for` 在 lifecycle body 和 script-block helper body 都允许，**不在 script-block member 位置**。区别在于 `function` 会引入 module-scope 副作用（被 emit 成顶层 Lua function），而 `while` / `for` 是纯 statement 不会越界。`synchronize()` 同时加三个 keyword 是因为它们都可能是 "loop 边界恢复点"，不论 scope。

#### 5.7.4.x+1 R5.0.1 — 可选括号 + `for` 半开区间（2026-07-13）

**状态**：✅ 完成。R5.0 落地后用户反馈两点：(a) 括号强制让人不习惯（人写 `if x > 0` 比 `if (x > 0)` 多），(b) `for (var i : 10)` 强制 1..N 偏离主流的 0..N-1 习惯。R5.0.1 在 R5.0 基础上加两条扩展（无 breaking change）。

**实施摘要**：

1. **`if` / `while` / `for` 三种 statement 的 condition 括号可选**：
   - 旧：`if (cond) { ... }` / `while (cond) { ... }` / `for (var i : N) { ... }` —— 括号强制
   - 新：`if cond { ... }` / `while cond { ... }` / `for var i : N { ... }` —— 同等合法
   - 混搭非法：`if (cond { ... }`（左括号配右括号，没右括号就 hard error，message 明确）
   - 实现：`parseIfStmt` / `parseWhileStmt` / `parseForStmt` 都用 `match(LeftParen)` peek 后 dispatch：match 成功走 "paren 形式"，失败走 "bare 形式"。两者 emit 出的 Lua 完全一致（BinaryExpr 的 `emitExpr` 自动加括号）。
   - 顺便也支持 `else if cond { ... }`（之前 R5.0 没显式支持 else if 链，R5.0.1 在 `parseIfStmt` 的 `else` 分支里 peek `If` 后递归 `parseIfStmt()`）。

2. **`for` 半开区间形式 `for (var i : start, end) { body }`**：
   - 旧（保留）：`for (var i : N)` → `for i = 1, N do`（1..N inclusive）
   - 新：`for (var i : 0, 10)` → `for i = 0, (10) - 1 do`（0..9 inclusive，即 [0, 10) 半开）
   - 实现：`parseForStmt` 解析 first expression 后 peek `,`：如果是 `,`，consume 它再 parseExpression 作为 endExpr，把 firstExpression 提到 startExpr（`ForStmt` 的 `start` field 非空）；否则走原路径（`ForStmt::start == nullptr`）。
   - **Codegen 双形态**：`emitForStmt` 看 `stmt.start` 是否非空：非空 emit `start, (end) - 1`；空 emit `1, bound`。
   - 括号包裹 `end` 是因为 end 可能是 `n*2` 这种算术表达式，直接拼 `n*2 - 1` 会被 Lua precedence 解析成 `n * (2 - 1) = n` —— 必须用括号显式 group。
   - 边界：`start == end` 时 body 不跑（Lua 看到 `1 > 0` 跳过）；`start > end - 1` 同理。Lua 5.5 numeric-for 的内置行为，不需要 codegen 特殊处理。

3. **AST 扩展**：
   - `ForStmt` 加 `ExprPtr start` field + 新 ctor `ForStmt(counterName, start, end, body)`，旧 ctor `ForStmt(counterName, bound, body)` 把 `start = nullptr` 标短形式。
   - 不影响 analyzer（`analyzeForStmt` 只 walk `bound` + `body`，新加的 `start` 同样 walk 一下）。
   - 不影响 codegen dispatch（还是 `dynamic_cast<ForStmt*>` 一处）。

**kLogiaPipelineVersion 7 → 8**：codegen 新增 `for i = start, (end) - 1 do` 形态，缓存强制失效。

**测试覆盖**（`Test_LogiaEmitDump.cpp` 21 cases —— 16 R5.0 + 5 R5.0.1）：

| # | Logia source | Expect success | 验证点 |
|---|---|---|---|
| 17 | `if x == nil { ... }` (bare) | true | emit `if (x == nil) then ... end`（与 case 05 等价）|
| 18 | `while count < 5 { ... }` (bare) | true | emit `while (count < 5) do ... end`（与 case 14 等价）|
| 19 | `for (var i : 0, 10) { ... }` | true | emit `for i = 0, (10) - 1 do ... end` |
| 20 | `for var i : 0, 10 { ... }` (bare header) | true | 同 19，但 paren-less |
| 21 | `for (var i : 10) { ... }` 旧短形式 | true | emit `for i = 1, 10 do`（**不**误判为 range），回归 guard |

**结果**：`AYScript_Test` **720/720 全绿**（R5.0 705 + R5.0.1 5 cases + 已有 10 个 dump-case 集成用例）。

**Defer 状态**：R5.0.1 完成了 `break` / `continue` / C-style `for` / range-token (`..<`) 之外的所有"循环语法优化"诉求。剩余 defer：
- `break` / `continue`：R5.0.1 锁"不破坏控制流图"原则，加 break-label 会膨胀 break-scope 设计面；下一切片
- C-style `for (i = 1; i <= N; i++)`：无 `++`、无 `;`-list inside paren；下一切片
- `..<` range token：R5.0.1 的 `start, end` 形式已经覆盖了 `0..<N` 的核心需求（half-open 语义），不需要单独加 token

**Lessons learned (R5.0.1 specific)**：

21. **"强制 parens" 是设计洁癖 vs 工程实用性的取舍**：R5.0 第一版强制 `()` 是为了"if/while/for 三种 control-flow 形态一致"，但用户的真实代码里 `if x > 0` 的出现频率比 `if (x > 0)` 高得多。R5.0.1 把括号从"强制"改成"可加可不加但要配对"——这是 1 行 `match(LeftParen)` 的代价，但消除了一个用户每天会撞到的体感摩擦。
22. **"0..N-1 vs 1..N"是另一个范式之争**：R5.0 选 1..N 是因为 Logia 的"循环即"counter: 1 to N""心智模型最简单（数组下标从 1 开始这个对不熟悉 Lua/JS 的人友好），但 Lua/JS/C 主流都是 0-indexed。R5.0.1 同时保留 1..N（短形式无歧义）+ 新增 `start, end` 半开（0..N-1 友好）。短形式不破坏是 R5.0.1 不做 breaking change 的关键。
23. **`for (var i : start, end)` 的 emit 必须用 `(end) - 1` 包裹**：`for i = 0, 10*2 - 1 do` 在 Lua 里是 `for i = 0, 10 * (2 - 1) do`（Lua operator precedence：`+`/`-` 在 `*` 之下）—— 整个循环变成 `0..10` 错位。emit 时显式 `emitExpr(end) + ")" + " - 1"` 拼装是必须的，这是 codegen 处理 arithmetic-in-control-flow 的通用模式。

#### 5.7.4.x+2 R5.1 — `break` / `continue`（2026-07-13）

**状态**：✅ 完成。R5.0 落地循环 + R5.0.1 加可选括号 / 半开区间后，唯一明显缺的是 loop 早退能力。R5.1 把 `break` / `continue` 提到第一类语法，闭环了"loop 怎么写"的问题。**不**做 labeled `break LABEL`（多层循环跳出），推到 R5.2。

**语法**：

```logia
// break: 立刻退出最近的 loop
for (var i : 0, 100) {
    if i == 42 { break }      // 跳出 for
}

// continue: 立刻进入下一次迭代
var n: int = 0
while n < 10 {
    n = n + 1
    if n % 2 == 0 { continue }   // 跳到 n+1 重新 evaluate
    sum = sum + n
}
```

- `break;`（分号可选，与 Logia 整体 `;`-optional 风格一致）
- `continue;`（同上）
- 两者都**必须**出现在 `while` / `for` 体内，否则 hard error

**实现要点**：

1. **Lexer**：`include/logia/AYToken.h` 加 `TokenType::Break` / `TokenType::Continue`；`src/logia/AYLexer.cpp` keywords map 收编 `break` / `continue`（从此保留字）。R3/R4 测试没有用这两个词做 identifier 的——grep 验证过。

2. **AST**：`include/logia/AYAst.h` 加 `BreakStmt` / `ContinueStmt`（无字段，单纯 marker node）。`analyzer` walk 子节点，break/continue 没有子节点，所以 analyzer 方法是 no-op。

3. **Parser loopDepth gate**（核心正确性保证）：
   - `Parser::_loopDepth` 字段，初始 0
   - `parseWhileStmt` / `parseForStmt` 在 parseBlockBody 前后 `++_loopDepth; ...; --_loopDepth;` push/pop
   - `parseBreakStmt` / `parseContinueStmt` 第一行检查 `_loopDepth == 0` 是则报 `'break' outside loop` / `'continue' outside loop` 并 `return nullptr`
   - **关键不变量**：`parseStatement` dispatch 顺序不变，`break` / `continue` 在 `if` / `while` / `for` 之后、`var` 之前
   - **关键不变量**：nested loop（`for { while { break } }`）自然 work——`break` 看的是 `_loopDepth > 0`（即"在某个 loop 里"），不需要知道具体是哪个 loop。这正好匹配 Lua 的"break 跳最近 loop"语义。

4. **Codegen 直 emit**：`emitBreakStmt` 输出 `break\n`；`emitContinueStmt` 输出 `continue\n`。Lua 5.2+ 原生支持两个关键字（Lua 5.5 是 AYScript 跑的 runtime），不需要 runtime helper / goto-juggling / 状态机。

5. **synchronize() 加 recovery token**：`While` / `For` / `Break` / `Continue` 都在 case 列表——loop body 内 parse error 能在下一个 loop-boundary 恢复。

**kLogiaPipelineVersion 8 → 9**：codegen 新增 `break` / `continue` 形态，缓存强制失效。

**测试覆盖**（`Test_LogiaEmitDump.cpp` 26 cases —— 21 R5.0+R5.0.1 + 5 R5.1）：

| # | Logia source | Expect success | 验证点 |
|---|---|---|---|
| 22 | `for + if { break }` | true | emit `for i = 0, (100) - 1 do ... break ... end` |
| 23 | `while + if { continue }` | true | emit `while (n < 10) do ... continue ... end` |
| 24 | nested `for` + `break` in inner body | true | emit 两个 `for ... do ... break ... end`；break 命中 inner |
| 25 | bare `break` in helper body | **false** | error: `'break' outside loop` |
| 26 | bare `continue` in on_start body | **false** | error: `'continue' outside loop` |

**结果**：`AYScript_Test` **734/734 全绿**（R5.0.1 720 + R5.1 5 cases + 已有 9 个 dump-case 集成用例）。

**Defer to R5.2+**：

| 不做 | 原因 | 计划 |
|---|---|---|
| Labeled `break LABEL` / `continue LABEL` | 现有 R5.1 已覆盖"break 跳最近 loop"的 95% 用例；多层 break 需要命名 loop（`for LABEL ...`）+ label stack，膨胀设计面 | R5.2 |
| `do { ... } end` block scoping | Logia 当前 `{ ... }` 只是 parser 边界，不引入新 scope。引入 `do/end` 作为显式 scope 需要 analyzer 加 scope stack push/pop，影响 5+ 文件 | R5.2 |
| Bound 类型校验（`for (var i : "foo")`） | 需要 expression-type inference | R5.2+ |
| C-style `for (i = 1; i <= N; i++)` | 无 `++`、无 `;`-list inside paren | R5.2+ |

**Lessons learned (R5.1 specific)**：

24. **loopDepth 是单一 source of truth**："break 在哪个 loop 里"这个问题，Lua 自身的语义是"最近的外层 loop"，所以 R5.1 的实现不需要 label / 名字 / 栈，只需要一个 int 计数器。push 在 while/for parse 入口、pop 在出口，break/continue parse 入口检查 `> 0`。这个设计在 nested loop 嵌套时自动 work，因为 inner 的 push/pop 不会动 outer 的 depth。

25. **parser 拒绝 vs codegen 拒绝的取舍**：R5.1 选择在 parser 拒绝 `'break' outside loop`（hard error，compile failure），而不是让 codegen 继续 emit 出 bare `break` 让 Lua runtime 报。理由是 Logia 用户是脚本层开发者，看到 `'break' outside loop` 这种 source-level error 比 Lua 自己的 `'<name>' expected near 'break'` 错误信息更直接。这是 Logia 整体"用户友好错误信息优先"原则的延续。

26. **Lua 5.2+ 的 `continue` 让 R5.1 实现极简**：如果 Lua 没有原生 `continue`（Lua 5.0/5.1），R5.1 需要做更复杂的 emit（如用 `goto` + 标签模拟 continue，或在 loop 末尾加一个 `if should_continue then continue_marker = true` 的状态机）。Lua 5.5 是 AYScript 跑的 runtime（per LogiaRuntimeBridge setup），所以 R5.1 享受了这个 Lua 演进的成果——这是"ship on a modern runtime"的隐性收益。

#### 5.7.4.x+3 R5.2-A → R5.2-H — Loop/condition 静态类型校验系列（2026-07-14）

**状态**：✅ 完成（R5.2-A do-block + R5.2-B label/break-label + R5.2-C bound type-check + R5.2-D optional step + R5.2-E bound expression-shape + R5.2-H if/while condition bool-check）。`kLogiaPipelineVersion` 9 → 21。`AYScript_Test` **1035/1035 全绿**。

##### 5.7.4.x+3.1 R5.2-A — `do { ... } end` block scope（2026-07-14）

**语法**：

```logia
do {
    var temp: int = compute()
    result = temp * 2
} end                       // temp 在此不可见
```

- `do` / `end` 关键字（`TokenType::Do` / `End`），跟 Lua 5.2+ 的 `do … end` 对齐
- AST 新增 `BlockStmt { body }`，这是**首个** block-shaped AST 节点（其他控制流都内嵌 `std::vector<StmtPtr>`，只有 `do` 因为 `do`/`end` 关键字配对需要独立 AST 节点）
- codegen 用 Lua 5.2+ 原生 `do … end`，纯 additive，无语义跳跃

##### 5.7.4.x+3.2 R5.2-B — 循环作用域的 `::L::` label + `break :L`（2026-07-14）

**语法**：

```logia
for (var i : 0, 10) {
    ::OUTER::
    for (var j : 0, 10) {
        if (j == 5) { break :OUTER }    // 跳出外层 for i
    }
}
```

- `::L::` 必须在 while / for body **直接**声明（loop-scoped），重复同名硬错
- `break :L` 必须能引用某个外层循环的 label
- **`continue :L` 不支持**（用户决策 2026-07-14 — `break label` 实际就是 `goto`，没真正使用 `continue label` 的场景）
- codegen 把 `::L::` **hoist 到 for/while 之后**（不前）— Lua 5.2+ `goto L` 是字面跳转，把 label 放 for 之前会让 for 头被重新执行 → 死循环
- analyzer 用 `_labelStack`（vector<unordered_set<string>>），在 while/for body push/pop；`break :L` 验证 `isLabelVisible(label)`

##### 5.7.4.x+3.3 R5.2-C — `for`-loop bound 类型校验（2026-07-14）

**为什么必须有**：R5.0 / R5.0.1 的 bound 表达式没有静态类型校验，`for (var i : "hello")` 默默通过直到 Lua runtime 报 `'for' initial value must be a number` — 错误信息迟且不友好。

**规则**：短形式 `for (var i : N)` 和半开区间 `for (var i : lo, hi)` 的 bound 表达式必须静态归约为 int，否则 `ErrorCode::TypeMismatch` 硬错，compile 失败。

**接受的 shape**（详细决策矩阵见 source comment on `boundIsStaticallyInt`）：

| leaf / node | 接受？ | 来源 |
|---|---|---|
| `IntLiteralExpr` | ✅ | 字面 |
| `IdentifierExpr` w/ `resolvedType=int` 或 `resolvedDecl→VarDeclStmt.typeName="int"` | ✅ | R5.2-C 修了 builtin-var `var n: int` 的 fallback 路径 |
| `MemberExpr` leaf w/ `resolvedType=int` | ✅ | `self.<int_field>` |
| `CallExpr` w/ `resolvedMethod` 返回 int | ✅ | `self.<int_method>()` |
| `FloatLiteralExpr` / `StringLiteralExpr` / `BoolLiteralExpr` | ❌ | 字面错 |
| `IndexExpr` / `TableExpr` | ❌ | 无 type inference |
| `n + 1`（`n: int`） | ❌（R5.2-C）→ ✅（R5.2-E 升级，见 §5.7.4.x+3.5） | — |

**用户决策 2026-07-14**：hard error（不是 soft warning）— strictness 一致性，匹配 R5.2-B 已建立的「loop 相关违规走 hard error」原则。

##### 5.7.4.x+3.4 R5.2-D — `for`-loop 可选第三参数 `step`（2026-07-14）

**语法**：

```logia
for (var i : 0, 10, 3) { }    // 0, 3, 6, 9
for (var i : 5, 1) { }        // 5, 4, 3, 2, 1 (默认 step=+1)
```

**step 接受规则**（用户决策 2026-07-14，三条一并）：

1. **只**接受正数（`step <= 0` 硬错，包括 `step == 0`）
2. **省略时默认 +1**（R5.0 / R5.0.1 兼容）
3. **省略两个参数时保持 1..N 短形式不变**（`for (var i : N)` 仍是 R5.0 兼容形式）

**step 必须 const-folded**（**不**接受 var / self.field）：

| step 形态 | 接受？ | 原因 |
|---|---|---|
| `3` / `1 + 2` / `(2 * 4) - 5` | ✅ | const-foldable to int |
| `n` where `n: int` | ❌ | var step |
| `n - 1` where `n: int` | ❌ | arithmetic on var step |
| `self.count` where `self.count: int` | ❌ | self.field step |

**为什么 step 严格、bound 宽松**：codegen 不能静态保证 `step != 0` 而不注入 `if step == 0 then error(...)` runtime check — 这偏离 "static-only" 原则。R5.2-E 把 bound 推到 type-recursion（接受 `n + 1`），但 step 仍严格 const-folded。要开 step var → 见 R5.2-G (deferred)。

**短形式不接受 step**：`for (var i : N, S)` 在 R5.0.1 已经 settle 为 `start=N, end=S`（range 形式），引入 short-form-with-step 会让 `for (var i : 10, 2)` 再次歧义。

##### 5.7.4.x+3.5 R5.2-E — bound 表达式级类型推断（2026-07-14）

**规则**：bound 接受 `n + 1` / `damage - 10` / `n + m` 等 int-typed var 参与的 arithmetic 表达式；step 仍 strict R5.2-D const-folded。

**接受的 shape**（R5.2-C 基础 + R5.2-E 扩展）：

| 表达式 | 接受？ | 来源 |
|---|---|---|
| `n + 1`（`n: int`） | ✅ | R5.2-E（latent in R5.2-C） |
| `damage - 10` | ✅ | R5.2-E |
| `n + m`（两 var） | ✅ | R5.2-E |
| `n + 1.5` | ❌ | Logia 无 implicit int↔float promotion |
| `n * 2 + 1` | ✅ | R5.2-E 递归 |

**为什么 0 行代码**：R5.2-C 的 `boundIsStaticallyInt` 已经支持 BinaryExpr 递归 + IdentifierExpr 路径在 R5.2-C 已经修了 `var n: int` 的 fallback。R5.2-E 做的事是 **document** 这条路径能用、**add fixture** 验证、加 bump。真正的 capability 是 latent in R5.2-C。

##### 5.7.4.x+3.6 R5.2-H — `if` / `while` 条件必须 bool（2026-07-14）

**规则**（用户决策 2026-07-14）："for 的三个参数已经是 expression，while/if 直接显示要求 bool，不做语法糖适配"。**Logia 无 C-style truthiness** — `if (n)` where `n: int` 是 hard error，不静默 truthy-coerce。

**接受的 shape**：

| 表达式 | 接受？ | 原因 |
|---|---|---|
| `ready` where `ready: bool` | ✅ | bool var |
| `n > 0` | ✅ | comparison op + 两个 primitive leaf |
| `a == 5` | ✅ | comparison op |
| `name == "foo"` | ✅ | string comparison |
| `ready && armed` | ✅ | logical op over two bool |
| `!ready` | ✅ | logical-not |
| `n` where `n: int` | ❌ | **核心规则**：无 implicit int-as-bool |
| `count + 1` | ❌ | arithmetic 不产 bool |
| `5` / `1.5` / `"hello"` | ❌ | literal 非 bool |
| `input.is_pressed("jump")` | ✅ | **R5.2-H.b carve-out**: ambient receiver → host shim contract yields bool |
| `j == 1` 在 `for (var j : 3) { ... }` 里 | ✅ | **R5.2-H.b carve-out**: for-loop counter 算 int leaf |

**两个 carve-out 是 host-boundary contract，不是 truthiness 放宽**：

- **(a) ambient-receiver CallExpr**：host runtime 注入 `input` / `log` / `time` 名字，contract 它们的方法 yield bool（`input.is_pressed` 返回 bool）。没有这个 carve-out，每个 canonical Logia example 会 hard-error，因为 `analyzeCallExpr` 只为 `self.<method>(...)` stamp `resolvedMethod`（见 `AYSemanticAnalyzer.cpp:1431`）。这是 "ambient shim contract yields bool"，**不是** "any non-bool is bool"。non-ambient receiver 的 strict no-truthiness rule 不变。
- **(b) for-loop counter**：R5.0 / R5.2-C 故意不让 counter 进 `_scope`（post-loop refs 仍 implicit-globals），但 R5.2-H 需要在 condition 验证里知道 `j` 是 int leaf。新增 `_loopCounters: vector<unordered_set<string>>` 跟 `_labelStack` 同款 push/pop pattern。

**Pre-R5.2-H 写法的迁移指南**：

| Lua-truthy 写法 | R5.2-H 正确写法 |
|---|---|
| `if (n) { ... }` | `if (n != 0) { ... }` |
| `while (count) { ... }` | `while (count > 0) { ... }` |
| `if (str) { ... }` | `if (str != "") { ... }` |

##### 5.7.4.x+3.7 ⚠️ 重要 foot-gun：`for`-loop counter 的 scope 语义

**R5.0 设计决策**：`for (var i : N) { body }` 的 counter `i` **不**进 analyzer 的 `_scope`，且 Lua `for i = 1, N do ... end` 本身让 `i` loop-local。具体行为：

```logia
for (var i : 0, 5) {
    log.info(tostring(i))      // ✅ i 是 loop-local，runtime 0..4
}
log.info(tostring(i))           // ⚠️ analyzer 不报错（_scope 没 i），runtime 报 nil
i = 99                          // ⚠️ analyzer 静默接受（隐式 implicit global），runtime 创建全局 i
```

**用户能观察到的两条规则**：

1. **counter 在 loop body 内可见、loop 外不可见（runtime 视角）**。analyzer 视角下 post-loop `i` 看起来像 implicit global 写 — 这是 R5.0 / R5.2-C 的 deliberate 设计（避免 analyzer/runtime 视图失配），**不是 bug**。要复用 counter 的最终值，**必须在循环内显式赋值到 outer var**：

   ```logia
   var lastI: int = -1
   for (var i : 0, 5) {
       lastI = i              // ✅ 显式持久化
   }
   // lastI = 4
   ```

2. **post-loop 写同名 identifier 是 implicit global**（Lua 语义）：`for (var i : 0, 5) { } i = 99` 不会创建 Logia var，而是在 Lua 全局 namespace 创建一个 `i = 99`。这跟 Logia S1 的 undeclared-identifier-write 规则一致；**analyzer 不会报**，因为它跟 counter 同名但 _scope 里没这个名字。

3. **嵌套 for-loop counter 自动 shadow**（Lua 保证）：`for (var i : 0, 3) { for (var i : 0, 3) { ... } }` — inner `i` shadow outer `i`，这是 Lua 语义，Logia 复用。analyzer 也按此处理（counter 没进 _scope，所以 shadow 在 analyzer 视角下不存在，但在 runtime 视角下存在 — 隐式一致）。

**为什么不把 counter 进 `_scope`**：counter 进 _scope 会让 post-loop `i = 5` 在 analyzer 视角下"看似声明过"，但 runtime 是 implicit global — analyzer/runtime 视图失配 → 静默类型错误（注释见 `AYSemanticAnalyzer.cpp:854`）。统一原则：**Logia 生成的代码里 counter 走 Lua loop-local，analyzer 也不假装拥有这个 counter**。

##### 5.7.4.x+3.8 静态类型校验的"小步快跑"哲学

R5.2-C/D/E/H 是同款 philosophy 的四个切片：

1. **每个新语法都先落地**（R5.0、R5.0.1、R5.1、R5.2-A、R5.2-B） — 让用户写出来能跑
2. **静态类型校验分批上**（R5.2-C、D、E、H） — 不一次铺开 expression-type-inference 子系统
3. **每批只覆盖一个静态不变量**：
   - R5.2-C: bound shape (int literal / typed identifier)
   - R5.2-D: step shape (positive-int constant) — **比 bound 更严**
   - R5.2-E: bound expression-shape (`n + 1` accept)
   - R5.2-H: condition shape (bool, no truthiness)
4. **每个 step 配 bump pipeline version** — cache invalidation 不留 stale entry
5. **R5.2-H.b carve-out 是 host-boundary trust，不是 type-system 放宽** — 区分 "Logia 类型系统放宽" vs "host shim contract trust" 是 orthogonal concerns

**`kLogiaPipelineVersion` 演进**：R5.0 = 7 → R5.0.1 = 8 → R5.1 = 9 → R5.2-A = 16 → R5.2-B = 17 → R5.2-C = 18 → R5.2-D = 19 → R5.2-E = 20 → R5.2-H = 21 → **S5 ED-02 = 22**。

#### 5.7.4.x+4 S5 ED-02 — Source-location stamping for analyzer diagnostics（2026-07-14）

**状态**：✅ 完成。`kLogiaPipelineVersion 21 → 22`。`AYScript_Test` **1058/1058 全绿**（1035 R5.2-H baseline + 11 S5 ED-02 新增 + 12 隐藏 slice 增长）。commit `4eae7f8`。

**为什么必须有**：R5.2-C/D/H 闭环 control flow 静态校验后,**用户体验**的最大缺口是 diagnostic 不知道"哪一行"。`for (var i : "hello")` 在 200 行 script 里默默报 `for-loop bound must be int, got string literal` 但 `0:0` 让用户找不到错误。CLI 的 `formatDiagnostic` (cli/main.cpp:148) 早就准备好 `file:line:col:` 格式,**只缺数据**。Slice 之前每个 analyzer-side 错误都 emission `0:0`,这是 debug 体验的最后一公里。

**用户决策 2026-07-14** (this conversation plan-mode Q&A):
- `CallExpr` / `MemberExpr` / `IndexExpr` postfix stamp → **inner expression's loc** (不是 punctuation)
- `VarDeclStmt` initializer 报错 → **var 关键字的 loc** (不是 initializer)
- Scope → include **ScriptDecl-level 错误**

**AST 改动** (include/logia/AYAst.h):
- `Expr` base 加 `SourceLocation sourceLoc = {}`(8 个 Expr subclass 继承)
- `Stmt` base 加 `SourceLocation sourceLoc = {}`(13 个 concrete subclass 继承 — 单一 base field vs 13 个 per-subclass field;1 declaration 覆盖全部 13,后续 R-n 无需重新声明)
- `ScriptDecl` 加 `SourceLocation sourceLoc = {}`(per scope decision)
- 拉入 `AYCompilerError.h` 头

**Parser 改动** (src/logia/AYParser.cpp + include/logia/AYParser.h):
- 8 个 helper 函数 thread keyword token through:`parseReturnStmt(ifTok)` / `parseIfStmt(ifTok)` / `parseWhileStmt(whileTok)` / `parseForStmt(forTok)` / `parseBreakStmt(breakTok)` / `parseContinueStmt(continueTok)` / `parseDoBlock(doTok)` / `parseLabelDecl(openColonColon)` / `parseLifecycleFunc(kind, keywordTok)`。Keyword 在 `parseStatement` / `parseMember` 的 `match()` 后 `previous()` 立即捕获。
- ~30 个 construction site stamp sourceLoc,**关键决定** (user-decided):
  - **`ExprStmt`** 取 **inner expr 的 loc**(不是 `previous()` — 那是 trailing `;`)
  - **`VarDeclStmt`** 取 **var 关键字的 loc**
  - **`ReturnStmt` / `IfStmt` / `WhileStmt` / `ForStmt` / `BreakStmt` / `ContinueStmt` / `BlockStmt(do)` / `LifecycleFuncDecl`** → keyword token 的 loc
  - **`LabelDeclStmt`** → label-name 的 loc(不是 `::`)
  - **`FunctionDeclStmt`** → function-name 的 loc
  - **`ScriptDecl`** → `script` keyword 的 loc
  - **`CallExpr`** → **callee 的 loc**(不是 `(`)
  - **`MemberExpr`** → **field-name 的 loc**(不是 `.`)
  - **`IndexExpr`** → **index expr 的 loc**(不是 `[`)
  - **`LiteralExpr` / `IdentifierExpr` / `BinaryExpr` / `UnaryExpr`** → 各自 distinguishing token 的 loc

**Analyzer 改动** (src/logia/AYSemanticAnalyzer.cpp):
- `sourceLocFor(Expr*)` 不再返 `{}`,读 `e->sourceLoc`
- 新 `sourceLocFor(Stmt*)` 返 `s->sourceLoc`(Stmt base field,单 field read 因为所有 13 concrete subclass 继承)
- 新 `sourceLocFor(ScriptDecl*)` 返 `s->sourceLoc`
- **7 个新 populated diagnostic sites**:
  - `analyzeScript` (unknown-host warning + strictInheritance error)
  - `analyzeVarDecl` (unknown-type error)
  - `analyzeLifecycle` (3 个 soft warnings:legacy params, system on_destroy, tool-only host)
  - `analyzeIdentifierExpr` (implicit-global warning)
  - `analyzeMemberExpr` (unknown-field warning)
  - `analyzeBreakStmt` (label-not-visible)
  - `analyzeLabelDeclStmt` (label-outside-loop x2 + duplicate-label)
- **6 个 pre-existing call sites** (R5.2-C/D/H validators:for-bound, for-start, for-step x2, while-condition, if-condition) 自动 pickup real line/col numbers 无需修改

**MSVC 踩坑**:`SourceLocation{line, column}` brace-init 失败,因为 `SourceLocation` 有 3 fields + 非平凡 `std::string file` default ctor — aggregate-initialization 要求全 fields。改用 explicit `.line = a; .column = b;` 赋值,**不**用 factory helper(保持 diff per-site-local + 避免 variadic template 在 8 个 ctor 的复杂度)。

**CLI 接线** (cli/main.cpp:148 `formatDiagnostic`):已有 `file:line:col: severity: message` 格式,**不需要任何改动** — 只需要 `d.location.line > 0` 就有输出。

**测试** (unittest/Test_LogiaSemantic.cpp + unittest/Test_LogiaCli.cpp):
- 新 helpers `hasErrorAt(r, line, code)` 和 `hasWarningAt(r, line, code)` — single-responsibility predicates walk `r.diagnostics` 匹配 severity + errorCode + location.line
- 10 个新 semantic TEST_CASEs 覆盖每个 AST node kind(for-bound-string TypeMismatch / unknown-identifier warning / unknown-field-member warning / break-outside-loop + continue-outside-loop parser-side UnexpectedToken / label-outside-loop analyzer-side InvalidStatement / if-condition + while-condition TypeMismatch / script-unknown-host-type warning / var-decl-unknown-type TypeMismatch)
- 1 个新 CLI smoke test:binary run planted bad source,assert stderr 含 `:3:` (planted for-bound line) 和 `error:`。证明端到端 chain `parser → analyzer → diagnostic → formatDiagnostic → CLI stderr` wired correctly

**Lessons learned**:
1. **Future-slice comment 是 contract 起点**:`sourceLocFor(Expr*) { return {}; }` 在 R5.2-C 时的注释 "Future slice can add Expr-level source locations" 直接成为 S5 ED-02 的 contract 起点,**0 调研成本**
2. **Thread keyword token through 8 helper functions 是 ~18 行的机械改动**,代价远低于 "在每个 helper 里重新 capture previous()" — keyword 在 dispatcher 里 `match()` 完就 `previous()`,但 helper 里再读 `previous()` 是 helper 内最新 consumed 的 token(`(` / `{`),不是 keyword 本身
3. **brace-init `{a, b}` 跟 aggregate-initialization 在有非-trivial default-ctor field 的 struct 上是 MSVC trap** — silent reject,加 `static_assert` 都 catch 不到。Lesson:helper 装 sourceLoc setter 时 explicit `.line = ; .column = ;` 比 `{a, b}` 安全

**`kLogiaPipelineVersion 21 → 22`** — 纯 diagnostic-surface 改但 stale cached diagnostics 会 silently flip from `0:0` 到 real lines,bump 强重 compile。

#### 5.7.4.x+5 S5 ED-03 — Lua runtime panic → Logia source line translation（2026-07-15）

**状态**：✅ 完成。`kLogiaPipelineVersion 22 → 23`。`AYScript_Test` **1066/1066 全绿**（1058 S5 ED-02 baseline + 8 S5 ED-03 新增: 4 codegen-layer + 3 bridge-layer + 1 CLI smoke）。

**为什么必须有**：S5 ED-02 让 **编译时** diagnostic 携带 Logia line/col,但 **运行时** panic 仍输出 raw Lua traceback。Gameplay dev 在 hot-reload 的 `.logia` 文件里写 `self.someNonexistentField()`,Lua 抛 `[string "..."]:42: attempt to index a nil value (local 'self')` — 那个 `42` 是 **生成的 Lua 行号**,不是用户 `.logia` 文件的行号。Dev 必须 mental-map "Lua line 42" → "Logia line N" 通过 codegen 模型。这是 debug 体验的最后一公里。

**用户决策 2026-07-15** (this conversation plan-mode Q&A):
- **Error API 形状** → `LogiaRuntimeBridge::getLastError()` 返 `TranslatedRuntimeError { luaMessage; SourceLocation logiaLoc; bool translated; }`。模仿 `AYPlugin::DynamicLib::_lastError` pattern (`include/DynamicLib.h:31`)
- **Source map 粒度** → per-line `std::vector<SourceLocation>` indexed by Lua line。O(1) lookup;~50 lines/script 的内存微不足道

**核心数据结构** (include/logia/AYLuaCodegen.h):
```cpp
struct LogiaSourceMap {
    std::vector<SourceLocation> luaLineToSource;  // index N = Lua line N → Logia loc
    SourceLocation lookup(int luaLine) const;     // O(1),out-of-range 返 {} (line 0)
};
struct LuaCodegenResult {
    ...
    LogiaSourceMap sourceMap;  // S5 ED-03
};
```

**Codegen 改动** (src/logia/AYLuaCodegen.cpp):
- **新 helper `writeLine(content)`** = `indent(); _out += content + "\n"; ++_luaLineCount; _sourceMap.luaLineToSource.resize + push _currentAnchor`。这是 S5 ED-03 的中心记账入口,replaces previously-unused `line()` helper(`grep _out += "..."\\n` 在 audit 中发现 ~35 sites 直接写 `_out`,13 sites 写裸 `\n`)。
- **`emitStmt` dispatcher** 在 dispatch 前 `_currentAnchor = stmt.sourceLoc` — 所有 emit path 共享 single dispatcher,每个 Stmt 触发一次 anchor 重置
- **新 `writeBlankLine()`** — codegen 在 lifecycle / helper 之间插入的可读性空行;push `{}` (无 Logia source)
- **新 `writeNoAnchor(content)`** — codegen preamble (`-- Auto-generated ...`, `local M = {}`) + epilogue (`return M`) 走这个,push `{}` 让 runtime translation 正确返回 "untranslatable" 而不是指向用户代码
- **`emitAssignmentTarget` 内嵌的 `local __tmp = ...`** 也走 `writeLine` — 之前是 `_out += "local ...\n"` 直接写,现在是显式 anchor-tracking 行
- **Emit 方法全重构** (emitVarDecl / emitIfStmt / emitWhileStmt / emitForStmt / emitBreak / emitContinue / emitBlock / emitLabel / emitReturn / emitVarDecl / emitLifecycleFunc / emitFunctionDecl / emitLocalVar / emitExprStmt) — multi-write 模式 (e.g. `local NAME = EXPR`) collapse 成单 `writeLine("local NAME = EXPR")`;compound assign (S3.10/3.11 reflect call rewrite) 写两行,都锚定到同一个 ExprStmt.sourceLoc (正确:runtime panic 落在任意一行都是同一个 Logia source line)

**Codegen output shape 不变**:`writeLine()` refactor 是 byte-identical emit shape 的纯记账改造。所有 1058 S5 ED-02 测试不变过。

**Pipeline 改动** (include/logia/AYLogiaPipeline.h + src/logia/AYLogiaPipeline.cpp):
- `LogiaToLuaResult.sourceMap` 字段新增
- `compileLogiaToLua` 把 `generated.sourceMap` move 到 `result.sourceMap`

**Bridge 改动** (src/AYScriptRuntimeBridge.cpp):
- **`Impl::sourceMaps` map** — 与 `Impl::scripts` map 并行,keyed by `scriptName`
- **`Impl::CompileCacheEntry`** 加 `LogiaSourceMap sourceMap` — cache hit 路径直接拿缓存的 sourceMap,无需重新跑 front end
- **`Impl::_lastError`**: `TranslatedRuntimeError` 字段,callLifecycle 失败时填充
- **`shutdown()`** clear `sourceMaps` + reset `_lastError`,保持与 `scripts` / `_compileCache` / `_compileHits` 同步
- **新 file-local helper `translateLuaErrorToLogia(scriptName, luaMessage, sourceMaps)`** — parse `[string "..."]:N:` 第一个 frame 的 Lua line,通过 source map 翻译成 Logia loc;返 `{}` 当无法 parse 或 scriptName 不在 map 中
- **`callLifecycle` 失败分支** — populate `_lastError`,**enrich `ayt::log::error(...)` 输出** 当 translation 成功 (`<input>:line:col:` prefix),失败 fall back 到 raw Lua traceback format。语义:dev 看 log 能直接定位 `.logia` 文件行号
- **`callLifecycle` 成功路径** — reset `_lastError` 到 default

**API 改动** (include/AYScriptRuntimeBridge.h):
```cpp
struct TranslatedRuntimeError {
    std::string luaMessage;
    logia::SourceLocation logiaLoc;
    bool translated = false;
};
[[nodiscard]] TranslatedRuntimeError getLastError() const noexcept;
```

**测试**:
- **Codegen-layer** (Test_LogiaSemantic.cpp): `s5ed03_sourcemap_size_matches_lua_lines` / `s5ed03_sourcemap_anchors_match_stmt_source_locs` / `s5ed03_sourcemap_header_footer_unanchored` / `s5ed03_sourcemap_lookup_out_of_range_returns_zero`
- **Bridge-layer** (Test_LogiaRuntime.cpp): `s5ed03_bridge_translates_runtime_error_to_logia_line` (planted `error("boom")` on line 4,assert `getLastError().logiaLoc.line == 4 && translated == true`) / `s5ed03_bridge_last_error_cleared_by_successful_call` (first script panics, second succeeds,after second call `_lastError` reset) / `s5ed03_bridge_chunk_load_failure_records_last_error` (load-time vs call-time 失败区分,environmental skip 当 front-end 拒绝 planted bad pattern)
- **CLI smoke** (Test_LogiaCli.cpp): `cli_s5ed03_codegen_output_unchanged_by_sourcemap_refactor` — 由于 CLI `compile` subcommand 不调用 lifecycle (runtime panic 是 in-process bridge 关注点),CLI smoke 改为 codegen shape unchanged 验证:assert CLI 输出 Lua 含 baseline 子串 (`local n = 0`, `function M.on_start(self)`, `local M = {}`, `return M`, `-- Auto-generated ...`) 证明 `writeLine()` refactor 不破坏 emit shape

**Lessons learned**:
1. **Unused helper 是埋点**:file audit 发现 `line()` helper 存在但 0 caller — S5 ED-03 直接 promote 它为 `writeLine()` 是 O(20 lines) 的 diff。如果 audit 没发现,就得为 ~48 sites 各自重写 anchor tracking
2. **`emitStmt` 是 S5 ED-03 的唯一 anchor 更新点**:所有 13 个 concrete Stmt subclass 都通过这个 dispatcher,把 `_currentAnchor = stmt.sourceLoc` 写一次就覆盖全部。如果每个 emit 方法各自 set anchor,会复制 13 次 + 漏掉的 risk
3. **Compound assign 的 2-line emit 都锚定到 ExprStmt.sourceLoc**:runtime panic 落在 `local __tmp = ...` 或 `ayt_reflect_set_field(...)` 任意一行,语义上是同一个 Logia source line (the `self.field += rhs` statement)。两者共享 anchor 是 correct
4. **Codegen preamble/epilogue 锚定到 `{}`**:让 runtime translation 返 "untranslatable" — 这才是正确行为,不是 bug。Codegen 自己生成的行不代表用户代码
5. **CLI smoke 在没有 `run` subcommand 时降级**:CLI 只能验证 codegen shape unchanged,不能验证 runtime translation path。Runtime panic translation 是 in-process bridge 关注点,Test_LogiaRuntime 的 bridge tests 覆盖。CLI 测试不能反映 in-process `_lastError` state
6. **MSVC `<input>` placeholder 是 contract 简化**:runtime anchor 的 `file` 字段空着(codegen 不知道 `.logia` 路径,loadScript 收 string 不收 path)。让 host 端从自己的 context 填 — 比 plumb 路径通过整条 chain 更 cheap

**`kLogiaPipelineVersion 22 → 23`** — codegen output byte-identical,但 `Impl::CompileCacheEntry` 新增 `LogiaSourceMap` 字段,stale cache entry 缺字段会让 cache-hit path 编译失败。bump 强重 compile cache。

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
| **R4** | `std::vector<T>` / `std::array<T,N>` args；嵌套 struct 字段；`T&` out-param | ⏳ 部分 (R4.0 + R4.1 + R4.2 ship;R4.2b std::string out-param 待) | R3.5 可选 |
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

- **API surface**：`LogiaRuntimeBridge::reloadScript(name, src, errs)`（3-arg 委派到 `defaultLogiaHostContext()`）+ `reloadScript(name, src, ctx, errs)`（4-arg 与 `loadScript` 4-arg 对称）。两个重载均 **不**接触 `ScriptSubSystem`、**不** link AYIO、**不** include `ayio/File.h`。文件路径加载 / FileWatcher 推到 S3.7b。
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
- **读文件**：SubSystem 用 `ayt::io::File::readAllText`；bridge **不** include `ayio/File.h`。
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
| 2026-07-11 | **Audit fix 完成（语法审计修复）**：三条独立 audit defect 一并修：(a) `function` 加进 Lexer + 新增 `FunctionDeclStmt` AST + `parseFunctionDeclStmt()`（只允许 script-block scope；`on_update` body 内部 `function` 是 parser hard-reject）+ `emitFunctionDecl()` 输出顶层 `function NAME(...) ... end`，让 lifecycle body 通过裸名调用；(b) `local`/`nil`/`function`/etc. keyword leak via soft warning — `ErrorCode::LuaKeywordLeak` + SemanticAnalyzer `analyzeStmt` 检测 `ExprStmt(IdentifierExpr("local"))` 形态 emit warning（不 fail compile，让 15+ R3/R4 测试继续 work）；(c) `Test_LogiaAdapter.cpp::adapter_maps_onUpdate_to_on_update_passes_dt` 升级 numeric-equality 检查（之前只 witness `"on_update_called"`，不验证 dt 数值实际到达 body）+ 清理 `parseLifecycleFunc` stale comment（"codegen ignores the params" 已被实际行为推翻）。`Test_LogiaEmitDump.cpp` 升级为正式 acceptance suite（13 cases，case 9 `function` 在 script-block scope 现在 success=true，case 11 加 helper-function round-trip，case 12 验证 internal-block reject，case 13 验证 `local` soft warning）。`kLogiaPipelineVersion` 5 → 6（codegen 形状 + diagnostic surface 变化）。**Plan-mode 阶段关键教训**：原 audit 把 `local\nname` 错定到 codegen `emitVarDecl` —— agent 通过 emit 实际 trace 确认**真正**根因是 parser 把 `local` 当 identifier 产生两个 statement。诊断与 parse-success 解耦让大批 R3 fixture 兼容。`AYScript_Test` **692/692 全绿**（baseline 671 + audit-fix 21 cases）。 |
| 2026-07-13 | **R5.0 完成（while / for 循环语句）**：`while (cond) { body }` 与 `for (var i : N) { body }` 在 R5.0 落地为 Logia 一级语法。`include/logia/AYToken.h` 加 `TokenType::While` / `TokenType::For`；`src/logia/AYLexer.cpp` keywords map 收编 `while` / `for`（从此保留字）；`include/logia/AYAst.h` 加 `WhileStmt(condition, body)` / `ForStmt(counterName, bound, body)` AST 节点；`src/logia/AYParser.cpp` 新增 `parseWhileStmt()` / `parseForStmt()` 在 `parseStatement()` dispatch 的 `If` 后插入，`for` header 是专用 counter-var pattern `var Identifier Colon Expr`，**不**复用 `parseVarDecl`（否则 `10` 会被当 type name 报 unknown-type），`synchronize()` 加 `While` / `For` recovery token；`src/logia/AYLuaCodegen.cpp` 新增 `emitWhileStmt` → `while <cond> do ... end` + `emitForStmt` → `for <counter> = 1, <bound> do ... end`（emit 不带 `var` / `:`，Lua `for` 隐式声明 counter），`emitStmt` dispatch 同步扩展；`src/logia/AYSemanticAnalyzer.cpp` 加 `analyzeWhileStmt` / `analyzeForStmt`，**关键不变量**：`for` counter 不进 analyzer `_scope`（Lua `for` 已是 loop-local，强行塞 scope 会与 runtime 视图失配），`analyzeStmt` dispatch 同步扩展；`include/AYScriptRuntimeBridge.h` 把 `kLogiaPipelineVersion` 6 → 7；`unittest/Test_LogiaEmitDump.cpp` 增 case 14（`while` 在 script-block helper 里）/ 15（`for` + 验证 emit 不带 `var`）/ 16（嵌套 `for`）。`AYScript_Test` **705/705 全绿**（baseline 692 + R5.0 3 cases + 已有的 10 个漏计入的 dump-case 集成用例）。Defer to R5.0.1 / R4.5+：`break` / `continue`、C-style `for`、range `for (i in 1..N)`（需 `..` token）、bound 类型校验（需 expression-type inference）。**Lessons learned**：var-vs-type 歧义由 parser 显式处理而非 grammar 升级；counter scope 由 Lua 提供而非 analyzer 提供（避免 analyzer/runtime 视图失配）；`while`/`for` 走 brace-only 但 emit 用 Lua 原生 `do/end`（brace-only 是 source-level，`do/end` 是 target-level，两者不必统一）。 |
| 2026-07-13 | **R5.0.1 完成（可选括号 + for 半开区间）**：R5.0 落地后用户反馈两点 — (a) 强制 `if (cond) { ... }` 括号违背人写代码习惯（多数人写 `if x > 0` 不写 `if (x > 0)`），(b) `for (var i : 10)` 强制 1..N 偏离主流 0..N-1 习惯。R5.0.1 在 R5.0 基础上加两条无 breaking change 的扩展：(1) `if cond { ... }` / `while cond { ... }` / `for var i : N { ... }` 三种 statement 括号可选（peented，但加了左括号就强制配对右括号；混搭 `if (cond { ... }` 是 clean parse error）；(2) `for (var i : start, end) { body }` 半开区间形式 emit `for i = start, (end) - 1 do`（Lua numeric-for 是 inclusive，两端相减模拟 [start, end) 语义；括号包裹 `end` 是因为 end 可能是 `n*2` 这种算术，不包会被 Lua precedence 解析成 `n * (2 - 1)`）。`include/logia/AYAst.h` `ForStmt` 加 `ExprPtr start` field + 新 ctor `ForStmt(counterName, start, end, body)`，旧 ctor 标短形式（`start == nullptr`）；`src/logia/AYParser.cpp` `parseIfStmt` / `parseWhileStmt` / `parseForStmt` 用 `match(LeftParen)` peek 后 dispatch 两种形态，`parseForStmt` 在 first expression 后 peek `,` 决定走 start-end range 还是单 bound 短形式，`parseIfStmt` 的 `else` 分支 peek `If` 后递归支持 `else if` 链；`src/logia/AYLuaCodegen.cpp` `emitForStmt` 看 `stmt.start` 双形态：非空 emit `start, (end) - 1`、空 emit `1, bound`；`include/AYScriptRuntimeBridge.h` 把 `kLogiaPipelineVersion` 7 → 8（codegen 新形态）；`unittest/Test_LogiaEmitDump.cpp` 增 case 17（`if` bare）/ 18（`while` bare）/ 19（`for (var i : 0, 10)` half-open）/ 20（`for var i : 0, 10 { ... }` 头 bare）/ 21（`for (var i : 10)` 短形式回归 guard）。`AYScript_Test` **720/720 全绿**（R5.0 705 + R5.0.1 5 cases + 已有 10 个 dump-case 集成用例）。**Lessons learned**：强制括号是设计洁癖 vs 工程实用性的取舍（1 行 `match(LeftParen)` 消除每天撞到的体感摩擦）；0..N-1 vs 1..N 是范式之争（保留短形式 + 加 range 是双赢）；`for i = 0, n*2 - 1 do` 必须 `(end) - 1` 包裹避免 Lua precedence 错位；MSVC `std::vector<unique_ptr>` initializer_list brace-init 容易触发 `construct_at` overload 解析失败，改用 `vector + push_back` 两步式避免。 |
| 2026-07-13 | **R5.1 完成（`break` / `continue`）**：loop 早退能力闭环。`include/logia/AYToken.h` 加 `TokenType::Break` / `TokenType::Continue`；`src/logia/AYLexer.cpp` keywords map 收编 `break` / `continue`（从此保留字）；`include/logia/AYAst.h` 加 `BreakStmt` / `ContinueStmt`（无字段，marker node）；`include/logia/AYParser.h` 加 `Parser::_loopDepth` int 字段（初始 0）；`src/logia/AYParser.cpp` `parseWhileStmt` / `parseForStmt` 在 `parseBlockBody` 前后 `++_loopDepth; ...; --_loopDepth;` push/pop，新加 `parseBreakStmt` / `parseContinueStmt` 在 loopDepth==0 时报 `'break' outside loop` / `'continue' outside loop` 并 return nullptr，`parseStatement` dispatch 在 `if` / `while` / `for` 之后、`var` 之前加 `Break` / `Continue` 分支，`synchronize()` recovery case 加两个新 token；`include/logia/AYLuaCodegen.h` 加 `emitBreakStmt` / `emitContinueStmt` 声明；`src/logia/AYLuaCodegen.cpp` `emitBreakStmt` 输出 `break\n`、`emitContinueStmt` 输出 `continue\n`（Lua 5.2+ 原生支持，零 runtime helper / 零 goto-juggling），`emitStmt` dispatch 同步扩展；`include/logia/AYSemanticAnalyzer.h` 加 `analyzeBreakStmt` / `analyzeContinueStmt` 声明（无字段 no-op）；`src/logia/AYSemanticAnalyzer.cpp` 实现两个 no-op 方法，`analyzeStmt` dispatch 同步扩展，`analyzeForStmt` 顺手 walk `f.start`（R5.0.1 漏掉的 `start` 表达式 validate）；`include/AYScriptRuntimeBridge.h` 把 `kLogiaPipelineVersion` 8 → 9（codegen 新形态）；`unittest/Test_LogiaEmitDump.cpp` 增 case 22（`for + break`）/ 23（`while + continue`）/ 24（nested `for` inner `break`）/ 25（`break` 在 helper body 里 → hard error）/ 26（`continue` 在 on_start 里 → hard error）。`AYScript_Test` **734/734 全绿**（R5.0.1 720 + R5.1 5 cases + 已有 9 个 dump-case 集成用例）。**Lessons learned**：`loopDepth` 一个 int 字段就足够支持 nested loop 的 break/continue（push/pop 在 while/for 入口/出口），不需要 label / 栈；parser 拒绝比 codegen 拒绝对脚本作者更友好（source-level error vs Lua runtime 错误信息）；Lua 5.2+ 原生 `continue` 让 R5.1 不需要 goto/状态机 hack。Defer 到 R5.2+：labeled `break LABEL` / `do { ... } end` block scoping / bound 类型校验 / C-style `for`。 |
| 2026-07-13 | **R4.1 完成（`std::vector<T>` / `std::array<T, N>` method args + return）**：method 边界 homogeneous 容器双向 marshal 闭环。`AYFoundation/AYReflect/AYReflect.h` 新增 `ArrayTypeInfo<T, N>` 模板（实现 `IContainerTypeInfo` 接口,compile-time N cap,`resize()` / `pushBack()` 是 no-op stub 因为 std::array 不能 grow）+ `registerArrayType<T, N>(name)` helper；`AYFoundation/AYReflect/interface/ayreflect/IReflect.h` 给 `IContainerTypeInfo` 加 `virtual bool isFixedSize() const` 默认 `false`（ArrayTypeInfo override `true`,VectorTypeInfo 保持 `false`）—— 这是 bridge 区分 array vs vector 的唯一信号,**vtable ABI 变化触发整仓 rebuild**。`include/logia/AYMethodInfoImpl.h` 给 `detail` 命名空间加 `is_std_vector_v` / `is_std_array_v` SFINAE traits，`MethodInfoImpl::readArg` 与 `MethodInfoImplConst::readArg` 的 if-constexpr ladder 加 `std::vector<T>` / `std::array<T, N>` 两个分支（slot 持有 heap-allocated 容器指针,readArg copy-construct 到 PMF 参数）。`src/logia/AYScriptRuntimeBridge.cpp` `ayt_reflect_call_method_c` 在 arg-marshal 阶段加 `dynamic_cast<IContainerTypeInfo*>` 分支（lua_len 取 N + lua_rawgeti 逐元素 + 是 vector 则 resize + push_back、是 array 则 cap + memset + memcpy），return-marshal 阶段加同名分支（`getContainerSize(retPtr)` + `getElementAt(retPtr, i)` 逐元素 + 1-indexed lua_rawseti）。`include/logia/AYParser.h` + `src/logia/AYParser.cpp` 给 `parsePrimary` 的 TableExpr 解析加 positional 形式 —— peek `isStartOfKeyedEntry()` (Identifier+`=` 紧邻) 决定走 keyed `{k=v, ...}` 还是 positional `{v1, v2, ...}` 路径；`TableExpr::Entry::key` 早就是 nullable（预留 for R3.5+）,codegen 早处理 `key==nullptr`,R4.1 落实这个预留。`include/AYScriptRuntimeBridge.h` 把 `kLogiaPipelineVersion` 9 → 10（bridge runtime marshal 路径新增）。`unittest/Test_LogiaReflectRuntime.cpp` 新增 `R4_1Player` fixture + 6 个 LG-12 R4.1 测试（vector<int> arg sum / vector<int> return / vector<int> arg shorter / array<float,4> arg / array<int,3> return / empty vector）。`AYScript_Test` **752/752 全绿**（R5.1 734 + R4.1 6 cases + 已有 12 个 dump-case + reflect 集成用例）。**Lessons learned**：MethodInfoImpl 的 if-constexpr ladder 是 bridge 协议扩展点（R3.0 加 enum/string/ref/pointer,R4.1 加 vector/array）;AST 节点预留 nullable 字段（TableExpr::Entry::key）降低后续切片成本;ITypeInfo vtable 改动必须整仓 rebuild（memory note 3）;`__test_witness = tostring(s)` 走 `getLuaGlobalString` 不是 `tryGetLuaGlobalNumber` —— tostring 出 string。Defer 到 R4.1b：struct/std::string 元素类型容器（pushFieldPrimitive struct 路径 + placement-new `std::string`）;R4.2：T&/T* out-param。 |
| 2026-07-13 | **R4.2 完成（`T&` / `T*` out-param 写回）**：method 边界 non-const lvalue ref / non-const pointer 参数双向闭环(C++ 经典 out-param idiom)。`AYFoundation/AYReflect/interface/ayreflect/IReflect.h` 给 `IMethodInfo` 加 `virtual bool getParamIsOut(size_t) const { return false; }` 默认 false(对 input-only IMethodImpls 源码兼容),`MethodInfoImpl` 与 `MethodInfoImplConst` 各自用 `is_lvalue_reference_v<ArgT> && !is_const_v<...>` 与 `is_pointer_v<ArgT> && !is_const_v<...>` 两条 partial-specialization 标 true。**vtable ABI 变化触发整仓 rebuild**(同 R4.1)。`include/logia/AYMethodInfoImpl.h` `detail` 命名空间新增 `is_out_param<ArgT, EnableT>` SFINAE trait 区分 const T&/T*(输入)与非 const T&/T*(输出);`readArg` 拆成 `read<I>` (decltype(auto) 包装,if constexpr 选择) + `readArg<I>` (input-only, by value 返回) + `readOutArg<I>` (out-param, 返回 T& 或 T* 指向 bridge heap slot),`readOutArg` 进一步拆成 T& overload + T* overload(用 `enable_if_t` 区分)以避开 MSVC C3487 持续触发。`is_std_vector_v` / `is_std_array_v` trait 加 `remove_cv_t<remove_reference_t<X>>` strip ref/const(R4.1 写时只考虑 exact match, R4.2 让 `const std::vector<int>&` 走 vector 分支后踩到 C3536 — Lesson 21)。`src/AYScriptRuntimeBridge.cpp` `ayt_reflect_call_method_c` 在 arg-marshal fallback `else` 内 `if (method->getParamIsOut(i) && paramType)` 检测 — int/float/struct 三种类型支持,heap-alloc T(seeded from Lua 初始值),register cleanup,`argPtrs[i] = heapPtr`。post-invoke 写回 loop 对 struct 类型用 `pushFieldPrimitive` 递归构造新 Lua table 覆盖原 arg slot(primitive 不写回 — 用 C++ 写到 self.* + Logia 读 self.* side-effect pattern,因为 Lua local variable 不能从 C 端 rebind)。`include/AYScriptRuntimeBridge.h` 把 `kLogiaPipelineVersion` 10 → 11(bridge runtime marshal 路径新增 + IMethodInfo vtable 变化)。`unittest/Test_LogiaReflectRuntime.cpp` 新增 `R4_2Player` fixture + 5 个 LG-12 R4.2 测试(`fillInt` / `addFive` / `fillFloat` / `fillViaPtr` / `mutatePoint`)。`AYScript_Test` **778/778 全绿**(R4.1 752 + R4.2 5 cases + 已有 21 个 dump/reflect 集成用例)。**Lessons learned**:为 R-n+1 写 trait 时先想 R-n 引入的 ref 修饰,提前 strip 一次(Lesson 21);MSVC `decltype(auto)` + `if constexpr` + 不同 ref/value 路径会触发 C3487,拆成独立函数各自 deduced return type 一致(Lesson 22);Lua 浮点 round-trip 不精确(`3.14f` stored as double 出 long form),R4.2 fixture 改用 `2.0f` 让 tostring 出 "2.0" 短形式(Lesson 24);Lua local variable 不能从 C 端 rebind,out-param 通过 self.* side-effect pattern 读回(Lesson 25)。Defer 到 R4.2b:`std::string` out-param(placement-new)。 |
| 2026-07-11 | **§14 + §14.8**:Phase S3 闭环指挥;**输入统一 AYDevice**,废弃 `AYInput`,INT-02 改接 `InputMapping`。 |

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
| **P-R5.0** | **R5.0** | `while (cond) { body }` / `for (var i : N) { body }` 循环语句 | ✅ 2026-07-13 |
| **P0** | **INT-01** | Editor/Game 注册 `ScriptSubSystem` + Play 加载 `.logia` | ⏳ |
| **P1** | **INT-02** | 真实 **AYDevice** `InputMapping` → `InputProvider` | ⏳ |
| **P2a** | **R1** | `ScriptVisible` / `ScriptReadOnly` 强制 | ⏳ |
| **P2b** | **R3.5** | `registerEnum` + 字段名 stripper + struct 内 string | ⏳ |
| **P2c** | **R4** | vector/array args、嵌套 struct、out-param | ⏳ 部分 (vector/array ship R4.1;nested ship R4.0;out-param int/float/struct ship R4.2;std::string out-param R4.2b 待) |
| **P3** | **S4.x** | signal / await / source map | ⏳ |
| opt | **INT-03** | 磁盘 compile cache、Editor `runTool` 菜单 | ⏳ |
| opt | **INT-04** | EventHandler host、`event.emit` | ⏳ |
| opt | **R5.0.1** | `break` / `continue`、C-style `for`、range `for`、bound 类型校验 | ⏳ |
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
- reloadScriptFromFile / ayio/File.h in bridge
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
| `self.method()` primitive + struct/enum/string | ✅ | vector/array (int/float) ship R4.1; out-param (int/float/struct) ship R4.2;std::string out-param R4.2b 待;vector<struct> R4.1b 待 |
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
| **R4** | `std::vector<T>`/`std::array` args（int/float ship R4.1）；嵌套 struct 字段（ship R4.0）；`T&` out-param（R4.2 待） | 新方法签名 unittest |
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

Read: design.md §5.7.3, §5.7.6, AYReflect ayreflect/IReflect.h FieldAttribute

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
