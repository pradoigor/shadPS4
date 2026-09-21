# shadPS4 · Xbox Lab

Aplicativo **UWP x64 / C++/WinRT / XAML** para medir a viabilidade de portar
shadPS4 ao Xbox Series X em Dev Mode. **Ainda não é um emulador PS4 no Xbox.**
O alvo é independente do CMake e das dependências desktop do núcleo.

O marco de interface inclui as áreas **Biblioteca** e **Diagnóstico**. Em
Biblioteca, o usuário pode selecionar um ELF/SELF e persistir uma cópia no
armazenamento do aplicativo. A tela identifica explicitamente que o loader do
núcleo ainda não está conectado; selecionar um arquivo não o executa.

## Compilar

O workflow **Xbox UWP diagnostics** compila a branch `xbox-uwp` no Windows 2022.
Os artefatos contêm APPX assinado para desenvolvimento, certificado público,
dependências, símbolos, log MSBuild e `build-info.json` com SHA-256 e commit.
A chave privada temporária é removida do runner e nunca integra o artefato.

Localmente em Windows, instale Visual Studio 2022 com C++ x64, ferramentas C++
UWP, SDK 10.0.22621.0 e Python 3.12, então execute no PowerShell:

```powershell
./xbox/scripts/build.ps1
```

O pacote declara `codeGeneration` para o teste de código próprio e não pede
acesso à internet. Não inclui firmware, jogos, chaves ou código de terceiros
adicional ao fork. A interface carrega XAML sem tipos personalizados; o build
usa C++/WinRT 2.0.250303.1 e shaders HLSL pré-compilados, sem compilador no Xbox.

## Instalar

1. Baixe o artefato da execução correspondente ao commit desejado.
2. No Dev Home, abra o endereço de Remote Access no navegador local.
3. Confira o certificado do console e faça login. Não desative a autenticação.
4. Em **Home → My games & apps → Add**, selecione `ShadPS4Xbox.appx`.
5. Selecione as dependências **x64** da pasta `Dependencies` e conclua.
6. Inicie **shadPS4 · Xbox Lab**. A abertura não inicia testes; use **Biblioteca** para selecionar conteúdo ou **Diagnóstico** para consultar o probe ativo. As etapas de inicialização ficam em `LocalState/startup.log`, inclusive erros anteriores à criação do relatório.

A assinatura é de desenvolvimento. Builds seguintes usam certificado temporário
com o mesmo Publisher. Se o console recusar uma atualização por assinatura,
preserve LocalState e o relatório antes de qualquer reinstalação; o projeto
nunca remove aplicativos automaticamente.

## Testar

- **Biblioteca:** seleciona extensões `.elf`, `.self` ou `.bin` e copia o arquivo para `LocalState/Library`. Nesta versão não há carregamento nem execução do conteúdo.

- **Executar testes automáticos:** armazenamento, orçamento, D3D11 com readback,
  dispositivo D3D12 e mapeamento RW pequeno. Sem resultados sintéticos.
- **Executar selecionado:** executa um teste, inclusive os testes isolados de
  endereço, proteção e código x64. Cada início é persistido e sincronizado antes
  de executar. Após interrupção, o resultado passa a **inconclusivo**.
- **Imagem/áudio:** exigem confirmação humana explícita. Não confirme um teste
  de áudio apenas porque a API aceitou a reprodução.
- **Controle:** pressione X após iniciar o teste; sem entrada em 30 segundos,
  o resultado é inconclusivo. Navegação da interface deve ser verificada também.
- **Ciclo de vida:** execute, saia para o painel e retorne. Aprovação exige os
  eventos reais Suspending e Resuming na mesma sessão. Fechar e reabrir durante
  esse teste produz inconclusivo, não um falso sucesso.
- **Exportar JSON:** grava arquivo com data em LocalState; baixe pelo File
  explorer do Device Portal. `report.json` sempre contém o estado mais recente.

O relatório inclui versão, commit, versão do sistema, resultado, duração,
medições e erro. Resultados de outra build ficam em `previous-build-report.json`
e não são atribuídos à build nova. Relatório inválido é preservado separadamente.

## Aceitação no console

Registrar: versão instalada, captura da abertura, relatório JSON, execução
individual dos probes, confirmação visual/auditiva, navegação pelo controle,
suspensão/retomada e uma segunda abertura com resultados preservados. Resultado
“aprovado” significa somente que a operação descrita naquele teste funcionou.

Consultar [PORTABILITY.md](PORTABILITY.md) para diferenças entre os probes e
os requisitos completos do núcleo. Ausência de uma evidência deve ser registrada
como pendência, jamais convertida em compatibilidade presumida.
