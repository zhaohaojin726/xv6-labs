# xv6 启动流程详解

本文档按 CPU 实际执行顺序说明 xv6-riscv 的启动路径，覆盖从 QEMU 跳入内核，到 xv6 完成初始化并进入调度器的过程。

核心路径：

```text
QEMU
-> kernel/entry.S:_entry
-> kernel/start.c:start()
-> kernel/start.c:timerinit()
-> mret
-> kernel/main.c:main()
-> scheduler()
```

## 1. 启动入口位置

QEMU 使用 `-kernel kernel/kernel` 启动 xv6 时，会把内核加载到物理地址 `0x80000000`，并让每个 hart，也就是每个 CPU 核，从这个地址开始执行。

这个地址由链接脚本决定：

```ld
ENTRY( _entry )
. = 0x80000000;
```

因此，xv6 的第一条内核代码是 `kernel/entry.S` 中的 `_entry`。

## 2. kernel/entry.S

### `_entry`

`_entry` 是 xv6 的汇编入口。它不负责完整初始化操作系统，只做一件必要的事：给当前 CPU 设置启动栈，然后跳到 C 函数 `start()`。

代码结构：

```asm
.section .text
_entry:
  la sp, stack0
  li a0, 1024*4
  csrr a1, mhartid
  addi a1, a1, 1
  mul a0, a0, a1
  add sp, sp, a0
  call start
spin:
  j spin
```

逐步解释：

```asm
la sp, stack0
```

把 `stack0` 的地址放进栈指针 `sp`。`stack0` 定义在 `kernel/start.c`：

```c
char stack0[4096 * NCPU];
```

这是一块启动阶段使用的栈空间，每个 CPU 分 4096 字节。

```asm
li a0, 1024*4
```

把 4096 放入 `a0`，表示每个 CPU 的启动栈大小。

```asm
csrr a1, mhartid
```

读取当前 CPU 的 hart id。`mhartid` 是 RISC-V 的 machine-mode CSR，每个 CPU 都有自己的值。

```asm
addi a1, a1, 1
mul a0, a0, a1
add sp, sp, a0
```

计算当前 CPU 的栈顶：

```text
sp = stack0 + 4096 * (mhartid + 1)
```

这里加 1 是因为栈向低地址增长。CPU 0 使用 `stack0[0..4095]`，所以它的栈顶应该是 `stack0 + 4096`。

```asm
call start
```

跳到 `kernel/start.c:start()`。此时 CPU 仍处于 machine mode。

```asm
spin:
  j spin
```

如果 `start()` 意外返回，就原地死循环。正常情况下不会走到这里，因为 `start()` 最后执行 `mret` 跳到 `main()`。

## 3. kernel/start.c 的全局对象

### `void main();`

声明 `main()`。`start()` 后面会把 `main` 的地址写入 `mepc`，让 `mret` 跳过去。

### `void timerinit();`

声明 `timerinit()`。`start()` 会调用它初始化当前 CPU 的 timer interrupt。

### `stack0`

```c
__attribute__ ((aligned (16))) char stack0[4096 * NCPU];
```

这是启动阶段每个 CPU 使用的栈。`entry.S` 会根据 `mhartid` 给每个 CPU 分一段。

`aligned(16)` 保证 16 字节对齐，符合 RISC-V 函数调用约定。

### `mscratch0`

```c
uint64 mscratch0[NCPU * 32];
```

这是 machine-mode timer interrupt 的 scratch 区。每个 CPU 分 32 个 `uint64`。

它不是寄存器，而是普通内存。为了避免多个 CPU 互相覆盖，xv6 用 `mhartid` 分区：

```text
hart 0 -> mscratch0[0..31]
hart 1 -> mscratch0[32..63]
hart 2 -> mscratch0[64..95]
```

每个 CPU 会把自己那一段 scratch 的地址写入自己的 `mscratch` CSR。

### `extern void timervec();`

声明汇编函数 `timervec()`。它定义在 `kernel/kernelvec.S`，是 machine-mode timer interrupt 的入口。

## 4. start()

`start()` 是 `entry.S` 之后执行的第一段 C 代码。它运行在 machine mode，目标是把 CPU 切到 supervisor mode，并为 timer interrupt 做基础配置。

### 设置 `mstatus.MPP`

```c
unsigned long x = r_mstatus();
x &= ~MSTATUS_MPP_MASK;
x |= MSTATUS_MPP_S;
w_mstatus(x);
```

