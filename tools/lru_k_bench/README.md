# LRU-K Bench 说明

这份文档解释 `bustub-lru-k-bench` 到底在测什么、数据从哪里来、程序内部是怎么跑起来的，以及最后应该怎样理解输出结果。

对应代码在 [lru_k_bench.cpp](./lru_k_bench.cpp)。

## 1. 这个 benchmark 的目标

它不是在做完整数据库吞吐测试，也不是直接跑 `BufferPoolManagerInstance` 的真实磁盘读写。

它做的事情更像是：

1. 先准备一条“页面访问序列”。
2. 然后假设 buffer pool 里只有固定数量的 frame。
3. 再把同一条访问序列，分别喂给不同的 `k`。
4. 每个 `k` 都使用同一个 `LRUKReplacer` 实现。
5. 最后比较谁的命中率更高，谁的替换开销更低。

所以它本质上是一个：

- `trace replay benchmark`
- `microbenchmark`
- `replacement policy simulator`

中文可以理解成：

- “访问轨迹回放测试”
- “微基准测试”
- “页面替换策略模拟器”

## 2. 它到底测的是谁

真正被测的核心对象是 `src/buffer/lru_k_replacer.cpp` 里的 `LRUKReplacer`。

也就是说，这个工具的重点不是测整个 BusTub 系统，而是单独评估：

- 在同样的访问模式下
- `k=1`、`k=2`、`k=3`...
- 哪个 `k` 更适合这类 workload

所以你可以把它理解成：

“我不改 workload，只改 `k`，看 buffer replacement 的效果变化。”

## 3. 用了什么技术

这个工具里面主要用了下面几类技术：

### 3.1 参数解析

用的是 `argparse`。

它负责解析命令行参数，比如：

- `--workload`
- `--frames`
- `--pages`
- `--ops`
- `--min-k`
- `--max-k`

所以程序启动时，先不是直接开始测试，而是先把这些参数读进 `BenchConfig`。

### 3.2 synthetic workload 生成

如果你没有提供真实 trace 文件，程序就会自己造一条“页面访问序列”。

这部分用了：

- `std::mt19937_64`：随机数生成器
- `std::uniform_int_distribution`：均匀分布
- `std::bernoulli_distribution`：按概率二选一

这就是 synthetic 数据的来源。

### 3.3 trace replay

无论数据是“随机生成的”还是“从文件读进来的”，最终都会变成一个：

```cpp
std::vector<page_id_t>
```

这个 vector 里每一个元素都代表“这一次访问的 page id 是谁”。

后面的测试不是边运行边随机决定，而是拿着这条已经准备好的序列，按顺序一条一条重放。

这就是为什么不同 `k` 之间是公平的：

- 所有 `k` 用的是同一条 trace
- frame 数完全一样
- workload 顺序完全一样

### 3.4 buffer pool 行为模拟

程序没有直接 new 一个完整 `BufferPoolManagerInstance` 去做真实磁盘 I/O，而是自己写了一个轻量模拟器 `ReplacerSimulator`。

这个模拟器只保留了和“页面是否在 buffer 中”相关的最小必要逻辑：

- `page_to_frame_`：page 当前在哪个 frame
- `frame_to_page_`：frame 当前装的是哪个 page
- `free_frames_`：还有哪些空 frame
- `LRUKReplacer replacer_`：真正负责淘汰决策

这是一种很常见的 benchmark 技术：

- 只保留关键路径
- 去掉和目标无关的噪声
- 让你更清楚地测 replacement policy 本身

### 3.5 参数 sweep

程序会在一个区间里遍历 `k`：

```text
min-k, min-k + step, ..., max-k
```

这叫做 parameter sweep，也就是“参数扫描”。

这样你不用手动一个一个改 `k` 再重复跑。

### 3.6 重复运行与计时

程序使用 `std::chrono::steady_clock` 做计时。

对于每个 `k`，它会跑 `repeat` 次：

- hit/miss/eviction 取第一轮
- `ns/op` 取多轮平均

因为 trace 是固定的，所以每次命中率结果应该一样；多次重复主要是为了让时间结果更稳一些。

## 4. 数据是怎么来的

这是最关键的一部分。

这个 benchmark 支持两大类数据来源：

1. synthetic trace
2. real trace file

---

