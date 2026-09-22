# Evidências de portabilidade

Base analisada: `42c555b7ab5d0678f531a7e4d505560ccc0f8add`.

Este alvo é um laboratório UWP dentro do fork. A extração do PKG do Apollo foi
executada no Xbox e persistiu na biblioteca com capa e metadados. A versão atual
mantém uma operação diagnóstica ativa: validação do ELF/SELF selecionado, auditoria
de requisitos runtime e execução de um ELF mínimo gerado pelo próprio projeto.

## Marco de execução do Apollo

Na build `0.62.0.0` (`824265f7`), o `eboot.bin` real chegou ao `e_entry`,
registrou `INFO: PS4_CreateDevice` e encerrou de forma controlada com código
`-1`. A chamada `_exit` não encerrou o aplicativo UWP. O traço da sessão
`1790118963-6581734-5872` mostra `sceKernelLoadStartModule` e
`scePigletSetConfigurationVSH` retornando `ENOSYS` antes da saída. Não houve
imagem PS4; esse resultado comprova a execução inicial da CPU, não a
compatibilidade gráfica.

O Apollo usa `libScePigletv2VSH` (EGL/OpenGL). O renderizador existente no
núcleo desktop é Vulkan para o caminho AMD GNM; não implementa Piglet. Portanto,
o porte do renderizador GNM, necessário para jogos que usam esse caminho, não
resolverá por si só a inicialização gráfica do Apollo. O trabalho gráfico deve
seguir duas frentes explícitas: backend UWP para o caminho GNM do núcleo e
implementação separada das chamadas Piglet usadas por homebrews como Apollo.
Não tratar `ENOSYS` desses imports como sucesso: isso ocultaria o bloqueio e
poderia fazer o convidado usar objetos gráficos inexistentes.

O relatório do Apollo na build `0.14.1.0` confirmou 265 relocações de símbolo
válidas, 265 NIDs conhecidos pelo registro AeroLib, zero NIDs desconhecidos,
8.005 relocações suportadas, zero alvos fora dos segmentos e nenhuma pendência
TLS. Esse resultado encerra os probes estruturais obrigatórios; não há outro
teste diagnóstico necessário antes do trabalho de runtime.

O inventário delimitou o primeiro porte: 131 imports de `libkernel`, 76 funções
EGL/OpenGL de `libScePigletv2VSH`, 7 funções FreeType, 5 de `libSceRegMgr` e os
serviços de áudio, controle, usuário, sistema, rede, diálogos e salvamento. O
registro de NIDs não é uma implementação: 107 desses símbolos ainda não têm
registro `LIB_FUNCTION` no núcleo desktop. A ponte ABI SysV e o carregamento
experimental de `e_entry` já foram exercitados; os imports ainda não portados
retornam `ENOSYS`. A ausência do renderer UWP para Piglet impede que esse
carregamento experimental exiba a interface do Apollo.

