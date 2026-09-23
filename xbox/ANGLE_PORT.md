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

## Ponte experimental 0.65.0.0

O PKG público do Apollo v2.3.2 foi extraído e o ELF interno analisado no Mac,
sem console. Foram identificados 264 imports, incluindo 76 Piglet/EGL/GLES.
A ponte `GuestGraphics.cpp` usa thunks SysV para as funções GLES2 e expõe
handles EGL do ANGLE. O contexto gráfico é liberado da thread da interface
antes de o convidado chamar `eglMakeCurrent` em sua própria thread.
`eglGetProcAddress` devolve um thunk SysV registrado pelo dispatcher, nunca
um ponteiro direto para a ABI Windows. O carregamento HLE dos nomes Piglet e
Shacc é restrito; SPRX do firmware não é executado como código nativo.

Esta implementação ainda precisa de validação no console. O mesmo backend
ANGLE já apresentou um quadro do host, mas nenhum quadro produzido pelo
Apollo foi confirmado. SDL2/Piglet, fontes, entrada, áudio e serviços do
aplicativo podem revelar outras incompatibilidades quando a execução avançar.

Na execução 0.65.0.0 (`1790125280-12898234-3040`), o Apollo alcançou
`eglGetDisplay`, `eglInitialize` e `eglChooseConfig`. As duas chamadas de
seleção não encontraram a configuração fixa da superfície UWP; SDL2 encerrou
com `-1` antes de criar superfície ou contexto do convidado. A versão
0.66.0.0 solicita RGBA8 e profundidade/stencil D24S8 para a superfície do
host e registra os atributos EGL solicitados pelo convidado no log do console.
Isso ainda não comprova a apresentação de quadros do Apollo.
