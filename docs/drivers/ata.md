# ATA Disk Driver (PIO Mode)

Este documento especifica a implementação e operação do driver de disco ATA em modo PIO (Programmed I/O) no kernel do PhotonOS.

---

## 1. Visão Geral e Registradores de Hardware

O driver ATA do PhotonOS controla discos rígidos IDE convencionais rodando em modo linear LBA de 28 bits (Logical Block Addressing) sobre o canal primário do barramento. Ele realiza transferências síncronas de setores através do controlador de E/S por portas.

As portas físicas do canal primário utilizadas são:
*   `0x1F0` (ATA_DATA): Leitura e escrita de palavras de dados de 16 bits.
*   `0x1F2` (ATA_SECTOR_COUNT): Número de setores a serem lidos/gravados.
*   `0x1F3` (ATA_LBA_LOW): LBA bits 0–7.
*   `0x1F4` (ATA_LBA_MID): LBA bits 8–15.
*   `0x1F5` (ATA_LBA_HIGH): LBA bits 16–23.
*   `0x1F6` (ATA_DRIVE): Seleção do drive (Master/Slave) e bits LBA 24–27.
*   `0x1F7` (ATA_STATUS / ATA_COMMAND): Leitura do registrador de status ou escrita de comandos de execução.
*   `0x3F6` (ATA_CONTROL): Registrador de controle de barramento (usado para gerar atrasos).

---

## 2. Inicialização e Identificação (`ata_init`)

Durante `ata_init()`, o driver:
1.  Inicializa o mutex de exclusão mútua (`ata_mutex`).
2.  Reseta o barramento IDE primário gravando `0` na porta de controle `0x3F6`.
3.  Envia o comando `0xEC` (ATA_CMD_IDENTIFY) para interrogar se um disco físico está presente.
4.  Se o barramento responder e a flag DRQ (Data Request) for ativada, lê o bloco de 512 bytes de informações de identificação (usando `insw` para ler 256 palavras da porta de dados). O disco é marcado como presente.

---

## 3. Operações de Leitura e Escrita de Setores

### Fluxo de Leitura (`ata_read_sectors`)
1.  **Acesso Exclusivo**: Adquire o `ata_mutex`.
2.  **Verificação de Disponibilidade**: Aguarda a liberação da flag BSY (Busy) via `ata_wait_ready()`.
3.  **Configuração de Endereçamento**: Grava a porta do drive (`0x1F6`) mesclando os bits LBA superiores, o número de setores na porta `0x1F2` e os LBA baixos nas portas `0x1F3`–`0x1F5`.
4.  **Emissão do Comando**: Envia o comando `0x20` (ATA_CMD_READ_PIO).
5.  **Coleta de Dados**: Para cada setor requisitado:
    *   Aguarda a ativação da flag `DRQ` via `ata_wait_drq()`.
    *   Lê 512 bytes (256 palavras) via instrução Assembly otimizada `rep insw`.
    *   Aplica um atraso de barramento de 400ns lendo repetidamente a porta de controle.
6.  **Liberação**: Destrava o `ata_mutex`.

### Fluxo de Escrita (`ata_write_sectors`)
1.  **Acesso Exclusivo**: Adquire o `ata_mutex`.
2.  **Configuração**: Carrega a geometria LBA e envia o comando `0x30` (ATA_CMD_WRITE_PIO).
3.  **Transmissão de Dados**: Para cada setor:
    *   Aguarda a ativação de `DRQ`.
    *   Escreve 512 bytes (256 palavras) do buffer para a porta de dados usando `rep outsw`.
    *   Aplica o atraso de 400ns.
4.  **Flushing de Cache**: Envia o comando `0xE7` (ATA_CMD_CACHE_FLUSH) para forçar o disco físico a descarregar buffers internos de escrita.
5.  **Aguardar Conclusão**: Aguarda a liberação de BSY.
6.  **Liberação**: Destrava o `ata_mutex`.

---

## 4. Segurança SMP (Mutex de Concorrência)

> [!IMPORTANT]
> **Blindagem Multiprocessador:**
> O acesso físico ao barramento IDE por diferentes processadores ou threads em paralelo causaria graves problemas de coerência e corrupção de dados (por exemplo, misturando palavras de diferentes setores no canal de dados). O driver é protegido pelo **`ata_mutex`** (exclusão mútua), garantindo que apenas um núcleo da CPU transmita dados no barramento IDE de cada vez.

---

## 5. Integração com VFS e Fallback de Montagem

O driver fornece uma interface de bloco registrada no VFS em `ata_vfs_init()`:
*   Mapeia o dispositivo em `/dev/hda` com ponteiros para rotinas internas de leitura e escrita de blocos alinhados a 512 bytes.
*   **Pipeline de Montagem**: O kernel tenta montar `/dev/hda` primeiramente utilizando o driver do sistema de arquivos **FAT16**. Se a assinatura de boot FAT16 for inválida, ele executa automaticamente um **fallback de montagem para o driver EXT2**, garantindo versatilidade na inicialização do disco virtual.
*   **Criação de Caminhos**: A chamada `ata_vfs_create()` roteia a criação para `fat16_vfs_create()`; caso falhe (retorno negativo), a criação é delegada para `ext2_vfs_create()`.
