# Cooperativo: do single-player ao co-op

Plano para adaptar a fase 1 (Veblensk) de single-player para co-op. A fase 1
é o modelo: o que ela resolver de forma confiável vira o padrão que as demais
fases reusam.

> **Onde o trabalho está.** Este documento foi escrito com a árvore no estado
> do co-op (sala, autoridade de IA, transporte de dano). Esse estado foi
> preservado e está no branch `archive/coop-2026-10-20` (commit `bf215e3` no
> repositório do jogo, `05c100e` no submódulo `sdk`). A branch `coop` voltou
> para `54ee274`, que é o MVP do Deathmatch mais a suíte de testes.

## 1. De onde viemos

**O MVP (`286bda2` + `e9fd457` + `d5cfb53`)** é Deathmatch, não co-op. O que ele
entregou e que o co-op reusa:

| Peça do MVP | Onde está | Serve para o co-op? |
|---|---|---|
| Protocolo UDP (join/ack/estado/tiro/chat/ping) | `src/net/network.{h,cpp}` | Sim, é a base do transporte |
| Vínculo de ator por jogador e por geração | `actor_instances_` em `multiplayer.cpp` | Sim, é o mecanismo de identidade |
| Escrita de pose de ator (`WriteActorTransformMatrix`) | `multiplayer.cpp` | Sim, é o espelhamento |
| Clips nativos de animação e hitboxes | `hooks.cpp` | Sim, para o corpo do parceiro |
| Lançamento direto da fase | `DirectMapLaunchTick` | Sim, é a entrada no co-op |
| Estacionar os inimigos da campanha | `ClearCampaignEnemies` | **Não** — no co-op os inimigos são o conteúdo |
| Morte, respawn e placar no host | `NetworkManager` | Parcial — no co-op a vida do inimigo é do host, a do jogador é local |

## 2. O problema central

Black é single-player e a fase 1 é um **roteiro**: o script do level
(`StLevel.bin`, que vem do ISO de varejo — o pacote `level_mp00` só troca
geometria, colisão, destrutíveis, palco, armas e as tabelas de fala) decide
quem nasce, quando, o que cada morte dispara e quando a missão termina. O
jogador é um corpo: `world+0x30`, com um controlador próprio.

Adaptar isso a co-op exige responder a três perguntas:

1. **Quem possui o mundo?** Os inimigos, as portas, os destrutíveis e os
   objetivos são uma coisa só, mas cada processo roda a fase inteira.
2. **Quem possui cada jogador?** Um corpo de jogador por processo é o
   protagonista da campanha; ele não pode ser duplicado sem reimplementar
   movimento, câmera, arma e animação.
3. **O que faz a missão "terminar" nos dois lados?** O fim é nativo e reativo a
   eventos do script.

### A decisão de arquitetura

> **Cópias sincronizadas, não mundo fundido.** Cada processo roda a fase de
> varejo por inteiro. O host é a autoridade do estado do mundo de que o script
> depende (IA, transform, vida dos atores). O convidado é uma réplica desse
> estado: roda o mesmo script, mas as decisões e a vida dos atores chegam do
> host.

É o oposto do que o Deathmatch faz, e é o que torna o trabalho viável:

- O convidado **não** precisa de um segundo corpo controlado pela rede: cada
  jogador controla o seu protagonista nativo, na sua cópia do mundo. Ninguém
  precisa simular a locomoção, a câmera e a arma de outra pessoa.
- O script do convidado reage aos mesmos eventos porque os atores morrem nos
  dois lados ao mesmo tempo (o dano é replicado) e as interações são replicadas.
- A alternativa (um mundo só, o host com o corpo do convidado) exige replicar
  *entrada* e simular um segundo protagonista com a cadeia nativa — o
  protagonista em Black não é um ator comum, é um objeto com controlador,
  câmera e arma. É ordens de grandeza mais caro.

O custo, aceito de saída: os dois jogadores não se veem, não colidem entre si
e não atiram um no outro. Para uma campanha co-op isso é aceitável na primeira
versão; ver a camada 3.

## 3. As quatro camadas do padrão

