# E1000 详解：xv6 net 实验中的网卡驱动

本文解释 xv6 net 实验里的 E1000：它是什么、xv6 如何发现和初始化它、驱动如何通过 descriptor ring 与它通信，以及 `e1000_transmit()` 和 `e1000_recv()` 应该完成什么工作。

相关文件：

```text
kernel/e1000.c       E1000 驱动主体，实验主要修改这里
kernel/e1000_dev.h   E1000 寄存器、descriptor、状态位定义
kernel/net.c         上层 Ethernet/IP/UDP/ARP 协议栈
kernel/net.h         mbuf、网络头部结构、字节序工具
kernel/pci.c         PCI 扫描，发现 E1000 并调用 e1000_init
kernel/trap.c        接收 E1000 中断并调用 e1000_intr
```

## 一、E1000 是什么

E1000 是 Intel 的一类以太网网卡。在 xv6 net 实验中，E1000 不是你电脑上的真实硬件，而是 QEMU 模拟出来的网卡。

对 xv6 来说，它看起来仍然像真实硬件：

```text
有 PCI 设备信息
有 MMIO 寄存器
有中断
有 DMA
有发送 descriptor ring
有接收 descriptor ring
```

你的驱动 `kernel/e1000.c` 不需要知道它是模拟的。驱动只要按照 E1000 设备模型操作寄存器和 descriptor ring，QEMU 就会模拟真实网卡行为。

## 二、E1000 在整个网络栈中的位置

发送路径：

```text
user write()
  -> sys_write
  -> filewrite
  -> sockwrite
  -> net_tx_udp
  -> net_tx_ip
  -> net_tx_eth
  -> e1000_transmit
  -> E1000
  -> QEMU
  -> host
```

接收路径：

```text
host
  -> QEMU
  -> E1000
  -> E1000 interrupt
  -> e1000_intr
  -> e1000_recv
  -> net_rx
  -> net_rx_ip / net_rx_arp
  -> net_rx_udp
  -> sockrecvudp
  -> sockread
  -> user read()
```

因此，E1000 驱动处在最底层：

```text
上层网络栈处理协议。
E1000 驱动处理硬件收发。
```

上层 `net.c` 已经能构造 Ethernet/IP/UDP 包，也能解析收到的包。你主要要补的是：

```text
如何把 mbuf 交给 E1000 发送
如何从 E1000 取出收到的 packet 并交给 net_rx
```

## 三、MMIO 寄存器

E1000 通过 MMIO 暴露寄存器。MMIO 的意思是：

```text
把设备寄存器映射到内存地址空间。
CPU 读写某些地址，本质上是在读写设备寄存器。
```

在 `kernel/e1000.c` 中：

```c
static volatile uint32 *regs;
```

`regs` 指向 E1000 的寄存器区域。`volatile` 很重要，因为这些值可能被硬件改变，编译器不能随便缓存或优化掉访问。

`e1000_init()` 接收：

```c
void e1000_init(uint32 *xregs)
```

其中 `xregs` 是 `pci_init()` 发现 E1000 后传进来的 MMIO 基地址。

```c
regs = xregs;
```

之后驱动通过：

```c
regs[E1000_TDT] = ...
regs[E1000_RDT] = ...
```

来操作网卡。

## 四、关键寄存器

定义在 `kernel/e1000_dev.h`。

### 1. 控制寄存器

```c
#define E1000_CTL  (0x00000/4)
```

用于设备控制。例如 reset：

```c
regs[E1000_CTL] |= E1000_CTL_RST;
```

### 2. 中断寄存器

```c
#define E1000_ICR  (0x000C0/4)
#define E1000_IMS  (0x000D0/4)
```

含义：

```text
ICR：Interrupt Cause Read，读取/确认中断原因
IMS：Interrupt Mask Set，启用哪些中断
```

当前代码：

```c
regs[E1000_IMS] = (1 << 7);
```

开启 RXDW 中断，也就是接收 descriptor 被写回时触发中断。

中断处理里：

```c
regs[E1000_ICR] = 0xffffffff;
```

表示告诉设备：这个中断我已经处理了。

### 3. 发送寄存器

```c
#define E1000_TDBAL  (0x03800/4)
#define E1000_TDLEN  (0x03808/4)
#define E1000_TDH    (0x03810/4)
#define E1000_TDT    (0x03818/4)
#define E1000_TCTL   (0x00400/4)
#define E1000_TIPG   (0x00410/4)
```

