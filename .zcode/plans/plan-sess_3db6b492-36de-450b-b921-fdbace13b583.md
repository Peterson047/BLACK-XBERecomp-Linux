# Plano: fechar o marco da ativação e devolver o portão ao verde

## Onde o processo está (corrigindo meu relatório anterior)

Meu relatório anterior estava defasado: ele descrevia um estado em que o co-op não
existia ainda no jogo. Isso era verdade às 19:00 e deixou de ser. Hoje o branch
`coop` tem **8 commits a mais** e a situação é outra:

| commit | o que entrou |
|---|---|
| `4c7c8a8` | a sessão, a camada de mundo e a camada de missão |
| `8468925` | o co-op entrou no jogo (`--coop`, `coop_adapter.cpp`, `menu.cpp:927`) |
| `429ac21` | o emparelhamento por posição, quando as gerações não batem |
| `1aef5e3` | **o co-op funciona entre duas instâncias de verdade** |
| `af05080` | a morte atravessa, com o estado do corpo que o dano precisa |
| `86ad862` | `docs/COOP-FINDINGS.md` — o que funciona e o que falta |
| `1ea9f17` | o painel de co-op dentro do jogo |
| `b7c0f8d` | o ator que o convidado não ativou é reportado, não falhado |

O portão de duas instâncias chegou a dar `epoch=1 snapshots=386 events=21
unpaired=0 divergent=0` — host e convidado de acordo. **A peça que faltava
(ativação dos atores) está em implementação agora, e é ela que quebrou o
portão.**

**E há uma complicação que preciso declarar:** outra sessão de agente está
trabalhando neste mesmo checkout. Ela editou 5 arquivos entre 21:05 e 21:06,
reconstruiu o binário às 21:07 e rodou o co-op em outra porta. A árvore está em
movimento enquanto eu trabalho.

## O que está quebrado agora

`tools/dev_check.sh coop` **falha** com o binário atual: o **convidado toma
SIGSEGV aos ~7,4 s**, logo depois de `coop: apply kind=activation`, e por isso
nunca escreve a linha `coop: summary` — o portão acusa "the guest log has no
'coop: summary' line". Foi reproduzido duas vezes, em portas diferentes
(15271 e 15199), então não é artefato de teste. O host fica bem; só o
convidado morre.

Os outros três portões estão verdes: `syntax` (12 TUs), `cpp` (5 testes
standalone, incluindo `test_coop_session`), `e2e` (121/121).

**Hipótese do crash** (a confirmar reproduzindo com a árvore atual): os dois
sinais são `0x100000008` e `0x100000090`, ou seja, `g_base + 8` e `g_base +
0x90` — os offsets `kActorKeyA` (0x08) e a terceira linha da base (0x90), lidos
com um **ator zero**. E `LAYOUT.md` §5 já avisa do mecanismo exato: *"um ator
criado só com a ativação não é um ator do motor"* — falta o controlador
(`actor+0x324` fica zero) e o cérebro, porque o caminho de spawn do motor
(call site 3, `sub_001DF440`) faz ativação **mais** `sub_001880C0`,
`sub_001899F0` e `sub_001D21F0`. A rotina de dano `0x001DF830` lê
`[controlador+0x90]` de um controlador nulo e mata o processo. O código novo
que li tem o caminho `waiting`/`adopted`/`took`, que é exatamente o desenho
certo — então é preciso **re-medir antes de consertar**, porque a outra
sessão pode já ter mudado isso.

## O plano

