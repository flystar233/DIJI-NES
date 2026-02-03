#include "cartridge.h"
#include "nes.h"

// Temporary MMC3 debug counters (set to 9999 to disable debug output)
static int mmc3DebugEvents = 9999;

Cartridge::Cartridge() : prg(nullptr), chr(nullptr), prgSize(0), chrSize(0), prgBankSelect(0) {
    memset(chrRam, 0, sizeof(chrRam));
    memset(sram, 0, sizeof(sram));
    chrWindow = chrRam;  // 默认使用 CHR RAM
    // 初始化 MMC3 状态，避免未定义的 mmc3PrevA12 导致首帧误判
    mmc3PrevA12 = false;
    mmc3IrqPending = false;
}

Cartridge::~Cartridge() {
    if (prg) {
        free(prg);
        prg = nullptr;
    }
    if (chr) {
        free(chr);
        chr = nullptr;
    }
}

bool Cartridge::load(const char* path) {
    File f = SD.open(path);
    if (!f) {
        Serial.printf("Cartridge: Failed to open %s\n", path);
        return false;
    }

    // 读取 iNES 头 (16 字节)
    uint8_t header[16];
    if (f.read(header, 16) != 16) {
        Serial.println("Cartridge: Failed to read header");
        f.close();
        return false;
    }

    // 验证 iNES 魔数 "NES\x1A"
    if (header[0] != 'N' || header[1] != 'E' || header[2] != 'S' || header[3] != 0x1A) {
        Serial.println("Cartridge: Invalid iNES header");
        f.close();
        return false;
    }

    // 解析头信息
    prgBanks = header[4];        // PRG ROM 大小 (16KB 单位)
    chrBanks = header[5];        // CHR ROM 大小 (8KB 单位)
    
    uint8_t flags6 = header[6];
    uint8_t flags7 = header[7];
    
    mirrorVertical = (flags6 & 0x01) != 0;
    hasBattery = (flags6 & 0x02) != 0;
    bool hasTrainer = (flags6 & 0x04) != 0;
    
    mapper = ((flags6 >> 4) & 0x0F) | (flags7 & 0xF0);

    Serial.println("=== Cartridge Info ===");
    Serial.printf("  PRG ROM: %d x 16KB = %dKB\n", prgBanks, prgBanks * 16);
    Serial.printf("  CHR ROM: %d x 8KB = %dKB\n", chrBanks, chrBanks * 8);
    Serial.printf("  Mapper: %d\n", mapper);
    Serial.printf("  Mirror: %s\n", mirrorVertical ? "Vertical" : "Horizontal");
    Serial.printf("  Battery: %s\n", hasBattery ? "Yes" : "No");
    
    // 检查 Mapper 支持
    if (mapper != 0 && mapper != 1 && mapper != 2 && mapper != 3 && mapper != 4) {
        Serial.printf("  WARNING: Mapper %d not fully supported!\n", mapper);
    } else {
        const char* mapperNames[] = {"NROM", "MMC1", "UxROM", "CNROM", "MMC3"};
        Serial.printf("  Mapper Name: %s\n", mapperNames[mapper]);
    }

    // 跳过 Trainer (如果存在)
    if (hasTrainer) {
        Serial.println("  Trainer: Yes (skipping 512 bytes)");
        f.seek(16 + 512);
    }

    // 计算实际大小
    prgSize = prgBanks * 0x4000;  // 每个 bank 16KB
    chrSize = chrBanks * 0x2000;  // 每个 bank 8KB

    // 分配 PRG ROM 内存
    if (prg) {
        free(prg);
    }
    prg = (uint8_t*)malloc(prgSize);
    if (!prg) prg = (uint8_t*)ps_malloc(prgSize);
    if (!prg) {
        Serial.println("  ERROR: Failed to allocate PRG memory!");
        f.close();
        return false;
    }
    Serial.printf("  PRG buffer allocated: %d bytes\n", prgSize);

    // 读取 PRG ROM
    memset(prg, 0xFF, prgSize);
    size_t prgRead = f.read(prg, prgSize);
    Serial.printf("  PRG loaded: %d bytes\n", prgRead);
    
    // 调试：显示 PRG ROM 的开头
    Serial.printf("  First 16 bytes: ");
    for (int i = 0; i < 16 && i < (int)prgRead; i++) {
        Serial.printf("%02X ", prg[i]);
    }
    Serial.println();
    
    // 显示向量区域 (在最后一个 16KB bank 的末尾)
    if (prgRead >= 0x4000) {
        uint32_t lastBankStart = prgRead - 0x4000;
        Serial.printf("  Last bank starts at offset: 0x%X\n", lastBankStart);
        
        // 最后 16 字节
        Serial.printf("  Last 16 bytes (vectors): ");
        for (uint32_t i = prgRead - 16; i < prgRead; i++) {
            Serial.printf("%02X ", prg[i]);
        }
        Serial.println();
        
        // 解析向量 (在最后 6 字节)
        uint16_t nmi   = prg[prgRead - 6] | (prg[prgRead - 5] << 8);
        uint16_t reset = prg[prgRead - 4] | (prg[prgRead - 3] << 8);
        uint16_t irq   = prg[prgRead - 2] | (prg[prgRead - 1] << 8);
        Serial.printf("  Vectors: NMI=$%04X RESET=$%04X IRQ=$%04X\n", nmi, reset, irq);
    }

    // 分配并读取 CHR ROM (如果存在)
    if (chrBanks > 0) {
        if (chr) {
            free(chr);
        }
        chr = (uint8_t*)ps_malloc(chrSize);
        if (!chr) {
            chr = (uint8_t*)malloc(chrSize);
        }
        if (chr) {
            size_t chrRead = f.read(chr, chrSize);
            Serial.printf("  CHR loaded: %d bytes\n", chrRead);
            chrWindow = chr;  // 使用 CHR ROM
        } else {
            Serial.println("  WARNING: Failed to allocate CHR ROM, using CHR RAM");
            chrBanks = 0;
            chrSize = 0x2000;
            chrWindow = chrRam;
        }
    } else {
        Serial.println("  CHR RAM: 8KB (no CHR ROM)");
        chrSize = 0x2000;
        chrWindow = chrRam;
    }
    
    // 初始化 Mapper 状态
    prgBankSelect = 0;
    mmc1ShiftReg = 0x10;
    mmc1WriteCount = 0;
    mmc1Control = 0x0C;  // PRG 固定最后 bank, CHR 8KB 模式
    mmc1ChrBank0 = 0;
    mmc1ChrBank1 = 0;
    mmc1PrgBank = 0;
    cnromChrBank = 0;
    mmc3BankSelect = 0;
    memset(mmc3Banks, 0, sizeof(mmc3Banks));
    mmc3PrgMode = false;
    mmc3ChrMode = false;
    mmc3IrqLatch = 0;
    mmc3IrqCounter = 0;
    mmc3IrqEnabled = false;
    mmc3IrqReload = false;
    mmc3IrqPending = false;
    
    updateBankCache();

    f.close();
    Serial.println("======================\n");
    return true;
}

