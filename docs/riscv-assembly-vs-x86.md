# RISC-V 汇编入门及与 x86 对比

本文面向 xv6/traps 实验，重点解释你读 xv6 汇编时最常遇到的内容：寄存器、调用约定、栈帧、访存、跳转、函数调用、CSR、trap 返回，并与 x86/x86-64 做对比。

## 一、整体风格差异

RISC-V 是典型 RISC 架构：

```text
指令简单
指令格式规整
大多数指令只操作寄存器
访存只能通过 load/store
```

x86 是典型 CISC 架构：

```text
指令复杂
寻址模式丰富
很多指令可以直接操作内存
历史兼容包袱较重
```

简单对比：

```text
RISC-V:
  add a0, a1, a2
  ld  a0, 0(sp)
  sd  a0, 8(sp)

x86-64:
  add rax, rbx
  mov rax, [rsp]
  mov [rsp+8], rax
```

RISC-V 中算术指令通常不能直接操作内存：

```asm
ld  t0, 0(sp)
add t0, t0, t1
sd  t0, 0(sp)
```

x86 可以更直接：

```asm
add QWORD PTR [rsp], rbx
```

## 二、寄存器对比

### RISC-V 常见寄存器

xv6 使用 RISC-V 64 位模式。常见通用寄存器如下：

```text
zero:
  永远是 0。

ra:
  return address，函数返回地址。

sp:
  stack pointer，栈指针。

gp:
  global pointer。

tp:
  thread pointer。xv6 中常用来保存 hartid。

t0-t6:
  临时寄存器，调用者保存。

s0-s11:
  saved registers，被调用者保存。
  s0 也常作为 frame pointer。

a0-a7:
  参数和返回值寄存器。
```

常见别名：

```text
x0  = zero
x1  = ra
x2  = sp
x8  = s0/fp
x10 = a0
x11 = a1
```

### x86-64 常见寄存器

```text
rax:
  返回值常用寄存器。

rsp:
  stack pointer。

rbp:
  frame pointer。

rdi, rsi, rdx, rcx, r8, r9:
  System V ABI 下前六个函数参数。

rbx, rbp, r12-r15:
  callee-saved。

rax, rcx, rdx, rsi, rdi, r8-r11:
  caller-saved。
```

### 参数传递对比

RISC-V：

```text
a0-a7 传前 8 个参数
a0/a1 也用于返回值
```

x86-64 System V：

```text
rdi, rsi, rdx, rcx, r8, r9 传前 6 个参数
rax 返回值
```

例子：

```c
int f(int x, int y);
```

RISC-V：

```text
x -> a0
y -> a1
返回值 -> a0
```

x86-64：

```text
x -> rdi
y -> rsi
返回值 -> rax
```

## 三、RISC-V 指令基本格式

### 算术指令

```asm
add rd, rs1, rs2
sub rd, rs1, rs2
mul rd, rs1, rs2
```

含义：

```text
rd = rs1 op rs2
```

例如：

```asm
add a0, a1, a2
```

表示：

```text
a0 = a1 + a2
```

x86-64 对比：

```asm
mov rax, rdi
add rax, rsi
```

x86 的 `add` 通常是二地址形式：

```text
目标寄存器同时也是一个源寄存器。
```

RISC-V 是三地址形式：

```text
目标和两个源分开写。
```

### 立即数指令

RISC-V：

```asm
addi rd, rs1, imm
```

例子：

```asm
addi sp, sp, -16
```

含义：

```text
sp = sp - 16
```

常见于函数开头分配栈帧。

x86-64：

```asm
sub rsp, 16
```

## 四、访存：load/store

RISC-V 只能通过 load/store 访问内存。

常见 load：

```asm
lb   rd, offset(rs1)   # load byte
lbu  rd, offset(rs1)   # load byte unsigned
lh   rd, offset(rs1)   # load half word
lw   rd, offset(rs1)   # load word, 32-bit
ld   rd, offset(rs1)   # load double word, 64-bit
```

常见 store：

```asm
sb   rs2, offset(rs1)
sh   rs2, offset(rs1)
sw   rs2, offset(rs1)
sd   rs2, offset(rs1)
```

例子：

```asm
ld a0, 8(sp)
```

含义：

```text
a0 = *(uint64 *)(sp + 8)
```

```asm
sd ra, 8(sp)
```

含义：

```text
*(uint64 *)(sp + 8) = ra
```

x86-64 对比：

```asm
mov rax, [rsp+8]
mov [rsp+8], rax
```

