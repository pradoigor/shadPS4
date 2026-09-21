# Evidências de portabilidade

Base analisada: `42c555b7ab5d0678f531a7e4d505560ccc0f8add`.

Este alvo é um laboratório UWP dentro do fork. A extração do PKG do Apollo foi
executada no Xbox e persistiu na biblioteca com capa e metadados. A versão atual
mantém uma operação diagnóstica ativa: validação do ELF/SELF selecionado, auditoria
de requisitos runtime e execução de um ELF mínimo gerado pelo próprio projeto.

## Carregamento controlado

O aplicativo lê o cabeçalho original `core/loader/elf.h`, verifica a identidade
PS4, limita a quantidade de program headers, valida cada `PT_LOAD`, confere
offsets, tamanhos, alinhamento e ponto de entrada, e copia os bytes para um
buffer privado sem permissão de execução. Para SELF sem proteção, ele também
resolve os `PT_LOAD` do ELF interno através dos segmentos SELF. O checksum e as
medidas são gravados no `report.json`.

Para SELF, o aplicativo valida a tabela de segmentos e detecta criptografia ou
compressão. O conteúdo protegido permanece bloqueado; esta etapa não tenta
descriptografar SELF e nunca chama o ponto de entrada.

Um resultado aprovado significa que a estrutura e os limites descritos foram
aceitos e que o probe próprio retornou `42`. O arquivo selecionado não é executado.
Isso não significa que o ABI, relocador, TLS, bibliotecas, renderer Vulkan ou o
código do homebrew funcionem no Xbox.

A auditoria registra a presença e os tamanhos dos metadados `PT_DYNAMIC` e `PT_TLS`,
as tabelas de relocação e as dependências declaradas. Ela classifica os tipos de
relocação e verifica se os alvos ficam em `PT_LOAD`, mas não aplica relocação,
resolve imports nem chama inicializadores.

## Bloqueios ainda abertos

- `src/core/address_space.cpp` depende de placeholders e backing executável que
  precisam de uma implementação própria para UWP.
- `src/core/signals.cpp` e o tratamento de falhas do código convidado ainda não
  foram ligados ao runtime UWP.
- A janela, entrada, áudio e renderer do núcleo dependem de SDL3/Vulkan; a
  interface do laboratório usa XAML e APIs do Windows.
- SELF criptografado ainda requer a cadeia de descriptografia compatível antes de
  qualquer mapeamento executável.

## Critérios de evolução

1. Validar no console o carregamento controlado usando o `eboot.bin` extraído.
2. Implementar relocação e TLS em estruturas já validadas, sem executar código
   recebido até haver isolamento e recuperação de falhas.
3. Definir uma camada gráfica compatível com as capacidades reais do Dev Mode.
4. Ligar relocação/imports/TLS a um carregador seguro e executar um homebrew mínimo sob limites de tempo e memória.
5. Só depois avaliar jogos e desempenho.

## Referências

- [SDL3 no Windows e remoção de UWP](https://wiki.libsdl.org/SDL3/README-windows)
- [VirtualAllocFromApp](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualallocfromapp)
- [Device Portal para Xbox](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/device-portal)
