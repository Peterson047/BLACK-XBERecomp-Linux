[Voltar ao índice](README.md)

# Rede, consistência e validação da campanha única

Status: **proposta de arquitetura**, baseada na leitura da implementação atual. As mensagens e invariantes abaixo ainda não estão implementadas. O MVP considera **um host e um convidado**.

## O que muda

O host executa uma única campanha com dois jogadores lógicos. O convidado carrega os recursos do cenário para renderizar, reproduzir apresentação e, quando necessário, consultar colisão local. Ele não decide spawns, dano, destruição ou progresso de missão. Carregar o mapa em dois computadores continua necessário quando ambos renderizam; deixa de ser necessário manter duas campanhas independentes concordando.

A rede não sincroniza arquivos de mapa continuamente. Ela transporta comandos dos jogadores e alterações do estado dinâmico. O modelo alternativo em que apenas o host carrega e renderiza o mapa exigiria transmitir vídeo de uma segunda câmera; isso é outra arquitetura e fica fora desta proposta.

O sprite ou billboard do aliado é uma escolha de apresentação. Seu identificador de rede aponta para uma entidade lógica com colisão, equipe, vida e participação na IA, mesmo quando o desenho é simples.

## Evidências da implementação atual

| Confirmado por leitura | Fonte | Consequência para a proposta |
|---|---|---|
| Há mensagens separadas para sala, carregamento, snapshots, eventos e ACK. | `src/net/coop_protocol.h`, `src/net/coop_session.cpp` | Reaproveitar a divisão e versionar o formato para os novos campos. |
| Eventos têm epoch e sequência, e eventos fora de ordem aguardam sua vez. | `CoopSession::OnCoopEvent`, `DrainPending` | É uma base útil, mas precisa de limites, recuperação e confirmação de duplicatas. |
| Snapshots têm epoch e campos de partes, mas não têm `snapshot_seq`, tick de simulação ou fronteira de eventos. | `SnapshotHeader`; `SendSnapshot` atualmente força uma parte | Não há informação suficiente para ordenar fotografias da mesma rodada nem conciliar HP com dano. |
| `PublishRoom` evita enviar conteúdo idêntico ao último publicado. | `CoopSession::PublishRoom` | O temporizador periódico não garante reenvio da mesma revisão perdida. |
| `SetLoaded` do convidado envia `Ready` naquele momento, sem reenvio periódico próprio. | `SetLoaded`, `SendReady`, `Update` | Uma confirmação de carregamento perdida pode bloquear a barreira. |
| Evento já aplicado retorna antes de produzir ACK. | `CoopSession::OnCoopEvent` | A perda do ACK original pode deixar o host retransmitindo uma mensagem já consumida. |
| O ACK apaga uma fila global e ignora `player_id`. | `CoopSession::OnCoopAck` | Um convidado funciona como escopo inicial; vários exigem confirmação independente. |
| No tratamento de `Shoot` recebido pelo host, o código marca flash e retransmite. | `src/net/network.cpp`, ramo `MsgType::Shoot` | Esse caminho não demonstra resolução autoritativa contra inimigos da campanha. |
| O adaptador inicia carregamentos e aplica eventos à camada de mundo e à de missão; usa existência de objetivos como sinal de carregamento. | `src/game/coop_adapter.cpp` | O novo modo precisa de uma barreira que também confirme a réplica pronta e seus recursos necessários. |

Essas observações são da cópia de trabalho examinada, que outro agente pode alterar. Não são uma garantia sobre versões futuras. `docs/COOP-FINDINGS.md` registra testes anteriores e limitações da ativação nativa; o sucesso ali descrito não comprova a nova arquitetura.

## Identidade e envelope de sessão

Toda mensagem que produz efeitos deve carregar uma versão de protocolo, `session_id` e `start_epoch`. O host atribui os jogadores e associa cada `player_id` ao peer aceito; receber um identificador no pacote não autoriza um peer a controlar outro jogador. O convidado aceita estado autoritativo apenas do host daquela sessão.

O epoch distingue carregamento de missão, reinício e checkpoint restaurado, conforme regra explícita de implementação. Pacotes antigos nunca podem reativar um objeto ou executar uma ação na missão seguinte. A barreira de carregamento deve se referir ao epoch que está sendo preparado, evitando depender de uma mudança implícita entre `Loading` e `InMission`.