## 5. synthetic trace 是怎么来的

synthetic 的意思是“人工构造的测试数据”，不是从真实数据库运行里采出来的。

代码里有三种 synthetic workload：

- `hotspot`
- `scan`
- `mixed`

### 5.1 `hotspot`

`hotspot` 的想法是：

- 少数页面非常热门
- 大多数页面很少访问

这很像：

- 热点索引页
- 高频访问的小表
- 某些被频繁 join / probe 的页面

它的生成逻辑大致是：

1. 先定义总页面数 `pages`
2. 再定义热点集合大小 `hotset`
3. 每次访问时，以 `hot_prob` 的概率从热点集合里取 page
4. 以 `1 - hot_prob` 的概率从冷数据里取 page

举个例子：

```text
pages = 10000
hotset = 200
hot_prob = 0.95
```

这表示：

- page id 的全集是 `0..9999`
- 其中 `0..199` 被当成热点页
- 每次访问有 95% 概率打到这 200 个热点页里
- 只有 5% 概率落到冷页

所以生成出来的访问序列会高度偏向热点页。

### 5.2 `scan`

`scan` 的想法是：

- 顺序地访问页面
- 访问完又从头开始

代码逻辑就是：

```text
0, 1, 2, 3, ..., pages-1, 0, 1, 2, ...
```

这很像：

- 全表扫描
- 大范围顺序读

这个 workload 往往会测试 replacer 抗 scan pollution 的能力。

### 5.3 `mixed`

`mixed` 是把 `scan` 和 `hotspot` 拼接在一起。

它不是完全随机混合，而是“分段地交替”：

1. 先来一段 scan
2. 再来一段 hotspot
3. 再 scan
4. 再 hotspot

具体长度由这两个参数控制：

- `scan_burst`
- `hot_burst`

比如：

```text
scan_burst = 256
hot_burst = 2048
```

表示：

- 连续顺序访问 256 次
- 然后连续热点访问 2048 次
- 然后继续下一轮

这类 workload 很适合模拟现实里“偶尔有扫描，但核心工作集仍然很热”的情况。

### 5.4 synthetic 数据的特点

synthetic 数据的优点：

- 可控
- 可复现
- 能单独测试某种访问模式

synthetic 数据的缺点：

- 不一定像你的真实业务
- 结果更适合做“趋势判断”，不一定适合做最终结论

所以通常建议：

1. 先用 synthetic workload 看 `k` 的大致行为
2. 再用真实 trace 做最后决定

---

## 6. real trace file 是怎么来的

如果你使用：

```bash
--workload trace --trace-file your_trace.txt
```

那么数据就不是程序生成的，而是从文件里读。

### 6.1 文件格式

代码里使用：

```cpp
while (trace_file >> raw_page_id) { ... }
```

这意味着：

- 它按“token”读取
- token 之间可以是空格、换行、tab
- 每个 token 都必须是一个整数 page id

所以这几种格式都可以：

```text
1 2 3 4 5
```

```text
1
2
3
4
5
```

```text
1 2
3 4
5
```

### 6.2 trace 文件里的数字代表什么

每一个数字都表示：

“一次页面访问中，被访问到的 page id”

比如这条 trace：

```text
1 2 3 1 4 1 2
```

表示发生了 7 次访问，顺序是：

1. 访问 page 1
2. 访问 page 2
3. 访问 page 3
4. 再访问 page 1
5. 访问 page 4
6. 再访问 page 1
7. 再访问 page 2

程序不会去猜这些 page 来自表、索引还是别的结构。

对 benchmark 来说，这些只是 page id。

### 6.3 trace 文件通常从哪里来

这份 benchmark 自己不会自动从 BusTub 运行时导出 trace。

所以真实 trace 通常来自你额外做的记录，比如：

1. 在 `FetchPgImp` / `NewPgImp` / `UnpinPgImp` 附近打日志
2. 记录每次 page access 的 `page_id`
3. 把它们输出到一个文件
4. 再喂给 `bustub-lru-k-bench`

也就是说：

- `hotspot / scan / mixed` 的数据是 benchmark 自己造的
- `trace` 的数据是你自己从真实 workload 采出来的

---

## 7. benchmark 真正运行时做了什么

无论 trace 是怎么来的，最终都会进入同一个执行流程。

