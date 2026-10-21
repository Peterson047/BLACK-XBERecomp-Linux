[Voltar ao índice](README.md)

# Jogadores, IA, combate e representação do parceiro

Este documento descreve uma proposta de mod para uma única campanha autoritativa no host. Não registra uma funcionalidade já implementada. A leitura de referência foi feita na cópia principal do projeto, em 20 de outubro de 2026; os símbolos abaixo devem ser reconferidos na revisão usada pela implementação.

## 1. Evidência existente e limite da proposta

**Confirmado na leitura do código:**

- Em `src/game/multiplayer.cpp`, `Player()` lê o mundo com `R32(layout::kWorld)` e retorna `world + kWorldPlayer`. `src/game/layout.h` define `kWorldPlayer = 0x30`. Portanto, nesse acesso o protagonista começa em uma região embutida em `world+0x30`; não se trata de carregar um ponteiro em `[world+0x30]`.
- O código de multiplayer lê olho em `player+0x100`, direção em `+0xF0` e a referência da arma em `+0x29C`. `Update()` publica `PlayerStateData`, incluindo posição, orientação e flags, aproximadamente a 30 Hz. Isso é publicação de estado local, não demonstração de um segundo controlador autoritativo no host.
- `UpdateEnemyPuppet()` escolhe um ator vivo já inicializado pelo motor e sobrescreve sua transformação. O caminho é evitado quando o adaptador de coop está habilitado. Reutilizar um inimigo da missão como parceiro retiraria ou alteraria conteúdo da campanha.
- `Draw3DPlayerModel()` desenha atualmente efeitos de disparo via ImGui; o nome da função não comprova que ela crie um novo modelo animado de parceiro.
- `src/game/coop_world.cpp` intercepta `sub_001DF830` para publicar dano nativo do host e bloquear o dano independente do convidado, permitindo a reaplicação autorizada. `World::FindLiveActor()` ignora o par de identidade nativo `{0, 0}`, reservado nesse caminho ao jogador local. Ainda falta identidade explícita de participantes que não dependa desse par.
- O hook de `sub_001F08D0` executa o tick original e chama `World::AfterActorTick()` depois. A ordem existe para evitar que a simulação nativa sobrescreva a pose recebida.

**Proposta:** manter uma tabela de participantes no host e integrar um segundo corpo lógico aos serviços originais necessários. O convidado envia comandos, recebe o resultado e renderiza sua câmera local. Criar um segundo personagem, substituir consultas ao protagonista e validar movimentos são trabalho novo.

**Investigação pendente:** descobrir as funções completas de construção e destruição de um participante, os leitores do protagonista global e os serviços de colisão, percepção e dano que podem ser reutilizados. Não há nesta proposta uma API nativa de `CreateSecondPlayer()` ou uma lista de callbacks descoberta e pronta.

## 2. Identidade própria e adaptação do protagonista

A camada de mod deve associar `(start_epoch, player_id, spawn_generation)` a um participante. O registro proposto contém estado de movimento, vida, equipamento, estado de conexão e referências locais ao corpo lógico e à representação visual. Os nomes são conceitos de desenho, não tipos já encontrados no jogo.

Separar a identidade do endereço evita que um slot reciclado receba comando de uma vida anterior. Ponteiros do guest e do host são referências internas; nunca são identidades de rede.

Não substituir globalmente `world+0x30` pelo jogador que estiver sendo processado. Câmera, arma em primeira pessoa, HUD, percepção e gatilhos podem consultar essa região com expectativas diferentes. Trocar a base durante chamadas encadeadas pode misturar os dois personagens e falhar em reentrância.

A investigação deve classificar cada consulta conhecida:

| Consulta original | Adaptação proposta |
|---|---|
| Câmera e arma em primeira pessoa | Usar o participante local daquela instância |
| Origem de tiro ou interação | Receber o participante autor do comando |
| Alvo de inimigo | Avaliar os participantes elegíveis no host |
| Gatilho espacial | Avaliar um ou ambos conforme a regra explícita |
| Checkpoint ou fim da fase | Avaliar a regra de grupo |
| HUD | Exibir o local e o estado confirmado do parceiro |

O inventário de leitores precisa ser confirmado no recomp e em execução. Uma busca pelo literal `0x30` isolado não distingue o protagonista de outros campos.

## 3. Corpo lógico e corpo visual

**Corpo lógico no host:** posição dos pés, cápsula ou volume equivalente, postura, vida, equipe, origem de mira, equipamento e ligação com o participante. Ele deve existir para colisão, percepção, raycast e interações. Registrar um participante apenas na tabela de rede não o insere automaticamente nesses sistemas do motor.

**Corpo visual em cada máquina:** sprite ou modelo, interpolado a partir do estado confirmado, com animação e efeitos. O desenho não decide vida, dano ou objetivos. A posição do olho transmitida hoje não deve ser tratada como posição dos pés sem conversão de postura explícita; os valores aproximados usados no Deathmatch não são um contrato físico validado para coop.