## 五、栈和函数调用

### RISC-V 栈增长方向

和 x86 一样，RISC-V 栈通常向低地址增长。

函数进入时常见：

```asm
addi sp, sp, -16
sd   ra, 8(sp)
sd   s0, 0(sp)
addi s0, sp, 16
```

含义：

```text
1. sp -= 16，分配栈帧。
2. 保存返回地址 ra。
3. 保存旧 frame pointer s0。
4. 设置新的 frame pointer。
```

函数返回前：

```asm
ld   ra, 8(sp)
ld   s0, 0(sp)
addi sp, sp, 16
ret
```

含义：

```text
1. 恢复 ra。
2. 恢复 s0。
3. 释放栈帧。
4. 跳回 ra。
```

### x86-64 栈帧对比

x86-64 常见函数序言：

```asm
push rbp
mov  rbp, rsp
sub  rsp, 16
```

函数返回：

```asm
leave
ret
```

或者：

```asm
mov rsp, rbp
pop rbp
ret
```

RISC-V 没有专门的 `push/pop` 指令，使用：

```asm
addi sp, sp, -8
sd   reg, 0(sp)
```

来模拟 push。

## 六、函数调用与返回

### RISC-V

函数调用：

```asm
call func
```

本质上会把返回地址保存到：

```text
ra
```

然后跳转到 `func`。

返回：

```asm
ret
```

等价于：

```asm
jalr zero, 0(ra)
```

也就是跳回 `ra`。

### x86-64

函数调用：

```asm
call func
```

x86 的 `call` 会把返回地址压入栈：

```text
push return_address
jump func
```

返回：

```asm
ret
```

会从栈上弹出返回地址并跳转。

### 关键差异

RISC-V：

```text
返回地址默认在 ra 寄存器里。
如果当前函数还要调用别的函数，通常要先把 ra 保存到栈上。
```

x86：

```text
返回地址天然在栈上。
```

这对 xv6 的 backtrace 很重要。

RISC-V 函数栈帧中通常保存：

```text
上一个 s0/fp
当前函数返回地址 ra
```

所以可以沿着 frame pointer 找调用链。

## 七、跳转和分支

### 无条件跳转

RISC-V：

```asm
j label
```

或者：

```asm
jal zero, label
```

x86：

```asm
jmp label
```

### 条件分支

RISC-V 常见：

```asm
beq  rs1, rs2, label
bne  rs1, rs2, label
blt  rs1, rs2, label
bge  rs1, rs2, label
bltu rs1, rs2, label
bgeu rs1, rs2, label
```

例子：

```asm
beq a0, zero, done
```

含义：

```text
if(a0 == 0) goto done;
```

x86 对比：

```asm
cmp rax, 0
je done
```

RISC-V 条件跳转通常直接比较两个寄存器。

x86 通常先 `cmp/test` 设置 flags，再根据 flags 跳转。

## 八、常见伪指令

RISC-V 汇编里经常看到伪指令。它们不是硬件真实指令，而是汇编器展开出来的。

```asm
mv rd, rs
```

等价于：

```asm
addi rd, rs, 0
```

```asm
li rd, imm
```

加载立即数。可能展开成一条或多条指令。

```asm
ret
```

等价于：

```asm
jalr zero, 0(ra)
```

```asm
nop
```

等价于：

```asm
addi zero, zero, 0
```

## 九、CSR 和 trap 相关指令

xv6 traps 实验会经常看到 CSR。

CSR 是 Control and Status Register，即控制状态寄存器。

常见 CSR：

```text
sstatus:
  supervisor 状态寄存器。

sepc:
  trap 发生时保存的用户/内核 PC。

scause:
  trap 原因。

stval:
  trap 相关地址或值。

stvec:
  trap 入口地址。

satp:
  当前页表根地址。

sscratch:
  trap 入口时临时保存值，xv6 用它辅助保存用户 a0 和 trapframe。
```

常见 CSR 指令：

```asm
csrr rd, csr
```

读 CSR：

```text
rd = csr
```

```asm
csrw csr, rs
```

写 CSR：

```text
csr = rs
```

```asm
csrrw rd, csr, rs
```

交换：

```text
tmp = csr
csr = rs
rd = tmp
```

在 `trampoline.S` 中常见：

```asm
csrrw a0, sscratch, a0
```

用于在用户寄存器和内核保存区域之间交换值。

### x86 对比

x86 也有控制寄存器和特殊寄存器，例如：

