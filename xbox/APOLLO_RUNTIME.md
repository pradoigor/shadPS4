# Apollo no runtime UWP — versão 0.72

## Resultado do teste 0.71

A sessão `1790205598-105671-6976` confirmou que o contexto EGL, o áudio e o
FreeType inicializaram. O Apollo abriu as fontes e avançou até ler os próximos
recursos, mas o trace não registrou `eglSwapBuffers`; portanto, ainda não há
evidência de que um quadro ou o menu tenham sido apresentados.

A primeira falha diretamente observada foi `_readv` na sequência 8575, que
retornou `ENOSYS`. A chamada seguinte a `lseek` retornou `EINVAL`, compatível
com uma operação de recuperação após a leitura falhar. No encerramento também
apareceram `_ioctl` e `sceSystemServiceLoadExec` sem implementação, além de uma
segunda tentativa de fechar a porta de áudio já fechada. Esses eventos ocorrem
depois do bloqueio de leitura e não explicam a ausência inicial do quadro.

## Alterações da versão 0.72

- Implementa `_readv`, `readv` e `sceKernelReadv` sobre os arquivos do VFS do
  aplicativo, validando descritores iovec, buffers convidados e limites de
  transferência. Leituras curtas no fim do arquivo retornam os bytes lidos.
- A leitura simples agora aceita buffers na pilha nativa do convidado, assim
  como já faziam as operações de escrita, e limita uma chamada a 16 MiB.
- O VFS distingue descritores abertos para leitura e escrita e rejeita modos de
  acesso inválidos.
- Corrige o indicador `implemented` do trace para alguns handlers que já
  existiam, mas apareciam incorretamente como indisponíveis.

## O que falta confirmar

Esta mudança é baseada no primeiro bloqueio do trace 0.71. A versão 0.72 precisa
ser executada no Xbox para confirmar se o Apollo chega a apresentar o menu.
Ainda aparecem no trace 0.71 chamadas sem handler para `sceKernelDlsym`,
`sceKernelSendNotificationRequest`, `_ioctl` e `sceSystemServiceLoadExec`.
Além disso, o caminho de montagem de saves do Apollo depende de serviços
privilegiados e bibliotecas internas do PS4 que não existem no ambiente UWP.
Mesmo que o menu apareça, isso não significará que gerenciamento, importação ou
exportação de saves estejam funcionando.