`mstatus` 是 machine-mode 状态寄存器。`MPP` 字段决定执行 `mret` 后进入哪个特权级。

这里把 `MPP` 设置为 supervisor mode：

```text
mret 后进入 supervisor mode
```

### 设置 `mepc`

```c
w_mepc((uint64)main);
```

`mepc` 是 machine exception program counter。执行 `mret` 时，CPU 会跳到 `mepc` 保存的地址。

这里把 `mepc` 设置成 `main`：

```text
mret 后跳到 kernel/main.c:main()
```

### 暂时关闭分页

```c
w_satp(0);
```

`satp` 控制 supervisor mode 的地址翻译。这里清零表示暂时关闭分页。

原因是此时内核页表还没有建立。真正创建并开启内核页表是在 `main()` 里：

```c
kvminit();
kvminithart();
```

### 委托异常和中断

```c
w_medeleg(0xffff);
w_mideleg(0xffff);
w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);
```

`medeleg` 控制哪些异常从 machine mode 委托给 supervisor mode。

`mideleg` 控制哪些中断从 machine mode 委托给 supervisor mode。

xv6 主要运行在 supervisor mode，所以尽量把可委托的 trap 都交给 supervisor mode 处理。

`sie` 是 supervisor interrupt-enable 寄存器。这里打开：

```text
SIE_SEIE: supervisor external interrupt，外部中断
SIE_STIE: supervisor timer interrupt，定时器中断
SIE_SSIE: supervisor software interrupt，软件中断
```

### 初始化 timer interrupt

```c
timerinit();
```

为当前 CPU 配置 machine-mode timer interrupt。详细见下一节。

### 保存 hart id 到 `tp`

```c
int id = r_mhartid();
w_tp(id);
```

读取当前 CPU 的 `mhartid`，保存到 `tp` 寄存器。

xv6 后面用 `tp` 实现 `cpuid()`。这样进入 supervisor mode 后，不需要再直接读 machine-mode CSR。

### 执行 `mret`

```c
asm volatile("mret");
```

`mret` 是 machine-mode return 指令。它根据前面的设置完成两件事：

```text
pc = mepc = main
privilege mode = mstatus.MPP = supervisor
```

因此，`start()` 不会普通返回，而是通过 `mret` 切换到 supervisor mode 并开始执行 `main()`。

## 5. timerinit()

`timerinit()` 为当前 CPU 设置周期性 timer interrupt。每个 CPU 都会运行自己的 `timerinit()`。

### 获取当前 CPU id

```c
int id = r_mhartid();
```

读取当前 hart id。因为每个 CPU 都有自己的 timer interrupt 源，所以配置时必须区分 CPU。

### 设置下一次时钟中断时间

```c
int interval = 1000000;
*(uint64*)CLINT_MTIMECMP(id) = *(uint64*)CLINT_MTIME + interval;
```

CLINT 是 RISC-V 的 Core Local Interruptor，负责本地时钟中断。

`CLINT_MTIME` 是当前时间计数器。

`CLINT_MTIMECMP(id)` 是当前 CPU 的比较寄存器。当 `mtime >= mtimecmp` 时，硬件触发 timer interrupt。

这里的含义是：

```text
下一次 timer interrupt 时间 = 当前时间 + interval
```

### 准备 scratch 区

```c
uint64 *scratch = &mscratch0[32 * id];
scratch[4] = CLINT_MTIMECMP(id);
scratch[5] = interval;
w_mscratch((uint64)scratch);
```

`timervec` 是汇编代码，中断发生时它需要临时保存寄存器，还需要知道下一次 timer interrupt 应该写哪个 `mtimecmp`、加多少间隔。

所以 xv6 把这些信息放入 scratch 区：

```text
scratch[0..3]: timervec 保存寄存器用
scratch[4]: 当前 CPU 的 MTIMECMP 地址
scratch[5]: timer interrupt 间隔
```

`w_mscratch()` 把 scratch 地址写入当前 CPU 自己的 `mscratch` CSR。

注意：`mscratch` 是每个 hart 私有的 CSR，不会被其他 CPU 覆盖。但 `mscratch0` 是共享内存，所以必须按 `id` 分区。

### 设置 machine-mode trap 入口

```c
w_mtvec((uint64)timervec);
```

`mtvec` 是 machine-mode trap vector。这里把它设置为 `timervec`。

之后 machine-mode timer interrupt 发生时，CPU 会跳到 `kernel/kernelvec.S:timervec`。

### 打开 machine-mode timer interrupt

