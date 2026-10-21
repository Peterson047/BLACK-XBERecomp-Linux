# O que a co-op já faz, e o que falta

Estado medido em duas instâncias de verdade do jogo (`tools/coop_run.sh`).
A especificação é `docs/COOP.md`; os offsets verificados são
`.agents/teamwork/coop/LAYOUT.md`; o contrato da rede é
`.agents/teamwork/coop/PROTOCOL.md`.

# O que a co-op faz, e o que falta

> A especificação é `docs/COOP.md`. Ela tem **seis afirmações erradas** sobre
> o motor, corrigidas aqui e em `LAYOUT.md` — leia estes dois antes de seguir
> a spec.

## 1. O que funciona

| peça | como se sabe |
|---|---|
| A sessão de rede | `tests/test_coop_session.cpp`: a barreira de carga, a sala, a sequência, o acuse de recibo, o timeout que sobrevive ao carregamento e duas threads publicando ao mesmo tempo |
| O formato da rede | `tests/test_coop_protocol.cpp`: ida e volta, os limites, o tamanho do datagrama |
| A camada de mundo | `tests/test_coop_world.cpp`: a identidade, o emparelhamento, a ordem dos eventos |
| A camada de missão | `tests/test_coop_mission.cpp`: o nome do evento, os objetivos, o fim da missão |
| As duas instâncias | `tools/dev_check.sh coop`: `epoch=1 snapshots=386 events=23 unpaired=0 divergent=0` |

O gate de log (`tests/coop/assert_coop_log.py`) compara as duas cópias do log:
a mesma rodada, nenhum ator sem par, nenhuma divergência, nenhum erro, as
sequências em ordem, o convidado nunca como fonte e a memória que o convidado
tomou do motor devolvida. Ele tem os seus próprios casos, porque um portão
que não pode falhar não é um portão.

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

## 4. A ativação dos atores (resolvida)

Medido numa execução real: às 25 s o convidado tinha **2** atores e o host
tinha um terceiro que o convidado nunca teve. No fim da execução, com o
nível todo carregado, os dois lados tinham **7** e casavam **7 de 7**.

Ou seja: **as duas cópias do level não ativam os mesmos objetos ao mesmo
tempo.** O script de level decide quem nasce, e ele reage a onde o jogador
está. As duas cópias do mundo não são o mesmo mundo enquanto isso não for
replicado.

**O que se descobriu** (as provas estão em `LAYOUT.md` §5):

- a ativação é `0x001DA4F0(actor, descriptor, third, ...)`: **nove**
  argumentos na pilha e o `third` no registrador EAX. A identidade do ator
  **não sai do descritor**: a rotina a recebe nos argumentos 4 e 5 e a
  escreve no ator (`actor+0x08` e `actor+0x0C`);
- o **descritor é uma unidade do level**, e as unidades moram na tabela do
  manager, em `manager + 0x7A20 + índice * 0x40`. A tabela é a mesma nos
  dois processos (o mesmo level carregado, o mesmo gerenciador no mesmo
  lugar da arena), e o **endereço do descritor é o mesmo nos dois** —
  verificado em execução de verdade;
- o caminho do spawn **não é só a ativação**: depois dela o motor ainda dá
  ao ator o seu lugar na tabela de controladores da IA e prepara o cérebro
  dele. Um ator criado só com a ativação fica sem o controlador e **crasha
  o convidado** na rotina de dano do motor.

**O que foi feito:**

- o host publica a ativação como um **evento** (`ActorActivated`), com a
  identidade, a geração, o tipo, a posição, a **posição do descritor na
  tabela de unidades** (e não o endereço, que só vale dentro de um
  processo), o `third`, os outros argumentos e o **conteúdo** do transform
  do spawn (ele mora na pilha de quem chamou);
- o convidado não cria o ator — deixa o script do level dele criar, pelo
  caminho do motor inteiro — e usa a palavra do host para dizer **qual
  geração** aquele ator tem. As gerações do host são uma fila, porque o
  host ativa o mesmo objeto mais de uma vez;
- o **dano** que chega para um ator que o convidado ainda não tem **espera**
  por ele, em vez de ser descartado. Sem isso a morte do host não cruzava:
  o convidado recebia o dano do host milissegundos antes de o script dele
  ativar o ator, e o dano morria no caminho.

**O que ainda não está resolvido:** o convidado continua nascendo ~2 s
depois do host, porque o script do level do convidado reage aonde o
jogador **dele** está. A replicação da ativação alinha a **identidade**
(qual ator é qual), não o **quando**.

## 5. A animação de osso: por que o corpo desliza congelado

As duas chamadas que o mundo já faz para colocar um ator no lugar —
`sub_001CE600` (a direção) e `sub_001CE660` (a posição) — **não tocam a
animação**. Não escrevem o registro de estado, não escolhem o clip, não
chamam o tick da animação. O corpo desliza pelo chão com a pose congelada:
é o que se vê quando os jogadores aparecem sem a animação de osso.

A animação de um personagem é:

