# Piglet via ANGLE no Xbox

O Apollo chega a `PS4_CreateDevice`, depois chama
`sceKernelLoadStartModule` e `scePigletSetConfigurationVSH` sem handlers. A
sessão `1790118963-6581734-5872` terminou com `-1` sem imagem. O alvo desktop
do shadPS4 não implementa as 76 funções EGL/OpenGL importadas pelo Apollo.

O ANGLE oficial fornece EGL/OpenGL ES sobre Direct3D 11 e mantém alvo UWP.
O workflow `xbox-angle-uwp.yml` compila um commit fixo, sem usar o pacote
`ANGLE.WindowsStore` antigo, e guarda DLLs, bibliotecas, cabeçalhos, licença e
hashes. A compilação da dependência não a instala no APPX nem declara o
Piglet implementado.

Para a integração, o próximo trabalho é ligar `libEGL` e `libGLESv2` ao
dispatcher SysV, traduzir os tipos e ponteiros da ABI convidada, criar uma
`SwapChainPanel` na UI UWP e conectar `eglCreateWindowSurface` a ela. A chamada
de configuração Piglet só pode passar a retornar sucesso quando o backend
gráfico estiver pronto. O carregamento de módulos do sistema PS4 também
precisa de uma política HLE própria; não se deve carregar SPRX do firmware
como DLL nativa do Xbox.

O caminho GNM do shadPS4, usado por jogos, permanece separado: o renderer
Vulkan desktop não é substituído pelo ANGLE. Ele precisará de um backend
compatível com o Xbox e da integração dos serviços gráficos do núcleo.