```c
w_mstatus(r_mstatus() | MSTATUS_MIE);
w_mie(r_mie() | MIE_MTIE);
```

`MSTATUS_MIE` 是 machine-mode 全局中断开关。

`MIE_MTIE` 是 machine timer interrupt 的单项开关。

两个都打开后，machine-mode timer interrupt 才能被接收。

### timer interrupt 的后续路径

timer interrupt 发生后，流程是：

```text
CLINT 触发 machine timer interrupt
-> CPU 跳到 kernelvec.S:timervec
-> timervec 安排下一次 timer interrupt
-> timervec 写 sip，触发 supervisor software interrupt
-> mret 返回
-> supervisor trap 路径接手
-> trap.c:devintr() 处理时钟事件
```

xv6 这样设计的原因是：硬件 timer interrupt 从 machine mode 来，但 xv6 内核主体运行在 supervisor mode，所以 `timervec` 做了一层中转。

## 6. mret 的作用

`mret` 不是普通函数返回。它是 RISC-V 的 machine-mode return 指令。

在 xv6 启动阶段，它的输入状态是：

```text
当前特权级: machine mode
mstatus.MPP: supervisor mode
mepc: main
```

执行后：

```text
当前特权级: supervisor mode
pc: main
```

如果用 `call main()`，只能跳转到 `main()`，不能降低特权级。因此 xv6 必须通过 `mret` 从 machine mode 进入 supervisor mode。

## 7. kernel/main.c

`main()` 是 xv6 内核主体初始化函数。`start()` 通过 `mret` 跳到这里后，CPU 已经处于 supervisor mode。

### `started`

```c
volatile static int started = 0;
```

`started` 用于多 CPU 启动同步。

CPU 0 负责做全局初始化。其他 CPU 先等待：

```c
while(started == 0)
  ;
```

等 CPU 0 初始化完成后设置 `started = 1`，其他 CPU 才继续执行自己的 per-hart 初始化。

`volatile` 防止编译器把循环优化成只读取一次。

### `main()`

`main()` 的逻辑分两条路径：

```text
CPU 0: 做完整系统初始化
其他 CPU: 等 CPU 0 完成后，只做本 CPU 必需的初始化
```

### `cpuid()`

```c
if(cpuid() == 0) {
```

`cpuid()` 读取 `tp` 寄存器。`tp` 已经在 `start()` 中被设置为当前 hart id。

CPU 0 是 boot CPU，负责初始化全局内核状态。

### `consoleinit()`

初始化控制台输入输出，包括 UART 和 console 相关锁。

它让内核能够通过 console 处理字符输入输出。

### `statsinit()`

当前仓库启用了 `LAB_PGTBL`，所以会调用 `statsinit()`。

它初始化 pgtbl/lock 实验用的 statistics 设备，使用户程序可以读取统计信息。

### `printfinit()`

初始化 `printf` 使用的锁。

xv6 多 CPU 并发运行时，多个 CPU 可能同时打印。`printfinit()` 准备锁，避免输出交错破坏。

### `printf()`

```c
printf("\n");
printf("xv6 kernel is booting\n");
printf("\n");
```

打印启动信息。此时 console 和 printf 已经初始化。

### `kinit()`

初始化物理页分配器。

xv6 后续需要动态分配物理页，例如页表页、用户内存页、内核栈等。`kinit()` 会把可用物理内存加入空闲链表。

### `kvminit()`

创建全局内核页表 `kernel_pagetable`。

它会映射：

```text
UART0
VIRTIO0
CLINT
PLIC
kernel text
kernel data
physical RAM
TRAMPOLINE
```

此时只是创建页表结构，还没有让 CPU 使用它。

### `kvminithart()`

把当前 CPU 的 `satp` 设置为内核页表，并执行 TLB 刷新。

从这里开始，当前 CPU 开启分页，地址访问走 xv6 建立的内核页表。

注意：`satp` 是 per-hart CSR，所以每个 CPU 都要单独调用一次 `kvminithart()`。

### `procinit()`

初始化进程表 `proc[NPROC]`。

它会：

```text
初始化每个 proc 的锁
为每个进程槽分配内核栈物理页
把内核栈映射到内核页表中的高地址 KSTACK
```

在当前 pgtbl 实验代码里，后续还会给每个进程建立自己的内核页表。

### `trapinit()`

初始化 trap 相关全局状态，主要是锁等全局资源。

trap 包括系统调用、异常、中断。

### `trapinithart()`

设置当前 CPU 的 supervisor trap vector。

它会让当前 CPU 的 supervisor-mode trap 跳到内核 trap 入口。

