# Extração e execução controlada UWP 0.37.0

O aplicativo executa extração, inspeciona metadados e oferece carregamento controlado. A biblioteca
abre ao iniciar, usa uma faixa horizontal navegável pelo controle e distingue
arquivos importados de diretórios extraídos. O visual azul e as ondas são próprios,
inspirados no PS4, sem recursos gráficos da Sony.

## Uso

1. Atualize o APPX, preservando LocalState.
2. O aplicativo usa por padrão os keysets FPKG portados do fluxo do shadPS4,
   sem pedir arquivo adicional. Importe `keys.json` pelo botão **Importar chaves**
   para substituir o padrão quando um pacote exigir outro conjunto. São necessários
   `PkgDerivedKey3Keyset` e `FakeKeyset`, com
   `PublicExponent` (4 bytes), `Modulus`, `PrivateExponent` (256 cada) e
   `Prime1`, `Prime2`, `Exponent1`, `Exponent2`, `Coefficient` (128 cada), em
   hexadecimal. O aplicativo valida todos os componentes antes da operação RSA.
   O aplicativo lê o modo do cabeçalho PFS antes de pedir chaves: imagens sem o
   bit de criptografia seguem o caminho sem chaves; imagens XTS exigem os conjuntos
   RSA. A presença isolada de `entry_keys`/`image_key` não dispara mais um pedido
   falso. O PKG do Apollo v2.3.2 foi confirmado como XTS-criptografado (modo PFS
   0x000D), portanto ele usa os keysets FPKG embutidos nesta versão.
3. Importe ou selecione o PKG já existente e pressione **Extrair pacote**.
4. Acompanhe arquivos/MiB; **Cancelar extração** interrompe entre blocos.
5. Após terminar, confira o cartão **EXTRAÍDO**, feche e reabra o aplicativo.
6. Se houver `eboot.bin`, pressione **Executar probe controlado**. A operação lê os
   cabeçalhos, verifica limites dos segmentos e mapeia bytes em buffer privado não
   executável. Em seguida, executa somente um ELF mínimo gerado pelo projeto, que
   retorna `42`; o arquivo selecionado nunca é chamado. O relatório também registra
   metadados `PT_DYNAMIC`, `PT_TLS`, relocações e imports para orientar o carregador.
   As relocações são lidas, classificadas e conferidas contra `PT_LOAD` e
   `PT_SCE_RELRO`, mas não são aplicadas no arquivo recebido. Relocações relativas
   são exercitadas em uma cópia privada não executável para medir o próximo bloqueio.
   Os símbolos usados por relocação também são conferidos nas tabelas de strings e símbolos,
   e seus nomes únicos e IDs de bibliotecas/módulos são exportados para preparar
   o mapeamento HLE. Nomes de bibliotecas, módulos e versões vêm da tabela de
   strings e não são inferidos pelo aplicativo.
   Baixe `LocalState/report.json`
   ou use **Diagnóstico → Exportar JSON**.

Se o seletor de arquivos não estiver disponível no console, envie o PKG para
`LocalState/Library` e as chaves para `LocalState/keys.json` pelo Device Portal,
então reabra a Biblioteca. Preserve qualquer keys.json anterior antes de trocar
o conjunto.

Os keysets portados fazem parte do código GPL de referência; chaves personalizadas
ficam somente em LocalState e não entram no relatório. A importação valida
tamanho/formato; a operação RSA verifica a compatibilidade com o pacote.

## Portabilidade e correções

Referência de formato: `AzaharPlus/shadPS4Plus`, commit `9d2e761`, arquivos
`src/core/file_format/pkg.cpp`, `pfs.h` e `src/core/crypto/crypto.cpp`, GPL-2.0-or-later.
As atribuições estão preservadas no código adaptado. Essa referência é um fork;
não é o extrator atual do QtLauncher oficial. A presença de campos de chaves no
QtLauncher não comprova um instalador ativo. Explicações anteriores que equiparavam
esses dois fluxos estavam incorretas.

- Crypto++ substituído por Windows CNG (bcrypt), disponível no alvo UWP.
- RSA PKCS#1 v1.5 verifica retorno e tamanho de 32 bytes, em vez de usar saída
  possivelmente inválida. SHA-256, HMAC, AES-CBC e XTS de setores de 4096 bytes.
- miniz obtido do submódulo já fixado pelo repositório, com API de descompressão
  limitada a blocos de 65536 bytes e verificação de erro.
- Offsets e tamanhos de arquivos em 64 bits; tabelas e alocações limitadas.
- Bloqueio de caminhos relativos, nomes reservados Windows, duplicatas e ciclos.
- Leituras/gravações verificadas; teste de espaço disponível por arquivo.
- Trabalho fora da thread da interface, progresso e cancelamento cooperativo.
- A instalação usa `InstallStaging/<id>` e só é promovida para `Installed/<id>`
  após concluir. Não substitui jogos nem remove o PKG original.
- Erros/cancelamento removem apenas a pasta temporária desta operação. Após morte
  do processo, o relatório fica inconclusivo; resíduos de staging podem permanecer.

## Limites explícitos

Implementação experimental para o layout RSA/PFS/PFSC da referência: PFSC em
fronteiras de 64 KiB no prefixo de 16 MiB, inodes de 0xA8 e blocos resolvidos pelo
mapa PFSC,
`uroot`, jogo base com `eboot.bin` e `sce_sys/param.sfo`. Não é um instalador
universal de PKG retail. Variantes não suportadas falham de forma explícita.
Não aplica patches/DLC sobre jogos existentes, não verifica autenticidade Sony,
não descriptografa executáveis SELF protegidos e não executa o jogo extraído.
Entradas de sistema copiadas: param.sfo e imagens selecionadas; licenças não são
decifradas. Ter chaves não garante suporte a qualquer pacote.

O CI valida a estrutura do pacote APPX e audita APIs incompatíveis. A extração do
Apollo foi validada no Xbox pelo usuário; essa evidência permanece no relatório de
extração e na biblioteca persistida. O probe de execução comprova apenas a transição
controlada de memória do próprio projeto, não a execução do conteúdo recebido.
