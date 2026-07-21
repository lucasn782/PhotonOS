# PCI Bus Driver (Peripheral Component Interconnect)

Este documento especifica a implementação do barramento PCI (Peripheral Component Interconnect) no kernel do PhotonOS, cobrindo o acesso ao espaço de configuração, o algoritmo de varredura ativa e o registro do driver e1000.

---

## 1. Espaço de Configuração PCI

O driver PCI (`src/drivers/pci.c`) acessa o espaço de configuração padronizado de 256 bytes de cada dispositivo de hardware através do mecanismo de E/S por portas da arquitetura x86:
*   **Porta de Endereço (`0xCF8`)**: Registrador de 32 bits onde o kernel escreve o endereço do dispositivo e registro desejado.
*   **Porta de Dados (`0xCFC`)**: Registrador de 32 bits de onde o kernel lê ou escreve o valor do registro selecionado.

O endereço físico de 32 bits a ser gravado em `0xCF8` é calculado como:
$$\text{Endereço} = \text{Bit de Habilitação} \mid (\text{Bus} \ll 16) \mid (\text{Slot} \ll 11) \mid (\text{Func} \ll 8) \mid (\text{Offset} \ \& \ \text{0xFC})$$
Onde o **Bit de Habilitação** é `0x80000000U`.

As funções públicas fornecem a interface de leitura/escrita:
```c
uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
```

---

## 2. Varredura Ativa do Barramento (`pci_init`)

Durante a inicialização (`pci_init`), o driver executa um algoritmo de varredura profunda (*active probing*) sobre todos os endereços lógicos possíveis do barramento:
*   **Barramentos (Bus)**: `0` a `255`.
*   **Slots**: `0` a `31`.
*   **Funções (Function)**: `0` a `7` (suporte a dispositivos multifunção).

Para cada combinação, a rotina lê o registro `0x00` (Vendor ID / Device ID). Se o Vendor ID retornado for `0xFFFFU`, o slot está vazio e o driver passa ao próximo.

---

## 3. Identificação de Classe e Subclasse

Quando um dispositivo é detectado, o kernel lê o registro `0x08` (`PCI_CLASS_OFFSET`) para extrair os códigos de classificação de hardware:
*   **Class Code** (bits 24–31): Especifica a categoria geral do dispositivo (ex: `0x02` indica controlador de rede).
*   **Subclass Code** (bits 16–23): Especifica a subcategoria (ex: `0x00` indica adaptador Ethernet).

Se o dispositivo corresponder a um controlador Ethernet (`Class 0x02, Subclass 0x00`), o driver inicia o protocolo de associação.

---

## 4. Associação de Adaptador Ethernet e Leitura de BARs

A rotina `pci_probe_ethernet` tenta associar um driver de rede ativo:
1.  **Filtro de Vendor/Device**: Verifica se o dispositivo corresponde à placa Intel e1000 suportada:
    *   Vendor ID: `0x8086U` (Intel Corporation).
    *   Device ID: `0x100EU` (82540EM) ou `0x100FU` (82545EM).
    *   Se for um dispositivo incompatível (como a Realtek RTL8139 `0x10EC:0x8139`), o kernel emite uma mensagem de log informando que a placa não possui suporte ativo.
2.  **Mapeamento de BARs (Base Address Registers)**:
    *   O kernel lê o registrador BAR0 (`0x10`) para determinar a base de E/S de memória do dispositivo.
    *   O driver suporta BARs de memória de 32 bits e de 64 bits (`is_64`). Se o BAR indicar alocação de 64 bits (tipo `0x4U`), o kernel lê o BAR1 seguinte (`0x14`) para extrair a metade superior de 32 bits do endereço físico, concatenando-os.
3.  **Habilitação de Recursos**:
    *   O kernel configura o registrador de controle PCI (`PCI_COMMAND_OFFSET = 0x04`) ativando os bits `PCI_COMMAND_MEMORY_SPACE` e `PCI_COMMAND_BUS_MASTER` (permitindo que o hardware faça transferências DMA de forma independente).
4.  **Inicialização do Adaptador**: Invoca a inicialização do driver e1000 (`e1000_init`) passando o endereço físico mapeado da BAR de memória.