Cada entidade possui `entity_id` estável durante sua vida na sessão, mais `generation` para reuso de slots. A camada host mantém um registro que traduz esse par para atores, controladores e objetos nativos. Ponteiros e endereços de memória nunca servem como identidade de rede. A dupla nativa de chave e geração já utilizada pode alimentar o registro quando sua validade estiver demonstrada; não se deve presumir que a ordem de ativação seja igual entre processos.

Armas, descritores de spawn e objetos estáticos usam IDs definidos por catálogo ou manifesto compatível. O handshake verifica versão do mod, missão e assinatura dos recursos de gameplay relevantes. Saber que ambos possuem um arquivo chamado `LevelDat.bin` não basta para garantir o mesmo conteúdo.

## Comandos do convidado para o host

| Mensagem proposta | Campos essenciais | Regra do host |
|---|---|---|
| `PlayerInput` | sessão, epoch, `player_id`, `input_seq`, `client_tick`, movimento, mira e botões | Consumir cada comando no máximo uma vez; verificar intervalo temporal e limites. |
| `ShotRequest` | contexto acima, `shot_seq`, `input_seq` associado, arma estável, direção e tick pretendido | Verificar estado, munição, cadência e pose do jogador; resolver no mundo do host. |
| `InteractionRequest` | `request_seq`, jogador, alvo estável e geração, tipo de ação | Verificar distância e possibilidade da ação na campanha; confirmar ou negar. |
| `LoadedState` | epoch preparado, revisão, recursos carregados e baseline aplicado | Confirmar explicitamente a revisão aceita. |
| `BaselineRequest` | último snapshot/evento válido e motivo | Recuperar lacunas sem exigir nova conexão. |

São dois domínios de sequência: entrada contínua e ações discretas. Se o disparo vier também como botão no input, ele deve compartilhar o mesmo `shot_seq` ou ter um único caminho de produção; não pode criar dois tiros. Ações discretas podem ser retransmitidas até confirmação, sempre com deduplicação. Inputs podem incluir uma janela curta de comandos recentes para tolerar perdas; inputs vencidos não ficam bloqueando movimento novo indefinidamente.

A origem fornecida pelo cliente ajuda diagnóstico, mas o host deriva a origem efetiva da entidade lógica autorizada. Compensação de latência por histórico de posições seria uma etapa posterior, com janela limitada; não é requisito do primeiro protótipo.

## Estado enviado pelo host

**Eventos confirmados:** sessão, epoch, `event_seq`, `server_tick`, identidade do alvo, identidade do jogador causador e dados do resultado. Exemplos: spawn/despawn, acerto, morte, porta aberta, cobertura destruída, item coletado e objetivo alterado. Informar o `shot_seq` original permite associar resposta e pedido.

**Snapshots:** sessão, epoch, `server_tick`, `snapshot_seq`, `last_event_seq` incorporado ao escopo declarado, identidade da baseline, partes e contagens. O conteúdo contém jogadores e entidades relevantes, vida, estados duráveis de objetos e revisões de missão. Quando houver reconciliação de movimento, inclui `last_processed_input_seq` por jogador.

Uma fotografia multipartes precisa compartilhar o mesmo identificador e uma fronteira consistente de simulação. Campos `part_index` e `part_count` isolados não tornam o conjunto atômico. Partes antigas ou incompletas expiram; o receptor não mistura partes de fotografias distintas. O tamanho dos datagramas deve ser calculado incluindo todos os cabeçalhos e validado contra o orçamento de MTU escolhido. O limite atual de 24 atores não é um limite seguro de campanha: exige particionamento ou seleção explícita, sem desaparecimento silencioso de entidades.

## HP e efeitos sem aplicar dano duas vezes

Uma baseline completa é o estado de todo o escopo autoritativo acordado após todos os eventos até `last_event_seq = K`. Sua instalação confirmada permite avançar a fronteira global até K. Um snapshot parcial de entidades não avança essa fronteira global: ele precisa declarar seu escopo e carregar revisão/fronteira por entidade ou componente. Omitir uma porta não autoriza descartar o evento que a abriu. Eventos de múltiplas entidades só podem ser considerados incorporados quando todos os efeitos relevantes estiverem cobertos. Dentro do escopo coberto, eventos já incorporados não subtraem vida novamente; eventos posteriores continuam a partir dessa fronteira. Amostras antigas não podem sobrescrever resultados recentes.

