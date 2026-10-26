# Edições do pacote e PGO por fabricante de GPU

Cada release traz o pacote em até três arquivos:

| Arquivo | O que é |
|---|---|
| `SotC-PC.zip` | **Livre**: sem configuração pronta; você ajusta tudo no launcher. É o que o atualizador do launcher baixa. |
| `SotC-PC-Otimizada-NVIDIA.zip` | Mesmos arquivos mais `user/launcher_settings.json` com DLSS Qualidade, 60 FPS, filtro 16x e texturas Alta. |
| `SotC-PC-Otimizada-AMD-Intel.zip` | Igual, com FSR 3.1 Qualidade no lugar do DLSS. |

Os presets ficam em `packaging/windows/presets/`. Tudo continua editável no launcher; a edição Livre
é o mesmo programa sem esses valores iniciais.

## PGO (otimização guiada por perfil)

O `bb-probe.exe` é compilado com o perfil `pgo/bbgpu.profdata` (treinado em NVIDIA: RTX 5060 Ti, demo da
tela de título mais o Santuário). Funções que mudaram depois do treino são compiladas sem perfil, por
isso o perfil é refeito quando o código da GPU muda (`tools/pgo_train.sh`).

**Limite:** só existe perfil treinado em NVIDIA. O código do jogo e do tradutor de GPU é o mesmo em
qualquer placa, mas os caminhos do driver (Vulkan) diferem, então um perfil de AMD ou Intel pode render um
pouco mais nessas placas. Não consigo treinar sem a placa.

### Como contribuir um perfil de AMD ou Intel

1. Clone o repositório, instale o MSYS2 CLANG64 (`packaging/windows/README.md`) e rode `bash build.sh`.
2. Com o dump do jogo e uma save depois do primeiro colosso em `out/save-template/CUSA08809`, rode
   `SOTC_GAME_DIR=<pasta do dump> bash tools/pgo_train.sh` (cerca de 15 minutos).
3. Envie `pgo/bbgpu.profdata` (um arquivo de ~2,6 MB) num issue, dizendo o modelo da placa e o driver.
4. O perfil entra em `pgo/vendor/amd_intel/bbgpu.profdata`, `tools/pgo_vendor_build.sh amd_intel` gera o
   binário e o pacote `Otimizada-AMD-Intel` passa a usá-lo. Quem enviou é creditado no CHANGELOG.

Compare sempre PGO contra PGO (o PGO vale 15 a 20%).