```text
cr3:
  页表根地址，类似 RISC-V satp。

rflags:
  标志寄存器，部分作用类似状态寄存器。

idtr:
  中断描述符表寄存器。
```

x86 切页表常见：

```asm
mov cr3, rax
```

RISC-V：

```asm
csrw satp, t1
sfence.vma zero, zero
```

## 十、trap 返回：sret 与 x86 iret

RISC-V 从 supervisor trap 返回用户态：

```asm
sret
```

它会根据：

```text
sepc:
  返回地址

sstatus.SPP:
  返回到 U-mode 还是 S-mode

sstatus.SPIE:
  返回后是否开中断
```

x86 中类似的是：

```asm
iret
```

或 x86-64 的：

```asm
iretq
```

它会从栈中恢复：

```text
RIP
CS
RFLAGS
RSP
SS
```

### 关键差异

RISC-V 的 trap 返回状态主要在 CSR 中：

```text
sepc
sstatus
satp
```

x86 的中断返回更多依赖硬件压栈和 `iretq` 恢复。

xv6 RISC-V 为了从用户态 trap 到内核态，需要手动用 `trampoline.S` 保存寄存器。

## 十一、xv6 中的 trampoline 读法

`kernel/trampoline.S` 是 traps 实验必须读的文件。

用户态进入内核的大致流程：

```text
用户态发生 syscall/interrupt/exception
CPU 跳到 stvec 指向的 uservec
uservec 保存用户寄存器到 TRAPFRAME
切换 satp 到内核页表
跳到 usertrap()
```

返回用户态：

```text
usertrapret()
设置 sepc/sstatus/stvec/trapframe
调用 userret(TRAPFRAME, user pagetable)
userret 切换到用户页表
恢复用户寄存器
sret 回用户态
```

读 `trampoline.S` 时重点看：

```text
1. a0 如何和 sscratch 交换。
2. 用户寄存器如何保存到 TRAPFRAME。
3. satp 什么时候从用户页表切到内核页表。
4. userret 什么时候从内核页表切回用户页表。
5. sret 之前恢复了哪些寄存器。
```

## 十二、xv6 backtrace 需要的汇编知识

traps 实验的 backtrace 依赖栈帧。

RISC-V 函数栈帧通常长这样：

```text
高地址
  saved ra
  saved old s0/fp
低地址
```

xv6 中可以通过当前 frame pointer：

```c
uint64 fp = r_fp();
```

沿着栈帧找：

```text
return address
previous frame pointer
```

常见约定是：

```text
return address 在 fp - 8
previous fp 在 fp - 16
```

所以 backtrace 大致逻辑：

```c
while(fp 在当前内核栈页内){
  ra = *(uint64 *)(fp - 8);
  printf("%p\n", ra);
  fp = *(uint64 *)(fp - 16);
}
```

这和 x86 用 `rbp` 链回溯调用栈很像。

## 十三、常见 RISC-V 指令速查

```text
add rd, rs1, rs2:
  rd = rs1 + rs2

addi rd, rs1, imm:
  rd = rs1 + imm

sub rd, rs1, rs2:
  rd = rs1 - rs2

ld rd, off(rs1):
  rd = *(uint64 *)(rs1 + off)

sd rs2, off(rs1):
  *(uint64 *)(rs1 + off) = rs2

beq rs1, rs2, label:
  if(rs1 == rs2) goto label

bne rs1, rs2, label:
  if(rs1 != rs2) goto label

jal ra, label:
  ra = next_pc; pc = label

jalr rd, off(rs1):
  rd = next_pc; pc = rs1 + off

csrr rd, csr:
  rd = csr

csrw csr, rs:
  csr = rs

sfence.vma:
  刷新地址翻译缓存 TLB

sret:
  从 supervisor trap 返回
```

## 十四、学习建议

读 xv6 RISC-V 汇编时，不要试图一次看懂所有指令。建议按这个顺序：

```text
1. 先看函数序言和返回：
   addi sp
   sd ra
   sd s0
   ld ra
   ld s0
   ret

2. 再看参数和返回值：
   a0-a7

3. 再看访存：
   ld/sd offset(register)

4. 再看跳转：
   beq/bne/j/call/ret

5. 最后看 trap 相关：
   csrr/csrw/csrrw/sret/sfence.vma
```

对 xv6 traps 实验来说，最重要的不是记住所有指令，而是理解：

```text
函数调用如何保存返回地址
栈帧如何串起来
trap 如何保存用户寄存器
satp 如何切换页表
sret 如何回到用户态
```