```
ator
 ├─ +0x328   o controlador de animação (outro objeto)
 ├─ +0x32C   o registro de estado (5 campos)   <- o que a rede escreve
 ├─ +0x324   o controlador de IA (outro objeto) <- tem que não ser nulo
 └─ +0x278   o corpo de movimento
```

**A decisão é do lado da IA, a execução é do lado da animação.** O número
do estado escolhe o tipo de locomoção, o conjunto de animação do tipo de
personagem escolhe o clip, e o controlador de animação só toca: faz a
transição, avança e escreve os onze ossos. Então quem escreve `actor+0x32C`
decide a pose — é aí que a rede entra.

A tabela foi **medida no jogo de verdade**
(`.agents/teamwork/coop/anim/STATE_TABLE.md`):

| o quê | o estado | o clip |
|---|---|---|
| a pose de pé | 1 | `SH_AR_S_540/541/560` e companhia |
| a mira | 13 (`0x0D`) | `SH_AR_S_150/151/160/161`, `SH_AR_C_160/161` |
| a arma pronta / no ombro | 10 e 11 | `SH_AR_U_101`, `SH_AR_U_SHO` |
| o disparo | 7 | o estado que a arma escreve |

**O registro não carrega a velocidade.** Os quatro parâmetros ficaram
constantes enquanto o personagem andava; o que muda é o *estado*. E o clip
da caminhada (`SH_AR_S_MOV`) **não vem do registro**: vem da escolha do
próprio conjunto de animação (`0x0017E280`), que olha o corpo de movimento
(`actor+0x278`).

Ou seja: **o registro diz o que o corpo está fazendo, e o conjunto de animação
diz como ele está se movendo.** Por isso o corpo do parceiro precisa de duas
coisas — o registro com a pose, e o movimento — ou o clip pedido pelo nome
(`0x0017F990`).

Três coisas que a investigação mediu e que mudam o desenho:

- **Sem arma, o estado é respeitado.** Um ator sem arma (`actor+0x29C = 0`)
  vai direto à escolha do clip, porque o caminho do corpo e da arma é o que
  sobrescreveria o estado. É o caminho mais curto da rede até os ossos.
- **Sem controlador de IA, o ator vivo não anima.** O tick do ator volta
  antes do tick da animação quando `actor+0x324` é nulo. O corpo precisa de
  um slot do gerenciador de IA com o índice do cérebro em −1, para a IA não
  pensar.
- **Com o par de identidade em 0/0, o corpo não é um objeto de script.** O
  script nunca pega o handle dele, e o matcher de objetivos, que só percorre
  o bus de eventos e a tabela de objetivos, nunca o casa. A missão não vê o
  corpo.

## 6. O que o portão encontrou (a corrida e os enganos)

O portão das duas instâncias é o que mede o comportamento de verdade, e ele
achou coisas que a leitura do código não achou. Todas estão corrigidas:

- **A sessão publicava de duas threads ao mesmo tempo.** O dano de um ator é
  publicado da thread do motor, dentro da rotina de dano, e os eventos do
  script do level são publicados da thread que desenha. A lista do que não
  foi acuse de recibo e o número da sequência são compartilhados, e não
  estavam sob uma trava: duas threads empurrando para o mesmo vetor
  corrompem o heap. O teste que publica de duas threads ao mesmo tempo
  aborta com `double free or corruption` contra o código antigo.
- **O convidado descartava o evento repetido sem responder.** O host reenvia
  o que não viu ser acuse de recibo, então um acuse perdido virava um
  reenvio eterno, e o contador de eventos descartados crescia sem parar.
- **A inbox da missão encheu com dano.** A missão enfileirava todo evento do
  host e jogava fora o que não usava; o dano de um ator ocupava o lugar do
  evento de uma missão, e sessenta e quatro acertos eram suficientes para
  empurrar o evento de uma missão para fora.
- **O convidado vazava a memória do motor.** Cada evento replicado tomava
  272 bytes do espaço do guest e não devolvia. O motor copia o nome, o texto
  e a posição para o slot dele, então o bloco pode voltar para a heap do
  guest. A missão conta o que tomou e o que devolveu, e o portão compara os
  dois números.
- **O gancho do frame tick da missão era código morto.** A função que só
  incrementa o contador de frames (`0x000BE550`) nunca é chamada: quem
  conta os frames é o laço principal (`0x0020F6A0`). Hookar a função morta
  não dava tick nenhum, e a missão dependia de outro caminho.
- **Um evento existia fora da rodada.** O host publicava a mudança do
  estado dos objetivos enquanto o level ainda carregava; o convidado aplicava
  e contava, e o host não. As duas sides discordavam do número de eventos e
  o portão acusava divergência onde não havia.
- **O próprio portão media a execução errada.** Ele escrevia em caminhos
  fixos de `/tmp` e usava uma porta e um display X fixos: duas execuções ao
  mesmo tempo se misturavam, e uma derrubava a janela da outra no meio da
  fase. Agora cada execução tem um diretório temporário, uma porta e um
  display seus.

## 6b. O crash: a corrida entre duas threads