含义：

```text
TDBAL：TX descriptor ring 的基地址
TDLEN：TX descriptor ring 的总大小
TDH：TX head，硬件处理到哪里
TDT：TX tail，驱动提交到哪里
TCTL：发送控制
TIPG：发送包之间的间隔
```

驱动最常操作的是：

```text
E1000_TDT
```

因为发送新包时，驱动填好 descriptor 后，要推进 TDT 通知硬件：

```text
新的 descriptor 可用了，你可以发送。
```

### 4. 接收寄存器

```c
#define E1000_RDBAL  (0x02800/4)
#define E1000_RDLEN  (0x02808/4)
#define E1000_RDH    (0x02810/4)
#define E1000_RDT    (0x02818/4)
#define E1000_RCTL   (0x00100/4)
```

含义：

```text
RDBAL：RX descriptor ring 的基地址
RDLEN：RX descriptor ring 的总大小
RDH：RX head，硬件接收处理到哪里
RDT：RX tail，驱动告诉硬件哪些 descriptor 可用
RCTL：接收控制
```

驱动最常操作的是：

```text
E1000_RDT
```

接收时，驱动处理完一个 descriptor，并补上新的空 buffer 后，要推进 RDT，把这个 descriptor 重新交给硬件使用。

## 五、descriptor ring 是什么

网卡和驱动之间不能直接传 C 结构体指针语义。它们通过一段共享内存通信，这段共享内存就是 descriptor ring。

ring 的意思是环形数组：

```text
0 -> 1 -> 2 -> ... -> 15 -> 0 -> 1 -> ...
```

当前 xv6 中：

```c
#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];
```

有两类数组：

```text
tx_ring / rx_ring：
  给硬件看的 descriptor 数组。

tx_mbufs / rx_mbufs：
  给内核自己看的 mbuf 指针数组。
```

为什么需要 `tx_mbufs` / `rx_mbufs`？

```text
descriptor 里主要存物理地址、长度、状态位。
驱动还需要知道这个 descriptor 对应哪个 struct mbuf。
所以额外用 tx_mbufs/rx_mbufs 记录 C 指针。
```

## 六、发送 descriptor：tx_desc

定义：

```c
struct tx_desc
{
  uint64 addr;
  uint16 length;
  uint8 cso;
  uint8 cmd;
  uint8 status;
  uint8 css;
  uint16 special;
};
```

本实验最重要字段：

```text
addr：
  要发送的数据 buffer 地址，也就是 m->head。

length：
  要发送的数据长度，也就是 m->len。

cmd：
  发送命令。

status：
  硬件处理状态。
```

关键命令位：

```c
#define E1000_TXD_CMD_EOP 0x01
#define E1000_TXD_CMD_RS  0x08
```

含义：

```text
EOP：End Of Packet。
  表示这个 descriptor 包含完整 packet 的最后一段。
  xv6 每个 packet 都只用一个 descriptor，所以必须设置。

RS：Report Status。
  要求硬件发送完成后写回 status。
  这样驱动之后可以通过 DD 位判断 descriptor 是否可复用。
```

关键状态位：

```c
#define E1000_TXD_STAT_DD 0x00000001
```

含义：

```text
DD：Descriptor Done。
  硬件已经处理完这个 descriptor。
  驱动可以安全复用这个槽位。
```

## 七、接收 descriptor：rx_desc

定义：

```c
struct rx_desc
{
  uint64 addr;
  uint16 length;
  uint16 csum;
  uint8 status;
  uint8 errors;
  uint16 special;
};
```

本实验最重要字段：

```text
addr：
  接收 buffer 地址。
  驱动初始化时把 rx_mbufs[i]->head 放进去。

length：
  硬件收到 packet 后写入实际长度。

status：
  硬件写入接收状态。
```

关键状态位：

```c
#define E1000_RXD_STAT_DD  0x01
#define E1000_RXD_STAT_EOP 0x02
```

含义：

```text
DD：
  Descriptor Done。
  表示硬件已经把 packet 写入这个 buffer。

EOP：
  End Of Packet。
  表示这个 descriptor 包含一个完整 packet 的最后一段。
```

xv6 的 buffer 大小是 2048：

