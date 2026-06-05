# Rinha de Backend 2026 - C

Implementação em C para a Rinha de Backend 2026. A solução recebe transações,
transforma cada payload no vetor definido pelo desafio, busca os vizinhos mais
próximos no conjunto público de referências e responde com a decisão de fraude.

O foco deste repositório é reduzir o caminho crítico da requisição sem trocar a
regra de negócio por atalhos ligados ao conjunto de teste. O índice é construído
a partir de `resources/references.json.gz`; os payloads do teste não são usados
como lookup.

## Arquitetura

```text
k6 / cliente
    |
    v
LB em C, TCP, round-robin simples
    |
    +--> API 1 em C -> mmap(/data/index.bin) -> busca vetorial
    |
    +--> API 2 em C -> mmap(/data/index.bin) -> busca vetorial
```

A topologia segue a regra do desafio: um load balancer e duas instâncias de API.
O balanceador não interpreta o payload, não calcula fraude e não decide resposta.
Ele aceita conexões TCP na porta `9999` e distribui a carga entre as APIs.

As APIs fazem o trabalho de negócio: parse do JSON, vetorização, consulta ao
índice e resposta HTTP.

## Como Replicar

Pré-requisitos:

- Docker com suporte a imagens `linux/amd64`.
- Máquina x86_64 com AVX2 para reproduzir o mesmo caminho de execução.
- Repositório oficial da Rinha disponível para rodar o teste.

Passo a passo:

1. Clone este repositório.

   ```bash
   git clone https://github.com/bmtec/rinha-backend-2026-c-try.git
   cd rinha-backend-2026-c-try
   ```

2. Confira se o arquivo de referências existe.

   ```bash
   ls -lh resources/references.json.gz
   ```

3. Construa a imagem. Durante o build, o `index.bin` será gerado dentro da
   própria imagem.

   ```bash
   docker compose build
   ```

4. Suba os três serviços.

   ```bash
   docker compose up -d
   ```

5. Rode o teste usando o harness oficial da Rinha.

   ```bash
   cd /caminho/para/rinha-de-backend-2026/test
   docker compose --profile test run --rm k6
   ```

6. Leia o resultado produzido pelo teste.

   ```bash
   python3 -m json.tool test/results.json
   ```

7. Ao terminar, derrube os serviços.

   ```bash
   cd /caminho/para/rinha-backend-2026-c-try
   docker compose down
   ```

Para submissão pública, a branch `submission` deve apontar para uma imagem
imutável publicada no GHCR ou outro registry público. Evite `latest` na
submissão final.

## Criação do `index.bin`

O `index.bin` é criado pelo binário `builder`, executado na etapa `index` do
`Dockerfile`.

Entrada:

- `resources/references.json.gz`
- Registros com vetor de referência e rótulo (`fraud` ou `legit`)

Saída:

- `/data/index.bin`, copiado para a imagem final

Etapas principais:

1. O builder descompacta `references.json.gz`.
2. Faz parse apenas dos vetores e rótulos públicos de referência.
3. Executa k-means para agrupar os vetores em centróides.
4. Atribui cada vetor ao centróide mais próximo.
5. Ordena os vetores dentro de cada célula por distância ao centróide.
6. Quantiza os valores para `int16_t`.
7. Calcula limites por célula e por bloco.
8. Escreve o arquivo binário alinhado para leitura por `mmap`.

Parâmetros ajustáveis no build:

```bash
docker build \
  --build-arg CENTROIDS=2048 \
  --build-arg KMEANS_ITERS=15 \
  --build-arg BUILDER_THREADS=0 \
  --build-arg INIT_MODE=rust \
  -t rinha-backend-2026-c-try:local .
```

Decisão técnica: o índice é gerado offline para que as APIs não gastem CPU com
preparação no startup. Em execução, a API apenas mapeia o arquivo, aquece as
páginas e atende requisições.

## Decisões Técnicas

### C no caminho crítico

A implementação evita runtime com GC, frameworks HTTP e serialização dinâmica.
O objetivo é manter a latência previsível sob limite total de `1 CPU` e `350 MB`.

### Load balancer separado

O LB é um processo separado para cumprir a topologia exigida. Ele opera em nível
TCP e faz round-robin simples. A comunicação com as APIs usa sockets Unix e
passagem de descritor de arquivo, evitando reprocessar HTTP no balanceador.

### Parse especializado

O payload do desafio tem estrutura conhecida. Por isso o parser procura campos
específicos e converte apenas o necessário para a vetorização. Isso evita o
custo de um parser JSON genérico por requisição.

### Vetores quantizados

Os vetores normalizados são representados como `int16_t` no índice. Essa escolha
reduz memória, melhora cache locality e permite calcular distância com AVX2
usando inteiros. A ordenação final dos candidatos é reavaliada em ponto flutuante
para preservar a decisão dos vizinhos mais próximos.

### Busca em duas fases

A busca começa nas células IVF mais próximas. Quando a primeira resposta cai em
uma zona ambígua, a API expande a busca para mais centróides. Assim, o caso comum
fica barato e os casos de fronteira recebem mais recall.

### Respostas pré-formatadas

O resultado só pode ter seis valores de `fraud_score`: `0.0`, `0.2`, `0.4`,
`0.6`, `0.8` ou `1.0`. As seis respostas HTTP completas ficam pré-montadas em
memória, eliminando formatação e alocação por requisição.

### `mmap` e aquecimento

O índice é aberto com `mmap` em cada API. No startup, a aplicação toca as páginas
do arquivo para reduzir page faults durante o teste.

## Conformidade

- Um load balancer.
- Duas instâncias de API.
- Porta pública `9999`.
- Soma de recursos declarados no compose dentro de `1 CPU` e `350 MB`.
- Sem uso de payloads do teste como lookup.
- Licença MIT.

## Stack

- C
- Docker
- Linux amd64
- AVX2
- sockets Unix
- `mmap`
