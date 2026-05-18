# xv6 net 实验中的 QEMU、E1000 与网络拓扑

本文解释 net 实验中这段描述的含义：

```text
You'll use a network device called the E1000 to handle network communication.
To xv6 (and the driver you write), the E1000 looks like a real piece of hardware
connected to a real Ethernet local area network (LAN). In fact, the E1000 your
driver will talk to is an emulation provided by qemu, connected to a LAN that is
also emulated by qemu. On this emulated LAN, xv6 (the "guest") has an IP address
of 10.0.2.15. Qemu also arranges for the computer running qemu to appear on the
LAN with IP address 10.0.2.2. When xv6 uses the E1000 to send a packet to
10.0.2.2, qemu delivers the packet to the appropriate application on the real
computer on which you're running qemu (the "host").
```

## 一、几个角色

这个实验里有四个关键角色：

```text
xv6 guest
QEMU
E1000 网卡
host 主机
```

含义分别是：

```text
xv6 guest：
  运行在 QEMU 里的 xv6 操作系统。

QEMU：
  模拟一台 RISC-V 机器，同时模拟 CPU、内存、磁盘、中断控制器和网卡。

E1000：
  QEMU 模拟出来的一张 Intel E1000 网卡。
  xv6 以为它是一张真实硬件网卡。

host：
  真正运行 QEMU 的电脑，也就是你自己的 Linux 主机。
```

注意：

```text
xv6 不直接访问你电脑的真实网卡。
xv6 访问的是 QEMU 模拟出来的 E1000 网卡。
QEMU 再负责把包转发到真实 host 上的程序。
```

## 二、实验拓扑

逻辑拓扑如下：

```text
                         real machine
                  +-------------------------+
                  |         host            |
                  |                         |
                  |  UDP server.py / ping.py|
                  |  host-side application  |
                  |                         |
                  |  QEMU user network      |
                  |  virtual IP: 10.0.2.2   |
                  +------------^------------+
                               |
                               | QEMU 转发 packet
                               |
                  +------------v------------+
                  |         QEMU            |
                  |                         |
                  |  emulated Ethernet LAN  |
                  |                         |
                  |  +-------------------+  |
                  |  | emulated E1000 NIC|  |
                  |  +---------^---------+  |
                  |            |            |
                  |            | MMIO/DMA   |
                  |            |            |
                  |  +---------v---------+  |
                  |  |      xv6 guest    |  |
                  |  | IP: 10.0.2.15     |  |
                  |  | kernel/e1000.c    |  |
                  |  +-------------------+  |
                  +-------------------------+
```

也可以简化成：

```text
xv6 guest 10.0.2.15
  |
  | 通过 E1000 driver 收发 Ethernet frame
  v
QEMU 模拟的 E1000 网卡
  |
  | QEMU 模拟的 LAN
  v
host 10.0.2.2
  |
  v
server.py / ping.py 等真实主机程序
```

## 三、为什么 xv6 看到的是 E1000

在 `Makefile` 中，net 实验启动 QEMU 时会加上类似配置：

```makefile
QEMUOPTS += -netdev user,id=net0,hostfwd=udp::$(FWDPORT)-:2000
QEMUOPTS += -device e1000,netdev=net0,bus=pcie.0
```

其中：

```text
-device e1000
```

表示：

```text
让 QEMU 给虚拟机模拟一张 E1000 网卡。
```

所以 xv6 启动后，会通过 PCI 发现一张网卡设备。对 xv6 来说，这张网卡看起来像真实硬件：

```text
有寄存器
有中断
有 DMA
有发送描述符环
有接收描述符环
```

但实际上：

```text
这些硬件行为都是 QEMU 软件模拟出来的。
```

你写的 `kernel/e1000.c` 驱动不需要知道这是模拟的。它只需要按 E1000 手册规定的方式操作寄存器和 descriptor ring。

## 四、guest 和 host

这里的两个重要概念是：

```text
guest：
  虚拟机里的系统。
  本实验中就是 xv6。

host：
  运行虚拟机的真实系统。
  本实验中就是你运行 make qemu 的 Linux。
```

对应关系：

```text
xv6 guest IP: 10.0.2.15
host 在 QEMU 虚拟 LAN 中的 IP: 10.0.2.2
```

这两个地址都不是普通意义上你真实局域网里的地址，而是 QEMU 用户态网络创建出来的虚拟地址。

## 五、10.0.2.15 和 10.0.2.2 的含义

在 QEMU user network 模式下，QEMU 会创建一个虚拟网络环境。常见地址约定是：

```text
10.0.2.15：
  guest 的默认 IP。
  在本实验中就是 xv6 的 IP。

10.0.2.2：
  guest 眼中的 host。
  xv6 向 10.0.2.2 发包时，QEMU 会把它转给真实主机上的程序。
```

因此，在 xv6 里：

```text
目标 IP = 10.0.2.2
```

意思不是访问互联网里的某台机器，而是：

```text
访问运行 QEMU 的真实主机。
```

## 六、发送路径

以 xv6 里的 `nettests` 发送 UDP 包为例：