注意：`stvec` 是 per-hart CSR，所以每个 CPU 都要调用一次。

### `plicinit()`

初始化 PLIC，即 Platform-Level Interrupt Controller。

PLIC 负责外部设备中断，例如 VirtIO 磁盘中断、UART 中断等。

### `plicinithart()`

为当前 CPU 配置 PLIC，使当前 CPU 能接收外部设备中断。

这也是 per-hart 初始化，每个 CPU 都要执行。

### `binit()`

初始化 buffer cache。

buffer cache 是文件系统和磁盘之间的缓存层，缓存磁盘块，减少重复磁盘 I/O。

### `iinit()`

初始化 inode cache。

inode 是文件系统中文件和目录的元数据结构。`iinit()` 准备 inode 缓存和相关锁。

### `fileinit()`

初始化全局文件表。

xv6 中打开文件、管道等对象都通过 `struct file` 表示，`fileinit()` 初始化文件表锁和状态。

### `virtio_disk_init()`

初始化 VirtIO 磁盘设备。

QEMU 为 xv6 提供虚拟磁盘，xv6 通过 VirtIO 驱动和它交互。文件系统依赖这个磁盘设备。

### `userinit()`

创建第一个用户进程。

它会创建 `initcode` 进程，让 xv6 从内核初始化进入用户态程序执行。之后 `initcode` 会通过 `exec("/init")` 启动真正的 `user/init`。

### `__sync_synchronize()`

内存屏障。

CPU 0 在设置 `started = 1` 前，必须确保前面的初始化写入对其他 CPU 可见。

### `started = 1`

通知其他 CPU：全局初始化已经完成。

### 其他 CPU 的启动路径

非 CPU 0 的 hart 进入 `main()` 后，会先等待：

```c
while(started == 0)
  ;
```

等 CPU 0 完成全局初始化后，它们执行：

```c
kvminithart();
trapinithart();
plicinithart();
```

这些都是 per-hart 状态，因为相关寄存器或中断配置是每个 CPU 私有的。

### `scheduler()`

所有 CPU 最后都会进入 `scheduler()`。

`scheduler()` 不返回。它循环扫描进程表，寻找 `RUNNABLE` 进程并切换运行。

从这一刻开始，xv6 的启动阶段结束，进入正常的进程调度阶段。

## 8. CPU 0 和其他 CPU 的区别

CPU 0 做全局初始化：

```text
console
printf
物理内存分配器
全局内核页表
进程表
trap 全局状态
PLIC 全局状态
buffer cache
inode cache
文件表
磁盘
第一个用户进程
```

其他 CPU 只做本 CPU 必需的初始化：

```text
开启本 CPU 的分页
设置本 CPU 的 trap vector
配置本 CPU 接收设备中断
进入 scheduler
```

这样避免多个 CPU 同时初始化同一份全局数据。

## 9. 完整启动时序

```text
1. QEMU 加载 kernel/kernel 到 0x80000000
2. 每个 CPU 跳到 _entry
3. _entry 根据 mhartid 设置当前 CPU 的启动栈
4. _entry 调用 start()
5. start() 设置 mstatus.MPP = supervisor
6. start() 设置 mepc = main
7. start() 关闭分页
8. start() 委托异常和中断给 supervisor
9. start() 调用 timerinit()
10. timerinit() 设置 CLINT 下一次 timer interrupt
11. timerinit() 设置 mscratch 和 mtvec
12. timerinit() 打开 machine timer interrupt
13. start() 把 hartid 保存到 tp
14. start() 执行 mret
15. CPU 进入 supervisor mode 并跳到 main()
16. CPU 0 做全局初始化
17. CPU 0 创建第一个用户进程
18. CPU 0 设置 started = 1
19. 其他 CPU 结束等待，做 per-hart 初始化
20. 所有 CPU 进入 scheduler()
```

## 10. 阅读建议

读启动代码时建议按这个顺序追：

```text
kernel/kernel.ld
kernel/entry.S
kernel/start.c
kernel/main.c
kernel/kalloc.c
kernel/vm.c
kernel/proc.c
kernel/trap.c
```

理解重点：

```text
entry.S: 为什么 C 代码执行前必须先有栈
start.c: 为什么需要 mret 从 machine mode 切到 supervisor mode
timerinit: timer interrupt 为什么要经过 timervec 中转
main.c: 哪些初始化是全局的，哪些初始化是 per-hart 的
scheduler: 启动结束后 CPU 去哪里执行
```
