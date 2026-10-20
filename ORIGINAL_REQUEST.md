# Original User Request

## Initial Request — 2026-10-19T14:34:15Z

Refatorar e reconstruir do zero a lógica de jogadores e arena multiplayer em BLACK (Mission 1: Veblensk), isolando os dados do mapa em um pacote multiplayer limpo (sem gatilhos, scripts ou assets da campanha), implementando o carregamento direto do mapa a partir do menu multiplayer sem passar por menus da campanha, e inserindo modelos nativos de soldados/operadores 3D animados do próprio motor do BLACK para todos os jogadores remotos.

O usuário quer que o time autônomo analise o jogo, o projeto e como podemos fazer isso, gerando opiniões, discussão aprofundada de arquitetura, plano de testes, execução e revisão.

Working directory: /home/peterson/Documentos/GitHub/BLACK-XBERecomp-Linux
Integrity mode: development

## Requirements

### R1. Criação e Isolamento do Mapa Multiplayer Limpo
Desenvolver uma ferramenta/pipeline para inspecionar e extrair/modificar os arquivos de dados da Fase 1 (`LevelDat.bin`, `Level.awd`, `StLevel.bin`, etc.), gerando uma versão limpa exclusiva para multiplayer (`Level_MP00` ou variante limpa de `Level_00`). O mapa deve conter apenas a geometria 3D estática, colisões, textura e iluminação da cidade de Veblensk, eliminando permanentemente cutscenes, zonas de derrota/vitória, gatilhos de objetivo e scripts de IA da campanha singleplayer.

### R2. Carregamento Direto do Mapa via Menu Multiplayer (Direct Map Launch)
Implementar uma transição direta para a fase a partir do menu do multiplayer ou ao conectar a um servidor:
- Realizar a chamada direta da rotina interna de carregamento de fase do motor (configurando os parâmetros de nível para Veblensk / Level 0 e chaveando o estado da engine diretamente para carregamento/gameplay).
- Dispensar por completo a navegação simulada por botões através das telas de introdução, título, seleção de missão e dificuldade (`PressStart` -> `blacktitlemenu` -> `MissionModeSelect` -> `Difficulty`).
- Permitir que a conexão ao servidor ou clique em "Entrar na Partida" dispare o carregamento do mapa de forma imediata e confiável.

### R3. Injeção e Controle de Modelos 3D Nativos de Soldados (Puppets de Rede)
Reconstruir do zero o subsistema de representação de jogadores remotos utilizando instâncias reais de modelos de operadores/soldados do motor de BLACK:
- Alocar e instanciar os modelos nativos de soldados (malha 3D completa, esqueleto e materiais) diretamente no grafo de renderização do motor.
- Controlar dinamicamente os parâmetros nativos de locomoção e animação (parado, caminhando, correndo, agachado, postura de mira e disparo) de acordo com o estado transmitido pela rede.
- Sincronizar em tempo real a matriz de transformação 3D (posição no espaço, rotação corporal yaw/pitch) e anexo da arma nas mãos do modelo.

### R4. Replicação Autorizada de Rede e Hitboxes 3D
Atualizar o protocolo e as estruturas de rede entre `BLACK-Server` e cliente para suportar as poses e estados anatômicos dos modelos de soldados:
- Replicação de posição, velocidade, postura e direcionamento da arma com interpolação suave (livre de jitter/teleportes).
- Mapeamento das hitboxes de colisão (Cabeça, Tronco, Abdômen, Pernas) diretamente sobre o esqueleto/volume do modelo nativo.
- Sistema de feedback de dano, registro de acertos e respawn tático sem interferência de regras de missão.

### R5. Pipeline de Testes e Validação Autônoma
Fornecer um ambiente de testes que valide a integridade do mapa limpo, o carregamento direto e a sincronização dos modelos através de:
- Teste automatizado de carregamento direto do mapa sem passar pelos menus e sem crash de ponteiros nulos ou dependências faltantes da campanha.
- Simulação/harness de teste com clientes virtuais para comprovar a instanciação dos modelos e a consistência das coordenadas no servidor e cliente.

## Acceptance Criteria

### Carregamento Direto e Estabilidade
- [ ] O mapa multiplayer é carregado diretamente ao clicar em conectar/iniciar no menu de multiplayer, sem passar pela cadeia de menus da campanha (`PressStart`, `blacktitlemenu`, `MissionModeSelect`, `Difficulty`).
- [ ] O mapa carrega sem travar (sem SIGSEGV ou ponteiros pendentes de cutscenes/scripts de IA) e entra diretamente na visão do jogador em Veblensk.
- [ ] Nenhum popup de objetivo da campanha, áudio de fala de missão ("speech.slb") ou condição de falha é disparado durante o jogo.

### Modelos 3D e Animações
- [ ] Jogadores remotos aparecem no mapa como soldados/operadores 3D completos renderizados nativamente pelo motor.
- [ ] As animações corporais (locomoção, postura de agachamento e ângulo da arma) refletem com fidelidade e sem travamentos as ações de cada jogador.
- [ ] A arma do jogador remoto está devidamente acoplada ao modelo e orientada na direção do disparo.

### Combate e Rede
- [ ] Disparos efetuados contra o modelo 3D do adversário registram dano proporcional às zonas anatômicas atingidas (headshot/corpo).
- [ ] O servidor dedicado `BLACK-Server` recebe e retransmite estados e eventos de eliminação sem perdas para todos os participantes.