A build `0.46.0.0` corrige o alocador de thunks SysV→Windows para preservar os
seis registradores inteiros, a pilha convidada e os registradores XMM em uma área
separada do shadow space exigido pelo ABI Windows. Um chamador de máquina gerado
pelo projeto injeta padrões distintos em registradores e argumentos de pilha e
impede o avanço se a captura divergir. Handlers que apenas retornam constantes,
como EGL/GL, rede, versão e identidade, permanecem classificados como stubs; eles
não contam mais como suporte implementado. Imports ausentes retornam o valor
Orbis correto de `ENOSYS` (`0x8002004E`).
O núcleo compartilhado ganhou `Core::PlatformMemory`: em AppContainer ele usa as
variantes `FromApp`, preserva placeholders e views coerentes e aplica a política
RW→RX exigida por `codeGeneration`. `Core::AddressSpace` passou a consumir essa
camada sem alterar o caminho Win32 desktop. A construção completa do espaço PS4
ainda depende de substituir a reserva física monolítica por compromisso sob demanda,
pois reservar e confirmar toda a memória do PS4 excede o orçamento prático do UWP.
O probe agora cria e conta thunks para todos os imports do ELF e aplica
os endereços em uma cópia privada não executável e mapeia essa cópia com proteção
por segmento para validar a faixa de memória. A imagem agora
é uma alocação coerente em uma base válida escolhida pelo UWP; o load bias real
substitui a base fixa do dry-run. Segmentos `PF_W` e HLE compartilham os mesmos
bytes. Segmentos `PF_X` passam por uma transição RW→RX depois das relocações,
enquanto qualquer segmento W+X é rejeitado.
O primeiro handler com ponteiro,
`sceKernelDebugOutText`, rejeita endereços fora da faixa antes de usar o log do
host. A imagem convidada continua sem memória executável; isso ainda é infraestrutura de
ligação, não uma autorização de execução.
O probe também chama `clock_gettime` com um ponteiro para uma faixa `PF_W` usando
um chamador SysV gerado e valida os segundos e nanossegundos escritos pelo handler.
O primeiro bloco funcional de `libSceUserService` e `libSceSystemService` foi
implementado com validação de ponteiros e códigos de erro Orbis: inicialização,
usuário inicial, usuários logados, nome e parâmetros inteiros do sistema.
Os cinco imports usados de `libSceRegMgr` agora operam em um registro isolado em
memória, limitado a 4 KiB por valor e acessível apenas por faixas convidadas
validadas. A persistência durável será ligada à pasta do título antes da execução.
O primeiro VFS UWP liga `/app0` à raiz extraída em modo somente leitura e mantém
dados mutáveis em `RuntimeData`. Seis chamadas de descritor estão implementadas;
o probe cria, grava, sincroniza, reposiciona, lê e fecha um arquivo privado.
No Xbox, `weakly_canonical` falhou com acesso negado ao enumerar ancestrais do
LocalState; a raiz já confiável agora usa `lexically_normal` e a contenção continua
garantida pela rejeição explícita de componentes absolutos, `..` e barras invertidas.
As aliases POSIX usadas pelo Apollo agora compartilham os descritores do VFS.
Operações de existência, criação/remoção de diretório, renomeação, remoção,
permissão compatível e lock validam os mesmos limites; o probe cobre o ciclo completo.
`stat`, `_fstat` e `ftruncate` completam o bloco de metadados básicos com o layout
binário Orbis. Diretórios abertos mantêm estado próprio e `getdents` devolve
registros tipados sem expor caminhos do host. O probe da versão 0.43 valida essas
operações no armazenamento real do AppContainer.
O bloco seguinte liga mutexes, condições e semáforos POSIX a objetos nativos UWP.
Os ponteiros Orbis funcionam como slots validados e nunca recebem endereços de
objetos do host. A versão 0.44 valida o ciclo básico antes do porte de criação de threads.
Na versão 0.45, a entrada de uma thread convidada preserva os registradores não
voláteis do Windows, inclusive XMM6–XMM15. Um ELF próprio completa
`pthread_create`/`pthread_join` e retorna `42`; o `e_entry` do Apollo permanece bloqueado.
TLS específico por thread, `pthread_once` e rwlocks usam estado nativo separado da
imagem convidada. A versão 0.46 valida set/get TLS, repetição de once e ciclos de
lock/unlock de leitura e escrita antes de avançar para inicializadores do executável.
O handler `sceKernelMprotect` foi acrescentado para a camada de memória: ele
aceita somente proteções sem execução dentro das faixas `PF_W` coerentes e
retorna erro para `PROT_EXEC` ou endereços fora dessas faixas. A mudança é
validada por compilação e análise estática; sua validação no console é opcional
até que um probe de ponteiro seja necessário para o próximo marco.
Os handlers de cópia/comparação de memória (`memcpy`, `memmove`, `memset`,
`memcmp` e `strlen`) usam a mesma validação e recusam faixas fora do mapa ou
destinos que não sejam graváveis. Esse suporte prepara a camada libc do
homebrew sem alterar APIs públicas do núcleo desktop.
`mmap`/`munmap` e `sceKernelMmap`/`sceKernelMunmap` agora têm uma camada anônima
isolada: o host aloca páginas UWP, enquanto o dispatcher retorna o mesmo endereço
usado como ponteiro nativo. O suporte é limitado a mapas anônimos sem execução e não
é uma implementação do espaço de endereços completo do PS4.
O probe do diagnóstico exerce diretamente uma página anônima: grava enquanto
`PAGE_READWRITE`, muda para somente leitura sem execução, confirma a leitura,
confirma que novas escritas são recusadas e libera o mapa. Essa verificação é
necessária no Xbox para validar as APIs UWP; ela não executa o ELF selecionado.
O probe interno agora exerce um thunk sem argumentos para
`sysKernelGetUpdVersion` e exige retorno zero. Essa chamada usa somente um
handler do próprio aplicativo; ela não usa memória ELF nem executa o Apollo.

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
relocação e verifica se os alvos ficam em `PT_LOAD` ou `PT_SCE_RELRO`. Um dry-run
aplica apenas `R_X86_64_RELATIVE` em uma cópia privada não executável; ele não
altera o arquivo, resolve imports ou chama inicializadores. Os NIDs das
relocações de símbolo são comparados com o registro AeroLib do núcleo para
identificar nomes conhecidos e pendências. Essa comparação é um inventário
estático: os registros conhecidos ainda não são endereços HLE válidos no UWP.

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
