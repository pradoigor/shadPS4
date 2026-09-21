# Evidências de portabilidade

Base analisada: `42c555b7ab5d0678f531a7e4d505560ccc0f8add`.

Este alvo é um laboratório UWP dentro do fork. Ele já compila diretamente os
headers originais `common/endian.h`, `core/file_format/psf.h`, `core/loader/elf.h`
e `core/file_sys/ifile.h`, além do codec original `src/core/file_format/psf.cpp`.
O único teste ativo compila o `src/core/loader/elf.cpp` original e executa
`Elf::Open(std::unique_ptr<IFile>)` com um backend UWP derivado de `IFile`; os
probes anteriores ficam apenas nas evidências históricas. Ainda não liga o
núcleo completo, não carrega ELF/PKG e não executa jogos. Resultado aprovado em um teste não
equivale a aprovação do subsistema completo do emulador.

| Área | Evidência no código original | Diagnóstico implementado | Ainda necessário |
|---|---|---|---|
| GPU | `src/video_core/renderer_vulkan/vk_instance.cpp`: Vulkan e extensões obrigatórias | D3D11 hardware, shader com readback, triângulo e criação de dispositivo D3D12 | Backend ou tradução compatível; shaders PS4, caches, sincronização e apresentação |
| Janela/entrada | `CMakeLists.txt`: SDL3 | XAML UWP, SwapChainPanel, Windows.Gaming.Input | Adaptar os consumidores SDL3; a distribuição oficial removeu UWP |
| Espaço de endereços | `src/core/address_space.cpp`: VirtualAlloc2 e placeholders | Reservas pequenas em três endereços representativos | Layout completo, colisões, alinhamentos e reservas simultâneas |
| Backing/alias | Mesmo arquivo: CreateFileMapping2, MapViewOfFile3 e backing grande executável | Duas visões de 64 KiB usando APIs FromApp | Placeholders, aliases executáveis e orçamento real do backing PS4 |
| Execução | Mesmo arquivo: PAGE_EXECUTE_READWRITE; `src/core/linker.cpp`: carregamento/execução | Seis bytes x64 próprios, RW para RX, retorno 42 | ABI, relocação, TLS, instruções, bibliotecas e execução de homebrew |
| Exceções | `src/core/signals.cpp`: AddVectoredExceptionHandler | Interrupções detectadas por journal persistente | Compatibilidade do mecanismo de tratamento de exceções do núcleo; journal não substitui um handler |
| Sistema/arquivos | Dependências desktop, bibliotecas e caminhos do núcleo | LocalState, persistência e áudio UWP | Adaptar acesso ao conteúdo e módulos, threads e dependências |
| Formatos do núcleo | `common/endian.h`, `core/file_format/psf.h`, `psf.cpp`, `core/loader/elf.h`, `elf.cpp` e `core/file_sys/ifile.h` | `Elf::Open` original e `FileReader` leem SELF, segmento e program header sintéticos por `IFile` UWP | SELF real/descriptografia, carregamento de segmentos e fontes restantes com dependências de logging/assert |

## Bloqueios confirmados do núcleo

- `src/core/address_space.cpp` requer placeholders e backing executável por
  `VirtualAlloc2`, `CreateFileMapping2` e `MapViewOfFile3`. As três chamadas
  falham no link UWP, embora as alternativas `FromApp` usadas pelos probes funcionem.
- `src/core/signals.cpp` depende de tratamento de exceções vetorizadas e de
  componentes do emulador. A API isolada liga para UWP, mas o fluxo completo ainda
  precisa ser exercitado com código convidado.
- A janela, entrada e áudio do núcleo dependem de SDL3. O alvo Xbox usa XAML,
  `Windows.Gaming.Input` e mídia UWP.
- O renderer em `src/video_core/renderer_vulkan` não pode usar diretamente os
  dispositivos D3D11/D3D12 validados pelo laboratório; é necessário um backend
  gráfico próprio ou uma camada Vulkan realmente disponível no Xbox Dev Mode.

`api-surface.json`, quando produzido no Windows, registra se chamadas nativas
representativas compilam e ligam para UWP x64. Compilar não comprova que elas
funcionam no Xbox; falhar não prova que uma alternativa não exista.

## Critérios de evolução

1. Confirmar pacote, abertura, relatórios e resultados no console físico.
2. Verificar operações de memória completas do núcleo, não apenas os probes.
3. Definir estratégia gráfica com base nas capacidades medidas e requisitos Vulkan.
4. Portar dependências e executar homebrew PS4 mínimo com rastreamento de falhas.
5. Só depois selecionar jogos e medir compatibilidade e desempenho.

## Referências primárias

- [SDL3 no Windows e remoção de UWP](https://wiki.libsdl.org/SDL3/README-windows)
- [VirtualAllocFromApp](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualallocfromapp)
- [Device Portal para Xbox](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/device-portal-xbox)
- [APIs do Device Portal](https://learn.microsoft.com/en-us/windows/uwp/debug-test-perf/device-portal-api-core)