Exemplo: o ator tinha 100 HP, o evento 42 causou 25 de dano e o snapshot informa 75 HP com fronteira 42. Receber o evento 42 depois não leva a vida para 50. Receber uma fotografia anterior à morte também não ressuscita o ator.

É necessário separar **estado de gameplay aplicado**, **eventos de apresentação consumidos** e **recebimento de transporte**. Um ACK cumulativo não pode afirmar que uma lacuna foi processada apenas porque chegou um pacote mais novo. Após instalar uma baseline completa, o cliente pode confirmar explicitamente que passou a representar sua fronteira K. Efeitos cosméticos antigos podem ser descartados; acertos recentes podem ser exibidos uma vez por ID. A confirmação deve representar uma dessas regras de forma inequívoca.

As rotinas nativas de dano também podem emitir eventos, liberar objetos e produzir morte. Reexecutá-las depois de escrever HP não é uma operação cosmética. O adaptador deve oferecer uma aplicação de transição idempotente, sem republicar eventos recebidos, e identificar quais efeitos nativos são indispensáveis. Se isso ainda não puder ser isolado, eventos causais e aplicação da fotografia precisam ser reconciliados sob uma única rotina; não é seguro manter dois caminhos independentes alterando HP.

## Controle confiável e filas limitadas

Sala, preparação de missão, carregamento, mudança de fase e baseline precisam de confirmação. O host retransmite a revisão atual mesmo quando seu conteúdo não mudou, até o peer confirmar ou até uma política explícita de timeout. O convidado repete `loaded` até confirmação. Uma duplicata válida produz novamente o ACK mais recente, sem repetir o efeito.

O MVP deve guardar o ACK do único convidado separado do estado local do host. A futura extensão para mais convidados mantém ACK, fila e baseline por peer; confirmar em um não libera dados ainda necessários ao outro.

Definir antes da implementação limites em bytes, quantidade e tempo para: eventos sem ACK, janela de reordenação, inputs, deduplicação de pedidos, partes de snapshot e entidades aguardando criação. Uma lacuna persistente aciona pedido de baseline após prazo limitado. Um peer lento não pode provocar crescimento ilimitado nem congelar o host. Snapshots intermediários podem ser substituídos pelo mais recente; eventos duráveis exigem recuperação consistente, em vez de descarte silencioso. Retransmissão tem orçamento de banda e prioridade sobre efeitos cosméticos, preservando uma cota para comandos e controle.

O estado da sessão distingue atraso tolerado, recuperação e falha. Valores de timeout e capacidade são parâmetros medidos no jogo, não constantes escolhidas apenas para satisfazer teste. Inputs inválidos, coordenadas não finitas e sequências fora da janela são rejeitados antes de tocar memória da engine.

## Baseline e recuperação dentro da sessão

A baseline é um retrato durável e completo da área relevante: entidades vivas, gerações/tombstones necessários, objetos modificados, objetivos, jogadores, revisão e fronteira de eventos. O cliente confirma recursos e aplica esse estado numa barreira segura. Eventos novos aguardam a conclusão e são aplicados após a fronteira; nenhum frame parcial deve liberar gameplay.

O mesmo mecanismo recupera lacunas prolongadas e estouro de janela **na conexão existente**. Reconexão após queda completa, entrada no meio da campanha e migração de host ficam fora do MVP. Essa redução de escopo não elimina a necessidade de recuperação de baseline durante uma sessão normal.

Até provar recuperação completa de uma região destruída, é aceitável limitar o MVP a uma área e bloquear progressão com diagnóstico explícito quando faltar recurso indispensável. Deixar o cliente continuar com portas, colisão ou inimigos ausentes não conta como recuperação.

## Apresentação, colisão e predição

Posições recebidas podem alimentar um pequeno histórico por `server_tick`; a renderização interpola entre amostras. A malha/sprite suavizada é separada do estado de colisão e da posição usada pelo host para acertar tiros e verificar gatilhos. Alterar apenas um transform gráfico não atualiza necessariamente os subsistemas nativos.

O cliente pode responder imediatamente à mira e a efeitos locais descartáveis. Não deve prever mortes, objetivos, abertura de portas ou destruição que altere passagem. Predição e replay de movimento só devem ser habilitados após existir um motor de movimento isolado, sem efeitos irreversíveis de campanha, com timestep e estado restaurável demonstrados. Reexecutar o tick nativo inteiro para corrigir posição pode duplicar gatilhos, som, munição e eventos.

