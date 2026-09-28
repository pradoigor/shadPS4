# Diagnóstico do runtime UWP

Ao iniciar um homebrew, o aplicativo cria uma sessão identificada por horário,
contador do sistema e processo. Um único arquivo
`LocalState/homebrew-report-<id>.jsonl` reúne os estágios, as chamadas HLE, a
saída de console (limitada a 1 MiB) e a exceção. Cada linha é um objeto JSON.
O botão **Exportar** informa o nome desse arquivo para baixar pelo Device Portal.
Sessões anteriores permanecem disponíveis em seus próprios arquivos.
Antes de carregar os PRXs e antes da entrada do executável, o evento
`import_audit` classifica cada import
como `xbox_handler`, `xbox_placeholder`, `guest_export`, `guest_data_export`,
`data_storage`, `known_stub` ou `unknown`.
Essa classificação permite priorizar lacunas antes de ocorrer uma exceção;
`xbox_handler` confirma apenas que existe um manipulador no XS4, sem garantir
que ele reproduza toda a semântica do PS4. `known_stub` significa que o catálogo
conhece o nome, sem implementação.
`xbox_placeholder` identifica retornos genéricos de sucesso (alguns são no-ops
intencionais). `data_storage` é memória substituta, sem garantir o valor ou ABI
da variável original; `guest_data_export` é uma variável ligada a um PRX.
Cada import contém `core_registered` e `core_sources`, obtidos do núcleo da
mesma revisão do build. `core_candidates` conta stubs e retornos genéricos
com registro correspondente no núcleo. Assim o relatório único já aponta
onde revisar/adaptar cada lacuna, sem depender de uma chamada ou exceção.
O evento `forward` indica que o thunk transferiu a chamada completa para um
PRX do título, preservando os registradores e a pilha da ABI SysV.
O build inclui `core-hle-inventory.json` no artefato da CI, com os NIDs
registrados pelo núcleo shadPS4 e os arquivos de origem. Cada registro é um
candidato a adaptação; o inventário não afirma compatibilidade com UWP.
Símbolos entre módulos são comparados por NID, nome/versão da biblioteca e
nome do módulo, como no linker do núcleo. IDs locais não são chaves globais. Metadados ausentes ou
exportações ambíguas não escolhem uma biblioteca arbitrariamente.

O pipeline executa o código gerado da ponte SysV em Windows x64, verificando
chamadas normais e encaminhadas, seis argumentos inteiros, oito registradores
XMM completos, AL de chamadas variádicas e argumentos/endereço da pilha.
Essa verificação também faz parte da condição de entrada do runtime no Xbox.

Uma exceção registrada inclui commit, ID da sessão, código e tipo de acesso,
endereço da falha, RIP virtual do convidado, bytes da instrução, registradores
gerais, palavras iniciais da pilha e estado/proteção das páginas consultadas.
Também registra a base virtual e o offset da imagem, metadados da região de
memória que contém a falha e endereços de retorno obtidos pela cadeia de frames
quando ela é válida; os endereços permitem mapear a falha ao EBOOT correspondente.
Os campos de memória ficam em zero quando a consulta não está disponível ou
quando não há endereço válido. O registro preserva a primeira exceção do
código convidado; um tratamento posterior não a substitui.

Esses dados localizam uma falha de execução, mas ainda não provam que o
homebrew ou um jogo terminou a inicialização. A saída visual e funcional no
Xbox continua sendo a validação final.