假设我们有：

- `frames = 3`
- trace = `1 2 3 1 4 1 2`

下面看看程序怎么跑。

### 7.1 初始状态

一开始：

- buffer 里什么都没有
- 3 个 frame 都在 `free_frames_`
- `page_to_frame_` 是空的
- `LRUKReplacer` 也是空的

### 7.2 访问 page 1

发现 page 1 不在 buffer 中：

- miss++
- 从 `free_frames_` 拿一个空 frame
- 把 `page 1` 放进去
- 调用 `Touch(frame)`

`Touch(frame)` 里做了三件事：

1. `RecordAccess(frame_id)`
2. `SetEvictable(frame_id, false)`
3. `SetEvictable(frame_id, true)`

它的意思是：

- 记录这次访问历史
- 然后把这个 frame 设成可淘汰

因为这个 benchmark 里没有真实 pin/unpin 生命周期，所以访问结束后就立刻认为它可以被淘汰。

### 7.3 访问 page 2 / 3

过程一样，继续 miss，继续填满空 frame。

现在 buffer 满了，可能是：

```text
frame0 -> page1
frame1 -> page2
frame2 -> page3
```

### 7.4 再访问 page 1

这次在 `page_to_frame_` 里能找到：

- hit++
- 不需要淘汰
- 只更新这页对应 frame 的访问历史

### 7.5 访问 page 4

page 4 不在 buffer，且空 frame 已经没有了：

- miss++
- 调用 `replacer_.Evict(&frame_id)`
- 由 `LRUKReplacer` 决定淘汰哪个 frame
- 被淘汰页从 `page_to_frame_` 里删除
- page 4 装进去

这一步才是“不同 `k` 产生差异”的核心。

因为：

- 同样都是访问 page 4
- `k=1`、`k=2`、`k=3` 的淘汰选择可能不一样
- 后续 hit rate 也就会不同

---

## 8. 为什么只改 `k` 就能比较出效果

关键原因是：

- trace 完全相同
- frame 数完全相同
- 初始状态完全相同
- 唯一变化的是 `LRUKReplacer(pool_size, k)` 里的 `k`

所以实验是“单变量控制”的。

这点很重要，因为只有这样你才能说：

“结果差异主要来自 `k` 的不同，而不是别的配置变化。”

---

## 9. 输出表格的每一列是什么意思

输出大概长这样：

```text
   k         hits       misses     hit_rate    evictions        ns/op
   1         1154          846     57.7000%          838       772.35
   2         1396          604     69.8000%          596       440.20
   3         1404          596     70.2000%          588       531.85
```

### 9.1 `k`

当前这行测试使用的 LRU-K 参数。

### 9.2 `hits`

访问的 page 已经在 buffer 里，不需要淘汰，也不需要装载新页。

### 9.3 `misses`

访问的 page 不在 buffer 里，需要放进 buffer。

如果此时 buffer 满了，就会触发淘汰。

### 9.4 `hit_rate`

计算方式：

```text
hits / (hits + misses)
```

这是选 `k` 时最重要的指标。

### 9.5 `evictions`

表示真正因为 buffer 已满而发生的替换次数。

注意：

- miss 不一定马上 evict
- 在 buffer 还没填满时，miss 只会消耗空 frame
- 只有 buffer 满了之后，miss 才会导致 eviction

### 9.6 `ns/op`

平均每次访问操作消耗多少纳秒。

这里的时间不是数据库真实请求延迟，而是：

- 在这个模拟器里
- 重放一条 page access
- 更新命中状态 / 调用 replacer / 更新映射结构

所花的平均时间。

所以它更适合比较：

- `k` 变大后 replacer 元数据维护是不是更贵

而不是直接当成真实数据库 latency。

---

## 10. `best_k_by_hit_rate` 是怎么选出来的

程序会从所有结果里选“最优”的一项。

规则是：

1. 先比 `hit_rate`
2. `hit_rate` 更高的更好
3. 如果 `hit_rate` 一样，再比 `ns/op`
4. `ns/op` 更低的更好

所以它本质上是在说：

“优先选命中率最高的；如果命中率打平，再选更快的那个。”

---

## 11. `warmup_ops` 是干什么的

`warmup_ops` 表示：

- 前多少次访问只用来“预热”
- 不计入最终统计

