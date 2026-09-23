# Apollo no runtime UWP — versão 0.73

## Resultado do teste 0.71

A sessão `1790205598-105671-6976` mostrou que o Apollo chegou a inicializar
EGL, áudio e FreeType, mas `_readv` ainda retornava `ENOSYS` na sequência 8575.
Não houve `eglSwapBuffers`.

## Resultado do teste 0.72

A sessão `1790207106-1612984-3300` confirmou que o contexto EGL, o áudio e o
FreeType inicializaram. `_readv` funcionou em 169 chamadas, mas o trace ainda
não registrou `eglSwapBuffers`; portanto, não há evidência de que um quadro ou
o menu tenham sido apresentados.

Na sequência 11007, `_readv` retornou `EFAULT` ao validar um dos buffers do
segundo vetor. Em seguida o Apollo liberou o recurso, tentou reposicionar o
arquivo e encerrou com `-1`. A sequência 11017 (`sceSystemServiceLoadExec`) e a
segunda tentativa de fechar o áudio ocorreram durante a saída.

## Alterações da versão 0.73

- A validação de ponteiros também aceita páginas privadas do processo que
  estejam comprometidas e com proteção de leitura/escrita apropriada. Isso
  cobre buffers do heap C/C++ do convidado que não pertençam às regiões ELF ou
  `mmap` registradas pelo runtime.
- Uma falha de `_readv` agora registra no log de console o índice do vetor,
  endereço e tamanho rejeitados, sem registrar conteúdo do arquivo.

## O que falta confirmar

Esta mudança é baseada no primeiro erro do trace 0.72. A versão 0.73 precisa
ser executada no Xbox para confirmar se o Apollo chega a apresentar o menu.
O trace 0.72 ainda mostra que `sceKernelLoadStartModule` não encontrou
`libSceFsInternalForVsh.sprx`; em consequência `sceKernelDlsym` não pode resolver
os símbolos privados do PS4. `sceKernelSendNotificationRequest`, `_ioctl` e
`sceSystemServiceLoadExec` também continuam sem handler.
Além disso, o caminho de montagem de saves do Apollo depende de serviços
privilegiados e bibliotecas internas do PS4 que não existem no ambiente UWP.
Mesmo que o menu apareça, isso não significará que gerenciamento, importação ou
exportação de saves estejam funcionando.
