# Plano de investigação e migração

**Estado: proposto.** Este plano organiza provas de viabilidade e implementação
posterior. Nenhuma etapa abaixo está marcada como concluída pela criação da
documentação. Consulte o [índice](README.md) para os contratos complementares.

## 1. Princípios de execução

1. Manter o trabalho atual do outro agente preservado e identificar uma base
   reproduzível antes de implementar a alternativa.
2. Usar um modo experimental explícito para a nova autoridade; não misturar
   regras de Deathmatch e de duas campanhas com as regras novas implicitamente.
3. Não editar `generated/game` para implementar o mod: investigar o código
   traduzido, mas manter as mudanças em hooks e componentes do projeto.
4. Registrar para cada rotina nativa sua assinatura, registradores, efeitos
   colaterais, thread permitida, ordem no frame e limpeza correspondente.
5. Não transportar ponteiros, vtables, endereços de controladores ou memória
   bruta. Resolver identidades de rede para objetos locais por registro.
6. Medir resultados da simulação; logs com contadores iguais são evidência
   insuficiente de uma campanha funcional.

## 2. P0 — Delimitar o experimento e instrumentar

**Trabalho:** escolher uma área curta e recursos da missão 1; registrar hashes
dos assets efetivos, build, protocolo e configuração. Identificar os pontos de
tick e as threads que executam o código do jogo. Separar relógio da simulação,
apresentação e temporização da rede.

**Investigar:** como suspender consequências de gameplay durante a carga e
retomar sem perder inicialização, eventos necessários ou limpeza de objetos.
O tick atual ligado à UI e o reconhecimento de tabela de objetivos carregada
não provam uma barreira real de simulação.

**Saída verificável:** host e cliente carregam os mesmos recursos e permanecem
sem avanço de gameplay até o início confirmado. O host usa ticks autoritativos
e consegue relatar o tick de cada ação.

## 3. P1 — Segundo participante lógico no host

**Trabalho:** criar um registro de dois jogadores e uma entidade experimental
com posição, volume de colisão, equipe e vida. O protagonista local continua
vinculado a câmera/HUD próprios; o convidado não precisa duplicar esse conjunto
para a primeira prova.

**Investigar:** quais referências globais pressupõem apenas o protagonista;
qual cadeia insere/remove a entidade nos gerenciadores; como a IA seleciona
alvos e como o dano distingue um aliado de inimigos de missão.

**Saída verificável:** uma entidade adicional existe no host sem sequestrar um
inimigo da campanha. Ela pode ser movida por comandos de um harness. Um inimigo
consegue percebê-la, selecioná-la e causar dano nela. Criar e remover a entidade
repetidamente não deixa slots, controladores ou referências residuais.

**Decisão de viabilidade:** se essa integração não for demonstrada, registrar
o bloqueio e a próxima hipótese. Um sprite visível não encerra esta etapa.

## 4. P2 — Controle remoto e combate mínimo

**Trabalho:** convidado envia comandos associados a jogador, sequência e tick.
O host atualiza sua entidade e retorna estado confirmado. Começar com controle
sem predição; a apresentação local pode antecipar câmera e efeitos cosméticos
desde que não confirme posição, dano ou consumo compartilhado por conta própria.

**Combate:** criar um caminho de intenção de tiro até a rotina nativa adequada,
com arma identificada por ID estável, verificação de cadência/munição e autoria
do dano. O tiro do anfitrião deve passar pela mesma política de autoridade.

**Saída verificável:** o convidado acerta e mata um inimigo que existe no host;
o inimigo também pode ferir o convidado. Cada comando de tiro identificado é resolvido no máximo uma vez; suas
consequências identificadas são aplicadas sem duplicação, mesmo quando pacotes
são duplicados. Um tiro pode causar múltiplos impactos, dano e morte. Testar um alvo
atrás de parede e um tiro que acerta cenário sem acertar ator.

## 5. P3 — Réplica do mundo e apresentação

**Trabalho:** host controla criação, atualização e remoção de entidades por
IDs de rede. Cliente cria representações pelo caminho completo necessário,
sem depender de seu jogador atravessar o mesmo gatilho de spawn.

**Visual:** desenhar o parceiro com sprite de profundidade correta. Evitar que
a representação visual conte como inimigo ou produza dano/objetivos próprios.
Modelo nativo e animação entram após a prova funcional.

**Rede:** implementar baseline, eventos confiáveis e snapshots ordenados com
o corte de eventos já incorporados. Recuperação reconstrói estado explícito,
não copia a arena do host. Consulte [rede](REDE-E-VALIDACAO.md).