| Camada | O que precisa ser igual nos dois lados | Estado no branch arquivado |
|---|---|---|
| **0. Mundo** | Quais atores existem, onde estão, o que decidem | **Feito** (host decide, convidado espelha) |
| **1. Vida** | A vida e a morte dos inimigos | **Quase**: o dano é replicado; falta a morte |
| **2. Missão** | Objetivo, interações, fim da fase | **Não feito**: só as mortes |
| **3. Corpos** | O jogador vê o parceiro | **Não feito** |

### Camada 0 — Mundo (resolvida)

O host decide a IA dos atores da campanha e publica a transform; o convidado
espelha e não decide. Os pontos de ancoragem estão confirmados no disassembly:

- Gerenciador de atores: `0x002D1980`; lista em `+0x79BC`, contador em
  `+0x79B8`, capacidade em `+0x79B4`. O bloco de atores é `0x002D12E8`, 32
  posições de `0x3C0` bytes, vtable `0x28C1E8`.
- Identidade nativa do ator: o par `actor+8`/`actor+C`, o descriptor em
  `actor+0x320`, o serial em `actor+0x374`, o tipo em `actor+0xC4`, o id de
  censor em `actor+0x39C`.
- Controladores de IA: `ai_manager+0x2B00`, stride `0x1FE0`, 16 slots, vtable
  `0x28BEF0`; o ator do controlador em `controller+0x8C`. O motor casa ator e
  controlador por `actor+0x39C` e confirma com `controller+0x280 != -1`.
- Rotina de decisão: `0x001E5EF0`, chamada de `0x001E5F70` com o controlador em
  EAX. No convidado o plano é substituído e a física e a animação continuam.

Medido em duas instâncias: 242 de 484 controladores visitados casam com o host
por janela de 2 s, e o ator do convidado fica a 0,05 unidade da pose do host.

### Camada 1 — Vida

O host tem a vida; o convidado reproduz. O caminho já existe:

`0x001DF830` (dano, reduz `actor+0x2F0`) → `0x002057D0` (morte). O host publica
o evento confirmado (com número de sequência e retransmissão) e o convidado
reaplica a mesma chamada nativa no seu ator.

Validado uma vez, de ponta a ponta: o convidado atirou, o host acertou o inimigo
`type=1`, o HP foi de 100 para 75 no host e o convidado reproduziu o mesmo
evento no ator dele.

Falta:

- A **morte** chegar a zero e rodar a cadeia nativa no convidado. O replay
  tem uma guarda (`health <= expected_health` descarta) que protege contra
  evento duplicado, mas precisa ser exercitada até o zero.
- A **contagem de mortes** e o crédito: quem matou o quê. Para o co-op da
  campanha importa que a contagem do *script* avance igual nos dois lados; a
  atribuição a um jogador específico continua em aberto.
- A vida do **jogador**: hoje é local em cada processo. Aceitável na primeira
  versão (PvP desligado), mas tem que ser decisão explícita, não acidente.

### Camada 2 — Missão (a que falta, e a que decide "zerar a fase")

O script do level reage a eventos. Os que a fase 1 usa, cruzados com as strings
de objetivo do ISO:

| Evento | Replicado hoje? | Observação |
|---|---|---|
| Morte de inimigo | Sim (dano) | Falta confirmar a morte e a reação do script |
| Interação (porta, destrutível) | **Não** | É o próximo item obrigatório |
| Coleta de item | **Não** | Depende do inventário de objetivos |
| Timers do script | Não precisa | Os dois processos começam a fase juntos |

O cruzamento das strings de `Language/Strings/MainUK.bin` com
`Levels/Level_00/LevelDat.bin` já identificou os objetivos da fase 1: encontro
com o Black Cell (`0x4E220`, `0x4E250`), captura de suspeitos Seventh Wave
(`0x4E238`, `0x4E298`), ataque ao museu (`0x4E268`), eliminação de reforços
(`0x4E280`), foto aérea (`0x4E2B0`), Uzi escondida (`0x4E2C8`) e contador de
destruição (`0x4E310`). Os textos provam a existência dos objetivos; os
*callbacks* e as condições de conclusão ainda não foram mapeados.

Sem as interações replicadas, o convidado fica esperando uma porta que só o
host abriu: a missão não termina nos dois lados. Por isso a camada 2 é o
bloqueio real, e não os inimigos.

