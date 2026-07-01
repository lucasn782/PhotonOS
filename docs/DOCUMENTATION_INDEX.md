# 📚 Índice de Documentação Técnica do PhotonOS v4.0

## 🎯 Start Here

**Para a especificação mais recente do sistema (EXT2 / v4.0):**
→ [ext2_filesystem.md](ext2_filesystem.md) — Especificação técnica completa do Sistema de Ficheiros EXT2 Nativo Gravável, incluindo o marco histórico de persistência do PhotonOS v4.0

**Para a especificação de COW / v3.1:**
→ [cow_memory_optimization.md](cow_memory_optimization.md) — Especificação técnica completa do Copy-On-Write

**Para o ciclo de vida de processos e bifurcação:**
→ [process_lifecycle.md](process_lifecycle.md) — Fork, COW, IPC e sincronização atômica

**Para a arquitetura SMP e protocolo APIC/IPI:**
→ [smp.md](smp.md) — Bootstrap de núcleos AP, LAPIC, TLB Shootdown

**Para a arquitetura núcleo e gráficos:**
→ [core_and_graphics_architecture.md](core_and_graphics_architecture.md)

**Para resumo executivo do driver de teclado:**
→ [IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md)

---

## 📖 Documentação Técnica Principal (PhotonOS v4.0)

### 0. ext2_filesystem.md — `[NOVO — v4.0]`
**Propósito:** Especificação técnica completa do Sistema de Ficheiros EXT2 Nativo Gravável e do marco histórico de persistência do PhotonOS v4.0  
**Escopo:** Driver ATA (mutex), Parser de Superbloco, Matemática de Inodes, Alocação Atômica de Blocos, Pipeline de Escrita, Integração VFS, lookup recursivo de diretórios e divisão de entradas de diretório  
**Público:** Engenheiros de sistemas operacionais, revisores de código Ring 0, especialistas em sistemas de ficheiros  
**Conteúdo:**
- Arquitetura em camadas do subsistema EXT2 (hardware → ATA → EXT2 → VFS → userspace)
- Blindagem concorrente no driver ATA (`ata_mutex`) contra race conditions SMP
- Parser do Superbloco com validação do número mágico `0xEF53`
- Tabela de Descritores de Grupos de Blocos carregada em RAM
- Aritmética modular de conversão inode → (grupo, índice, bloco, offset)
- Protocolo Read-Modify-Write para `ext2_write_inode()`
- Navegação recursiva de diretórios a partir do Inode Raiz 2
- Motores de varredura atômica de bitmaps de blocos e inodes
- Suporte a ponteiros diretos (`i_block[0..11]`) e simplesmente indiretos (`i_block[12]`)
- Algoritmo de divisão de entradas de diretório (*directory entry splitting*)
- Detecção automática FAT16 → EXT2 fallback
- Regras de Ring 0 e conformidade `klog` (sem especificadores de formato)
- Referência de estruturas on-disk (`ext2_superblock`, `ext2_inode`, `ext2_dir_entry_2`)

**Leia este se:** Você precisa entender o subsistema de armazenamento persistente EXT2 do v4.0 em profundidade.

---

### 1. cow_memory_optimization.md — `[v3.1]`
**Propósito:** Especificação técnica completa do mecanismo Copy-On-Write  
**Escopo:** PMM (refcounts), VMM (clone COW), IDT (INT 0x0E), SMP (TLB Shootdown)  
**Público:** Engenheiros de sistemas operacionais, revisores de código Ring 0  
**Conteúdo:**
- Motivação e comparação com deep-copy
- Array `pmm_refcounts`: estrutura, inicialização, alocação e liberação condicional
- Protocolo COW em `vmm_clone_address_space`: manipulação de flags de PTE (`PAGE_COW = 0x200`)
- Fluxo assembly `page_fault_stub` e rotina C `vmm_page_fault_handler`
- Lógica de page splitting (refcount > 1 vs refcount == 1)
- TLB Shootdown via LAPIC IPI (Vector `0x79`) em ambientes SMP
- Tabela de impacto de performance e invariantes Ring 0

**Leia este se:** Você precisa entender a otimização de memória do v3.1 em profundidade.

---

### 2. process_lifecycle.md — `[ATUALIZADO — v3.1]`
**Propósito:** Ciclo de vida de processos com COW, IPC e sincronização  
**Conteúdo:**
- Fluxo de `sys_fork` (Syscall 23) do espaço de usuário até Ring 0
- Clonagem COW preguiçosa em `vmm_clone_address_space`
- Handler de Page Fault COW (`INT 0x0E`)
- Regra de Ouro do Fork (RAX = 0 no filho)
- `sys_exit` / `sys_wait` e liberação segura de frames compartilhados
- Sincronização atômica em Pipes (IPC)

