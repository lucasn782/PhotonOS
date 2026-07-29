# 🔢 Gerenciamento de Portas TCP — PhotonOS v4.2

## 🌐 Visão Geral

O gerenciamento de portas TCP do **PhotonOS v4.2** é responsável por alocar, validar, reutilizar e desalocar portas locais (bem conhecidas e efêmeras) para conexões de rede. Ele garante a unicidade da porta vinculada e previne colisão de portas em ambientes concorrentes multi-threaded/SMP.

---

## ⚓ Tipos de Alocação de Portas

### 1. Bind Explícito (`tcp_bind`)
Quando a aplicação especifica uma porta local não-nula (ex: porta `80` ou `8080` para servidores web):
- A função valida se a porta requisitada já está em uso por outro PCB usando `tcp_port_in_use_locked()`.
- Uma porta é considerada em colisão se outro PCB registrado possuir a mesma porta e o mesmo endereço IP (ou se um dos IPs for `INADDR_ANY` = `0`).
- Se livre, atribui a porta, configura o IP local e marca a flag `TCP_PCB_FLAG_BOUND`.

### 2. Alocação de Porta Efêmera (`tcp_allocate_ephemeral_port`)
Quando a aplicação solicita porta `0` em `bind()` ou realiza uma chamada ativa `connect()` sem realizar `bind()` prévio:
- O kernel seleciona uma porta efêmera no intervalo dinâmico IANA:
  - **Início**: `TCP_EPHEMERAL_PORT_FIRST` (49152)
  - **Fim**: `TCP_EPHEMERAL_PORT_LAST` (65535)
- O contador estático `tcp_next_ephemeral` busca ciclicamente a próxima porta livre.
- Quando o valor atinge `65535`, ocorre o wrap-around automático para `49152`.
- O algoritmo verifica a disponibilidade da porta candidata via `tcp_port_in_use_locked()`. Se a porta estiver em uso, tenta a próxima até varrer todo o intervalo efêmero (16384 portas).

### 3. Liberação de Porta (`tcp_release_port`)
- Executada quando um socket ou PCB é destruído/fechado.
- Zera a porta local e o IP local do PCB, removendo a flag `TCP_PCB_FLAG_BOUND`.
- Torna a porta imediatamente disponível para reuso por novas conexões.

---

## 🔒 Garantias de Concorrência

1. Toda a alocação e verificação de disponibilidade de portas ocorre sob a proteção do mutex global `tcp_pcbs_lock`.
2. O mutex do PCB individual (`pcb->lock`) é adquirido durante a atribuição dos campos `local_port` e `flags`, prevenindo qualquer race condition entre fios de execução concorrentes.
3. Não há vazamento de portas nem retenção de portas órfãs.
