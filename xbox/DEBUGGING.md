# Diagnóstico do runtime UWP

Ao iniciar um homebrew, o aplicativo cria uma sessão identificada por horário,
contador do sistema e processo. Um único arquivo
`LocalState/homebrew-report-<id>.jsonl` reúne os estágios, as chamadas HLE, a
saída de console (limitada a 1 MiB) e a exceção. Cada linha é um objeto JSON.
O botão **Exportar** informa o nome desse arquivo para baixar pelo Device Portal.
Sessões anteriores permanecem disponíveis em seus próprios arquivos.

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