O co-op morria em cerca de **um run em quatro**, com o heap corrompido. A
assinatura enganava: o crash caía no **cache de shaders** do renderizador
(`src/d3d/d3d8.cpp:104`) e no **texto dos prompts**
(`src/game/prompts.cpp:44`), a léguas do código que o causou. Um heap
corrompido aparece longe da causa.

A causa: a camada de mundo e a de missão guardam estado que **duas threads**
tocam — a thread do motor publica o dano e as ativações, e a thread que
desenha monta o snapshot e aplica o que chega. O pior era
`World::host_activations_`: um `std::vector` de `std::vector`, crescido pelo
script do level do convidado na thread do motor **e** pela palavra de
ativação do host chegando na thread de desenho. Dois threads empurrando no
mesmo vetor corrompem o heap. Era o mesmo bug que a sessão tinha, um vetor
mais abaixo.

A correção: contadores e flags atômicos, os contêineres compartilhados sob o
lock, e o publicador **copiado** sob o lock e **chamado** com ele solto, para
a ordem de locks nunca cruzar (mundo → sessão → rede).

E uma correção de segurança de memória de verdade: o *set-position* do motor
escreve na matriz do ator **e na sua caixa**, então só é chamado num ator
**vivo**. O slot de um ator morto ou reciclado pode já pertencer a outro
objeto.

O segundo crash, pego no gdb: o convidado morre no *attach* da animação do
motor (`0x0017E420`) lendo `+0x14` do **descriptor** do ator
(`actor+0x320`) sem teste de nulo — o descriptor é zero. É o mesmo crash do
corpo do parceiro, e ele diz o que falta: **o corpo precisa de uma unidade do
level de verdade**.

## 6c. A revisão

Um agente leu o código do co-op commitado e procurou o que estava errado.
Onze achados, e a maioria era séria:

- `StartRound` limpava a lista de eventos não-confirmados e zerava o
  contador de sequência **sem o lock**, enquanto a thread do motor publica
  sob ele — a mesma corrupção de heap que já tinha sido paga uma vez, e
  ainda podia rolar a sequência para trás e fazer o convidado descartar um
  evento para sempre;
- o dano que ficava esperando o ator era reenfileirado **no fim**, e o
  convidado aplicava os eventos fora de ordem — e a cadeia de morte do
  motor não é segura fora de ordem;
- o casamento do "gêmeo" na camada de missão olhava só a **cabeça** da fila:
  um evento local sem par envenenava a fila para sempre, o convidado
  contava cada objetivo **duas vezes** e terminava a fase sozinho;
- o hash do nome cobria `length-1` bytes; o motor hasha o nome inteiro;
- o contador de divergência contava um buraco que o reenvio fechava, e o
  gate reprovava a rodada por isso;
- o *fallback* do dano casava atores pelo **ponto de impacto**, que não é
  onde o ator está;
- a barreira de carga dependia de **um único datagrama**: perdido, o par
  ficava no lobby para sempre, e o timeout não salvava, porque o convidado
  continuava dando ping;
- um snapshot que não cabia era enviado **truncado**, e os atores que
  faltavam ficavam faltando para sempre.

E uma correção de fundo: **a memória do motor é lida na thread do motor e
copiada** — a thread que desenha trabalha sobre a cópia.

O que a revisão confirmou como certo: **todos os offsets** do motor
(reverificados um a um no código recompilado), e as invariantes do
protocolo — o convidado nunca é fonte, o lock nunca é segurado durante um
envio, um evento repetido é aplicado uma vez e reconfirmado, a camada de
mundo nunca escreve num ator morto ou reciclado, e as guardas de
`Current()` impedem que a memória do motor seja tocada na thread de
desenho.

## 7. O que está Explicitamente fora de escopo ainda

- **O corpo do parceiro** (camada 3): o corpo **existe e anima** — ele
  nasce com o seu próprio controlador de animação, escolhe o seu clip, e
  troca para o clip do disparo (`SH_AR_S_103`, estado 7) no instante em que o
  tiro do parceiro chega. O que falta é ele **aparecer em cena**: ele é feito
  à mão e não entra na lista de vivos do motor, então o motor não o desenha.
  O caminho é o spawn nativo do personagem, com uma identidade que nenhum
  objeto de script usa — assim o script nunca pega o handle do corpo e o
  matcher de objetivos nunca o casa.
- **A vida do jogador**: hoje é local em cada processo, com o PvP desligado.
  É uma decisão, não um acidente, e está escrita em `MODE.md`.
- **A reconexão**: se um par cai no meio da fase, a sessão acaba. É o
  comportamento natural e ainda não foi decidido o outro.
- **O tempo**: o convidado continua nascendo uns dois segundos depois do
  host, porque o script do level do convidado reage a onde o jogador **dele**
  está. A replicação da ativação alinha a **identidade** (qual ator é qual),
  não o **quando**.
- **A reconexão**: se um par cai no meio da fase, a sessão acaba. É o
  comportamento natural e ainda não foi decidido o outro.