Para o primeiro teste é preferível medir movimento autoritativo sem replay a declarar predição pronta com correções que quebram missão. A opção provisória de posição enviada pelo cliente exige validação no host e deve ser descrita como confiança limitada, pois não equivale a simular movimento por comandos.

## Validação que demonstra coop

Os testes atuais cobrem serialização, lógica pura e sessão com UDP real em localhost. O teste de sessão verifica, entre outros casos, eventos duplicados, fora de ordem e epoch antigo. O verificador de logs examina contadores e morte induzida pelo harness. Isso é útil, mas não comprova IA atacando o convidado, tiros dele causando dano ou igualdade de portas, colisões e objetivos. Além disso, `tests/coop/assert_coop_log.py` atualmente relata `missing > 0` sem reprovar o teste. Nesta investigação não foram executados testes novos nem uma campanha end-to-end.

Critérios propostos, com duas instâncias reais e identificação dos comandos/resultados nos logs:

1. **Barreira:** perder o primeiro `Room`, o primeiro `loaded`, a transição para `InMission` e seus ACKs. Ambos entram uma única vez no mesmo epoch, ou terminam com erro explícito dentro do prazo definido.
2. **Combate do convidado:** só ele dispara contra inimigo da campanha; o host registra pedido validado, dano e morte; o convidado converge. Um pedido duplicado consome munição e causa dano uma única vez.
3. **Segundo jogador:** um inimigo seleciona o convidado lógico como alvo, movimenta-se e causa dano nele; vida e morte convergem nos dois lados.
4. **Causalidade:** entregar snapshot pós-dano antes do evento, evento antes de snapshot anterior e fotografia pós-morte antes de dano atrasado. HP nunca sofre duplicação nem regressão.
5. **Mapa funcional:** somente o convidado abre uma porta e destrói uma cobertura; ambos atravessam a mesma passagem, enxergam o mesmo bloqueio e resolvem tiros com o mesmo resultado autorizado.
6. **Ativação:** host e convidado ficam em pontos diferentes da área permitida. Inimigos autorizados existem na réplica sem depender de o convidado atravessar o gatilho de campanha.
7. **Missão:** um gatilho acionado pelo convidado altera o objetivo correto; reinício de checkpoint invalida pacotes anteriores e preserva uma única transição nativa.
8. **Falhas de rede:** usar um intermediário controlado, com semente reproduzível, para perda, duplicação, atraso e reordenação nas duas direções, incluindo rajadas. Medir convergência, tempo de recuperação, tamanho máximo de fila e banda. Localhost sem perturbação não substitui esse teste.
9. **Recuperação:** uma lacuna que ultrapassa a janela provoca baseline e convergência sem desconectar. Um cliente pausado não faz o host acumular memória indefinidamente.

Comparar estado canônico por IDs e revisões: jogadores, atores relevantes, HP/morte, portas/destrutíveis, itens e objetivos. Não exigir hashes de toda a memória nativa ou frames de partículas. Ausência persistente de entidade relevante deve reprovar; janelas transitórias são medidas com prazo e orçamento explícitos. Os limites aceitáveis de latência e convergência precisam ser definidos e registrados antes da avaliação.

## Pendências que antecedem a implementação

- Demonstrar como comandos movem o segundo jogador lógico e como IA e colisão o reconhecem no host.
- Isolar aplicação de estado e apresentação sem replay irreversível de dano ou script.
- Definir a representação durável de spawn, destruição, checkpoint e objetivos necessária à baseline.
- Escolher limites de buffers, orçamento de MTU, frequências e relógio/timestep a partir de medições; os 10 Hz atuais são ponto de partida, não validação de qualidade.
- Versionar o protocolo e decidir a migração do `Shoot` legado para um único fluxo autoritativo.

Expandir para vários convidados só depois de provar esse ciclo com dois jogadores. Isso exige, além de ACK por peer, interesse/streaming que cubra as áreas necessárias a todos e medições de CPU, memória e banda.

## Referências navegáveis

- [Protocolo](../../src/net/coop_protocol.h) e [sessão](../../src/net/coop_session.cpp).
- [Transporte](../../src/net/network.cpp) e [adaptador](../../src/game/coop_adapter.cpp).
- [Teste de sessão](../../tests/test_coop_session.cpp), [protocolo](../../tests/test_coop_protocol.cpp) e [verificador de logs](../../tests/coop/assert_coop_log.py).
- [Plano de implementação](PLANO-DE-IMPLEMENTACAO.md).