**Saída verificável:** com o convidado parado, o host ativa um encontro e ele
aparece no cliente; morte/remoção também cruzam. Um baseline recompõe o estado
após perda sem duplicar dano ou reabrir um objeto já destruído.

## 6. P4 — Uma interação e uma destruição compartilhadas

**Trabalho:** selecionar uma porta ou mecanismo e um objeto destruível que
altere colisão/visibilidade. Mapear identidade, condição, transição, efeitos
colaterais e descarregamento desses objetos.

**Saída verificável:** qualquer jogador pode solicitar a interação; o host
confirma uma vez. Os dois atravessam a mesma passagem e seus tiros respeitam
a mesma colisão após a mudança. Repetir confirmação não concede item nem
executa explosão novamente.

## 7. P5 — Objetivo, morte e checkpoint

**Trabalho:** adaptar uma cadeia curta de gatilhos para considerar ambos os
participantes no host. Definir explicitamente quem pode iniciar encontros,
quando exigir proximidade e quais condições são compartilhadas.

Replicar estados individuais dos objetivos e o resultado da cadeia nativa.
Mapear a condição real de fim; não usar "todos os objetivos, inclusive
opcionais, concluídos" como substituto sem evidência.

**Política inicial proposta:** morte de qualquer participante interrompe a
ação compartilhada e conduz ambos ao último checkpoint comum; sem checkpoint
compatível demonstrado, reiniciar a área experimental nos dois. Reviver e
reaparecimento independente ficam para depois. Não iniciar gravação de
progresso definitivo antes de definir propriedade e compatibilidade dos saves.

**Saída verificável:** ambos veem o mesmo objetivo por ID, atravessam a mesma
transição e retornam ao mesmo estado após morte. O cliente não encerra sua
campanha por uma condição local divergente.

## 8. P6 — Trecho jogável de ponta a ponta

Roteiro mínimo, com inputs e observação das duas instâncias reais:

1. Entrar, carregar e iniciar sob a barreira de gameplay.
2. Convidado se move; host e IA reconhecem sua entidade.
3. Ambos atiram e confirmam mortes compartilhadas.
4. Convidado abre uma passagem e host a utiliza.
5. Host destrói uma cobertura e convidado atira pela abertura.
6. Um objetivo avança por sua identidade, não apenas pela contagem total.
7. Morte reinicia a sessão de gameplay no checkpoint/área acordado.
8. Ambos concluem o trecho e recebem a mesma transição autoritativa.

Executar também com perda, duplicação, atraso e reordenação de pacotes.
Repetir com FPS de apresentação diferentes e janela sem foco. Estes são
critérios futuros; esta documentação não afirma que tenham sido executados.

## 9. O que reaproveitar e o que revisar

| Parte atual | Uso possível | Revisão obrigatória |
|---|---|---|
| Transporte UDP | Base de comunicação | Confiabilidade de controle, autenticação do remetente de sessão e limites |
| Sessão co-op | Estados e participantes | Barreira real, retransmissão e reset completo entre rodadas |
| Hooks de dano | Instrumentação e chamada nativa | Dano de ambos os jogadores e causalidade com snapshots |
| Registro de atores | Mapeamento para entidades locais | IDs atribuídos pelo host e ciclo de vida sem heurística de proximidade |
| Escrita de pose | Apresentação de réplicas | Colisão, tick e animação sem consequências locais independentes |
| Barramento de eventos | Apresentação e integração identificadas | Não presumir que ele represente toda a lógica da missão |
| Testes atuais | Regressão dos contratos existentes | Acrescentar provas da entidade remota e do trecho real |

Referências: [adapter](../../src/game/coop_adapter.cpp),
[mundo](../../src/game/coop_world.cpp),
[missão](../../src/game/coop_mission.cpp),
[multiplayer](../../src/game/multiplayer.cpp),
[sessão](../../src/net/coop_session.cpp).

## 10. Condições para ampliar o escopo

Mais jogadores exigem orçamento medido para atores, controladores e recursos,
além de ACKs por destinatário. Separação ampla exige streaming para as áreas
necessárias a todos, sem descarregar objetos ainda referenciados. Predição
exige movimento reproduzível e reconciliação demonstrada. Reconexão exige
baseline completo e política de posse do participante ausente.

Nenhum desses itens deve ser tratado como consequência automática de o trecho
de dois jogadores funcionar. Primeiro demonstrar a campanha compartilhada;
depois medir cada ampliação e documentar seus novos limites.