### Passo 0 — Re-medir (obrigatório antes de qualquer edição)
Recompilar e rodar os quatro portões com a árvore do momento, para trabalhar
sobre fatos e não sobre o instantâneo que li. Confirmar se o crash ainda
existe e em que linha morre (o handler já imprime o RIP do guest e o "last
indirect call").

### Passo 1 — Estabilizar a ativação (o que bloqueia)
1. Se o crash persistir: localizar a leitura com ator zero. Os candidatos que
   já tenho são `WriteActorState` (escreve as linhas da base em `actor+0x70/80/90`)
   e `FindLiveActor` (lê `actor+0x08`) — ambos assumem ator válido.
2. Corrigir a **premissa**, não o sintoma: o convidado **não cria o ator**. O
   script de nível do convidado cria o ator dele, no caminho inteiro do motor,
   e a palavra do host diz apenas **qual geração** aquele ator toma
   (`coop_world.cpp:616-706`, `ledger.generations` / `own`).
3. Blindar a invariante: no ramo `kAdopt`, `existing` tem de ser != 0 — hoje
   `W32(existing + kActorGeneration, ...)` escreveria em `g_base + 0x374` se
   `DecideActivation` devolvesse `kAdopt` com `existing == 0`. Guarda
   explícita + contador.
4. Ordem do dano: o dano de um ator que o convidado ainda não tem **espera**
   pelo ator (`kMaxActorWaits`), e não é descartado. Verificar que o
   `reason=created` que aparece no log do crash foi removido do caminho do
   convidado.
5. Critério de pronto: `tools/dev_check.sh coop` verde, com o convidado vivo
   até o fim e as duas linhas `coop: summary` batendo em `epoch`, `events`,
   `unpaired=0`, `divergent=0`.

### Passo 2 — Fechar os defeitos que o caminho novo expôs
Em ordem de dano, todos com teste que falha antes e passa depois:

1. **Ack perdido vira tempestade.** Em `coop_session.cpp:357-361` o convidado
   descarta o evento duplicado **sem responder**. Se um ack se perde, o host
   reenvia a cada 200 ms para sempre e o `dropped_events_` do convidado cresce
   sem parar. O duplicado tem de voltar a ser reconhecido com um novo ack.
2. **A inbox da missão é poluída pelo dano.** `coop_adapter.cpp` entrega todo
   evento ao mundo *e* à missão; `Mission::ApplyEvent` enfileira qualquer tipo
   (capacidade 64) e `ApplyPending` joga fora o `ActorDamaged` no `default`.
   64 acertos e o próximo evento de missão真 é descartado com
   `coop: error what=inbox_full`. Filtrar por `event_type` antes de enfileirar.
3. **`Mission::Pump()` toca memória do motor na thread errada.** O cabeçalho
   diz que tem de rodar na thread do guest; o adapter chama da thread de render
   (`menu.cpp:927`) e o hook chama da thread do guest. `ReadObjectives()` lê
   `world+0x930` sem trava, e `ended_`/`published_count_` são campos simples
   escritos dos dois lados. Escolher uma thread só.
4. **Vazamento de memória do guest.** `FireLocal` aloca 272 bytes do espaço do
   guest por evento replicado e nunca devolve (`coop_mission.cpp:430`).
5. **Throttle duplo do snapshot.** O adapter publica a 10 Hz e a sessão tem o
   próprio timer de 10 Hz: a latência efetiva vai de 50 a 200 ms.
6. **API morta.** `Phase::Ended` nunca acontece (só é lido); `SetReady` nunca é
   chamado pelo jogo, então `ready_mask_` é sempre 0 no fio; `Summary()` da
   sessão não é usada; `GetSessionId`/`GetMissionId` não têm chamada.
7. **O gancho de depuração da ativação** (`sub_001DA4F0`) despeja uma matriz
   de 16 floats por ativação, sem limite de taxa. Sai quando o diagnóstico
   fechar.

### Passo 3 — Documentos que estão mentindo
- `LAYOUT.md:241`: os descritores medidos são `{0, 29, 30, 36, 37}`, não
  `{0, 29, 30, 35, 37}`. Duas fontes independentes confirmam 36 (o campo
  `unit=` do motor e o `descriptor+0x04`).
- `PROJECT.md`: a lista de verificação tem 1 de 17 marcado, mas F0b, F0c e o
  começo de F1 já estão feitos e medidos. Marcar o que é verdade.
- `docs/COOP-FINDINGS.md`: reescrever com o estado medido depois do Passo 1.
- `PROTOCOL.md` §8b: o evento de ativação existe no código; o texto precisa
  bater com o que foi implementado (o campo `generation` no evento, o ledger).

### Passo 4 — Commits
Um commit por assunto, cada um com os portões verdes: a correção do crash
sozinha, os defeitos em pequenos, os documentos por último. Assim o trabalho
da outra sessão não se perde se ela commitar no meio — e o meu também não.

## O que eu **não** vou fazer

- **Camada 3 (corpo do parceiro).** `BODY.md` acabou de ser escrito e tem uma
  pergunta de design aberta (de onde vem a animação do ator remoto). Começar
  uma camada nova sem essa resposta é chute.
- **Mexer no desenho da ativação sem re-medir.** A outra sessão já trocou o
  desenho uma vez; eu mexo no que a medição apontar.
- **A camada 2 (missão) além do que já existe.** O objetivo é fechar o marco.

## Risco assumido

Outra sessão edita a mesma árvore. Mitigação: cada passo começa com
re-leitura do arquivo imediatamente antes da edição, as edições são cirúrgicas,
e nada é commitado sem os portões verdes. Se a outra sessão mudar o desenho da
ativação no meio, o Passo 0 se repete e o plano se ajusta.
