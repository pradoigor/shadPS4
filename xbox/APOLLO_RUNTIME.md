# Apollo: execução UWP, versão 0.69

## Evidência que motivou a correção

Na sessão `1790127459-15077140-4548` (0.68), o contexto EGL foi criado e
o Apollo enviou as texturas dos caracteres bitmap. As chamadas 8569 e 8570,
`FT_Init_FreeType` e `FT_New_Face`, receberam ENOSYS. Em seguida o programa
executou sua rotina de encerramento e `_exit(-1)`, sem `eglSwapBuffers`.
O erro em `_ioctl` ocorreu durante a limpeza, depois da falha de fontes.

## Implementação

- FreeType estático UWP, fixado no submódulo upstream, com estruturas públicas
  LP64 espelhadas para impedir a exposição de estruturas LLP64 ao convidado.
- Todas as sete funções FreeType importadas por esta versão do Apollo e
  `FT_Done_Face`; arquivos em memória são copiados e mantidos até liberar a face.
- Fontes de sistema substituídas explicitamente por Noto: ver `FONTS.md`.
- Alias `/mnt/sandbox/<processo>/app0` resolvido para a instalação atual, com
  limites de caminho e proteção de escrita iguais a `/app0`. O binário Apollo
  usa esse prefixo em suas fontes, imagens, idiomas e música; rejeitá-lo impediria
  o carregamento dos recursos mesmo depois de implementar FreeType.
- `scePadReadState` lê botões, gatilhos e analógicos pelo Windows.Gaming.Input.
  A/B/X/Y correspondem a cross/circle/square/triangle, Menu a Options e View
  ao clique do touchpad. Vibração usa os motores do Xbox; não há sensor de
  movimento, coordenadas de touchpad ou lightbar simulados como disponíveis.
- AudioOut PCM mono/estéreo S16 ou float, 48 kHz, via XAudio2; saída síncrona
  com buffer próprio e timeout. Os parâmetros observados do Apollo são
  256 amostras, 48 kHz, estéreo S16. O renderer não depende de retorno fictício
  de sucesso para reproduzir áudio.
- O trace conserva startup completo e, após 32768 chamadas, registra erros,
  APIs ausentes, apresentações, saída e uma amostra por 512 chamadas. O trace
  recente gira a cada 4096 chamadas; o arquivo de sessão mantém o histórico.

## Verificação e limites

Os testes de fontes executam rasterização real Latin/CJK, leitura das estruturas
no formato PS4, liberação dos bytes de origem, handles inválidos e fontes
malformadas. O mesmo teste roda no Mac LP64 e no Windows LLP64 do Actions.
O pacote deve conter fontes e licenças além das DLLs ANGLE verificadas.

Ainda é necessária a execução no Xbox para validar a imagem produzida pelo
Apollo e os backends de dispositivos. Serviços de saves PS4, diálogos do
sistema, rede e funções de jailbreak do Apollo não equivalem aos recursos do
Xbox e não estão completos. Chegar ao menu não demonstra essas funcionalidades.
