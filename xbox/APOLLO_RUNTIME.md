# Apollo no runtime UWP — versão 0.74

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

## Resultado do teste 0.73

A sessão `1790208047-2554687-1824` registrou 256 chamadas bem-sucedidas a
`eglSwapBuffers`, mas nenhuma criação de programa, ligação de shaders ou chamada
de desenho. O Apollo enviou repetidamente `glShaderBinary` com formato `0`,
contêineres de 1249 bytes e shaders que nunca alcançaram o estágio de programa.
Isso explica o quadro preto: apresentar o buffer EGL não significa que houve
desenho visível.

O contêiner Piglet observado no EBOOT contém GLSL junto com microcódigo específico
do PS4. O caminho da versão 0.73 encaminhava o contêiner inteiro ao ANGLE, que não
consome esse formato proprietário. A versão 0.74 reconhece o cabeçalho Piglet,
valida o tamanho da fonte embutida e pede ao ANGLE que compile esse GLSL.

O aplicativo também registrou uma violação de acesso `0xC0000005`. A instrução
estava dentro do EBOOT, mas o endereço lido ficava fora da imagem ELF e em uma
página `PAGE_NOACCESS`. O campo `fault_domain` anterior classificava o endereço
pela localização da instrução; na versão 0.74, `instruction_domain` e
`fault_domain` separam essas duas informações. A causa da exceção ainda não está
identificada pelo relatório atual.

## Alterações da versão 0.74

- Traduz `glShaderBinary` Piglet para `glShaderSource` e `glCompileShader` do
  ANGLE quando o contêiner inclui uma fonte GLSL íntegra. Formatos diferentes
  continuam no caminho nativo existente.
- Registra formato, tamanhos, resultado de compilação e, em caso de falha, a
  mensagem do compilador para as primeiras tentativas, sem salvar a fonte.
- Separa o domínio da instrução do domínio do endereço que causou a falha.

Essa adaptação ainda precisa ser verificada no Xbox. Ela cobre a conversão de
shader Piglet observada no Apollo, não todo o renderer Piglet nem os shaders GNM
de jogos PS4.

## O que falta confirmar

O trace 0.73 mostra que `sceKernelLoadStartModule` não encontrou
`libSceFsInternalForVsh.sprx`; em consequência `sceKernelDlsym` não pode resolver
os símbolos privados do PS4. `sceKernelSendNotificationRequest`, `_ioctl` e
`sceSystemServiceLoadExec` também continuam sem handler.
Além disso, o caminho de montagem de saves do Apollo depende de serviços
privilegiados e bibliotecas internas do PS4 que não existem no ambiente UWP.
Mesmo que o menu apareça, isso não significará que gerenciamento, importação ou
exportação de saves estejam funcionando.
