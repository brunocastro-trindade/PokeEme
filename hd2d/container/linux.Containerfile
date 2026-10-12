# Ambiente de build do port de PC (Linux). Mesmo Fedora do host,
# para o binário gerado rodar direto no sistema.
FROM registry.fedoraproject.org/fedora:44
RUN dnf -y install gcc gcc-c++ make git diffutils findutils which \
        pkgconf-pkg-config sdl2-compat-devel libpng-devel \
        mesa-libGL-devel mesa-libEGL-devel \
        python3 python3-pillow \
    && dnf clean all
WORKDIR /src
