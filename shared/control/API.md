# 双客户端共享 Lua 控制运行体

`Runtime.cpp/.h` 同时编译进北斗 DLL 和原生无头客户端。宿主通过一个
`Adapter` 提供当前状态、原生动作与释放事实；Lua 的协程、状态投影、
指令预算、取消和结果投影只有这一份实现。它不包含物理、伤害或网络协议模型。

## 生命周期

宿主在确认角色就绪后调用 `Start(id, source, limitMs, error)`，每轮原生更新后
调用 `Tick(sceneChanged)`。`limitMs=0` 明确表示持续执行。
脚本返回、错误、限时或 `Cancel(reason)` 都先调用适配器释放，再等
`Released()`，最后发布脚本终态。`inGame=false` 必须表示宿主已确认退出；
短暂换图可以保留 `inGame=true`、`ready=false`。

```lua
local initial = game.state()
if not initial.ready then return end

local moved = game.move("right", 0.3)
assert(moved.control_released)
game.face("left")
game.jump()
game.attack()
game.pickup()
game.use_item(2000000)
game.stop()
```

## 共同 API

| 调用 | 语义 |
| --- | --- |
| `game.state()` | 当前宿主采样的副本；未知字段为 `nil`，已知空列表为 `{}` |
| `game.capabilities()` | 七个语义动作与 `raw_keys/skill` 能力布尔值，以及 `backend` |
| `game.move(direction, seconds)` | 正常方向控制，默认 0.3 秒，不承诺寻路 |
| `game.face(direction)` | 转向一次；不承诺坐标不变 |
| `game.jump([seconds])` | 默认 0.06 秒的普通跳跃尝试 |
| `game.attack([seconds])` | 默认 0.06 秒的普通攻击尝试，不承诺命中或伤害 |
| `game.pickup([seconds])` | 默认 0.06 秒的近距离原生拾取尝试 |
| `game.use_item(itemId)` | 物品 ID 指定的正常原生使用；宿主限制明确报错 |
| `game.stop()` | 解除本脚本控制并等待确认，然后继续 Lua |
| `game.sleep(seconds)` | 协程等待，不阻塞宿主更新；0 表示交还到下一轮 |
| `game.time()` | 单调秒数；不能当 Unix 时间或与另一进程的绝对值比较 |
| `game.log(text)` | 本次恢复结束后批量提交日志 |

方向使用小写 `left/right/up/down`，`face` 只接受左右。时长最多 30 秒。
六项动作和 `stop` 会让出执行权，控制解除后再返回结果表：

```lua
{
  dispatched = true,
  control_released = true,
  settled = nil,       -- 未确认物理/攻击收束时保留未知
  effect = "unknown", -- 派发不等于游戏效果
  reason = "..."
}
```

每次动作结果在对应调用恢复时取得。随后执行 `stop` 的结果只表示这次释放，
不会把前一个攻击或拾取自动改成游戏成功。

`press/keys` 是可选原始键扩展，`skill` 是可选原生技能扩展；不支持的适配器
返回明确的 `unsupported` 错误，不切换到另一个后端。

## 执行边界

开放 base/table/string/math/utf8 的控制子集，禁用动态加载、受保护调用、
元表以及 os/io/debug/package/coroutine。每次恢复约 50000 条 Lua 指令后让出。
这不是安全沙箱，也不限制任意 C 库调用的 CPU 或内存。只执行受信本地脚本。

状态包含实例、采样序号、Unix 毫秒采样时间、场景代次、角色、地图、位置、
朝向及宿主已知的怪物、背包和掉落；两端缺失的观测保持未知。
当前目标是两端可用，不要求逐帧轨迹或随机结果精确一致。
