[Voltar ao índice](README.md)

# Mundo, missão e ciclo de vida no coop como mod

Este documento propõe uma única campanha autoritativa no host, com adaptações no recomp. O convidado carrega recursos e mantém uma representação local para desenhar, receber input e apresentar resultados. Ele não decide uma segunda progressão de campanha. A proposta ainda exige investigação e implementação; não descreve funcionalidades já concluídas.

## Evidência e limites da leitura

A análise foi feita na cópia principal do projeto em 20/10/2026, consultando `docs/COOP-FINDINGS.md`, `src/game/coop_world*`, `src/game/coop_mission*` e `.agents/teamwork/coop/LAYOUT.md`. Esses arquivos podem avançar durante o trabalho de outros agentes. As referências abaixo são relativas à pasta `docs/coop-host-autoritativo`; pressupõem que a versão consultada do código esteja disponível no checkout. Ausência de um arquivo em outro checkout significa diferença de revisão, não prova de ausência no projeto.

Fatos observados:

- [`World::ApplyActivation`](../../src/game/coop_world.cpp) alinha a geração de um ator já existente ou aguarda o script local criá-lo. Isso não estabelece o instante autoritativo de nascimento.
- [`Mission::ApplyPending`](../../src/game/coop_mission.cpp) trata `ObjectInteracted` apenas com log nesse caminho. Um evento de objetivo não substitui a alteração física de uma porta.
- [`MissionFinished`](../../src/game/coop_mission_logic.cpp) considera concluída a missão quando todos os objetivos da tabela estão completos. A equivalência com a transição nativa, dificuldade e objetivos opcionais precisa ser comprovada.
- [`LAYOUT.md`](../../.agents/teamwork/coop/LAYOUT.md) registra que ativação parcial, sem controlador e cérebro, causou crash; registra também que bloquear eventos locais indiscriminadamente quebrou dependências de inicialização.
- O mesmo documento distingue `StLevel.bin`, com dados de stage, da tabela de objetivos em `LevelDat.bin`. Essa descoberta não demonstra que todo comportamento da fase se reduza à tabela e ao bus de eventos FE.

As anotações históricas divergem em alguns layouts e interpretações. Endereços e assinaturas só devem entrar no mod depois de conferidos no recomp da versão efetivamente usada. Este desenho evita depender de offsets discutidos.

## O mapa permanece local, a campanha pertence ao host

Na variante de renderização local, ambos os computadores precisam da geometria, texturas, animações e colisão necessárias para sua representação. Uma única campanha significa uma única autoridade sobre acontecimentos, não um único carregamento de arquivos entre duas máquinas.

O host conserva o estado canônico dos dois jogadores, inimigos, objetos modificáveis, gatilhos e objetivos. O cliente solicita ações e aplica resultados. O host deve avaliar a participação dos dois jogadores: não basta deixar a campanha reagir apenas à posição do protagonista original.

Transmitir apenas vídeo permitiria não carregar o mapa no convidado, mas exigiria câmera adicional, renderização, codificação e streaming no host. É outra arquitetura e fica fora desta proposta.

A camada gráfica da RenderWare organiza transformações, geometria e objetos renderizáveis. Um sprite ou billboard de aliado serve para apresentação; não substitui colisão, percepção da IA, dano ou participação em gatilhos. Os layouts específicos do BLACK e suas extensões devem ser confirmados; conceitos gerais da RenderWare não provam compatibilidade direta com suas estruturas internas.

## Fronteiras do mod

O recomp pode receber adaptadores que transformem decisões de campanha em comandos autoritativos e apresentação local. O cliente ainda precisa executar o ciclo de vida que cria recursos, atualiza animações e libera objetos.

Não se deve remover toda a IA ou o script do convidado em um único hook. A rotina de IA documentada também inclui locomoção, animação e limpeza. O objetivo é isolar decisões e efeitos de gameplay, preservando ou substituindo explicitamente as dependências necessárias. Até essa separação estar comprovada, a implementação existente deve permanecer como caminho experimental, e não ser rotulada como cliente de apresentação completo.

Cada fronteira precisa responder: quem decide, quem aplica, qual objeto é afetado, qual revisão está sendo aplicada e como reconstruí-la após perda de estado. Operações no motor devem ocorrer na thread e no contexto guest apropriados; recepção de rede enfileira comandos, não chama rotinas nativas arbitrariamente.

## Carregamento, streaming e barreira real

O MVP comporta dois jogadores na mesma área carregada da missão. Uma saída de área exige a presença dos dois ou uma regra explícita de reagrupamento; a distância máxima deve ser definida e medida durante a prova de conceito. Separação ampla só entra depois de mapear streaming, ativação e limites de memória.