Um proxy lógico próprio pode ser mais simples que duplicar todo o protagonista. Mas, se a IA ou o raycast só reconhecem classes nativas, será necessário um adaptador nos pontos de consulta ou um ator nativo corretamente construído. Uma cápsula externa sem integração não será atingida pelo motor por mera proximidade visual.

### Sprite billboard como primeira representação

É uma opção válida para provar a mecânica com menor dependência da animação nativa: um quad texturizado ligado à posição do parceiro, com estados parado, andando, agachado, disparando e incapacitado. Variantes direcionais podem mostrar melhor a orientação.

O sprite deve passar pelo teste de profundidade e ser ocultado pela geometria. Projetá-lo sobre o HUD via `WorldToScreen()` sem oclusão produz um aliado visível através de paredes. Também é preciso considerar recorte contra o chão, transparência, escala em perspectiva e alinhamento com o volume de colisão. Um billboard plano oferece informação limitada sobre direção, postura e cobertura; isso é uma escolha visual, não uma solução de física.

### Modelo nativo em etapa posterior

Reaproveitar um modelo existente exige entender animação, recursos, controlador e descarte. `.agents/teamwork/coop/BODY.md` registra a pergunta ainda aberta entre animação dirigida pelo plano de IA e seleção direta de clips. `UpdateEnemyPuppet()` conserva componentes originais; não é prova de que copiar campos de pose produza animação correta de um participante novo.

## 4. Movimento autoritativo em duas etapas

**Primeiro experimento:** o convidado envia eixos de movimento, mira e botões, com sequência de comando; o host move um corpo cinemático e devolve sua pose confirmada. Começar com uma área pequena e colisão validada permite testar presença e combate sem duplicar o controlador inteiro de primeira pessoa. O movimento do protótipo pode ter latência perceptível e não equivaler ao comportamento original.

Esse estágio só é autoritativo se o host calcula ou valida o deslocamento e as colisões. Receber uma posição arbitrária do convidado e copiá-la não torna o movimento autoritativo. Se usado como degrau temporário, esse modo precisa ser identificado como tal.

**Etapa posterior:** movimento local previsto no convidado, ACK do último comando processado pelo host, correção para o estado confirmado e reaplicação dos comandos pendentes. Isso exige um controlador reexecutável suficientemente compatível nos dois lados. Os controladores originais, seus efeitos colaterais e seu tratamento de tempo precisam ser estudados antes de prometer replay determinístico.

Câmera e mira podem responder localmente; posição física, dano e interação continuam sob decisão do host. Desconexão ou ausência prolongada de comandos deve parar o movimento do proxy, sem manter indefinidamente o último botão pressionado.

## 5. Fazer a IA perceber dois jogadores

A primeira prova útil é um inimigo alternar entre dois alvos válidos, enxergar o remoto, perder sua linha de visão atrás de cobertura e causar dano nele. O corpo visual não basta.

A investigação deve localizar aquisição, retenção e descarte de alvo, equipe, distância, linha de visão, memória e origem dos disparos da IA. O adaptador deve oferecer candidatos host e convidado sem obrigar cada inimigo a escolher sempre o mais próximo. Preservar o comportamento original de cobertura e reação é preferível até que a política de alvos possa ser alterada com evidência.

`.agents/teamwork/coop/LAYOUT.md` identifica a decisão em `sub_001E5EF0`, chamada de `sub_001E5F70`, e alerta que a rotina também participa da liberação via `sub_001DEC20` e `sub_001DA1A0`. Portanto, bloquear toda a função no cliente ou congelar controladores indiscriminadamente não é um atalho seguro: pode impedir reciclagem, deixar cadáveres registrados e congelar animação.

Na arquitetura nova, a IA autoritativa roda no host. No cliente, os trechos necessários para apresentação e ciclo de vida devem ser separados dos efeitos autoritativos gradualmente. Até mapear essa separação, manter um modo de compatibilidade e bloquear efeitos específicos é mais defensável que desligar o sistema inteiro.

## 6. Combate de ambos na mesma campanha

`NotifyLocalShot()` já captura origem e direção no momento do disparo nativo; `CheckLocalPlayerShooting()` publica o disparo. Para a nova abordagem, o comando proposto contém identidade do participante, `shot_id`, sequência/tick, arma estável e mira. O host valida sequência, cadência, munição e origem compatível com o corpo antes de resolver o disparo.

A referência lida de `player+0x29C` é enviada hoje em `weapon_id`; não deve ser reutilizada como identificador portátil sem um mapeamento. Uma tabela versionada de tipos de arma deve resolver o ID estável para os recursos locais e rejeitar tipos desconhecidos.

Pontos nativos identificados em `LAYOUT.md` para investigação e adaptação:

