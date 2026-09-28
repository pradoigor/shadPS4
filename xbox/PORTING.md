# Integração progressiva com o núcleo shadPS4

O runtime Xbox permanece em uso. O objetivo é substituir lacunas por código
adaptado do núcleo e validar cada subsistema antes de ampliar a compatibilidade.

## Entregue nesta etapa

- Inventário de registros HLE gerado da mesma árvore usada no build.
- Relatório único com todos os imports antes da execução e seus arquivos
  correspondentes no núcleo, incluindo stubs, retornos genéricos e dados substitutos.
- Identificação de exportações por NID, biblioteca/versão e nome do módulo, seguindo
  o princípio usado por `src/core/linker.cpp`; IDs locais de ELFs não são globais.
- Preferência consistente por handlers Xbox no executável e nos imports dos PRXs.
- Encaminhamento de funções ausentes para exportações correspondentes dos PRXs,
  com teste executável da ponte SysV em Windows x64.

## Trabalho de portabilidade ainda necessário

1. Adaptar famílias de HLE apontadas por `core_sources`, revisando seus contratos,
   estruturas PS4, erros e dependências de plataforma. Um registro pode conter
   apenas um stub; não deve ser copiado ou contado como suporte completo.
2. Completar o carregador: dependências transitivas, ordem de inicialização,
   ligação de dados do executável e inicialização de TLS dos módulos.
3. Comparar os contratos do VFS, threads e sincronização com o núcleo. A existência
   de um handler não prova equivalência semântica.
4. Integrar os subsistemas de emulação ainda ausentes. O inventário HLE e os PRXs
   de um título não substituem o restante do emulador, nem garantem todos os PKGs.

As verificações de CI cobrem o pacote, símbolos e a ponte de chamadas em Windows.
A execução dos títulos no Xbox permanece uma etapa de validação separada.