**Leia este se:** Você precisa entender o pipeline completo de bifurcação de processos.

---

### 3. smp.md
**Propósito:** Multiprocessamento Simétrico — bootstrap de núcleos AP  
**Conteúdo:**
- Protocolo INIT-SIPI via LAPIC ICR
- Código trampolim em `0x7000`
- Spinlocks atômicos e `ap_kmain`
- `smp_tlb_shootdown_handler` (Vector `0x79`) — integração com COW

---

## 📖 Documentação Legada (Keyboard Driver v2)

### 4. README_KEYBOARD_DRIVER_V2.md
**Purpose:** Complete implementation and deployment guide  
**Length:** ~8-10 pages  
**For:** Developers, implementation teams  
**Contains:**
- Full architecture
- Build instructions
- Test procedures
- Performance analysis
- Troubleshooting guide

**Read this if:** You need to compile and test the driver

---

### 5. QUICK_REFERENCE_KEYBOARD_V2.md
**Purpose:** Quick reference for common tasks  
**Length:** ~4-5 pages  
**For:** Daily reference, quick lookups  
**Contains:**
- Critical characters table
- Build & test quick commands
- Debug commands
- Troubleshooting quick fixes
- Scancode reference table

**Read this if:** You need quick answers while working

---

### 6. KEYBOARD_DRIVER_IMPROVEMENTS.md
**Purpose:** Comprehensive technical specification  
**Length:** ~15-20 pages  
**For:** Technical architects, deep understanding  
**Contains:**
- Complete technical specification
- Detailed flow diagrams
- Character mapping tables
- Hardware-to-software flow
- Extended scancode support
- Shell integration details
- Compilation steps
- Troubleshooting procedures

**Read this if:** You want complete technical details

---

### 7. KEYBOARD_CHANGELOG.md
**Purpose:** Version history and feature documentation  
**Length:** ~10-12 pages  
**For:** Project tracking, feature verification  
**Contains:**
- Changelog entry
- What was implemented
- Files modified
- How to compile and test
- Structure of code
- Validation procedures
- Compatibility matrix
- Future enhancements

**Read this if:** You need version history and feature list

---

### 8. KEYBOARD_DIFF_DETAILED.md
**Purpose:** Exact before/after code diff with explanations  
**Length:** ~12-15 pages  
**For:** Code reviewers, implementation verification  
**Contains:**
- Detailed diff for each change
- Line-by-line explanations
- Benefits of each change
- Impact analysis
- Verification checklist

**Read this if:** You need to review exact code changes

---

### 9. test_keyboard_special_chars.sh
**Purpose:** Automated test script for special characters  
**Length:** ~200 lines  
**For:** Validation, automated testing  
**Contains:**
- Automated keyboard input sequences
- QEMU integration
- Result analysis
- Serial log parsing
- Pass/fail reporting

**Run this if:** You want automated test verification

---

## 🗺️ Navigation by Purpose

### "I need to understand the project quickly"
1. IMPLEMENTATION_SUMMARY.md (5 min)
2. QUICK_REFERENCE_KEYBOARD_V2.md (10 min)
3. README_KEYBOARD_DRIVER_V2.md (20 min)

### "I need to compile and test"
1. README_KEYBOARD_DRIVER_V2.md → Build section
2. test_keyboard_special_chars.sh
3. QUICK_REFERENCE_KEYBOARD_V2.md → Debug section

### "I need to understand the code"
1. KEYBOARD_DIFF_DETAILED.md (exact changes)
2. KEYBOARD_DRIVER_IMPROVEMENTS.md (technical details)
3. Review kernel.c directly (lines 155-205, 1150-1170, 1195-1320)

### "I need to verify everything"
1. KEYBOARD_CHANGELOG.md → Verification checklist
2. KEYBOARD_DIFF_DETAILED.md → Verification section
3. test_keyboard_special_chars.sh

### "I need to troubleshoot an issue"
1. QUICK_REFERENCE_KEYBOARD_V2.md → Troubleshooting section
2. README_KEYBOARD_DRIVER_V2.md → Troubleshooting section
3. KEYBOARD_DRIVER_IMPROVEMENTS.md → Detailed troubleshooting

---

## 📊 Document Sizes

