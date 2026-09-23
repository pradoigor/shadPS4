# Fontes do runtime Xbox

O FreeType usado por este alvo é o submódulo do shadPS4, fixado em
`b91f75bd02db43b06d634591eb286d3eb0ce3b65`, compilado estaticamente para UWP.
É distribuído sob a opção GPLv2 descrita em `FreeType.txt` e `FreeType-GPLv2.txt`.

Os arquivos de fontes Noto vêm do próprio repositório shadPS4 e mantêm a licença
SIL Open Font License 1.1 incluída em `Noto-OFL.txt`:

- NotoSans-Regular.ttf: Copyright 2022 The Noto Project Authors
  (https://github.com/notofonts/latin-greek-cyrillic).
- NotoSansCJK-Regular.ttc: Copyright 2014-2021 Adobe (http://www.adobe.com/).

As fontes originais do sistema PS4 não são distribuídas. Os dois caminhos usados
pelo Apollo são substituídos explicitamente por Noto Sans CJK e Noto Sans.
Isso pode alterar o aspecto e as métricas do texto. Fontes em `/app0` são lidas
da instalação selecionada, sem substituição silenciosa de arquivos ausentes.

O adaptador converte as estruturas públicas FreeType de LP64 (PS4) para LLP64
(Windows). O conjunto atual cobre Init, Done, NewFace, NewMemoryFace, DoneFace,
SetPixelSizes, GetCharIndex e LoadGlyph; callbacks e APIs FreeType adicionais
continuam indisponíveis. Nenhuma função não implementada deve ser anunciada
como suporte completo à biblioteca.
