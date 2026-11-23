# Plano: envios de buffer da CPU numa segunda fila da GPU (#50, 23/11/2026)

Teste #50 de `MEMORIA-TESTES.md`. Objetivo: tirar da fila gráfica a cópia dos ~10 MB por quadro que a CPU escreve
(staging → arena, `BufferCache::SynchronizeMemory`), hoje 0,97 ms de GPU por quadro na floresta densa
(`barrier+upload+bufcopy`), e fazer essa cópia correr em paralelo ao trabalho gráfico. Ganho esperado
(parceira): ~1 ms/quadro, floresta densa ~52 → ~54–55 FPS. Não leva a 60.

## Ponto de partida (código de hoje)
- Não há fila de transferência; os buffers são `eExclusive` (uma família). Usar uma fila de **outra** família
  exigiria transferência de posse (barreiras de liberação/aquisição) ou `eConcurrent` em todos os buffers.
- Existe a infraestrutura da fila do cull (`SOTC_CULL_QUEUE`, `sotc_cull_queue.h/.cpp`): uma segunda fila da
  **mesma família gráfica** (sem transferência de posse), agendador próprio, semáforo de linha do tempo,
  dependências por página (`SOTC_CULL_QUEUE_PAGES`), e a regra "todo envio gráfico espera o envio mais novo da
  fila 2 ainda não terminado". Ela não subiu o FPS para o cull (#10, #14), mas a parte de filas e semáforos
  funcionou (0 diferenças no VERIFY, sem queda).

## Revisão da parceira (23/11 16:20) — muda o desenho
- **A 2ª fila da família gráfica não dá sobreposição na NVIDIA**: as filas da família gráfica vão para o mesmo
  motor 3D/compute; o motor de cópia (DMA) só é alcançado pela **família só-transferência**. Usar essa família,
  com as **arenas de buffer em `eConcurrent`** (gráfica + transferência; quase sem custo em buffers na NVIDIA),
  em vez de transferência de posse por faixa. Risco: o `SOTC_PRESENT_QUEUE=2` registrou device lost numa
  família não gráfica (driver 617, RTX 5090): testar sob carga.
- **Lote**: mandar cedo, em lotes pequenos (~1–2 MB), assim que fechar, enquanto a GPU ainda roda o envio
  anterior (17–18 envios por quadro). Esperar o envio gráfico seguinte mataria a sobreposição.
- **Guarda**: "nenhum acesso da GPU em voo nem na submissão atual" (mais restrita que o #50a). Buffers
  reescritos todo quadro tendem a falhar. Faltam timestamps próprios na fila de cópia (o GPU_TIMES não a vê).
- **Etapa 1 decide**: < 50% dos bytes do denso passando → ganho < ~0,4 ms, não implementar. ≥ 70% com o
  motor de cópia → ~0,6–0,9 ms (~52 → ~54 FPS).

## Desenho (original; itens 1 e 4 corrigidos acima)
1. **Fila**: reaproveitar a segunda fila da família gráfica e o agendador da fila do cull (`SOTC_COPY_QUEUE=1`
   liga só a parte de cópia; o cull continua na fila gráfica).
2. **Onde**: em `SynchronizeMemory`, a cópia staging → arena (`runtime.CopyBuffer`) é gravada no buffer de
   comando da fila 2, e não no primário.
3. **Guarda (obrigatória, parceira)**: só redirecionar se **nenhuma página da faixa teve acesso da GPU, por
   qualquer caminho, desde o último envio gráfico terminado** — leitura ou escrita (binds, texturas lidas
   direto da memória, varredura de DMA, argumentos de indiretos, destinos de leitura de volta). Fonte: o tick
   por página que a fila do cull já mantém (`PageDep`/`dma_access_tick`), estendido a toda leitura. Na dúvida,
   cópia na fila gráfica como hoje.
4. **Envio da fila 2**: em lote, quando o lote passa de N MB ou na hora do envio gráfico seguinte, o que vier
   primeiro. O envio gráfico que usa a faixa espera (semáforo) o tick da fila 2 no estágio de transferência.
5. **Staging**: a região de staging só volta ao reservatório quando a fila 2 terminar (tick dela).
6. **Rede de segurança**: qualquer caso não coberto vai pelo caminho de hoje.

## Etapas (uma partida do usuário cada, 20 s+ na floresta sem pausar)
- **Etapa 1 — medir a guarda**: contador de quantos bytes passam na guarda completa (item 3). Seguir só se
  ≥ 70% dos ~10 MB/quadro do denso passarem.
- **Etapa 2 — redirecionar** com `SOTC_GPU_TIMES=1 SOTC_GPU_TIMES_EVERY=10`. Aprovação (parceira):
  `barrier+upload+bufcopy` na fila gráfica ≤ 0,3 ms/quadro; entre ops ≤ 2,7 ms; média nas janelas com
  6 mil+ comandos ≥ 53,5 em 2 partidas; nenhum erro de imagem (grama, cavalo, capa).
- **Etapa 3 — estabilidade**: partida limpa até o final do jogo sem queda antes de virar padrão.

## Riscos
- Ordem de memória entre filas: a lição do `SOTC_VIS_DIRECT` e do device lost de 17/11 (`SOTC_RB_PUSH`: cópia
  num envio ainda não mandado e o bloco desvinculado antes dela rodar).
- O semáforo pode serializar mais do que ganha (cada envio gráfico esperando a fila 2): por isso o lote (item 4).
- Custo de CPU da guarda por página em ~6–10 mil envios por segundo.