No host, a seleção dos recursos relevantes deve considerar os dois participantes. No cliente, só se confirma prontidão depois de carregar o conjunto exigido para sua apresentação e resolver os descritores necessários. Renderizar um objeto fora da área ou aplicar dano a um ator sem recursos válidos não pode ser consequência normal de um pacote adiantado.

Fluxo proposto de início ou transição:

1. Host anuncia `start_epoch`, missão, dificuldade, versão do mod/protocolo e manifesto de recursos.
2. Cada lado resolve o conteúdo efetivo e prepara a área; reporta prontidão para aquela combinação de epoch e manifesto.
3. Gameplay permanece congelado: movimento autoritativo, dano, temporizadores de missão, gatilhos e spawn de campanha não avançam. Rede, carregamento e apresentação de espera continuam operacionais.
4. Host publica uma baseline completa; cliente aplica e confirma a revisão.
5. Host autoriza o início por mensagem confiável e idempotente. Só então o gameplay avança.

Uma flag de sala `Loading` sem bloquear o avanço do motor não implementa essa barreira. É pendência de engenharia localizar o ponto de pausa seguro e verificar se subsistemas usam relógios diferentes; zerar `delta time` em apenas uma rotina pode deixar temporizadores ativos.

O manifesto deve representar os arquivos realmente consumidos pelo resolvedor de assets: overlay, arquivos extraídos e fallback do ISO, respeitando a precedência real. Incluir identificador/hash do XBE, build do recomp/mod, esquema do manifesto, missão/dificuldade e hashes dos recursos relevantes. Comparar somente o nome da missão, `LevelDat.bin` ou um ISO que nem é a fonte efetiva permite conteúdo divergente. O manifesto detecta incompatibilidade; não transfere conteúdo automaticamente.

## Spawn completo e identidade

O host precisa publicar a existência e a remoção dos atores. O cliente não pode depender de atravessar o mesmo gatilho com seu jogador para receber o inimigo.

Proposta de comando: `SpawnEntity(entity_id, generation, archetype_id, descriptor_id, transform, initial_state, revision)`. O identificador canônico deve sobreviver à mudança de slot de memória; geração distingue reativações. Identidades de descritores são resolvidas no conteúdo local validado pelo manifesto. Nunca transmitir ponteiros, vtables ou blocos de memória guest.

O adaptador de spawn deve passar pelo caminho completo verificado: obter slot, construir/ativar, registrar no gerenciador e nas estruturas espaciais, associar controlador/cérebro quando exigidos e validar recursos. A rotina isolada de ativação não satisfaz esse contrato. Quais etapas são indispensáveis para um inimigo de apresentação e quais para um participante jogável são pendências distintas.

Manter um registro local `entity_id → handle válido` e tombstones para entidades removidas. Um snapshot antigo não ressuscita um ator morto; um evento atrasado de uma geração não afeta a geração seguinte. Se faltar recurso ou spawn falhar, o cliente pede recuperação e permanece fora do gameplay quando a entidade for essencial.

## Destruição, portas, colisão e itens

Cada objeto persistente recebe identidade estável por missão/área e instância de conteúdo, com revisão. Devem ser localizados os pontos nativos que aplicam cada transição, não apenas o evento FE produzido depois dela.

| Objeto | Estado autoritativo mínimo | Efeito que precisa ser aplicado |
|---|---|---|
| Porta | fechada/abrindo/aberta, trava, revisão | passagem, colisão, estado visual e dependências da missão |
| Destrutível | intacto/danificado/destruído, variante | malha/visibilidade, colisão e bloqueios de passagem |
| Item | disponível/coletado, destinatário | remoção, inventário e objetivo quando aplicável |
| Acionador | estado, ocorrência, pré-condições | alteração do cenário e avanço autorizado |

O cliente envia pedido de uso/coleta com jogador, objeto e comando único. O host verifica distância, disponibilidade e pré-condições na simulação canônica. Uma confirmação única produz inventário e transição; retransmissão não duplica recompensa nem objetivo. A regra de munição/vida compartilhada ou individual é decisão de design que deve ser fixada antes da implementação do pickup.

Destruição com impacto no combate deve concordar em colisão e passagem. Poeira, partículas e fragmentos sem consequência podem permanecer visuais locais. Um fragmento que causa dano, bloqueia passagem ou participa de um objetivo precisa de tratamento autoritativo.

## Gatilhos e objetivos

Para cada gatilho, definir explicitamente se ativa com qualquer jogador, todos os jogadores, um jogador elegível ou uma ação confirmada. O host avalia posições canônicas e registra a ocorrência para impedir duplicação. Eventos repetíveis exigem contador/identificador de ocorrência; comparar apenas nome e texto não identifica um acontecimento.