| Document | Pages | Words | Purpose |
|----------|-------|-------|---------|
| IMPLEMENTATION_SUMMARY.md | 4 | ~1,500 | Executive overview |
| QUICK_REFERENCE_KEYBOARD_V2.md | 5 | ~1,800 | Quick reference |
| README_KEYBOARD_DRIVER_V2.md | 10 | ~3,500 | Complete guide |
| KEYBOARD_DRIVER_IMPROVEMENTS.md | 20 | ~6,000 | Technical spec |
| KEYBOARD_CHANGELOG.md | 12 | ~3,500 | Version history |
| KEYBOARD_DIFF_DETAILED.md | 15 | ~4,000 | Code diff |
| test_keyboard_special_chars.sh | 1 | ~500 | Test script |
| **TOTAL** | **67** | **~21,300** | **Complete documentation** |

---

## 🔑 Key Sections by Topic

### Character Mappings
- KEYBOARD_DRIVER_IMPROVEMENTS.md § 5 - Mapeamento de Caracteres Especiais
- KEYBOARD_DIFF_DETAILED.md § Change 1-2 - Keymap expansions
- QUICK_REFERENCE_KEYBOARD_V2.md § Critical Characters

### Scancode Information
- KEYBOARD_DRIVER_IMPROVEMENTS.md § 5.1 - Tabela de Referência
- QUICK_REFERENCE_KEYBOARD_V2.md § Keyboard Scancode Reference
- KEYBOARD_DRIVER_IMPROVEMENTS.md § 12 - Referências Técnicas

### Implementation Details
- KEYBOARD_DIFF_DETAILED.md - Detailed before/after diffs
- README_KEYBOARD_DRIVER_V2.md § Architecture - System flow
- KEYBOARD_DRIVER_IMPROVEMENTS.md § 4 - Fluxo Completo de Entrada

### Debug & Troubleshooting
- README_KEYBOARD_DRIVER_V2.md § Debug & Troubleshooting
- KEYBOARD_DRIVER_IMPROVEMENTS.md § 11 - Troubleshooting
- QUICK_REFERENCE_KEYBOARD_V2.md § Troubleshooting Quick Fix

### Build & Compilation
- README_KEYBOARD_DRIVER_V2.md § Build & Compilation
- KEYBOARD_CHANGELOG.md § Como compilar e testar
- QUICK_REFERENCE_KEYBOARD_V2.md § Build & Test (Quick)

### Testing
- test_keyboard_special_chars.sh - Automated test script
- README_KEYBOARD_DRIVER_V2.md § Run & Test
- KEYBOARD_CHANGELOG.md § Testes

---

## 📋 Quick Command Reference

### Compilação (WSL Unificado)
```bash
# Build completo: limpeza + compilação + imagem de disco FAT16
make clean; make; make fat16-disk
```

### Testing
```bash
# See: QUICK_REFERENCE_KEYBOARD_V2.md § Build & Test
make run-fat16
```

### Debug
```bash
# See: README_KEYBOARD_DRIVER_V2.md § Debug & Troubleshooting
qemu-system-x86_64 -drive file=photon.img -serial file:debug.log
```

### Test Script
```bash
# See: test_keyboard_special_chars.sh
bash test_keyboard_special_chars.sh
```

---

## ✅ Verification Checklist

### Code Changes
- [ ] Review KEYBOARD_DIFF_DETAILED.md for exact changes
- [ ] Verify kernel.c lines 155-205 (keymaps)
- [ ] Verify kernel.c lines 1150-1170 (conversion)
- [ ] Verify kernel.c lines 1195-1320 (handler)

### Implementation
- [ ] Build: `make clean && make all`
- [ ] Disk: `make fat16-disk`
- [ ] Test: `make run-fat16`
- [ ] Debug: Capture serial log

### Functionality
- [ ] Pipe works: `echo "test" | upper`
- [ ] Quotes work: `echo 'test'` and `echo "test"`
- [ ] Backslash works: Path and escape
- [ ] Forward slash works: File paths

### Documentation
- [ ] Read IMPLEMENTATION_SUMMARY.md
- [ ] Review KEYBOARD_DRIVER_IMPROVEMENTS.md
- [ ] Check KEYBOARD_DIFF_DETAILED.md
- [ ] Verify with QUICK_REFERENCE_KEYBOARD_V2.md

---

## 🎓 Learning Path