原因是很多 workload 在最开始一定会冷启动：

- buffer 还是空的
- 命中率会非常低

有时候你不想让这些冷启动阶段影响最终判断，就可以设置 warmup。

程序中的处理方式是：

- 前 `warmup_ops` 次照样执行
- 但不计入 `hits/misses/evictions`
- `ns/op` 计时也从 warmup 结束后才开始

---

## 12. `repeat` 是干什么的

对于每个 `k`，程序会重复执行多次。

这样做的目的主要是稳定计时结果。

因为：

- 同一条 trace
- 同一组参数
- 命中率结果应该是一样的

所以代码里：

- `hits/misses/evictions` 直接取第一轮
- `ns/op` 取多轮平均

这样既减少重复输出，又保留了比较稳定的时间数据。

---

## 13. 为什么这个 benchmark 没有直接跑真实 BufferPoolManager

这是一个很自然的问题。

原因是如果直接上完整 `BufferPoolManagerInstance`，你会同时测到很多别的东西：

- page table 开销
- 磁盘读写
- memset
- pin/unpin 生命周期
- 甚至未来别的模块副作用

那样当然更“真实”，但就不够“专门”。

而这个 benchmark 想解决的问题更聚焦：

“在同样的 page access trace 下，`k` 的选择会怎样影响 replacement 效果？”

所以这里采用的是模拟器方案：

- 只保留 buffer residency
- 把淘汰决策交给真实的 `LRUKReplacer`
- 去掉不必要的系统噪声

这让结果更容易解释。

---

## 14. 这个 benchmark 的局限性

你理解了流程之后，也要知道它的边界。

### 14.1 它主要测 replacement policy，不是整机性能

`ns/op` 不是实际数据库请求延迟。

### 14.2 synthetic workload 只是模型

如果你的真实 workload 和 synthetic 模式差很多，那么 synthetic 上的最优 `k` 不一定就是线上最优。

### 14.3 这里默认“访问后立刻可淘汰”

因为 benchmark 没有完整 pin/unpin 生命周期，所以 `Touch()` 里会在记录访问后把 frame 设回 evictable。

这适合测 replacement 策略本身，但它比真实系统简化了一层。

### 14.4 trace 文件只包含 page id

它不区分：

- 表页
- 索引页
- 元数据页

如果你需要更细分析，就要在采集 trace 时自己带上更多维度。

---

## 15. 怎样理解你已经跑出来的结果

如果你已经测出了一个结果，比如：

- `k=2` 很好
- `k=4` 略高一点
- `k=8` 又开始变差

那通常可以这样理解：

### 15.1 `k` 太小

更接近普通 LRU。

优点：

- 对近期变化反应快

缺点：

- 更容易被短期 scan 干扰

### 15.2 `k` 适中

往往能更稳定地区分：

- 真正反复访问的页
- 只是偶然访问过一次的页

这通常是最常见的最优区间。

### 15.3 `k` 太大

可能会出现两个问题：

1. 需要更长的访问历史才能体现出优势
2. 对 workload 切换的反应变慢

所以你经常会看到：

- `k=2` 或 `k=3` 很稳
- 特定 workload 下 `k=4` 更好
- 再往上不一定继续涨

---

## 16. 最后的结论

这个 benchmark 的核心思想可以浓缩成一句话：

“先准备一条 page access trace，再让不同的 `k` 在同样的 buffer 容量下回放这条 trace，比较命中率和开销。”

数据来源分两类：

- synthetic：程序自己按规则生成
- trace：你自己从真实运行里采集

程序内部真正做的事情分三步：

1. 生成或读取访问序列
2. 用轻量 buffer 模拟器回放这条序列
3. 把淘汰决策交给真实 `LRUKReplacer`

如果你要决定“最终用哪个 `k`”，最靠谱的顺序通常是：

1. 用 `hotspot / scan / mixed` 看趋势
2. 再用真实 trace 做最终选择

## 17. 如果你还想更进一步

下一步最有价值的是补一个“真实 trace 导出”工具。

也就是在 BusTub 运行 workload 时，把访问过的 `page_id` 记下来，形成 trace 文件，再交给这个 benchmark 回放。

这样你测出来的 `best k` 就不是“对一个人工模型最好”，而是“对你自己的 workload 最好”。
