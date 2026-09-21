# shadPS4 · Xbox Lab

Aplicativo **UWP x64 / C++/WinRT / XAML** para medir a viabilidade de portar
shadPS4 ao Xbox Series X em Dev Mode. **Ainda não é um emulador PS4 no Xbox.**
O alvo é independente do CMake e das dependências desktop do núcleo.

A versão **0.25.0.0** inclui biblioteca horizontal inspirada no PS4, importação de
PKG/ELF, keysets FPKG embutidos, importação de chaves personalizadas, extração em
segundo plano e um probe de execução controlada com auditoria de requisitos runtime,
relocação e inventário de NIDs HLE. O ELF/SELF selecionado é apenas
validado e mapeado em buffer não executável; o único código executado é um ELF
mínimo gerado pelo próprio projeto, com retorno esperado `42`. O relatório também
conta segmentos dynamic/TLS, relocações e dependências importadas, valida os tipos
e os alvos das relocações em `PT_LOAD` e `PT_SCE_RELRO`, mas ainda não as aplica
nem resolve imports. As relocações relativas são aplicadas somente em uma cópia
privada não executável durante o dry-run; o arquivo selecionado continua intocado.
Os símbolos pendentes também têm seus índices e nomes conferidos contra as
tabelas internas. O relatório cruza cada NID com o registro AeroLib derivado do
núcleo e registra o nome conhecido ou a ausência de correspondência. Esse
inventário identifica símbolos e o probe cria endereços HLE temporários, mas não
altera relocações no arquivo nem executa o conteúdo. O relatório também lista os IDs codificados
das bibliotecas/módulos importados para montar a ponte HLE. Os nomes das
bibliotecas e versões são lidos da tabela de strings do ELF, sem executar o
conteúdo.

O probe também calcula um gate de runtime. Enquanto a ponte ABI, os endereços HLE
e o renderer UWP não estiverem prontos, o gate permanece bloqueado e registra os
motivos no relatório; nenhum `e_entry` recebido pode ser chamado.
O código também contém o primeiro alocador de thunks SysV→Windows, protegido como
RX e ainda sem dispatcher HLE; ele é uma fundação interna e não habilita execução.
O dispatcher agora possui slots explícitos e duas operações de baixo risco
(`sceKernelUsleep` e `sysKernelGetUpdVersion`). O probe cria um thunk temporário
para cada import detectado, mede handlers implementados e mantém `ENOSYS` para os
demais. O dispatcher agora também cobre handlers sem acesso à memória para
versão do sistema, identidade do processo, yield, estado EGL/GL, rede básica e
tela inicial. `sceKernelDebugOutText` agora traduz e valida um ponteiro de texto
contra a memória convidada somente leitura antes de enviá-lo ao log de depuração.
O probe aplica os endereços em uma cópia privada não executável para
validar as relocações, sem gravá-los no arquivo ou promover o convidado a execução.
Essa cópia também é mapeada em memória UWP com proteção por segmento: páginas
`PF_W` ficam graváveis e código/read-only continuam sem permissão de execução.
O probe interno também chama o thunk sem argumentos e exige retorno `0`, sem
envolver o ELF selecionado.

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

O pacote declara `codeGeneration` e não pede acesso à internet. Não inclui firmware,
jogos nem chaves reais. O submódulo miniz já fixado no fork fornece descompressão;
os keysets FPKG portados são os da referência GPL. A interface carrega XAML sem tipos personalizados; o build
usa C++/WinRT 2.0.250303.1 e shaders HLSL pré-compilados, sem compilador no Xbox.

## Instalar

1. Baixe o artefato da execução correspondente ao commit desejado.
2. No Dev Home, abra o endereço de Remote Access no navegador local.
3. Confira o certificado do console e faça login. Não desative a autenticação.
4. Em **Home → My games & apps → Add**, selecione `ShadPS4Xbox.appx`.
5. Selecione as dependências **x64** da pasta `Dependencies` e conclua.
6. Inicie **shadPS4 · Xbox Lab**. Use **Biblioteca** para selecionar conteúdo, extrair
um PKG ou executar o probe controlado sobre o `eboot.bin`/ELF/SELF. As etapas de inicialização ficam em
`LocalState/startup.log`, inclusive erros anteriores à criação do relatório.

A assinatura é de desenvolvimento. Builds seguintes usam certificado temporário
com o mesmo Publisher. Se o console recusar uma atualização por assinatura,
preserve LocalState e o relatório antes de qualquer reinstalação; o projeto
nunca remove aplicativos automaticamente.

## Testar

Use **Biblioteca → Importar PKG / ELF → Extrair pacote**. Depois, selecione o cartão
extraído e pressione **Executar probe controlado**. O relatório combina a validação
do arquivo selecionado com a execução do ELF mínimo do projeto; os limites estão
documentados em [EXTRACTION.md](EXTRACTION.md).

O relatório `extraction-report.json` contém versão/commit, duração, estado,
contadores e erro; pode ser exportado pela área Diagnóstico. O PKG original é
preservado. Falhas não promovem a pasta temporária a conteúdo instalado.

## Aceitação no console

Registrar: versão instalada, captura da abertura, relatório JSON, probe individual
do ELF/SELF, navegação pelo controle, suspensão/retomada e uma segunda
abertura com resultados preservados. Resultado “aprovado” significa somente que a
operação descrita naquela validação funcionou.

Consultar [PORTABILITY.md](PORTABILITY.md) para diferenças entre os probes e
os requisitos completos do núcleo. Ausência de uma evidência deve ser registrada
como pendência, jamais convertida em compatibilidade presumida.
