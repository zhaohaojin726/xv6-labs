# traps 实验过程与当前实现记录

本文档整理 traps 实验中的 trap 总流程，以及当前代码已经完成的工作。重点记录系统调用、backtrace、alarm 的实现思路、当前代码如何工作，以及目前还需要检查的问题。

## 一、trap 总流程

trap 指用户程序或内核执行过程中，因为系统调用、异常或中断而进入内核处理逻辑。

常见来源：

```text
系统调用:
  用户程序执行 ecall。

异常:
  page fault、非法指令等。

中断:
  timer interrupt、设备中断等。
```

用户态 trap 的总体路径是：

```text
user mode
-> ecall / exception / interrupt
-> trampoline.S:uservec
-> 保存用户寄存器到 p->trapframe
-> 切换到内核页表和内核栈
-> kernel/trap.c:usertrap()
-> usertrapret()
-> trampoline.S:userret
-> 恢复用户寄存器
-> sret 返回用户态
```

## 二、用户态进入内核

用户态发生 trap 时，硬件自动做的事很少：

```text
1. 保存当前 PC 到 sepc。
2. 设置 scause 表示 trap 原因。
3. 设置 stval 保存 fault 地址等附加信息。
4. 切换到 supervisor mode。
5. 跳到 stvec 指向的地址。
```

硬件不会自动：

```text
保存所有通用寄存器
切换内核页表
切换内核栈
调用 C 函数
```

所以 xv6 需要 `trampoline.S:uservec` 完成这些事情。

### `sscratch` 的作用

用户态正常运行时：

```text
sscratch = TRAPFRAME
```

进入 `uservec` 后第一条关键指令：

```asm
csrrw a0, sscratch, a0
```

交换前：

```text
a0       = 用户程序自己的 a0
sscratch = TRAPFRAME
```

交换后：

```text
a0       = TRAPFRAME
sscratch = 用户程序自己的 a0
```

这样 `uservec` 就能用 `a0` 作为 trapframe 地址，保存所有用户寄存器，同时不丢失用户原来的 `a0`。

## 三、`usertrap()` 的职责

当前 `usertrap()` 位于：

```text
kernel/trap.c
```

主要流程：

```c
if((r_sstatus() & SSTATUS_SPP) != 0)
  panic("usertrap: not from user mode");

w_stvec((uint64)kernelvec);

struct proc *p = myproc();
p->trapframe->epc = r_sepc();
```

含义：

```text
1. 确认 trap 来自用户态。
2. 进入内核后，把 stvec 改成 kernelvec。
3. 保存用户态被打断时的 PC 到 p->trapframe->epc。
```

### 系统调用路径

```c
if(r_scause() == 8){
  if(p->killed)
    exit(-1);

  p->trapframe->epc += 4;
  intr_on();
  syscall();
}
```

`scause == 8` 表示 user mode `ecall`。

`epc += 4` 的原因：

```text
sepc 指向 ecall 指令本身。
如果不加 4，返回用户态会再次执行 ecall，形成死循环。
```

### 设备中断路径

```c
} else if((which_dev = devintr()) != 0){
  // ok
}
```

`devintr()` 返回值中：

```text
which_dev == 2:
  timer interrupt
```

alarm 实验就是在 timer interrupt 路径里插入逻辑。

### 异常路径

```c
else {
  printf("usertrap(): unexpected scause ...");
  p->killed = 1;
}
```

如果用户态发生 xv6 不处理的异常，就杀掉当前进程。

用户异常不应该导致内核 panic。

## 四、返回用户态

`usertrapret()` 做返回用户态前的准备：

```text
1. 关闭中断。
2. 设置 stvec 指向 uservec。
3. 设置 trapframe 中内核恢复信息。
4. 设置 sstatus，使 sret 返回 user mode。
5. 设置 sepc 为 p->trapframe->epc。
6. 调用 trampoline.S:userret。
```

`userret` 做：

```text
1. 切换到用户页表。
2. 恢复用户寄存器。
3. 最后用 csrrw 恢复用户 a0，并把 TRAPFRAME 放回 sscratch。
4. sret 返回用户态。
```

关键点：

```text
返回用户态时真正跳到哪里，由 p->trapframe->epc 决定。
```

alarm 实验正是利用这一点，把：

```c
p->trapframe->epc
```

改成用户 handler 地址。

## 五、backtrace 实现

### 目标

backtrace 要打印当前内核调用栈中的返回地址。

评分脚本期望 `bttest` 输出的地址能对应到：

```text
sysproc.c
syscall.c
trap.c
```

### 当前实现

当前代码中已有：

```text
kernel/printf.c: backtrace()
kernel/defs.h: void backtrace();
kernel/sysproc.c: sys_sleep() 中调用 backtrace()
kernel/riscv.h: r_fp()
```

`r_fp()` 的作用是读取当前 `s0/fp`：

```c
static inline uint64
r_fp()
{
  uint64 x;
  asm volatile("mv %0, s0" : "=r" (x) );
  return x;
}
```