// ============================================================================
// Bank Cache Update
// ============================================================================

void Cartridge::updateBankCache() {
    if (!prg || prgSize < 0x4000) {
        prgBank0Offset = 0;
        prgBank1Offset = 0;
        return;
    }
    
    switch (mapper) {
        case 0:  // NROM
            prgBank0Offset = 0;
            prgBank1Offset = (prgSize > 0x4000) ? 0x4000 : 0;
            break;
            
        case 1:  // MMC1
            updateMmc1Banks();
            break;
            
        case 2:  // UxROM
            prgBank1Offset = prgSize - 0x4000;
            prgBank0Offset = (uint32_t)prgBankSelect * 0x4000;
            if (prgBank0Offset >= prgSize) prgBank0Offset = 0;
            break;
            
        case 3:  // CNROM
            prgBank0Offset = 0;
            prgBank1Offset = (prgSize > 0x4000) ? 0x4000 : 0;
            break;
            
        case 4:  // MMC3
            updateMmc3Banks();
            break;
    }
}

void Cartridge::updateMmc1Banks() {
    // MMC1 镜像模式
    switch (mmc1Control & 0x03) {
        case 0: mirrorVertical = false; break;  // 单屏 lower
        case 1: mirrorVertical = false; break;  // 单屏 upper
        case 2: mirrorVertical = true;  break;  // 垂直镜像
        case 3: mirrorVertical = false; break;  // 水平镜像
    }
    
    // PRG 模式
    uint8_t prgMode = (mmc1Control >> 2) & 0x03;
    uint8_t prgBank = mmc1PrgBank & 0x0F;
    
    switch (prgMode) {
        case 0:
        case 1:
            // 32KB 模式: 忽略 bank 的 bit 0
            prgBank0Offset = ((prgBank & 0x0E) * 0x4000);
            prgBank1Offset = prgBank0Offset + 0x4000;
            break;
        case 2:
            // 固定第一个 bank, 切换第二个
            prgBank0Offset = 0;
            prgBank1Offset = prgBank * 0x4000;
            break;
        case 3:
            // 切换第一个 bank, 固定最后一个
            prgBank0Offset = prgBank * 0x4000;
            prgBank1Offset = prgSize - 0x4000;
            break;
    }
    
    // 确保不越界
    if (prgBank0Offset >= prgSize) prgBank0Offset = 0;
    if (prgBank1Offset >= prgSize) prgBank1Offset = prgSize - 0x4000;
}
void Cartridge::updateMmc3Banks() {
    // MMC3 镜像由 $A000 控制 (在 cpuWrite 中处理)
    
    // PRG bank 计算
    uint8_t prg0 = mmc3Banks[6] & (prgBanks * 2 - 1);
    uint8_t prg1 = mmc3Banks[7] & (prgBanks * 2 - 1);
    uint8_t prgLast = prgBanks * 2 - 1;
    uint8_t prgSecondLast = prgBanks * 2 - 2;
    
    if (mmc3PrgMode) {
        // 模式 1: $C000 可切换, $8000 固定倒数第二
        prgBank0Offset = (uint32_t)prgSecondLast * 0x2000;
        // 注意: MMC3 使用 8KB PRG banks
    } else {
        // 模式 0: $8000 可切换, $C000 固定倒数第二
        prgBank0Offset = (uint32_t)prg0 * 0x2000;
    }
    
    prgBank1Offset = (uint32_t)prg1 * 0x2000;
}

