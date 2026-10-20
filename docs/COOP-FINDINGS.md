# O que a co-op já faz, e o que falta

Estado medido em duas instâncias de verdade do jogo (`tools/coop_run.sh`).
A especificação é `docs/COOP.md`; os offsets verificados são
`.agents/teamwork/coop/LAYOUT.md`; o contrato da rede é
`.agents/teamwork/coop/PROTOCOL.md`.

## 1. O que funciona

| peça | como se sabe |
|---|---|
| A sessão de rede | `tests/test_coop_session.cpp`: a barreira de carga, a sala, a sequência, o acuse de recibo e o timeout que sobrevive ao carregamento |
| O formato da rede | `tests/test_coop_protocol.cpp`: ida e volta, os limites, o tamanho do datagrama |
| A camada de mundo | `tests/test_coop_world.cpp`: a identidade, o emparelhamento, a ordem dos eventos |
| A camada de missão | `tests/test_coop_mission.cpp`: o nome do evento, os objetivos, o fim da missão |
| As duas instâncias | `tools/dev_check.sh coop`: `epoch=1 snapshots=389 events=21 unpaired=0 divergent=0` |

O gate de log (`tests/coop/assert_coop_log.py`) compara as duas cópias do log:
a mesma rodada, nenhum ator sem par, nenhuma divergência, nenhum erro, as
sequências em ordem e o convidado nunca como fonte. Ele tem os seus próprios
casos, porque um portão que não pode falhar não é um portão.

## 2. As três coisas que a spec errou

A spec `docs/COOP.md` tem seis afirmações que a verificação no código
recompilado refutou. Todas estão corrigidas em `LAYOUT.md`:

1. o bloco de atores está em `manager + 0xA0`, não em `0x002D12E8` (que é a
   base do arena);
2. a lista de atores vivos cabe **16**, não 32 (o bloco tem 32 slots);
3. `actor+0x39C` é um id de **grupo**, não a chave do ator;
4. no raycast, `out+0x24` é o objeto **estático**; o ator dinâmico é
   `out+0x30`;
5. `0x001DA4F0` ativa atores e IA, mas não gatilhos;
6. o script da missão **não está em `StLevel.bin`** (é asset de stage): a
   missão está em `LevelDat.bin` e o "script" é um bus de eventos nomeados.

A sexta é a que mais muda o desenho: a tabela de objetivos não precisa
cruzar a rede, porque os dois processos carregam o mesmo `LevelDat.bin`. O
que cruza é o **evento**, e o matcher do convidado avança o objetivo dele
igual ao do host.

## 3. O que a tentativa anterior errou, e o que mudou

Os logs de `logs/coop-*.log` são de execuções reais de duas instâncias.

- **A geração do ator era um contador local.** A geração real é
  `actor+0x374`, o valor do contador do gerenciador (`manager+0x8524`) no
  momento da ativação. Dois processos que rodam o mesmo script ativam os
  mesmos objetos na mesma ordem, então ela concorda sozinha. A tentativa
  anterior a tratava como local, e o par deixava de casar: de 15 registros no
  convidado, 10 casaram.
- **O estado do corpo do dano não viajava.** A rotina de dano lê a parte do
  corpo do controlador (`controller+0xCA8`) e **ignora o dano** para as
  partes 29, 30 e 31. Sem o estado, o convidado reproduzia a chamada e o dano
  não acontecia. O estado viaja agora e é restaurado depois.
- **A identidade tinha de ser lida antes da chamada.** A morte limpa o par
  em `actor+0x08` e `actor+0x0C`; um evento publicado depois da chamada
  carregava uma identidade de zero.
- **A geração se permuta entre os processos.** O contador de ativação é
  compartilhado, mas a ordem de ativação não é: o script reage a onde o
  jogador está, e os dois jogadores estão em lugares diferentes. O par
  (a chave do motor para o objeto de script) continua igual, então o
  emparelhamento tem uma segunda chance por posição.

## 4. O que falta: a ativação dos atores

**Esta é a próxima peça, e é a mais importante que falta.**

Medido numa execução real: às 25 s o convidado tinha **2** atores e o host
tinha um terceiro que o convidado nunca teve. No fim da execução, com o
nível todo carregado, os dois lados tinham **7** e casavam **7 de 7**.

Ou seja: **as duas cópias do level não ativam os mesmos objetos ao mesmo
tempo.** O script de level decide quem nasce, e ele reage a onde o jogador
está. As duas cópias do mundo não são o mesmo mundo enquanto isso não for
replicado.

O caminho:

- A ativação é `0x001DA4F0(actor, descriptor, third)`. O `descriptor` vai
  para `actor+0x320` e é o objeto do script de level; a identidade do par
  sai dele.
- O host publica, para cada ator que ele tem e o convidado não tem, o par
  de identidade, o tipo e a posição.
- O convidado procura o descritor correspondente nos **dados do próprio
  level dele** (o mesmo `LevelDat.bin` que os dois carregaram) e chama a
  ativação com ele.
- O convidado nunca é a fonte: ele só ativa o que o host mandou.

**O que ainda falta saber** (para o próximo): onde moram os descritores dos
objetos do script nos dados do level, e como o descriptor de um ator se
liga ao par de identidade. A identidade sai do descriptor, então o
caminho inverso existe — é ele que precisa ser achado.

## 5. O que está Explicitamente fora de escopo ainda

- **O corpo do parceiro** (camada 3): o convidado não vê o host de pé no
  mapa. O mecanismo existe — o gerenciador tem 32 slots e a fase 1 usa menos
  de dez — mas um ator livre corre o risco de o motor reagir a ele.
- **A vida do jogador**: hoje é local em cada processo, com o PvP desligado.
  É uma decisão, não um acidente, e está escrita em `MODE.md`.
- **A reconexão**: se um par cai no meio da fase, a sessão acaba. É o
  comportamento natural e ainda não foi decidido o outro.
