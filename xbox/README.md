# shadPS4 · Xbox Lab

Aplicativo **UWP x64 / C++/WinRT / XAML** para medir a viabilidade de portar
shadPS4 ao Xbox Series X em Dev Mode. **Ainda não é um emulador PS4 no Xbox.**
O alvo é independente do CMake e das dependências desktop do núcleo.

A versão **0.3.2.0** inclui biblioteca horizontal inspirada no PS4, importação de
PKG/ELF, importação local de chaves e extração experimental em segundo plano,
com progresso, cancelamento e relatório. Consulte [EXTRACTION.md](EXTRACTION.md)
para formatos suportados, instruções, testes e limites. Extrair não executa o jogo.

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
acesso à internet. Não inclui firmware, jogos nem chaves reais. O submódulo miniz já
fixado no fork fornece descompressão; o artefato de teste usa apenas chaves
RSA sintéticas geradas no CI. A interface carrega XAML sem tipos personalizados; o build
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

Use **Biblioteca → Importar PKG / ELF → Extrair pacote**. Os diagnósticos antigos
foram retirados. O fluxo desta versão e o pacote sintético incluído no artefato
estão documentados em [EXTRACTION.md](EXTRACTION.md).

O relatório `extraction-report.json` contém versão/commit, duração, estado,
contadores e erro; pode ser exportado pela área Diagnóstico. O PKG original é
preservado. Falhas não promovem a pasta temporária a conteúdo instalado.

## Aceitação no console

Registrar: versão instalada, captura da abertura, relatório JSON, execução
individual dos probes, confirmação visual/auditiva, navegação pelo controle,
suspensão/retomada e uma segunda abertura com resultados preservados. Resultado
“aprovado” significa somente que a operação descrita naquele teste funcionou.

Consultar [PORTABILITY.md](PORTABILITY.md) para diferenças entre os probes e
os requisitos completos do núcleo. Ausência de uma evidência deve ser registrada
como pendência, jamais convertida em compatibilidade presumida.