// ============================================================================
// CPU Read
// ============================================================================

uint8_t IRAM_ATTR Cartridge::cpuRead(uint16_t addr) {
    // SRAM 区域 ($6000-$7FFF)
    if (addr >= 0x6000 && addr < 0x8000) {
        return sram[addr & 0x1FFF];
    }
    
    if (addr < 0x8000 || !prg) {
        return 0;
    }

    switch (mapper) {
        case 0: return cpuReadMapper0(addr);
        case 1: return cpuReadMapper1(addr);
        case 2: return cpuReadMapper2(addr);
        case 3: return cpuReadMapper3(addr);
        case 4: return cpuReadMapper4(addr);
        default:
            // Unknown mapper - simple wrap
            uint32_t prgAddr = (addr - 0x8000);
            if (prgSize) prgAddr %= prgSize;
            return prg[prgAddr];
    }
}


uint8_t Cartridge::cpuReadMapper0(uint16_t addr) {
    // NROM: 16KB 或 32KB
    uint32_t prgAddr;
    if (prgSize <= 0x4000) {
        prgAddr = (addr - 0x8000) & 0x3FFF;
    } else {
        prgAddr = (addr - 0x8000) & 0x7FFF;
    }
    return prg[prgAddr];
}

uint8_t Cartridge::cpuReadMapper1(uint16_t addr) {
    // MMC1
    if (addr < 0xC000) {
        uint32_t idx = prgBank0Offset + (addr - 0x8000);
        if (idx >= prgSize) idx %= prgSize;
        return prg[idx];
    } else {
        uint32_t idx = prgBank1Offset + (addr - 0xC000);
        if (idx >= prgSize) idx %= prgSize;
        return prg[idx];
    }
}

uint8_t Cartridge::cpuReadMapper2(uint16_t addr) {
    // UxROM: Hot path
    uint32_t base = (addr < 0xC000) ? prgBank0Offset : prgBank1Offset;
    uint32_t off  = (addr < 0xC000) ? (uint32_t)(addr - 0x8000) : (uint32_t)(addr - 0xC000);
    uint32_t idx  = base + off;
    if (idx >= prgSize) idx %= prgSize;
    return prg[idx];
}

uint8_t Cartridge::cpuReadMapper3(uint16_t addr) {
    // CNROM: 与 NROM 相同的 PRG 布局
    uint32_t prgAddr;
    if (prgSize <= 0x4000) {
        prgAddr = (addr - 0x8000) & 0x3FFF;
    } else {
        prgAddr = (addr - 0x8000) & 0x7FFF;
    }
    return prg[prgAddr];
}

uint8_t Cartridge::cpuReadMapper4(uint16_t addr) {
    // MMC3: 4 个 8KB banks
    uint8_t bank;
    uint16_t offset;
    
    if (addr < 0xA000) {
        // $8000-$9FFF
        if (mmc3PrgMode) {
            bank = prgBanks * 2 - 2;  // 倒数第二个 8KB bank
        } else {
            bank = mmc3Banks[6];
        }
        offset = addr - 0x8000;
    } else if (addr < 0xC000) {
        // $A000-$BFFF
        bank = mmc3Banks[7];
        offset = addr - 0xA000;
    } else if (addr < 0xE000) {
        // $C000-$DFFF
        if (mmc3PrgMode) {
            bank = mmc3Banks[6];
        } else {
            bank = prgBanks * 2 - 2;  // 倒数第二个
        }
        offset = addr - 0xC000;
    } else {
        // $E000-$FFFF
        bank = prgBanks * 2 - 1;  // 最后一个 8KB bank
        offset = addr - 0xE000;
    }
    
    bank &= (prgBanks * 2 - 1);
    uint32_t idx = (uint32_t)bank * 0x2000 + offset;
    if (idx >= prgSize) idx %= prgSize;
    return prg[idx];
}

// ============================================================================
// CPU Write
// ============================================================================

void IRAM_ATTR Cartridge::cpuWrite(uint16_t addr, uint8_t val) {
    // SRAM 区域 ($6000-$7FFF)
    if (addr >= 0x6000 && addr < 0x8000) {
        sram[addr & 0x1FFF] = val;
        return;
    }
    
    if (addr < 0x8000) return;
    
    switch (mapper) {
        case 1: cpuWriteMapper1(addr, val); break;
        case 2: cpuWriteMapper2(addr, val); break;
        case 3: cpuWriteMapper3(addr, val); break;
        case 4: cpuWriteMapper4(addr, val); break;
    }
}

