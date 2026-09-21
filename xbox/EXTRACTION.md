# Extração UWP 0.3.0

O aplicativo agora executa extração, além de inspecionar metadados. A biblioteca
abre ao iniciar, usa uma faixa horizontal navegável pelo controle e distingue
arquivos importados de diretórios extraídos. O visual azul e as ondas são próprios,
inspirados no PS4, sem recursos gráficos da Sony.

## Uso

1. Atualize o APPX, preservando LocalState.
2. Importe `keys.json` pelo botão **Importar chaves**, caso o pacote use o caminho
   RSA/PFS suportado. São necessários `PkgDerivedKey3Keyset` e `FakeKeyset`, com
   `PublicExponent` (4 bytes), `Modulus`, `PrivateExponent` (256 cada) e
   `Prime1`, `Prime2`, `Exponent1`, `Exponent2`, `Coefficient` (128 cada), em
   hexadecimal. O aplicativo valida todos os componentes antes da operação RSA.
   O aplicativo verifica a tabela antes de pedir chaves: PKGs sem `entry_keys` e
   `image_key` não pedem o arquivo. O layout PFS sem criptografia ainda termina
   como variante não suportada, em vez de apresentar uma exigência falsa de chave.
3. Importe ou selecione o PKG já existente e pressione **Extrair pacote**.
4. Acompanhe arquivos/MiB; **Cancelar extração** interrompe entre blocos.
5. Após terminar, confira o cartão **EXTRAÍDO**, feche e reabra o aplicativo.
6. Baixe `LocalState/extraction-report.json` pelo portal ou use **Diagnóstico →
   Exportar JSON**. A versão, commit, resultado e contadores ficam registrados.

Se o seletor de arquivos não estiver disponível no console, envie o PKG para
`LocalState/Library` e as chaves para `LocalState/keys.json` pelo Device Portal,
então reabra a Biblioteca. Preserve qualquer keys.json anterior antes de trocar
o conjunto por chaves sintéticas de teste.

As chaves reais ficam somente em LocalState, não entram no relatório. A importação
valida tamanho/formato; a operação RSA verifica a compatibilidade com o pacote.
O artefato contém `synthetic-test/valid.pkg` e `synthetic-test/keys.json`, produzidos
com chaves aleatórias de teste. Não são chaves de PS4 e não servem para jogos.
Importar essas chaves substitui as chaves locais; use-as apenas para validar a
extração sintética antes de importar seu próprio conjunto.

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
fronteiras de 64 KiB no prefixo de 16 MiB, inodes de 0xA8, blocos contíguos,
`uroot`, jogo base com `eboot.bin` e `sce_sys/param.sfo`. Não é um instalador
universal de PKG retail. Variantes não suportadas falham de forma explícita.
Não aplica patches/DLC sobre jogos existentes, não verifica autenticidade Sony,
não descriptografa executáveis SELF protegidos e não executa o jogo extraído.
Entradas de sistema copiadas: param.sfo e imagens selecionadas; licenças não são
decifradas. Ter chaves não garante suporte a qualquer pacote.

O CI testa arquivos sintéticos criptografados com extração byte a byte,
truncamento, limites de tabela/inodes, corrupção zlib, RSA inválido, traversal,
ciclos, chaves ausentes e cancelamento. Esses testes não comprovam um PKG real
nem a apresentação visual no console. A validação final no Xbox continua pendente.