### Beginner (30 minutes)
1. IMPLEMENTATION_SUMMARY.md (5 min)
2. QUICK_REFERENCE_KEYBOARD_V2.md (10 min)
3. README_KEYBOARD_DRIVER_V2.md § Architecture (15 min)

### Intermediate (1-2 hours)
1. README_KEYBOARD_DRIVER_V2.md (30 min)
2. KEYBOARD_CHANGELOG.md (30 min)
3. KEYBOARD_DRIVER_IMPROVEMENTS.md § 4, 5 (30 min)

### Advanced (2-4 hours)
1. KEYBOARD_DIFF_DETAILED.md (1 hour)
2. KEYBOARD_DRIVER_IMPROVEMENTS.md (full) (1.5 hours)
3. Review kernel.c source directly (1 hour)
4. Run test script and debug (0.5 hour)

---

## 📞 Getting Help

### "What was changed?"
→ KEYBOARD_DIFF_DETAILED.md - Exact diffs with explanations

### "How do I build it?"
→ README_KEYBOARD_DRIVER_V2.md § Build & Compilation

### "How do I test it?"
→ QUICK_REFERENCE_KEYBOARD_V2.md § Build & Test (Quick)

### "Why does my test fail?"
→ QUICK_REFERENCE_KEYBOARD_V2.md § Troubleshooting Quick Fix

### "What's the complete specification?"
→ KEYBOARD_DRIVER_IMPROVEMENTS.md - 20+ pages of details

### "What features are included?"
→ KEYBOARD_CHANGELOG.md - Complete feature list

### "What were the changes exactly?"
→ KEYBOARD_DIFF_DETAILED.md - Line-by-line diff

---

## 🔗 Cross-References

### Special Characters
**Pipe (|)**
- Def: KEYBOARD_DRIVER_IMPROVEMENTS.md § 5.2
- Impl: KEYBOARD_DIFF_DETAILED.md § Change 2
- Ref: QUICK_REFERENCE_KEYBOARD_V2.md § Pipe

**Quotes (' and ")**
- Def: KEYBOARD_DRIVER_IMPROVEMENTS.md § 1
- Impl: KEYBOARD_DIFF_DETAILED.md § Changes 1-2
- Ref: QUICK_REFERENCE_KEYBOARD_V2.md § Quotes

**Backslash (\)**
- Def: KEYBOARD_DRIVER_IMPROVEMENTS.md § 1.1
- Impl: KEYBOARD_DIFF_DETAILED.md § Change 1
- Ref: QUICK_REFERENCE_KEYBOARD_V2.md § Backslash

### Code Sections
**keymap_normal[128]**
- Location: kernel.c lines ~155-205
- Details: KEYBOARD_DIFF_DETAILED.md § Change 1
- Docs: KEYBOARD_DRIVER_IMPROVEMENTS.md § 1.1

**keymap_shift[128]**
- Location: kernel.c lines ~208-260
- Details: KEYBOARD_DIFF_DETAILED.md § Change 2
- Docs: KEYBOARD_DRIVER_IMPROVEMENTS.md § 1.2

**keyboard_char_from_scancode()**
- Location: kernel.c lines ~1150-1170
- Details: KEYBOARD_DIFF_DETAILED.md § Change 3
- Docs: KEYBOARD_DRIVER_IMPROVEMENTS.md § 2.1

**keyboard_handle_scancode()**
- Location: kernel.c lines ~1195-1320
- Details: KEYBOARD_DIFF_DETAILED.md § Change 4
- Docs: KEYBOARD_DRIVER_IMPROVEMENTS.md § 3

---

## 📈 Statistics

- **Total Documentation:** 67 pages
- **Total Words:** 21,300+
- **Diagrams & Tables:** 20+
- **Code Examples:** 50+
- **Commands & Checklists:** 30+
- **Implementation Time:** Complete
- **Testing Readiness:** Full

---

## 🎉 Summary

You have access to comprehensive documentation covering:
- ✅ Executive summaries
- ✅ Quick reference guides
- ✅ Complete technical specifications
- ✅ Exact code diffs
- ✅ Build instructions
- ✅ Testing procedures
- ✅ Troubleshooting guides
- ✅ Automated test scripts

**Start with:** IMPLEMENTATION_SUMMARY.md (5 minutes)  
**Then read:** README_KEYBOARD_DRIVER_V2.md (20 minutes)  
**Then build:** make clean && make all  
**Then test:** make run-fat16  

---

**Documentation Status:** ✅ Complete  
**Last Updated:** 2026-07-01  
**Version:** 3.0  

Happy hacking! 🚀