void Cartridge::cpuWriteMapper1(uint16_t addr, uint8_t val) {
    // MMC1: 串行写入
    if (val & 0x80) {
        // 重置移位寄存器
        mmc1ShiftReg = 0x10;
        mmc1WriteCount = 0;
        mmc1Control |= 0x0C;  // 设置 PRG 模式为 3
        updateBankCache();
        return;
    }
    
    // 将 bit 0 移入移位寄存器
    mmc1ShiftReg = ((val & 0x01) << 4) | (mmc1ShiftReg >> 1);
    mmc1WriteCount++;
    
    if (mmc1WriteCount == 5) {
        // 第 5 次写入: 更新目标寄存器
        uint8_t regVal = mmc1ShiftReg & 0x1F;
        
        if (addr < 0xA000) {
            // $8000-$9FFF: Control
            mmc1Control = regVal;
        } else if (addr < 0xC000) {
            // $A000-$BFFF: CHR bank 0
            mmc1ChrBank0 = regVal;
        } else if (addr < 0xE000) {
            // $C000-$DFFF: CHR bank 1
            mmc1ChrBank1 = regVal;
        } else {
            // $E000-$FFFF: PRG bank
            mmc1PrgBank = regVal;
        }
        
        mmc1ShiftReg = 0x10;
        mmc1WriteCount = 0;
        updateBankCache();
    }
}

void Cartridge::cpuWriteMapper2(uint16_t addr, uint8_t val) {
    // UxROM: 写入任何 $8000-$FFFF 地址都会设置 bank
    uint8_t newBank = val & 0x0F;
    if (newBank >= prgBanks) {
        newBank = prgBanks - 1;
    }
    prgBankSelect = newBank;
    updateBankCache();
}

void Cartridge::cpuWriteMapper3(uint16_t addr, uint8_t val) {
    // CNROM: 选择 CHR bank
    cnromChrBank = val & 0x03;
    if (chr && chrSize > 0x2000) {
        uint32_t offset = (uint32_t)cnromChrBank * 0x2000;
        if (offset < chrSize) {
            chrWindow = chr + offset;
        }
    }
}

void Cartridge::cpuWriteMapper4(uint16_t addr, uint8_t val) {
    // MMC3
    if (addr < 0xA000) {
        if (addr & 1) {
            // $8001: Bank data
            uint8_t reg = mmc3BankSelect & 0x07;
            mmc3Banks[reg] = val;
            updateBankCache();
            if (mmc3DebugEvents < 80) {
                Serial.printf("MMC3 WRITE $8001 reg=%d val=0x%02X banks[0]=0x%02X banks[1]=0x%02X banks[2]=0x%02X banks[3]=0x%02X\n",
                              reg, val, mmc3Banks[0], mmc3Banks[1], mmc3Banks[2], mmc3Banks[3]);
                mmc3DebugEvents++;
            }
        } else {
            // $8000: Bank select
            mmc3BankSelect = val;
            mmc3PrgMode = (val & 0x40) != 0;
            mmc3ChrMode = (val & 0x80) != 0;
            updateBankCache();
            if (mmc3DebugEvents < 80) {
                Serial.printf("MMC3 WRITE $8000 bankSelect=0x%02X prgMode=%d chrMode=%d\n", mmc3BankSelect, mmc3PrgMode, mmc3ChrMode);
                mmc3DebugEvents++;
            }
        }
    } else if (addr < 0xC000) {
        if (addr & 1) {
            // $A001: PRG RAM protect (忽略)
        } else {
            // $A000: Mirroring
            mirrorVertical = (val & 0x01) == 0;
        }
    } else if (addr < 0xE000) {
        if (addr & 1) {
            // $C001: IRQ reload
            mmc3IrqReload = true;
        } else {
            // $C000: IRQ latch
            mmc3IrqLatch = val;
        }
    } else {
        if (addr & 1) {
            // $E001: IRQ enable
            mmc3IrqEnabled = true;
            if (mmc3DebugEvents < 80) {
                Serial.printf("MMC3 WRITE $E001 IRQ ENABLE (enabled=1)\n");
                mmc3DebugEvents++;
            }
        } else {
            // $E000: IRQ disable
            mmc3IrqEnabled = false;
            mmc3IrqPending = false;
            if (mmc3DebugEvents < 80) {
                Serial.printf("MMC3 WRITE $E000 IRQ DISABLE (enabled=0)\n");
                mmc3DebugEvents++;
            }
        }
    }
}

// ============================================================================
// PPU Read
// ============================================================================

uint8_t IRAM_ATTR Cartridge::ppuRead(uint16_t addr) {
    if (addr >= 0x2000) return 0;
    
    switch (mapper) {
        case 1: return ppuReadMapper1(addr);
        case 3: return ppuReadMapper3(addr);
        case 4: return ppuReadMapper4(addr);
        default:
            // Mapper 0, 2: 使用 chrWindow
            return chrWindow[addr & 0x1FFF];
    }
}