| Símbolo/endereço | Papel registrado | Limite |
|---|---|---|
| `sub_001DAFF0` | Raycast | Resultado estático em `out+0x24`, ator dinâmico em `out+0x30`; confirmar contrato na revisão alvo |
| `sub_001D89E0` | Resolução de acerto/escala de dano | Não é uma API de tiro remoto pronta |
| `sub_001CE790` | Despacho de dano | Monta estrutura e direção usadas pelo dano nativo |
| `sub_001DF830` | Dano do ator | Exige ator/controlador válidos e argumentos corretos |
| `sub_002057D0` | Parte da morte | Não chamar isoladamente para substituir a cadeia de dano |

O quarto argumento do dano nativo é uma estrutura de evento, conforme a correção final de `LAYOUT.md` e `ReplayDamage()`; não um vetor de impacto isolado. Não copiar receitas antigas desse documento sem ler suas correções posteriores.

O host deve produzir um resultado único por disparo e ordenar as consequências de dano, morte e destruição. O convidado pode mostrar recuo e clarão de imediato, mas a confirmação de acerto depende do host. Correção de HP e replay de dano precisam ter um contrato causal para não descontar o mesmo tiro duas vezes.

### Dano recebido pelos jogadores

A replicação existente de atores de campanha não prova que o dano de dois jogadores já funcione. O protagonista usa um caminho particular e a busca de atores exclui `{0, 0}`. Mapear dano de bala, explosão, ambiente e queda no protagonista é uma investigação própria.

O host deve manter a vida dos dois participantes. Adaptar o dano nativo do jogador ou usar vida própria no proxy exige também ligar efeitos locais, HUD, incapacitação e regras de derrota. Morrer um participante não deve disparar automaticamente o game over single-player do grupo sem uma decisão de design. Política inicial proposta neste diretório: a morte de qualquer participante interrompe o gameplay e leva os dois ao checkpoint comum validado; sem checkpoint compatível demonstrado, reinicia-se a área experimental. Reviver é uma extensão posterior. Essa escolha ainda não descreve uma regra implementada.

## 7. Construção, capacidade e limpeza

O pool possui 32 blocos de ator de `0x3C0`, mas a lista viva tem capacidade 16 (`manager+0x79B4`), contagem em `+0x79B8` e ponteiro em `+0x79BC`. A tabela de controladores de IA possui 16 slots conforme `LAYOUT.md`. Slot livre no pool não prova espaço em todos os gerenciadores envolvidos.

`sub_001DA4F0` inicializa parte do ator; o caminho de spawn registrado em `sub_001DF440` também chama `sub_001880C0`, `sub_001899F0` e `sub_001D21F0`. Criar um ator chamando apenas a ativação já foi associado a controlador inválido. Não fabricar vtable ou ocupar um bloco livre manualmente.

Antes de escolher um ator nativo para o parceiro, medir pico de atores, controladores, corpos e recursos nas fases pretendidas. Reservar capacidade ou expandir limites exige auditar loops, alocação, limites fixos e reciclagem; alterar somente a capacidade publicada corrompe estruturas.

A conexão, morte, troca de mapa e desconexão precisam remover o proxy dos sistemas em que foi registrado e liberar recursos na ordem correta. A identidade do participante não deve satisfazer acidentalmente predicados de inimigos ou objetivos de contagem. Excluí-lo do conteúdo de campanha não significa excluí-lo dos gatilhos de jogador: são categorias distintas.

## 8. Critérios para avançar

1. Um corpo adicional é criado e removido repetidamente sem consumir inimigos nem vazar slots.
2. Comandos externos movem esse corpo com colisão e postura coerentes.
3. Um inimigo percebe e atinge o corpo remoto; cobertura interrompe a linha de visão.
4. Um tiro remoto resolve uma única vez contra um inimigo e um objeto do host.
5. O parceiro aparece com profundidade correta; desaparecer visualmente não remove sua entidade lógica.
6. Troca de mapa, desconexão e morte deixam contagens e referências válidas.
7. Só depois: conectar predição, modelos animados, reviver e regras mais amplas de missão.

Estes itens são critérios de aceite propostos. A documentação não afirma que esses experimentos foram executados.

## Referências do repositório

- `src/game/layout.h`: `layout::kWorldPlayer` e `layout::coop`.
- `src/game/multiplayer.cpp`: `Player`, `UpdateEnemyPuppet`, `NotifyLocalShot`, `CheckLocalPlayerShooting`, `Update`, `Draw3DPlayerModel`.
- `src/game/hooks.cpp`: captura do disparo e exemplos de preservação/chamada do código original.
- `src/game/coop_world.cpp`: `FindLiveActor`, `ReplayDamage`, `ApplyActivation`, hooks `sub_001DF830`, `sub_001F08D0` e `sub_001DA4F0`.
- `.agents/teamwork/coop/LAYOUT.md`: gerenciadores, dano, ativação, correções de orientação e alertas sobre ciclo de vida da IA.
- `.agents/teamwork/coop/BODY.md`: diagnóstico do corpo remoto e investigação da animação.

Fontes navegáveis: [layout](../../src/game/layout.h), [multiplayer](../../src/game/multiplayer.cpp), [hooks](../../src/game/hooks.cpp), [mundo](../../src/game/coop_world.cpp) e [achados históricos](../COOP-FINDINGS.md).
