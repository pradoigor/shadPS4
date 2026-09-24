# Apollo no runtime UWP — versão 0.78

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

## Resultado do teste 0.74

A sessão `1790210045-4552031-5816` confirmou que a conversão funcionou: os três
shaders registrados foram aceitos pelo compilador ANGLE, houve duas chamadas de
`glLinkProgram`, 512 chamadas de desenho e 256 trocas de buffer. O viewport
convidado foi `(0,0,1920,1080)`. Isso confirma atividade de desenho do Apollo,
mas o trace não identifica a tela mostrada nem permite avaliar seu centramento.

O aplicativo fechou com a mesma instrução e violação de acesso da versão 0.73,
tentando ler uma página fora da imagem mapeada. O trace termina após abrir
`/data/apollo/cache/ver.check` e receber `ENOSYS` de `_ioctl` para `TIOCGWINSZ`;
isso é uma pista, mas ainda não prova que a chamada causou a exceção.

## Alterações da versão 0.75

- Implementa `sceNetCtlGetInfo` com o resultado PS4 de rede indisponível usado
  pelo shadPS4, para o Apollo seguir o caminho offline em vez de receber `ENOSYS`.
- Implementa o comportamento POSIX de `_ioctl(TIOCGWINSZ)`: dimensões para
  descritores de terminal e `ENOTTY` para arquivos comuns.
- Registra as dimensões em pixels da superfície EGL e os primeiros viewports
  convidados para comparar a resolução lógica com a área real de apresentação.

## Resultado do teste 0.75

A sessão `1790211450-5956984-6644` confirma compilação dos três shaders Piglet,
512 chamadas de desenho e 256 apresentações de buffer. O log também mostra a
causa objetiva do enquadramento: Apollo pediu viewport de `1920x1080`, mas a
superfície EGL do Xbox tinha `960x540` pixels. Como o viewport não era adaptado,
o desenho ocupava apenas uma parte da área real e parecia descentralizado.

O trace tem 18.418 chamadas HLE completas; a última chamada registrada,
`clock_gettime`, retornou normalmente. Depois disso, o EBOOT sofreu uma leitura
inválida (`0xC0000005`) no endereço `0x1E93CE` relativo à imagem convidada. Os
bytes da instrução começam por `mov rax,[rdi]`; `rdi` apontava para fora da imagem
e para uma página `PAGE_NOACCESS`. Isso mantém a origem do ponteiro inválido em
aberto: os arquivos não identificam uma chamada HLE específica como causa.

## Alterações da versão 0.76

- Adapta ao tamanho da superfície EGL um viewport convidado de tela inteira
  somente quando ele excede a superfície e mantém praticamente a mesma
  proporção. Viewports menores, subáreas e conversões de proporção não são
  alterados.
- Registra o viewport solicitado e o efetivamente aplicado para verificar o
  enquadramento no próximo teste.

Essa correção trata o enquadramento observado; ainda é necessário verificar a
imagem no console. O fechamento por acesso inválido permanece sem correção até
que haja evidência suficiente para apontar a origem do ponteiro.

## Alterações da versão 0.77

A sessão `1790212518-7025015-272` confirmou o viewport centralizado em
`960x540`, mas o Apollo encerrou com uma leitura inválida. O trace identifica
uma sequência de inicialização de rede seguida da tentativa de abrir
`/data/apollo/cache/ver.check`, usada pela verificação automática de versão do
Apollo. O código do Apollo trata a falha de `fopen` retornando do callback de
atualização.

- `sceSysmoduleLoadModuleInternal` deixa de reportar NET e NETCTL como carregados
  quando o runtime UWP não fornece a pilha de rede PS4; esses IDs recebem
  `ENOSYS`.
- A abertura POSIX de escrita apenas para `/data/apollo/cache/ver.check` retorna
  `-1` com `ENETUNREACH`. Isso faz o Apollo pular a verificação automática de
  atualização pelo caminho de erro previsto no próprio código. Não habilita
  acesso à internet nem downloads online.
- O relatório de exceção registra a base virtual, o offset na imagem, a região
  de memória da falha e os endereços de retorno recuperáveis do stack frame.

O teste no Xbox precisa confirmar se o Apollo agora chega e permanece no menu.
As funções online, gerenciamento de saves e execução de jogos continuam fora
do que esse marco comprova.

## Resultado do teste 0.77 e correção 0.78

A sessão `1790215832-10339218-6036` mostrou uma regressão introduzida na 0.77:
na chamada HLE 27, `sceSysmoduleLoadModuleInternal(0x80000010)` recebeu
`0x8002004E`; em seguida o homebrew chamou `_exit(-1)`, sem apresentar um
quadro. Na sessão 0.76, a mesma chamada retornou zero e a execução avançou até
os quadros do Apollo. O resultado negativo de carregamento do módulo não é um
caminho tolerado nessa inicialização.

A versão 0.78 restaura o retorno zero anterior para a carga interna de módulos.
O tratamento de rede offline permanece na consulta `sceNetCtlGetInfo` e na
abertura POSIX de escrita de `/data/apollo/cache/ver.check`; o relatório de
exceção ampliado da 0.77 também permanece. O Xbox ainda precisa confirmar se a
falha de abertura evita a leitura inválida posterior ao primeiro quadro.

## O que falta confirmar

Os traces mostram que o runtime ainda não fornece todos os serviços privados do
PS4 usados pelo Apollo, incluindo `sceKernelDlsym`, montagem de saves e
`sceSystemServiceLoadExec`. A versão 0.75 cobre somente a consulta de estado de
rede e o `TIOCGWINSZ` observados nesta sessão; ela não implementa acesso a rede,
serviços privados ou saves.
Além disso, o caminho de montagem de saves do Apollo depende de serviços
privilegiados e bibliotecas internas do PS4 que não existem no ambiente UWP.
Mesmo que o menu apareça, isso não significará que gerenciamento, importação ou
exportação de saves estejam funcionando.