/**
 * NameTable 读取 - 处理动态镜像
 * 
 * MMC3 可以在运行时通过写入 $A000 切换镜像模式
 * 因此必须在这里处理镜像，而不是在 PPU 中缓存
 */
uint8_t IRAM_ATTR Cartridge::readNameTable(uint16_t addr) {
    if (!vram) return 0;
    
    uint16_t vAddr = addr & 0x0FFF;  // $2000-$2FFF 映射到 $000-$FFF
    
    if (mirrorVertical) {
        // 垂直镜像: $2000=$2800, $2400=$2C00
        // NT0 和 NT2 相同，NT1 和 NT3 相同
        vAddr &= 0x07FF;
    } else {
        // 水平镜像: $2000=$2400, $2800=$2C00
        // NT0 和 NT1 相同，NT2 和 NT3 相同
        if (vAddr >= 0x0800) {
            vAddr = (vAddr & 0x03FF) | 0x0400;
        } else {
            vAddr &= 0x03FF;
        }
    }
    
    return vram[vAddr & 0x07FF];
}

uint8_t IRAM_ATTR Cartridge::ppuReadMapper1(uint16_t addr) {
    // MMC1 CHR banking
    if (!chr || chrBanks == 0) {
        return chrRam[addr & 0x1FFF];
    }
    
    bool chr8kMode = (mmc1Control & 0x10) == 0;
    uint32_t chrAddr;
    
    if (chr8kMode) {
        // 8KB 模式
        uint8_t bank = mmc1ChrBank0 >> 1;
        chrAddr = (bank * 0x2000) + (addr & 0x1FFF);
    } else {
        // 4KB 模式
        if (addr < 0x1000) {
            chrAddr = ((uint32_t)mmc1ChrBank0 * 0x1000) + (addr & 0x0FFF);
        } else {
            chrAddr = ((uint32_t)mmc1ChrBank1 * 0x1000) + (addr & 0x0FFF);
        }
    }
    
    if (chrAddr >= chrSize) chrAddr %= chrSize;
    return chr[chrAddr];
}

uint8_t IRAM_ATTR Cartridge::ppuReadMapper3(uint16_t addr) {
    // CNROM: chrWindow 已经指向正确的 bank
    return chrWindow[addr & 0x1FFF];
}


// uint8_t IRAM_ATTR Cartridge::ppuReadMapper4(uint16_t addr) {
//     // =====================================================
//     // MMC3 IRQ —— A12 上升沿检测（核心！）
//     // =====================================================
//     bool a12 = (addr & 0x1000) != 0;

//     // Obtain current PPU absolute cycle for A12 debounce (scanline*341 + dot)
//     uint32_t absCycle = 0;
//     if (nes) {
//         int sl = nes->getPPU().getCurrentScanline();
//         int dot = nes->getPPU().getCurrentDot();
//         absCycle = (uint32_t)sl * 341u + (uint32_t)dot;
//     }

//     // A12 debounce: only treat 0->1 transitions as valid if A12 stayed low
//     // for at least 8 PPU cycles. Record low start time on high->low transitions.
//     if (!a12) {
//         if (mmc3PrevA12) {
//             // transitioned high -> low: mark low start
//             mmc3A12LowStart = absCycle;
//         }
//     } else {
//         if (!mmc3PrevA12) {
//             // potential 0->1 transition: validate low duration
//             uint32_t lowDur = absCycle - mmc3A12LowStart;
//             // Only clock if in visible scanlines and low lasted >= 8 cycles
//             int curScanline = nes ? nes->getPPU().getCurrentScanline() : -1;
//             if (curScanline >= 0 && curScanline < 240 && lowDur >= 8) {
//                 // clock IRQ counter
//                 if (mmc3IrqCounter == 0 || mmc3IrqReload) {
//                     mmc3IrqCounter = mmc3IrqLatch;
//                     mmc3IrqReload = false;
//                 } else {
//                     mmc3IrqCounter--;
//                 }

//                 if (mmc3IrqCounter == 0 && mmc3IrqEnabled) {
//                     mmc3IrqPending = true;
//                     if (mmc3DebugEvents < 120) {
//                         Serial.printf("MMC3 A12_VALID_RISE: scan=%d dot=%d lowDur=%d latch=%d counter=0 pending=1\n",
//                                       curScanline, nes->getPPU().getCurrentDot(), lowDur, mmc3IrqLatch);
//                         mmc3DebugEvents++;
//                     }
//                 }
//             } else {
//                 if (mmc3DebugEvents < 120) {
//                     Serial.printf("MMC3 A12_IGNORED: scan=%d dot=%d lowDur=%d\n",
//                                   curScanline, nes ? nes->getPPU().getCurrentDot() : -1, lowDur);
//                     mmc3DebugEvents++;
//                 }
//             }
//         }
//     }
//     mmc3PrevA12 = a12;