### Camada 3 — Corpos

O convidado não vê o host e o host não vê o convidado. O mecanismo do MVP já
existe (vínculo de ator, escrita de pose, clips nativos), mas no Deathmatch os
atores da campanha eram estacionados para virar os corpos dos jogadores. No
co-op os atores da campanha são o conteúdo e não podem ser reciclados.

A via que faz sentido: o gerenciador tem 32 posições e a fase 1 usa menos de
dez, então há posições livres. O corpo do parceiro é um ator livre que o motor
não trata como ator de missão, dirigido pela transform do outro processo. O
risco é o motor reagir a esse ator (IA, spawn, trigger) — o `0x001DA4F0` é o
único caminho de ativação dos três, e é observável, o que permite excluir o
ator do parceiro.

## 4. Sequência de trabalho

**F0 — Consolidar a base** (feito no branch arquivado)

- A reaplicação da pose do host depois do tick nativo do ator: o espelhamento
  era escrito no início do frame e o tick nativo do convidado o sobrescrevia no
  mesmo frame. Agora é escrito de novo depois, como os puppets do Deathmatch.
  Medido: 0,05 unidade de desvio.
- O teto de 256 atores: o cache do host guardava os *tombstones* para sempre e a
  validação era do lote inteiro. Um id novo com o cache cheio derrubava todo o
  lote, inclusive as posições dos atores vivos. Agora o tombstone é a única
  coisa que sai, e um ator ruim não derruba os outros.
- A detecção de alvo do raycast: o retorno nativo é um booleano e o objeto
  atingido fica em `out+0x24`. O código checava o retorno, e um tiro que
  acerta o cenário era lido como acerto. Agora o objeto é conferido.

**F1 — A morte e o objetivo**

Fazer o dano chegar a zero e a cadeia nativa de morte rodar no convidado, e
conferir que o script avança igual nos dois lados. Verificável sem jogador
humano: um harness aponta a mira pelo `yaw`/`pitch` do próprio controlador (o
mesmo caminho do mouse, sem mover o jogador) e puxa o disparador.

**F2 — As interações**

Mapear o que o script reage além das mortes. O caminho de entrada é o raycast
(`0x001DAFF0`) e a função de dano (`0x001DF830`), que já são hookados: o
interagir é um raio que acerta um objeto com tipo próprio. O evento replicado
precisa carregar o id do objeto e o tipo, para que o convidado aplique a mesma
transformação local e o script reaja igual.

**F3 — O corpo do parceiro**

Vincular um ator livre ao jogador remoto e replicar a transform e o estado de
animação. A base é o MVP.

**F4 — Acabamento**

HUD com o estado do parceiro, chat, reconexão, e a decisão explícita sobre a
vida de cada jogador.

## 5. O que ainda não sabemos

- Os callbacks e as condições de conclusão dos objetivos da fase 1. As strings
  foram achadas; os gatilhos não.
- Qual evento do script dispara cada objetivo, e em que ordem. É o inventário
  que F2 tem que produzir.
- Se o script da fase reage a algo além de mortes, interações e timers.
- Quanto as duas cópias do script divergem na prática. A identidade dos atores
  protege a camada 0, mas nada ainda protege a camada 2.

## 6. Riscos

- **Divergência do script.** Cada processo roda a fase inteira. Se um dos lados
  perder um evento, a missão para num lado. A mitigação é a mesma da camada 0:
  o convidado nunca é a fonte, só reage.
- **A identidade do ator é frágil.** O par `+8`/`+C` com a geração e a posição
  de spawn. Um spawn que não bate deixa o ator sem par e o lado diverge. Hoje
  isso é silencioso; precisa de contador visível.
- **A barra de carga é a única barreira real.** O mundo é replicado a 10 Hz sem
  interpolação: a posição do ator do convidado é um degrau a 10 Hz.
- **Suíte E2E com falha neste commit.** Os testes do "clean dummy speech bank"
  esperam um `speech.slb` de 104 bytes, mas o pacote `level_mp00` tem o de
  6944 bytes (o de varejo, com as tabelas de recursos que o loader exige). A
  incompatibilidade é do commit com os assets atuais, não do código.