```c
#define MBUF_SIZE 2048
```

普通以太网包最大约 1518 字节，所以一个 `mbuf` 足够装下完整 packet。因此实验里通常期望：

```text
每个收到的 packet 占一个 RX descriptor。
```

## 八、mbuf 和 E1000 的关系

`mbuf` 是 xv6 网络栈里的 packet buffer：

```c
struct mbuf {
  struct mbuf  *next;
  char         *head;
  unsigned int len;
  char         buf[MBUF_SIZE];
};
```

发送时：

```text
net.c 已经构造好完整 Ethernet frame。
mbuf->head 指向 Ethernet header 开始位置。
mbuf->len 是整个 frame 长度。
e1000_transmit 把 m->head 和 m->len 填进 tx_desc。
```

接收时：

```text
e1000_init 预先为每个 rx_desc 分配一个空 mbuf。
rx_desc.addr 指向 mbuf->head。
E1000 收包后把 packet DMA 到这个地址。
e1000_recv 设置 mbuf->len，然后把 mbuf 交给 net_rx。
```

## 九、e1000_init 做了什么

`e1000_init()` 由 `pci_init()` 调用。

### 1. 保存寄存器基址

```c
regs = xregs;
```

之后所有 `regs[...]` 都是对 E1000 MMIO 寄存器的访问。

### 2. reset 设备

```c
regs[E1000_IMS] = 0;
regs[E1000_CTL] |= E1000_CTL_RST;
regs[E1000_IMS] = 0;
__sync_synchronize();
```

含义：

```text
先关中断
复位设备
再次关中断
做一次内存屏障
```

`__sync_synchronize()` 是内存屏障，防止编译器或 CPU 把前后的内存访问乱序。

### 3. 初始化 TX ring

```c
memset(tx_ring, 0, sizeof(tx_ring));
for (i = 0; i < TX_RING_SIZE; i++) {
  tx_ring[i].status = E1000_TXD_STAT_DD;
  tx_mbufs[i] = 0;
}
regs[E1000_TDBAL] = (uint64) tx_ring;
regs[E1000_TDLEN] = sizeof(tx_ring);
regs[E1000_TDH] = regs[E1000_TDT] = 0;
```

重点：

```text
TX descriptor 初始都标记为 DD。
表示这些槽位一开始都是空闲可用的。
TDBAL 告诉硬件 TX ring 在内存哪里。
TDLEN 告诉硬件 ring 多大。
TDH/TDT 都从 0 开始。
```

### 4. 初始化 RX ring

```c
memset(rx_ring, 0, sizeof(rx_ring));
for (i = 0; i < RX_RING_SIZE; i++) {
  rx_mbufs[i] = mbufalloc(0);
  rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
}
regs[E1000_RDBAL] = (uint64) rx_ring;
regs[E1000_RDH] = 0;
regs[E1000_RDT] = RX_RING_SIZE - 1;
regs[E1000_RDLEN] = sizeof(rx_ring);
```

重点：

```text
RX ring 初始化时，每个 descriptor 都有一个空 buffer。
RDBAL 告诉硬件 RX ring 在哪里。
RDLEN 告诉硬件 ring 多大。
RDT = RX_RING_SIZE - 1 表示所有 RX descriptor 都交给硬件使用。
```

### 5. 设置 MAC 地址

```c
regs[E1000_RA] = 0x12005452;
regs[E1000_RA+1] = 0x5634 | (1<<31);
```

对应 QEMU 使用的 MAC：

```text
52:54:00:12:34:56
```

注意低位/高位和小端字节序会让写入形式看起来反过来。

### 6. 启用发送

```c
regs[E1000_TCTL] = E1000_TCTL_EN |
  E1000_TCTL_PSP |
  (0x10 << E1000_TCTL_CT_SHIFT) |
  (0x40 << E1000_TCTL_COLD_SHIFT);
regs[E1000_TIPG] = 10 | (8<<10) | (6<<20);
```

含义：

```text
启用 transmitter。
允许自动填充短 packet。
设置碰撞相关参数。
设置 packet 间隔。
```

### 7. 启用接收

```c
regs[E1000_RCTL] = E1000_RCTL_EN |
  E1000_RCTL_BAM |
  E1000_RCTL_SZ_2048 |
  E1000_RCTL_SECRC;
```

含义：

