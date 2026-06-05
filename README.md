# Rinha de Backend 2026 - C

Esta submissao e uma implementacao em C para o desafio de deteccao de fraude da
Rinha de Backend 2026. O objetivo e responder muito rapido sem abrir mao da
qualidade da classificacao: a decisao final continua baseada na vizinhanca do
vetor de referencia, e nao em regras fixas sobre os payloads do teste.

## Visao Geral

A solucao segue a topologia exigida pelo regulamento: um balanceador de carga e
duas instancias de API. O balanceador recebe as conexoes TCP na porta 9999 e
distribui as requisicoes entre as duas APIs em round-robin simples. As APIs
mantem o indice de referencia em memoria mapeada e executam a busca vetorial
para calcular a resposta.

O indice e gerado durante o build da imagem a partir do arquivo publico de
referencias. Isso evita trabalho pesado no startup do container e deixa a etapa
online focada apenas em receber a requisicao, transformar a transacao no vetor
esperado, consultar os vizinhos mais proximos e responder.

## Decisoes de Arquitetura

A principal decisao foi reduzir o caminho critico da requisicao. Cada syscall,
alocacao, copia de memoria e parse generico aparece no p99 quando a meta e
sub-milissegundo. Por isso a solucao evita frameworks HTTP, evita serializacao
dinamica na resposta e usa um protocolo interno simples entre o balanceador e
as APIs.

O balanceador foi mantido separado das APIs para respeitar a regra do desafio,
mas ele nao interpreta a regra de negocio. Sua responsabilidade e operacional:
aceitar conexoes, manter a distribuicao entre as duas instancias e entregar o
trabalho para a API escolhida. A logica de fraude fica nas APIs.

Nas APIs, o tradeoff principal foi entre recall e latencia. Buscar em mais
particoes aumenta a chance de encontrar exatamente os melhores vizinhos, mas
tambem aumenta o custo por requisicao. A configuracao atual usa uma busca curta
como caminho comum e expande a busca apenas quando a resposta esta em uma zona
ambigua. Isso preserva a precisao medida sem pagar o custo maximo em todas as
transacoes.

Outro tradeoff importante foi representar o indice de forma compacta. A base de
referencia tem valores normalizados com baixa precisao decimal, entao uma
representacao inteira compacta preserva a ordenacao relevante e reduz pressao
de memoria. Em um limite total de 350 MB para tres servicos, essa escolha ajuda
as duas APIs a manterem o indice quente e diminui variancia por falta de cache.

## Conformidade com o Desafio

Esta submissao usa pelo menos um load balancer e duas instancias de API, com os
limites de CPU e memoria declarados no compose respeitando o teto total de 1 CPU
e 350 MB.

A solucao nao usa os payloads do teste como lookup. O indice e construido a
partir do conjunto de referencias do desafio, e cada resposta e calculada a
partir da transacao recebida no momento da requisicao. O conjunto rotulado de
teste e usado apenas para validar desempenho e acuracia durante os testes.

O repositorio esta sob licenca MIT.

## Resultado Local

No servidor Haswell usado para validar a submissao, a execucao local do harness
oficial de preview retornou:

- p99: 0.34 ms
- falsos positivos: 0
- falsos negativos: 0
- erros HTTP: 0
- pontuacao final: 6000

Execucoes repetidas ficaram na mesma faixa, com variacao normal do ambiente de
teste.

## Stack

- C
- Docker
- Linux amd64
- AVX2
