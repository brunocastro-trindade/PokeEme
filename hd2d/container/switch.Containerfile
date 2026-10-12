# Ambiente de build do Switch: o mesmo Fedora do build de Linux (as ferramentas
# auxiliares do pokeemerald são compartilhadas) + o devkitPro copiado da imagem oficial.
FROM docker.io/devkitpro/devkita64:latest AS dkp

FROM localhost/emerald-linux:latest
COPY --from=dkp /opt/devkitpro/devkitA64 /opt/devkitpro/devkitA64
COPY --from=dkp /opt/devkitpro/libnx /opt/devkitpro/libnx
COPY --from=dkp /opt/devkitpro/portlibs /opt/devkitpro/portlibs
COPY --from=dkp /opt/devkitpro/tools /opt/devkitpro/tools
ENV DEVKITPRO=/opt/devkitpro \
    DEVKITA64=/opt/devkitpro/devkitA64 \
    PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:/opt/devkitpro/portlibs/switch/bin:/usr/local/bin:/usr/bin:/bin