```text
启用 receiver。
接收广播包。
RX buffer 大小是 2048。
硬件去掉 Ethernet CRC。
```

### 8. 启用接收中断

```c
regs[E1000_RDTR] = 0;
regs[E1000_RADV] = 0;
regs[E1000_IMS] = (1 << 7);
```

含义：

```text
每收到包就尽快产生中断。
打开 RXDW 中断。
```

## 十、e1000_transmit 应该怎么工作

函数原型：

```c
int e1000_transmit(struct mbuf *m)
```

输入：

```text
m：
  已经包含完整 Ethernet frame 的 mbuf。
  m->head 指向 Ethernet header。
  m->len 是 frame 长度。
```

目标：

```text
把这个 mbuf 填入 TX descriptor ring，并通知 E1000 发送。
```

典型步骤：

```text
1. acquire(&e1000_lock)
2. 读取 regs[E1000_TDT]，得到当前 tail index
3. 检查 tx_ring[index].status 是否有 E1000_TXD_STAT_DD
4. 如果没有 DD，说明 ring 满或这个槽位仍被硬件使用，返回 -1
5. 如果 tx_mbufs[index] 不为 0，释放旧 mbuf
6. 填写 tx_ring[index].addr = (uint64)m->head
7. 填写 tx_ring[index].length = m->len
8. 设置 tx_ring[index].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS
9. 清理或设置必要状态字段
10. tx_mbufs[index] = m
11. regs[E1000_TDT] = (index + 1) % TX_RING_SIZE
12. release(&e1000_lock)
13. 返回 0
```

为什么要保存 `tx_mbufs[index] = m`？

```text
硬件异步发送。
e1000_transmit 返回时，硬件可能还没发完。
所以不能立刻 mbuffree(m)。
必须等以后看到该 descriptor 的 DD 位，确认硬件用完这个 buffer 后，再释放旧 mbuf。
```

为什么要设置 `RS`？

```text
要求硬件发送完成后写回 status。
否则驱动之后可能不知道 descriptor 是否已经发送完成。
```

为什么要检查 `DD`？

```text
如果 DD 没置位，说明硬件还没处理完该 descriptor。
驱动不能覆盖它。
否则会破坏正在发送的数据。
```

## 十一、e1000_recv 应该怎么工作

函数原型：

```c
static void e1000_recv(void)
```

它由中断处理调用：

```c
void e1000_intr(void)
{
  regs[E1000_ICR] = 0xffffffff;
  e1000_recv();
}
```

目标：

```text
检查 RX ring 中已经收到的 packet。
把 packet 封装为 mbuf 交给 net_rx。
同时给 descriptor 补充新的空 mbuf。
```

典型步骤：

```text
1. 从 (regs[E1000_RDT] + 1) % RX_RING_SIZE 开始
2. 检查 rx_ring[index].status 是否有 E1000_RXD_STAT_DD
3. 如果没有 DD，说明没有更多已收到 packet，结束
4. 取出 m = rx_mbufs[index]
5. 设置 m->len = rx_ring[index].length
6. 为这个 descriptor 分配新 mbuf new = mbufalloc(0)
7. 如果分配失败，可以丢弃当前 packet 或停止处理
8. rx_mbufs[index] = new
9. rx_ring[index].addr = (uint64)new->head
10. rx_ring[index].status = 0
11. regs[E1000_RDT] = index
12. 调用 net_rx(m)
13. 继续检查下一个 descriptor
```

为什么从 `RDT + 1` 开始？

```text
RDT 是驱动最后交给硬件的 descriptor。
硬件会从 RDT 后面的 descriptor 开始写入收到的包。
所以驱动检查接收完成时，也从 RDT 后面开始看。
```

为什么要补新的 `mbuf`？

```text
收到的旧 mbuf 要交给上层网络栈。
该 descriptor 如果还要继续给硬件接收后续 packet，就必须换一个新的空 buffer。
```

为什么要更新 `RDT`？

```text
更新 RDT 等于告诉硬件：
这个 descriptor 我处理完了，并且已经放了新的空 buffer。
你可以再次使用它。
```

## 十二、E1000 中断处理

E1000 接收到包后会触发中断。xv6 中断路径大致是：

```text
E1000 interrupt
  -> PLIC
  -> kerneltrap / devintr
  -> trap.c 检查 irq == E1000_IRQ
  -> e1000_intr()
```

