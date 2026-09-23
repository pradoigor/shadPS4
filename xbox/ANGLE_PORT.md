# Piglet via ANGLE no Xbox

O Apollo chega a `PS4_CreateDevice`, depois chama
`sceKernelLoadStartModule` e `scePigletSetConfigurationVSH` sem handlers. A
sessão `1790118963-6581734-5872` terminou com `-1` sem imagem. O alvo desktop
do shadPS4 não implementa as 76 funções EGL/OpenGL importadas pelo Apollo.

O ANGLE oficial fornece EGL/OpenGL ES sobre Direct3D 11 e mantém alvo UWP.
O workflow `xbox-angle-uwp.yml` compila o commit
`dba7ad242852bfb3775c490cb8c567f234e2a649` (linha Chromium 7700,
SDK Windows 10.0.26100), sem usar o pacote
`ANGLE.WindowsStore` antigo, e guarda DLLs, bibliotecas, cabeçalhos, licença e
hashes. A compilação da dependência, isoladamente, não declara o Piglet
implementado.

A build `35799790663` compilou `libEGL.dll` e `libGLESv2.dll` UWP x64. A
versão `0.63.0.0` inclui os binários em `xbox/third_party/angle` e os copia
para o APPX após verificar os hashes; a licença e a proveniência também vão
no pacote. A versão `0.64.0.0` usa as DLLs num `SwapChainPanel` UWP. No Xbox,
o relatório `report-export-1790123324.json` registrou EGL 1.5, GLES2 e
`eglSwapBuffers` aprovado em 960×540; o usuário confirmou que o quadro
apareceu. O mesmo relatório registrou o Apollo encerrando com `-1`: o teste
prova a apresentação gráfica do host, não a tradução da API gráfica PS4.

O código-fonte público do Apollo vincula SDL2 e `libScePigletv2VSH` ao ELF.
O trace da versão `0.64.0.0` chega a `sceKernelLoadStartModule` e
`scePigletSetConfigurationVSH`; ambas estão sem handler. Para a integração,
o próximo trabalho é ligar EGL/GLES do convidado ao dispatcher SysV, traduzir
os tipos e ponteiros da ABI convidada e compartilhar a superfície UWP já
validada. A chamada de configuração Piglet só pode passar a retornar sucesso
quando esse backend estiver disponível para o convidado. O carregamento de
módulos do sistema PS4 também precisa de uma política HLE própria; não se
deve carregar SPRX do firmware como DLL nativa do Xbox.

O caminho GNM do shadPS4, usado por jogos, permanece separado: o renderer
Vulkan desktop não é substituído pelo ANGLE. Ele precisará de um backend
compatível com o Xbox e da integração dos serviços gráficos do núcleo.