RISC-V 中 `s0` 也叫 `fp`，即 frame pointer。

常见栈帧布局：

```text
fp - 8:
  保存的返回地址 ra

fp - 16:
  上一层函数的 fp
```

backtrace 的核心逻辑是：

```text
1. 从当前 fp 开始。
2. 读取 *(fp - 8) 打印返回地址。
3. 读取 *(fp - 16) 跳到上一层栈帧。
4. 当 fp 跳出当前内核栈页时停止。
```

判断结束的方式：

```text
当前 fp 不在当前 kernel stack 页内时停止。
```

常见边界：

```c
uint64 bottom = PGROUNDDOWN(fp);
uint64 top = PGROUNDUP(fp);
```

## 六、添加系统调用的工作

alarm 实验需要添加：

```c
sigalarm(interval, handler)
sigreturn()
```

当前已经改动：

### `user/user.h`

新增用户态声明：

```c
int sigalarm(int, void (*handler)());
int sigreturn(void);
```

### `user/usys.pl`

新增 syscall stub：

```perl
entry("sigalarm");
entry("sigreturn");
```

这会生成用户态汇编入口，大致效果是：

```text
a7 = SYS_sigalarm 或 SYS_sigreturn
ecall
ret
```

### `kernel/syscall.h`

新增系统调用号：

```c
#define SYS_sigalarm 22
#define SYS_sigreturn 23
```

### `kernel/syscall.c`

新增内核函数声明和分发表项：

```c
extern uint64 sys_sigalarm(void);
extern uint64 sys_sigreturn(void);

[SYS_sigalarm]  sys_sigalarm,
[SYS_sigreturn] sys_sigreturn
```

系统调用完整路径：

```text
用户 sigalarm(...)
-> user/usys.S
-> ecall
-> usertrap()
-> syscall()
-> sys_sigalarm()
```

## 七、alarm 的进程状态

当前在 `struct proc` 中新增字段：

```c
uint64 alarm_interval;
uint64 ticks_cnt;
uint64* handler;
int alarm_running;
struct trapframe alarm_trapframe;
```

含义：

```text
alarm_interval:
  每隔多少 timer tick 触发一次 alarm。

ticks_cnt:
  当前已经累计了多少 timer tick。

handler:
  用户传入的 handler 函数地址。

alarm_running:
  当前是否已经在执行 alarm handler。
  用来防止 handler 重入。

alarm_trapframe:
  保存 timer interrupt 打断前的完整用户寄存器状态。
```

建议后续把：

```c
uint64* handler;
```

改成：

```c
uint64 handler;
```

因为 handler 本质上是一个用户虚拟地址，不是内核要解引用的指针。

## 八、`sys_sigalarm()`

当前实现位置：

```text
kernel/sysproc.c
```

当前逻辑：

```c
uint64 
sys_sigalarm(void)
{
  int t;
  uint64 h;
  if(argint(0, &t) < 0)
    return -1;
  if(argaddr(1, &h) < 0)
    return -1;
  struct proc* p = myproc();
  p->alarm_interval = t;
  p->handler = (uint64*)h;
  p->ticks_cnt = 0;
  p->alarm_running = 0;
  return 0;
}
```

参数来源：

```text
sigalarm(interval, handler)

interval:
  用户第 0 个参数，来自 a0，由 argint(0, &t) 获取。

handler:
  用户第 1 个参数，来自 a1，由 argaddr(1, &h) 获取。
```

注意：

`handler` 是用户空间虚拟地址。内核不需要把它转换成物理地址，也不应该在内核中直接调用它。

后续触发 alarm 时，内核只是设置：

```c
p->trapframe->epc = handler;
```

真正执行 handler 时，CPU 已经通过 `sret` 回到用户态，并使用用户页表翻译该地址。

## 九、timer interrupt 中触发 alarm

当前在 `usertrap()` 的 timer interrupt 路径中加入了：

```c
if(which_dev == 2){
  if(p->alarm_interval>0 && !p->alarm_running){
    p->ticks_cnt ++;
    if(p->ticks_cnt >= p->alarm_interval){
      p->alarm_trapframe = *p->trapframe;
      p->trapframe->epc = (uint64)p->handler;
      p->alarm_running = 1;
      p->ticks_cnt = 0;
    }
  }
  yield();
}
```

含义：

```text
1. 只有 timer interrupt 才累计 ticks_cnt。
2. alarm_interval > 0 表示 alarm 已启用。
3. alarm_running == 0 表示当前不在 handler 中。
4. 达到 interval 后，保存当前 trapframe。
5. 把 epc 改成 handler 地址。
6. 设置 alarm_running，防止重入。
7. 清零 ticks_cnt，开始下一轮计数。
```

为什么改 `epc` 就能跳转：

```text
usertrapret() 会把 p->trapframe->epc 写入 sepc。
sret 返回用户态时会跳到 sepc。
```

