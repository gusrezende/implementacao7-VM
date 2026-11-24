# Gerenciador de Memória Virtual (Implementação 6)

Implementação em C do projeto *Designing a Virtual Memory Manager* (Silberschatz et al., *Operating System Concepts*, 10ª ed., p. P-51), com memória física de **128 frames**, algoritmos de substituição **FIFO/LRU** para páginas e para a TLB, e busca na TLB feita por **uma thread por entrada**.

## Parâmetros do sistema

| Item | Valor |
|---|---|
| Endereço lógico | 16 bits (8 bits de página + 8 bits de deslocamento) |
| Tabela de páginas | 256 entradas |
| Tamanho da página/frame | 256 bytes |
| Memória física | 128 frames (32 768 bytes) |
| TLB | 16 entradas |
| Backing store | `BACKING_STORE.bin` (65 536 bytes) |

## Compilação

```sh
make         # gera o executável ./vm
make clean   # remove o executável e o correct.txt gerado
```

Requer `gcc` (ou compatível) e POSIX threads (Linux, Unix ou macOS).

## Execução

```sh
./vm <arquivo_de_enderecos> <fifo|lru> <fifo|lru>
```

- 1º argumento: arquivo de endereços lógicos (um inteiro por linha, como o `addresses.txt`);
- 2º argumento: algoritmo de substituição de **página** (`fifo` ou `lru`);
- 3º argumento: algoritmo de substituição da **TLB** (`fifo` ou `lru`).

Exemplo (mesma configuração do `correct.txt` fornecido):

```sh
./vm addresses.txt lru fifo
```

O arquivo `BACKING_STORE.bin` precisa estar no diretório de execução. A saída é gravada em `correct.txt`, no formato:

```
Virtual address: 16916 TLB: 0 Physical address: 20 Value: 0
...
Number of Translated Addresses = 1000
Page Faults = 539
Page Fault Rate = 0.539
TLB Hits = 54
TLB Hit Rate = 0.054
```

Em cada linha, `TLB:` é o índice da entrada da TLB usada na tradução: a entrada onde a página foi encontrada (TLB hit) ou a entrada onde ela foi inserida (TLB miss).

## Funcionamento

1. Para cada endereço: `página = (end >> 8) & 0xFF` e `deslocamento = end & 0xFF`.
2. A TLB é consultada criando 16 threads (`pthread_create`), cada uma verificando uma entrada; a que encontrar a página registra o índice (protegido por mutex).
3. Em caso de TLB miss, consulta-se a tabela de páginas. Se a página não estiver na memória (page fault), ela é lida do `BACKING_STORE.bin` para um frame:
   - os frames são preenchidos em ordem, de 0 a 127;
   - com a memória cheia, escolhe-se a vítima por **FIFO** (página carregada há mais tempo) ou **LRU** (página acessada há mais tempo); a página vítima é invalidada na tabela de páginas e na TLB.
4. O mapeamento é inserido na TLB, substituindo uma entrada por **FIFO** (fila circular) ou **LRU** (entrada usada há mais tempo; entradas vazias são usadas primeiro).
5. O byte (com sinal) no endereço físico é lido e a linha é gravada em `correct.txt`.

## Tratamento de erros

O programa valida tudo antes de iniciar a simulação e termina com mensagem de erro e código de saída 1 quando:

- o número de argumentos é diferente de 3;
- o 2º ou o 3º argumento não é `fifo` ou `lru`;
- o arquivo de endereços não existe, não é um arquivo regular ou não pode ser aberto/lido;
- o arquivo está vazio ou fora do formato: linha que não seja um único inteiro, valor negativo, valor fora de 0..65535 ou linha em branco no meio do arquivo;
- `BACKING_STORE.bin` não existe ou não tem 65 536 bytes;
- `correct.txt` não pode ser criado ou gravado.

## Resultados (addresses.txt fornecido)

| Página | TLB | Page faults | TLB hits |
|---|---|---|---|
| lru | fifo | 539 (0.539) | 54 (0.054) |
| lru | lru | 539 (0.539) | 55 (0.055) |
| fifo | fifo | 538 (0.538) | 54 (0.054) |
| fifo | lru | 538 (0.538) | 55 (0.055) |

A saída de `./vm addresses.txt lru fifo` é idêntica ao `correct.txt` fornecido. O registro completo dos testes está em `evidencias.log`.

## Arquivos

- `vm.c`: código-fonte
- `Makefile`: compilação (`make`) e limpeza (`make clean`)
- `evidencias.log`: execuções, comparação com o gabarito e testes de erro
- `README.md`: este arquivo
