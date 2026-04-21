
# Relatório — Missão Orion: Pipeline de Telemetria Espacial
**INF01142 — Sistemas Operacionais — 2026/1**

---

## 1. Regiões Críticas e Sincronização

O programa possui três classes de regiões críticas: acesso aos buffers circulares, incremento dos contadores globais e leitura desses contadores nos `printf` finais.

**`buf_orion_lua`** é acessado por N threads Orion (inserção) e pela thread Relay (remoção) simultaneamente. Sem proteção, os campos `in`, `out` e `count` da estrutura `Buffer` seriam corrompidos. A solução usa `mutex_orion_lua` para exclusão mútua, `semaforo_orion_lua_vazio` para controlar espaço disponível e `semaforo_orion_lua_cheio` como semáforo geral de itens presentes — além de `semaforo_orion_lua_cheio_alerta` e `semaforo_orion_lua_cheio_normal` para a lógica de prioridade.

**`buf_lua_terra`** é acessado pela thread Relay (inserção) e pela thread Terra (remoção). Protegido por `mutex_lua_terra`, `semaforo_lua_terra_vazio` e `semaforo_lua_terra_cheio`.

**Contadores globais** `total_enviados`, `total_relay` e `total_recebidos` são incrementados por threads distintas e lidos no `printf` final da thread Terra. Todos os incrementos e as leituras do `printf` final são protegidos por `mutex`, com snapshot local antes de imprimir para minimizar o tempo de posse do lock.

---

## 2. Tabela de Sincronização

| Variável | Threads que acessam | Mecanismo |
|----------|---------------------|-----------|
| `buf_orion_lua` | N Orions (escrita), Relay (leitura) | `mutex_orion_lua` + `semaforo_orion_lua_vazio` + `semaforo_orion_lua_cheio` + `semaforo_orion_lua_cheio_alerta` + `semaforo_orion_lua_cheio_normal` |
| `buf_lua_terra` | Relay (escrita), Terra (leitura) | `mutex_lua_terra` + `semaforo_lua_terra_vazio` + `semaforo_lua_terra_cheio` |
| `total_enviados` | N Orions (escrita), Terra (leitura) | `mutex` |
| `total_relay` | Relay (escrita) | `mutex` |
| `total_recebidos` | Terra (escrita e leitura) | `mutex` |

---

## 3. Requisitos de Dijkstra

### `buf_orion_lua`

**Exclusão mútua:** `mutex_orion_lua` é adquirido sempre após o semáforo correspondente, garantindo que nunca duas threads executam `buffer_inserir` ou `buffer_remover` simultaneamente. Inverter a ordem (lock antes de wait) causaria deadlock.

**Progresso:** Uma Orion só avança se `semaforo_orion_lua_vazio > 0`; o Relay só avança se `semaforo_orion_lua_cheio > 0`. Sempre que há trabalho disponível, pelo menos uma thread progride.

**Espera limitada:** Com N Orions competindo, cada uma espera no máximo que as outras N−1 liberem `semaforo_orion_lua_vazio`. Como cada thread produz um número finito de pacotes (`n_pacotes`), nenhuma espera indefinidamente.

### `buf_lua_terra`

**Exclusão mútua:** `mutex_lua_terra` garante acesso exclusivo entre Relay e Terra, seguindo a mesma ordem semáforo → lock.

**Progresso:** Relay avança se `semaforo_lua_terra_vazio > 0`; Terra avança se `semaforo_lua_terra_cheio > 0`.

**Espera limitada:** Com um único produtor e um único consumidor, a espera de cada um é limitada pelo tempo de processamento finito do outro.

---

## 4. Análise da Prioridade

A prioridade é implementada com três semáforos para `buf_orion_lua`. Ao inserir, cada Orion classifica o pacote e faz `sem_post` no semáforo de tipo correspondente (`semaforo_orion_lua_cheio_alerta` ou `semaforo_orion_lua_cheio_normal`) e também em `semaforo_orion_lua_cheio`. Ao consumir, o Relay executa:

```c
sem_wait(&semaforo_orion_lua_cheio);
if (sem_trywait(&semaforo_orion_lua_cheio_alerta) != 0)
    sem_wait(&semaforo_orion_lua_cheio_normal);
```

O `sem_wait(&semaforo_orion_lua_cheio)` é essencial: garante que o Relay só prossegue quando há pelo menos um pacote disponível, evitando o deadlock que ocorreria se o `sem_trywait` falhasse e o `sem_wait(&semaforo_orion_lua_cheio_normal)` bloqueasse indefinidamente enquanto apenas alertas chegassem.

**Limitação:** a prioridade é de melhor esforço. Se o Relay já executou `sem_trywait` sem encontrar alertas e bloqueou em `sem_wait(&semaforo_orion_lua_cheio_normal)`, um alerta que chegue nesse intervalo só será processado na iteração seguinte — após o pacote normal em curso. Isso ocorre porque o buffer circular é FIFO e não reordena elementos.

---

## 5. Declaração de Uso de IA

Este trabalho utilizou assistência de IA (Claude, Anthropic) para estruturação e revisão deste relatório, além de ser utilizado como ferramenta de depuração para o arquivo `orion.c`. A análise conceitual e argumentação formal foram elaboradas e validadas pelos autores.s