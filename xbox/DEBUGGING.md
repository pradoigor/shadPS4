# Diagnóstico do runtime UWP

Ao iniciar um homebrew, o aplicativo cria uma sessão identificada por horário,
contador do sistema e processo. Os arquivos ficam em `LocalState`:

- `homebrew-runtime.json`: último estágio ou primeira exceção capturada.
- `homebrew-session-<id>.jsonl`: estágios persistidos em ordem, inclusive a
  exceção, sem substituir as sessões anteriores.
- `homebrew-hle-trace.jsonl`: chamadas HLE da tentativa mais recente.
- `homebrew-hle-<id>.jsonl`: cópia da sequência HLE associada à sessão.

O botão **Exportar** inclui esses dados em `homebrew_debug` dentro do
`report-export-<horário>.json`. Ele limita a inclusão a 8192 eventos HLE;
os arquivos originais permanecem disponíveis pelo Device Portal.

Uma exceção registrada inclui commit, ID da sessão, código e tipo de acesso,
endereço da falha, RIP virtual do convidado, bytes da instrução, registradores
gerais, palavras iniciais da pilha e estado/proteção das páginas consultadas.
Os campos de memória ficam em zero quando a consulta não está disponível ou
quando não há endereço válido. O registro preserva a primeira exceção do
código convidado; um tratamento posterior não a substitui.

Esses dados localizam uma falha de execução, mas ainda não provam que o
homebrew ou um jogo terminou a inicialização. A saída visual e funcional no
Xbox continua sendo a validação final.