//     // =====================================================
//     // CHR RAM 情况
//     // =====================================================
//     if (!chr || chrBanks == 0) {
//         return chrRam[addr & 0x1FFF];
//     }

//     // =====================================================
//     // MMC3 CHR banking（保持你原来的逻辑）
//     // =====================================================
//     uint8_t bank;
//     uint16_t offset;

//     if (mmc3ChrMode) {
//         // 模式 1: 2KB banks 在 $1000-$1FFF
//         if (addr < 0x0400) {
//             bank = mmc3Banks[2]; offset = addr;
//         } else if (addr < 0x0800) {
//             bank = mmc3Banks[3]; offset = addr - 0x0400;
//         } else if (addr < 0x0C00) {
//             bank = mmc3Banks[4]; offset = addr - 0x0800;
//         } else if (addr < 0x1000) {
//             bank = mmc3Banks[5]; offset = addr - 0x0C00;
//         } else if (addr < 0x1800) {
//             bank = mmc3Banks[0] & 0xFE;
//             offset = (addr & 0x0400) ? (addr - 0x1400) : (addr - 0x1000);
//             if (addr & 0x0400) bank++;
//         } else {
//             bank = mmc3Banks[1] & 0xFE;
//             offset = (addr & 0x0400) ? (addr - 0x1C00) : (addr - 0x1800);
//             if (addr & 0x0400) bank++;
//         }
//     } else {
//         // 模式 0: 2KB banks 在 $0000-$0FFF
//         if (addr < 0x0800) {
//             bank = mmc3Banks[0] & 0xFE;
//             offset = (addr & 0x0400) ? (addr - 0x0400) : addr;
//             if (addr & 0x0400) bank++;
//         } else if (addr < 0x1000) {
//             bank = mmc3Banks[1] & 0xFE;
//             offset = (addr & 0x0400) ? (addr - 0x0C00) : (addr - 0x0800);
//             if (addr & 0x0400) bank++;
//         } else if (addr < 0x1400) {
//             bank = mmc3Banks[2]; offset = addr - 0x1000;
//         } else if (addr < 0x1800) {
//             bank = mmc3Banks[3]; offset = addr - 0x1400;
//         } else if (addr < 0x1C00) {
//             bank = mmc3Banks[4]; offset = addr - 0x1800;
//         } else {
//             bank = mmc3Banks[5]; offset = addr - 0x1C00;
//         }
//     }

//     // =====================================================
//     // bank 越界保护
//     // =====================================================
//     uint8_t chrBankCount = chrBanks * 8; // 1KB banks
//     if (chrBankCount) {
//         bank &= (chrBankCount - 1);
//     }

//     uint32_t idx = (uint32_t)bank * 0x0400 + (offset & 0x03FF);
//     if (idx >= chrSize) idx %= chrSize;

//     return chr[idx];
// }