O bus FE é um caminho conhecido para eventos e atualização de objetivos. Não cobre por si só streaming, estado físico dos objetos, todos os temporizadores ou todo o roteiro da fase. O adaptador precisa investigar essas dependências separadamente.

Objetivos devem ter estado canônico por identidade, incluindo progresso relevante, versão e dificuldade. Replicar somente a quantidade concluída não distingue dois conjuntos diferentes de objetivos. O cliente apresenta os valores confirmados e aplica os efeitos nativos necessários de forma seletiva, sem reexecutar decisões independentes.

A conclusão deve vir da transição nativa comprovada ou de uma regra substituta explicitamente especificada e comparada com a campanha. Não presumir que todos os registros, inclusive opcionais, precisam estar completos.

## Checkpoint, morte e mudança de fase

Para o MVP, propor morte de qualquer participante ou falha de missão como interrupção do gameplay e retorno conjunto ao último checkpoint confirmado; vida de cada jogador continua individual. Sem checkpoint compatível demonstrado, reiniciar a área experimental nos dois processos. Reviver, substituição por bot e retorno individual são expansões posteriores, pois alteram regras de combate e missão.

Um checkpoint coop deve conter o estado persistente da campanha e os dois participantes, ou restaurar um checkpoint nativo acompanhado de uma restauração validada da camada coop. Restaurar apenas posição e vida deixa portas, itens e objetivos em outra história. A compatibilidade do save nativo com esse procedimento permanece pendente.

Ao reiniciar: entrar em barreira, invalidar comandos pendentes, carregar/restaurar o checkpoint, atribuir novo epoch, aplicar baseline e liberar os dois. Eventos anteriores nunca podem atingir o mundo restaurado.

A transição de fase é decisão do host, seguindo as pré-condições da campanha e a regra de presença dos jogadores. Após entrar em transição, bloquear novas ações de gameplay e executar a mesma barreira de recursos/baseline. Sem recuperação de conexão no MVP, perda do host encerra a sessão de forma explícita; migração de host não é requisito inicial.

## Snapshot de recuperação

Eventos descrevem mudanças; uma baseline descreve o estado persistente atual. Precisamos dos dois para recuperar perda de pacote, divergência ou reconstrução do cliente sem repetir toda a campanha desde o começo.

A baseline completa do escopo autoritativo acordado inclui epoch, tick, revisão, última sequência de evento incorporada, manifesto/área, estado dos jogadores, entidades existentes/gerações e remoções, portas, destrutíveis, itens, gatilhos consumidos/repetíveis, objetivos, checkpoint e fase da missão. Estados internos ainda não mapeados, como temporizadores que afetam campanha, precisam ser inventariados antes de prometer recuperação arbitrária.

Aplicar a baseline como transação em ponto seguro: preparar recursos, materializar entidades, aplicar estados persistentes, atualizar apresentação e só então aceitar eventos posteriores à sequência incorporada. O protocolo precisa evitar dano aplicado duas vezes quando o HP do snapshot já inclui o evento. Uma atualização parcial não confirma globalmente eventos de objetos que omitiu; precisa de revisão ou fronteira por entidade/escopo. Essa baseline é um modelo semântico versionado, não uma cópia de memória do host.

## Provas de aceitação propostas

Estas são verificações futuras, não testes executados neste documento:

- Host avança e convidado fica dentro da área permitida: o mesmo inimigo aparece sem depender do gatilho local do convidado.
- Qualquer jogador abre uma porta: ambos atravessam e os raycasts concordam com a nova colisão.
- Destruir cobertura muda visibilidade e tiros nos dois lados, inclusive após recuperação por baseline.
- Dois pedidos do mesmo pickup produzem a quantidade de recompensa definida, sem duplicação por retransmissão.
- Cada jogador ativa um gatilho elegível; gatilhos de presença conjunta aguardam os dois.
- Congelar carregamento de um lado impede avanço de inimigos, dano e temporizadores de campanha no outro, enquanto a rede permanece ativa.
- Perder/reordenar eventos e reconstruir o cliente restaura objetos e objetivos individuais, sem ressuscitar entidades.
- Reiniciar checkpoint e trocar de fase invalida todos os pacotes do epoch anterior.

Primeiro marco: dois participantes na mesma área enfrentam um inimigo canônico, qualquer um causa dano confirmado pelo host, uma porta e um destrutível atualizam a colisão, e um objetivo individual muda nos dois lados. Streaming distante e uma campanha inteira ficam condicionados à validade desse marco.
