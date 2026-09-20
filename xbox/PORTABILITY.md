# Evidências de portabilidade

Base analisada: `42c555b7ab5d0678f531a7e4d505560ccc0f8add`.

Este alvo é um laboratório UWP independente dentro do fork. Não liga o núcleo,
não carrega ELF/PKG e não executa jogos. Resultado aprovado em um teste não
equivale a aprovação do subsistema completo do emulador.

| Área | Evidência no código original | Diagnóstico implementado | Ainda necessário |
|---|---|---|---|
| GPU | `src/video_core/renderer_vulkan/vk_instance.cpp`: Vulkan e extensões obrigatórias | D3D11 hardware, shader com readback, triângulo e criação de dispositivo D3D12 | Backend ou tradução compatível; shaders PS4, caches, sincronização e apresentação |
| Janela/entrada | `CMakeLists.txt`: SDL3 | XAML UWP, SwapChainPanel, Windows.Gaming.Input | Adaptar os consumidores SDL3; a distribuição oficial removeu UWP |
| Espaço de endereços | `src/core/address_space.cpp`: VirtualAlloc2 e placeholders | Reservas pequenas em três endereços representativos | Layout completo, colisões, alinhamentos e reservas simultâneas |
| Backing/alias | Mesmo arquivo: CreateFileMapping2, MapViewOfFile3 e backing grande executável | Duas visões de 64 KiB usando APIs FromApp | Placeholders, aliases executáveis e orçamento real do backing PS4 |
| Execução | Mesmo arquivo: PAGE_EXECUTE_READWRITE; `src/core/linker.cpp`: carregamento/execução | Seis bytes x64 próprios, RW para RX, retorno 42 | ABI, relocação, TLS, instruções, bibliotecas e execução de homebrew |
| Exceções | `src/common/signal_context.cpp`, contexto por plataforma | Interrupções detectadas por journal persistente | Compatibilidade do mecanismo de tratamento de exceções do núcleo; journal não substitui um handler |
| Sistema/arquivos | Dependências desktop, bibliotecas e caminhos do núcleo | LocalState, persistência e áudio UWP | Adaptar acesso ao conteúdo e módulos, threads e dependências |

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