uint8_t IRAM_ATTR Cartridge::ppuReadMapper4(uint16_t addr) {
    // =====================================================
    // MMC3 IRQ —— A12 上升沿检测
    // =====================================================
    bool a12 = (addr & 0x1000) != 0;

    uint32_t absCycle = 0;
    int curScanline = -1;
    int curDot = -1;
    bool rendering = false;
    if (nes) {
        curScanline = nes->getPPU().getCurrentScanline();
        curDot = nes->getPPU().getCurrentDot();
        absCycle = (uint32_t)curScanline * 341u + (uint32_t)curDot;
        rendering = nes->getPPU().isRendering(); // ✅ 使用 nes->getPPU() 获取 isRendering
    }

    // A12 debounce
    if (!a12) {
        if (mmc3PrevA12) {
            mmc3A12LowStart = absCycle;
        }
    } else {
        if (!mmc3PrevA12) {
            uint32_t lowDur = absCycle - mmc3A12LowStart;
            if (curScanline >= 0 && curScanline < 240 && rendering && lowDur >= 8) {
                // clock IRQ counter
                if (mmc3IrqCounter == 0 || mmc3IrqReload) {
                    mmc3IrqCounter = mmc3IrqLatch;
                    mmc3IrqReload = false;
                } else {
                    mmc3IrqCounter--;
                }

                if (mmc3IrqCounter == 0 && mmc3IrqEnabled) {
                    if (nes) {
                        nes->cpu.irq();
                        if (mmc3DebugEvents < 120) {
                            Serial.printf("MMC3 A12_VALID_RISE: -> cpu.irq() scan=%d dot=%d lowDur=%d latch=%d\n",
                                          curScanline, curDot, lowDur, mmc3IrqLatch);
                            mmc3DebugEvents++;
                        }
                    } else {
                        mmc3IrqPending = true;
                    }
                }
            } else {
                if (mmc3DebugEvents < 120) {
                    Serial.printf("MMC3 A12_IGNORED: scan=%d dot=%d lowDur=%d\n",
                                  curScanline, curDot, lowDur);
                    mmc3DebugEvents++;
                }
            }
        }
    }
    mmc3PrevA12 = a12;

    // =====================================================
    // CHR RAM / CHR ROM
    // =====================================================
    if (!chr || chrBanks == 0) {
        return chrRam[addr & 0x1FFF];
    }

    // =====================================================
    // MMC3 CHR banking
    // =====================================================
    uint8_t bank;
    uint16_t offset;

    if (mmc3ChrMode) {
        // 模式 1: 高 4KB 在 $1000-$1FFF
        if (addr < 0x0400) {
            bank = mmc3Banks[2]; offset = addr;
        } else if (addr < 0x0800) {
            bank = mmc3Banks[3]; offset = addr - 0x0400;
        } else if (addr < 0x0C00) {
            bank = mmc3Banks[4]; offset = addr - 0x0800;
        } else if (addr < 0x1000) {
            bank = mmc3Banks[5]; offset = addr - 0x0C00;
        } else if (addr < 0x1800) {
            bank = mmc3Banks[0] & 0xFE;
            offset = (addr & 0x0400) ? (addr - 0x1400) : (addr - 0x1000);
            if (addr & 0x0400) bank++;
        } else {
            bank = mmc3Banks[1] & 0xFE;
            offset = (addr & 0x0400) ? (addr - 0x1C00) : (addr - 0x1800);
            if (addr & 0x0400) bank++;
        }
    } else {
        // 模式 0: 低 4KB 在 $0000-$0FFF
        if (addr < 0x0800) {
            bank = mmc3Banks[0] & 0xFE;
            offset = (addr & 0x0400) ? (addr - 0x0400) : addr;
            if (addr & 0x0400) bank++;
        } else if (addr < 0x1000) {
            bank = mmc3Banks[1] & 0xFE;
            offset = (addr & 0x0400) ? (addr - 0x0C00) : (addr - 0x0800);
            if (addr & 0x0400) bank++;
        } else if (addr < 0x1400) {
            bank = mmc3Banks[2]; offset = addr - 0x1000;
        } else if (addr < 0x1800) {
            bank = mmc3Banks[3]; offset = addr - 0x1400;
        } else if (addr < 0x1C00) {
            bank = mmc3Banks[4]; offset = addr - 0x1800;
        } else {
            bank = mmc3Banks[5]; offset = addr - 0x1C00;
        }
    }

    // bank 越界保护
    uint8_t chrBankCount = chrBanks * 8; // 1KB banks
    if (chrBankCount) bank &= (chrBankCount - 1);

    uint32_t idx = (uint32_t)bank * 0x0400 + (offset & 0x03FF);
    if (idx >= chrSize) idx %= chrSize;

    return chr[idx];
}


// ============================================================================
// PPU Write
// ============================================================================

void Cartridge::ppuWrite(uint16_t addr, uint8_t val) {
    if (addr >= 0x2000) return;
    
    switch (mapper) {
        case 1:
            ppuWriteMapper1(addr, val);
            break;
        case 4:
            ppuWriteMapper4(addr, val);
            break;
        default:
            // CHR RAM: 只有当没有 CHR ROM 时才可写
            if (chrBanks == 0) {
                chrRam[addr & 0x1FFF] = val;
            }
            break;
    }
}

void Cartridge::ppuWriteMapper1(uint16_t addr, uint8_t val) {
    if (chrBanks == 0) {
        chrRam[addr & 0x1FFF] = val;
    }
}

void Cartridge::ppuWriteMapper4(uint16_t addr, uint8_t val) {
    if (chrBanks == 0) {
        chrRam[addr & 0x1FFF] = val;
    }
}

// ============================================================================
// MMC3 IRQ
// ============================================================================

void Cartridge::clockIrqCounter() {
    if (mapper != 4) return;
    
    if (mmc3IrqCounter == 0 || mmc3IrqReload) {
        mmc3IrqCounter = mmc3IrqLatch;
        mmc3IrqReload = false;
    } else {
        mmc3IrqCounter--;
    }
    
    if (mmc3IrqCounter == 0 && mmc3IrqEnabled) {
        if (nes) {
            nes->cpu.irq();
            if (mmc3DebugEvents < 80) {
                Serial.printf("MMC3 clockIrqCounter: -> cpu.irq() irqCounter=%d latch=%d enabled=%d\n",
                              mmc3IrqCounter, mmc3IrqLatch, mmc3IrqEnabled);
                mmc3DebugEvents++;
            }
        } else {
            mmc3IrqPending = true;
        }
    }
}

/**
 * 简化的 PPU scanline 回调 (Anemoia 风格)
 * 每条可见扫描线结束时调用
 * 不依赖 A12 检测，直接使用扫描线计数
 */
void IRAM_ATTR Cartridge::ppuScanline() {
    if (mapper != 4) return;  // 只有 MMC3 需要处理
    
    // Anemoia 风格的简化 IRQ 逻辑
    if (mmc3IrqCounter == 0) {
        mmc3IrqCounter = mmc3IrqLatch;
    } else {
        mmc3IrqCounter--;
        if (mmc3IrqCounter == 0 && mmc3IrqEnabled) {
            mmc3IrqPending = true;
        }
    }
}

