# SD-WAN功能实现说明

## 功能概述

本项目已成功集成SD-WAN功能,基于ZeroTier实现VPN隧道前置连接。

## 实现内容

### 1. 用户界面
- 在主界面和VPN信息标签页之间新增了"SD-WAN"标签页
- SD-WAN标签页包含:
  - 启用复选框
  - 连接状态显示
  - 节点许可文件选择
  - 网络ID输入框

### 2. 配置管理
- SD-WAN配置保存在QSettings中
- 配置项包括:
  - enabled: 是否启用SD-WAN
  - nodeLicensePath: 节点许可文件路径
  - networkId: ZeroTier网络ID

### 3. ZeroTier集成
- ZeroTier二进制文件和驱动位于 `node/windows/` 目录
- 安装时应将这些文件部署到 `C:\Program Files\aVPN\node\`
- 实现了以下功能:
  - ZeroTier服务启动
  - 节点许可(planet文件)部署
  - ZeroTier网络加入
  - 进程管理和清理

### 4. VPN连接逻辑修改
- 连接VPN时检查SD-WAN是否启用
- 如果启用:
  1. 验证网络ID已填写
  2. 启动ZeroTier服务 (`zerotier-one_x64.exe -C "."`)
  3. 检查是否已加入该网络(通过lastJoinedNetworkId记录)
  4. 如果网络ID变化:
     - 先执行 `leave` 离开旧网络
     - 再执行 `join` 加入新网络
  5. 成功后再连接VPN服务器
- 如果未启用:
  - 按原有逻辑直接连接VPN

**优化说明**: 
- 系统会记住上次加入的网络ID,避免重复执行join命令
- 网络ID变化时自动离开旧网络,确保始终只有一个ZeroTier虚拟网卡
- 离开网络时会同时清除 `networks.d/[networkId].conf` 配置文件
- 应用退出时自动离开当前网络并清理资源
- 取消勾选SD-WAN时清空网络记录

## 文件修改清单

1. `src/dialog/mainwindow.ui` - 添加SD-WAN标签页UI
2. `src/dialog/mainwindow.h` - 添加SD-WAN相关方法和成员变量
3. `src/dialog/mainwindow.cpp` - 实现SD-WAN功能逻辑

## 部署说明

### Windows安装包
需要将以下文件打包到安装程序中:
```
C:\Program Files\aVPN\node\
├── zerotier-one_x64.exe
├── zerotier-one.port
├── zttap300.cat
├── zttap300.inf
└── zttap300.sys
```

## 使用流程

1. 启动aVPN应用程序
2. 切换到"SD-WAN"标签页
3. 勾选"启用"复选框
4. (可选)点击"浏览..."选择node.lic许可文件
5. 输入16位ZeroTier网络ID
6. 返回主界面,选择VPN服务器
7. 点击"连接"按钮
8. 系统将:
   - 先启动ZeroTier并加入指定网络
   - 再连接VPN服务器(通过ZeroTier隧道)

## 注意事项

- 节点许可文件(node.lic)会被复制为planet文件到ZeroTier目录
- ZeroTier进程在应用退出时会自动清理
- 网络ID必须是有效的16位字符串
- SD-WAN状态会实时显示在界面上