`e1000_intr()` 很短：

```c
void
e1000_intr(void)
{
  regs[E1000_ICR] = 0xffffffff;
  e1000_recv();
}
```

它做两件事：

```text
确认中断，避免设备一直重复报同一个中断。
处理所有已经到达的 packet。
```

## 十三、发送和接收的本质区别

发送：

```text
软件主动把 mbuf 交给硬件。
驱动填写 TX descriptor。
驱动推进 TDT。
硬件异步发送。
```

接收：

```text
软件预先把空 buffer 交给硬件。
硬件收到包后 DMA 写入 buffer。
硬件设置 RX descriptor 状态位。
硬件触发中断。
驱动取出 packet 并补新 buffer。
```

可以概括为：

```text
TX：驱动生产 descriptor，硬件消费。
RX：驱动生产空 buffer，硬件填充 packet，驱动再消费 packet。
```

## 十四、常见错误

### 1. 发送后立刻释放 mbuf

错误原因：

```text
硬件发送是异步的。
e1000_transmit 返回时，硬件可能还没读完 m->head。
```

正确做法：

```text
保存到 tx_mbufs[index]。
等以后 descriptor DD 置位，再释放旧 mbuf。
```

### 2. 没有检查 TX descriptor 的 DD 位

后果：

```text
可能覆盖硬件还没发送完的 descriptor。
导致发包丢失或数据损坏。
```

### 3. 接收后没有补新的 mbuf

后果：

```text
RX ring 很快没有可用 buffer。
之后硬件无法继续接收 packet。
```

### 4. 接收后没有清 status

后果：

```text
驱动可能反复认为同一个 descriptor 有新 packet。
```

### 5. 忘记更新 RDT

后果：

```text
硬件不知道 descriptor 已经重新可用。
接收可能停止。
```

### 6. 忘记加锁

`e1000_transmit()` 可能被多个进程同时调用：

```text
多个用户进程 write socket
多个 CPU 并发进入驱动
```

所以访问：

```text
tx_ring
tx_mbufs
regs[E1000_TDT]
```

应该用 `e1000_lock` 保护。

接收路径通常由中断进入，但为了简单和一致，也可以用同一把锁保护共享 ring 状态。

## 十五、调试建议

### 1. 先确认能启动 net 实验

```sh
make qemu
```

QEMU 参数中应包含：

```text
-device e1000
```

### 2. 运行 host 侧 server

另一个终端：

```sh
make server
```

### 3. 在 xv6 中运行测试

```sh
nettests
```

### 4. 观察 packet 抓包文件

net 实验的 QEMU 参数中包含：

```text
-object filter-dump,id=net0,netdev=net0,file=packets.pcap
```

因此运行后会生成：

```text
packets.pcap
```

可以用 Wireshark 或 tcpdump 分析：

```sh
tcpdump -nn -r packets.pcap
```

### 5. GDB 断点

建议断点：

```gdb
b e1000_init
b e1000_transmit
b e1000_intr
b e1000_recv
b net_rx
b sockrecvudp
```

查看寄存器：

```gdb
p regs[E1000_TDT]
p regs[E1000_RDT]
p tx_ring[0]
p rx_ring[0]
```

## 十六、最小实现目标

本实验最小目标是让下面流程跑通：

```text
xv6 nettests write UDP packet
  -> e1000_transmit 成功交给 QEMU
host server.py 收到并回复
QEMU E1000 触发接收中断
  -> e1000_recv 取出 packet
  -> net_rx 解析
  -> sockrecvudp 入队
xv6 nettests read 收到回复
```

因此你真正要写的核心逻辑就是：

```text
TX：填 tx_desc，推进 TDT。
RX：取 rx_desc，补 mbuf，推进 RDT，调用 net_rx。
```

## 十七、一句话总结

E1000 是 xv6 net 实验里的底层网卡设备模型。上层网络栈已经负责构造和解析 Ethernet/IP/UDP 包，E1000 驱动只负责通过 MMIO 寄存器和 TX/RX descriptor ring 与网卡交换 packet buffer。

这个实验的核心不是协议本身，而是理解：

```text
驱动如何把内核里的 mbuf 交给硬件 DMA 发送。
驱动如何从硬件 DMA 填好的 buffer 中取回 packet。
```
