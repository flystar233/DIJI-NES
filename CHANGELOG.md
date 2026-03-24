# Changelog / 更新日志

All notable changes to this project will be documented in this file.
本文件记录项目的所有重要变更。

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [v0.2.0] - 2026-03-24

### Performance / 性能优化 (48 FPS → 60 FPS)

- **MMC3 4-bank PRG 缓存**: 预计算 `prgBank0~3Offset`，消除 `cpuReadMapper4` 中的运行时分支和乘法
  Pre-computed `prgBank0~3Offset`, eliminated runtime branching in `cpuReadMapper4`
- **CHR bank 指针缓存**: `chrBankPtrs[8]` 直接指向 8 个 1KB CHR 页，PPU 访问零开销
  `chrBankPtrs[8]` direct pointers to 8×1KB CHR pages, zero-overhead PPU access
- **Nametable 指针缓存**: `ntPtrs[4]` 直接指向 4 个 nametable，消除镜像计算
  `ntPtrs[4]` direct pointers to 4 nametables, eliminated mirror calculation
- **CPU 时钟重构**: 赤字追踪模式 (`cycles -= target; while(cycles<0) cycles += step()`)，每次 `clock(113)` 消除约 75 次空转循环
  Deficit-tracking mode, eliminated ~75 idle loops per `clock(113)` call
- **OAM 预评估**: 帧开始时构建 `spriteIndicesPerLine[240][8]`，每条扫描线不再遍历 64 个精灵
  Built sprite index list at frame start, no more scanning 64 sprites per scanline
- **无分支背景像素写入**: 全透明 tile 快速路径 + 32 位写入
  Transparent tile fast path + 32-bit writes for full tiles
- **指针内联**: `renderBackgroundLine`/`renderSpriteLine`/`checkSprite0HitFast` 将 `ntPtrs`/`chrBankPtrs` 缓存为局部变量，每条扫描线消除约 132 次函数调用
  Cached pointers as local variables, eliminated ~132 function calls per scanline
- **IRAM_ATTR**: 关键热路径函数放入 IRAM 加速执行
  Critical hot-path functions placed in IRAM for faster execution

### Bug Fixes / 缺陷修复

- **[严重] PRG RAM ($6000-$7FFF) 总线路由缺失**: CPU 读写只转发 `addr >= 0x8000` 给卡带，$6000-$7FFF (SRAM) 读返回 0、写被丢弃。**这是超级玛丽 3 黑屏的根本原因** — SMB3 大量使用 PRG RAM 存储游戏变量和关卡数据
  CPU read/write was only forwarded to cartridge for `addr >= 0x8000`. $6000-$7FFF returned 0 on read. **Root cause of SMB3 black screen**
- **[严重] MMC3 IRQ 电平触发修复**: `acknowledgeIrq()` 在 `cpu.irq()` 检查 I-flag 之前清除 pending，当 CPU I-flag 置位时 IRQ 永久丢失。现在 pending 只由游戏写 $E000 清除
  `acknowledgeIrq()` cleared pending before I-flag check → IRQ permanently lost. Now only cleared by $E000 write
- **[严重] IRQ 跟随实际渲染状态**: `ppu.renderEnabled` 硬编码为 `true`，即使游戏关闭渲染 ($2001) MMC3 IRQ 计数器仍在递减。导致 KOF97 菜单切换时 VRAM 更新被破坏。修复为检查实际 `ppuMask & 0x18`
  `ppu.renderEnabled` was hardcoded `true`, causing spurious IRQs when rendering disabled. Fixed to check actual `ppuMask`
- **CPU 中断周期计数**: `irq()` 和 `nmi()` 使用 `cycles = 7` (绝对赋值) 丢弃上一条指令剩余周期，改为 `cycles += 7` (累加)
  `irq()`/`nmi()` used absolute `cycles = 7`, changed to additive `cycles += 7`
- **VBlank 周期数修正**: 从 2501 修正为 2274 (20 条扫描线 × ~113.67 周期)
  Corrected from 2501 to 2274 (20 scanlines × ~113.67 cycles)
- **脏 iNES 头检测**: 盗版 ROM 的 header bytes 8-15 常有垃圾数据，导致 mapper 高位错误。现已自动检测并仅使用 `flags6` 低半字节
  Bootleg ROMs with garbage in header bytes 8-15 now auto-detected, uses only `flags6` low nibble
- **ntPtrs 空指针崩溃**: `updateNtPtrs()` 在 `load()` 中 `setVramPointer()` 之前调用导致空指针。已加保护并在 `setVramPointer()` 中自动调用
  `updateNtPtrs()` called before `setVramPointer()` causing null dereference. Fixed with null protection

### New Features / 新功能

- **无 SD 卡启动界面**: 不插 SD 卡不再黑屏，显示菜单提示"No SD card detected"和"Press A to retry"
  Without an SD card inserted, the screen no longer goes black. A menu appears showing “No SD card detected” and “Press A to retry.”

### Compatibility / 兼容性

- **超级玛丽 3 (Super Mario Bros. 3)**: 现已完全可玩（之前黑屏） / Now fully playable (was black screen)
- **MMC3 游戏**: 分屏滚动和扫描线 IRQ 时序正常工作 / Split-screen scrolling and scanline IRQ timing working correctly
- **脏头 ROM**: 自动检测并正确处理 / Dirty header ROMs auto-detected and handled gracefully

---

## [v0.1.0] - 2026-02-23

### Initial Release / 首次发布

- 6502 CPU 全指令集模拟 (~150 操作码) / 6502 CPU full instruction set emulation (~150 opcodes)
- PPU: 背景渲染、滚动、64 个精灵 (8×8 和 8×16 模式) / Background rendering, scrolling, 64 sprites
- APU: 方波、三角波、噪声、DMC 通道，通过 I2S DAC 输出 / Square, triangle, noise, DMC via I2S DAC (MAX98357A)
- 双核架构: Core 0 (音频+显示), Core 1 (模拟) / Dual-core: Core 0 (audio+display), Core 1 (emulation)
- Mapper 支持: NROM (0), MMC1 (1), UxROM (2), CNROM (3), MMC3 (4, 部分) / Mapper 0-4 support
- SD 卡 ROM 浏览菜单 / SD card ROM browser with menu system
- 暂停菜单：存档/读档 / Pause menu with save/load state
- 大部分游戏约 50 FPS / ~50 FPS for most games
- SPI ST7789 320×240 显示 (LovyanGFX) / SPI ST7789 320×240 display via LovyanGFX