所以修改 `p->trapframe->epc` 就能让用户态从 handler 开始执行。

## 十、`sys_sigreturn()`

handler 执行结束后，测试代码会主动调用：

```c
sigreturn();
```

例如：

```c
void periodic()
{
  count = count + 1;
  printf("alarm!\n");
  sigreturn();
}
```

当前实现：

```c
uint64
sys_sigreturn(void)
{
  struct proc *p = myproc();
  if(p->alarm_running == 0)
    return 0;
  *p->trapframe = p->alarm_trapframe;
  p->alarm_running = 0;
  return 0;
}
```

作用：

```text
1. 用 alarm_trapframe 恢复被 timer interrupt 打断前的用户寄存器。
2. 清除 alarm_running，允许下一次 alarm。
3. 返回用户态后，继续执行原来被打断的位置。
```

需要注意：

系统调用返回值通常会写入 `trapframe->a0`。为了不破坏被恢复的用户 `a0`，更稳妥的写法通常是：

```c
*p->trapframe = p->alarm_trapframe;
p->alarm_running = 0;
return p->trapframe->a0;
```

当前 `return 0` 可能会影响 test1 中对寄存器恢复的检查，后续建议调整。

## 十一、alarmtest 的三个测试含义

### test0

目标：

```text
确认 handler 至少会被调用一次。
```

只要 timer interrupt 达到 interval 后能改 `epc` 跳到 handler，test0 基本可以通过。

### test1

目标：

```text
确认 handler 可以被多次调用。
确认 handler 返回后，原程序寄存器状态完全恢复。
```

关键要求：

```text
必须保存并恢复完整 trapframe。
```

否则用户循环变量、函数参数、返回地址等可能被 handler 破坏。

### test2

目标：

```text
确认 handler 不会重入。
```

也就是说，handler 还没执行完时，即使又来了 timer interrupt，也不能再次跳到 handler。

当前通过：

```c
alarm_running
```

避免重入。

## 十二、当前 make grade 问题

当前已经修复：

```text
gradelib.py 中 from pipes import quote 在新 Python 中失败的问题。
```

修复方式：

```python
try:
    from shlex import quote
except ImportError:
    from pipes import quote
```

当前 `make grade` 仍有两个非实验逻辑问题：

```text
1. 缺少 answers-traps.txt。
2. qemu-gdb 需要创建 tcp::26000，本环境 sandbox 不允许，需要提升权限运行 make grade。
```

错误表现：

```text
answers-traps.txt: Cannot read answers-traps.txt

qemu-system-riscv64:
  Failed to create a socket: Operation not permitted
```

## 十三、当前仍需检查的代码点

### 1. `sys_sigreturn()` 返回值

当前：

```c
return 0;
```

建议改为恢复后返回原 `a0`：

```c
return p->trapframe->a0;
```

原因：

避免系统调用返回值覆盖被恢复的用户寄存器状态。

### 2. `handler` 字段类型

当前：

```c
uint64* handler;
```

建议：

```c
uint64 handler;
```

原因：

handler 是用户虚拟地址。内核只保存并写入 `epc`，不解引用它。

### 3. `sigalarm(0, 0)` 的行为

当前 `sys_sigalarm()` 会：

```c
p->alarm_interval = t;
p->handler = (uint64*)h;
p->ticks_cnt = 0;
p->alarm_running = 0;
```

这可以关闭 alarm，因为 `alarm_interval == 0` 后 `usertrap()` 不再触发。

### 4. `proc` 初始化

需要确认 `allocproc()` 或进程初始化路径中清零了新增字段：

```text
alarm_interval
ticks_cnt
handler
alarm_running
alarm_trapframe
```

如果 `struct proc` 复用旧槽位，字段没初始化可能导致随机 alarm 行为。

## 十四、整体总结

trap 实验中的核心理解是：

```text
trap 进入内核时，硬件只保存少量 CSR。
xv6 通过 trampoline.S 保存完整用户寄存器。
用户态返回位置由 trapframe->epc 决定。
```

alarm 的实现正是利用：

```c
p->trapframe->epc = handler;
```

把 timer interrupt 之后的用户态返回点改成用户 handler。

handler 调用 `sigreturn()` 后，内核恢复之前保存的：

```c
p->alarm_trapframe
```

从而让用户程序继续从被 timer interrupt 打断的位置执行。

当前已经完成的主要工作：

```text
1. 添加 sigalarm/sigreturn 系统调用链路。
2. 在 proc 中保存 alarm 状态。
3. 在 timer interrupt 中统计 tick 并跳转 handler。
4. 在 sigreturn 中恢复 trapframe。
5. 实现 backtrace 相关逻辑。
6. 修复 gradelib.py 的 Python 兼容问题。
```

下一步建议：

```text
1. 创建 answers-traps.txt。
2. 调整 sys_sigreturn 返回值。
3. 确认 alarm 字段初始化。
4. 用提升权限运行 make grade。
5. 根据 alarmtest/backtrace 输出继续修正。
```