```text
user/nettests.c
  -> write(fd, buf, len)
  -> kernel/sysnet.c:sockwrite
  -> kernel/net.c:net_tx_udp
  -> kernel/net.c:net_tx_ip
  -> kernel/net.c:net_tx_eth
  -> kernel/e1000.c:e1000_transmit
  -> QEMU 模拟的 E1000
  -> QEMU user network
  -> host 上的 server.py
```

其中 `net_tx_udp()`、`net_tx_ip()`、`net_tx_eth()` 已经帮你构造好了协议头：

```text
UDP header
IP header
Ethernet header
```

你在本实验中主要补的是最后一步：

```text
e1000_transmit()
```

它负责把已经构造好的 Ethernet frame 放进 E1000 的发送描述符环，让网卡发送。

## 七、接收路径

host 上的程序回复 UDP 包后，路径反过来：

```text
host server.py
  -> QEMU user network
  -> QEMU 模拟的 E1000
  -> E1000 写入 RX descriptor 指向的 buffer
  -> E1000 触发中断
  -> kernel/e1000.c:e1000_intr
  -> kernel/e1000.c:e1000_recv
  -> kernel/net.c:net_rx
  -> kernel/net.c:net_rx_ip
  -> kernel/net.c:net_rx_udp
  -> kernel/sysnet.c:sockrecvudp
  -> 唤醒 read(fd, ...)
```

你在本实验中还要补：

```text
e1000_recv()
```

它负责从 E1000 的接收描述符环中取出已经收到的 packet，并调用：

```c
net_rx(m);
```

把 packet 交给上层网络栈。

## 八、E1000 driver 眼中的硬件模型

你的驱动主要和两种结构交互：

```text
TX ring：
  发送描述符环。
  xv6 把要发送的 packet 放进去，通知 E1000 发送。

RX ring：
  接收描述符环。
  xv6 预先放好空 buffer，E1000 收到 packet 后把数据 DMA 进去。
```

### 发送

发送时，驱动做的是：

```text
1. 查看 E1000_TDT，找到下一个可用 TX descriptor。
2. 检查该 descriptor 是否已经被硬件处理完。
3. 如果这个位置之前有旧 mbuf，释放旧 mbuf。
4. 把新 mbuf 的地址和长度填入 descriptor。
5. 设置 EOP、RS 等命令位。
6. 更新 E1000_TDT，通知网卡有新包要发。
```

### 接收

接收时，驱动做的是：

```text
1. 从 E1000_RDT 后面的 descriptor 开始检查。
2. 如果 descriptor 的 DD 位为 1，说明硬件已经写入了一个 packet。
3. 取出对应 mbuf。
4. 设置 mbuf->len 为 descriptor.length。
5. 分配新的空 mbuf 放回该 descriptor。
6. 清除 descriptor 状态。
7. 更新 E1000_RDT，把 descriptor 重新交给硬件。
8. 对收到的旧 mbuf 调用 net_rx(m)。
```

## 九、为什么这不是直接连真实局域网

QEMU 的 user network 是一种简化网络模式。它不要求你创建真实网桥，也不要求 xv6 出现在你真实局域网中。

因此：

```text
xv6 不是你真实 LAN 上的一台独立机器。
xv6 的 10.0.2.15 只在 QEMU 虚拟网络内部有意义。
10.0.2.2 是 QEMU 提供给 guest 的 host 入口。
```

好处是：

```text
配置简单
不需要 root 权限配置 tap/bridge
适合教学实验
```

限制是：

```text
它不是完整真实以太网环境
一些底层网络行为由 QEMU 代管
guest 和 host 之间的通信经过 QEMU 转发
```

## 十、和本实验代码的关系

你主要需要理解三层：

```text
第一层：用户态测试
  user/nettests.c
  调用 connect/read/write。

第二层：xv6 网络栈
  kernel/sysnet.c
  kernel/net.c
  已经实现 UDP/IP/Ethernet/ARP 的主要逻辑。

第三层：E1000 驱动
  kernel/e1000.c
  需要你实现 transmit 和 recv。
```

完整关系：

```text
user/nettests.c
  |
  v
sys_connect / sys_read / sys_write
  |
  v
sockread / sockwrite
  |
  v
net_tx_udp / net_rx_udp
  |
  v
net_tx_ip / net_rx_ip
  |
  v
net_tx_eth / net_rx
  |
  v
e1000_transmit / e1000_recv
  |
  v
QEMU emulated E1000
  |
  v
QEMU user network
  |
  v
host application
```

## 十一、一句话总结

net 实验中的网络并不是 xv6 直接使用真实网卡。

实际结构是：

```text
xv6 运行在 QEMU 里。
QEMU 模拟了一张 E1000 网卡。
xv6 驱动操作这张模拟网卡。
QEMU 把模拟网卡收发的包转发到 host 上的真实程序。
xv6 的虚拟 IP 是 10.0.2.15。
host 在这个虚拟 LAN 里的地址是 10.0.2.2。
```

因此，你写的 `kernel/e1000.c` 本质上是在学习真实网卡驱动的基本模式，但实验环境由 QEMU 安全地模拟出来。