// ============================================================================
// Save State
// ============================================================================

size_t Cartridge::getStateSize() const {
    size_t size = 0;
    size += sizeof(mapper);
    size += sizeof(mirrorVertical);
    size += sizeof(prgBankSelect);
    
    // MMC1
    size += sizeof(mmc1ShiftReg);
    size += sizeof(mmc1WriteCount);
    size += sizeof(mmc1Control);
    size += sizeof(mmc1ChrBank0);
    size += sizeof(mmc1ChrBank1);
    size += sizeof(mmc1PrgBank);
    
    // CNROM
    size += sizeof(cnromChrBank);
    
    // MMC3
    size += sizeof(mmc3BankSelect);
    size += sizeof(mmc3Banks);
    size += sizeof(mmc3PrgMode);
    size += sizeof(mmc3ChrMode);
    size += sizeof(mmc3IrqLatch);
    size += sizeof(mmc3IrqCounter);
    size += sizeof(mmc3IrqEnabled);
    size += sizeof(mmc3IrqReload);
    size += sizeof(mmc3IrqPending);
    
    // SRAM
    size += sizeof(sram);
    
    // CHR RAM (if used)
    if (chrBanks == 0) {
        size += sizeof(chrRam);
    }
    
    return size;
}

void Cartridge::saveState(uint8_t* buf, size_t& offset) const {
    // Mapper 状态
    buf[offset++] = mapper;
    buf[offset++] = mirrorVertical ? 1 : 0;
    buf[offset++] = prgBankSelect;
    
    // MMC1
    buf[offset++] = mmc1ShiftReg;
    buf[offset++] = mmc1WriteCount;
    buf[offset++] = mmc1Control;
    buf[offset++] = mmc1ChrBank0;
    buf[offset++] = mmc1ChrBank1;
    buf[offset++] = mmc1PrgBank;
    
    // CNROM
    buf[offset++] = cnromChrBank;
    
    // MMC3
    buf[offset++] = mmc3BankSelect;
    memcpy(buf + offset, mmc3Banks, sizeof(mmc3Banks));
    offset += sizeof(mmc3Banks);
    buf[offset++] = mmc3PrgMode ? 1 : 0;
    buf[offset++] = mmc3ChrMode ? 1 : 0;
    buf[offset++] = mmc3IrqLatch;
    buf[offset++] = mmc3IrqCounter;
    buf[offset++] = mmc3IrqEnabled ? 1 : 0;
    buf[offset++] = mmc3IrqReload ? 1 : 0;
    buf[offset++] = mmc3IrqPending ? 1 : 0;
    
    // SRAM
    memcpy(buf + offset, sram, sizeof(sram));
    offset += sizeof(sram);
    
    // CHR RAM
    if (chrBanks == 0) {
        memcpy(buf + offset, chrRam, sizeof(chrRam));
        offset += sizeof(chrRam);
    }
}

void Cartridge::loadState(const uint8_t* buf, size_t& offset) {
    // Mapper 状态
    mapper = buf[offset++];
    mirrorVertical = buf[offset++] != 0;
    prgBankSelect = buf[offset++];
    
    // MMC1
    mmc1ShiftReg = buf[offset++];
    mmc1WriteCount = buf[offset++];
    mmc1Control = buf[offset++];
    mmc1ChrBank0 = buf[offset++];
    mmc1ChrBank1 = buf[offset++];
    mmc1PrgBank = buf[offset++];
    
    // CNROM
    cnromChrBank = buf[offset++];
    
    // MMC3
    mmc3BankSelect = buf[offset++];
    memcpy(mmc3Banks, buf + offset, sizeof(mmc3Banks));
    offset += sizeof(mmc3Banks);
    mmc3PrgMode = buf[offset++] != 0;
    mmc3ChrMode = buf[offset++] != 0;
    mmc3IrqLatch = buf[offset++];
    mmc3IrqCounter = buf[offset++];
    mmc3IrqEnabled = buf[offset++] != 0;
    mmc3IrqReload = buf[offset++] != 0;
    mmc3IrqPending = buf[offset++] != 0;
    
    // SRAM
    memcpy(sram, buf + offset, sizeof(sram));
    offset += sizeof(sram);
    
    // CHR RAM
    if (chrBanks == 0) {
        memcpy(chrRam, buf + offset, sizeof(chrRam));
        offset += sizeof(chrRam);
    }
    
    // 更新 bank 缓存
    updateBankCache();
    
    // 更新 CHR window
    if (mapper == 3 && chr && chrSize > 0x2000) {
        uint32_t chrOffset = (uint32_t)cnromChrBank * 0x2000;
        if (chrOffset < chrSize) {
            chrWindow = chr + chrOffset;
        }
    } else if (chrBanks == 0) {
        chrWindow = chrRam;
    } else if (chr) {
        chrWindow = chr;
    }
}
