#!/usr/bin/env bash
set -euo pipefail

MAIN_BRANCH=${MAIN_BRANCH:-main}
VERSION_ARG=${1:-}
LOG_DIR=${LOG_DIR:-logs}
BUILD_LOG=${BUILD_LOG:-$LOG_DIR/release_build.log}
PAYLOAD_DIR=${PAYLOAD_DIR:-build/release_payload}

fail() {
    echo "ERRO: $*" >&2
    exit 1
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || fail "comando obrigatorio nao encontrado: $1"
}

print_section() {
    echo
    echo "=================================================="
    echo " $1"
    echo "=================================================="
}

validate_version() {
    local version=$1

    if [[ ! "$version" =~ ^v[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?$ ]]; then
        fail "versao invalida: use formato semantico como v1.0.0"
    fi
}

need_cmd git
need_cmd make
need_cmd tar
need_cmd tee
need_cmd grep

git rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
    fail "este diretorio nao parece ser um repositorio Git"

current_branch=$(git symbolic-ref --quiet --short HEAD || true)
if [[ "$current_branch" != "$MAIN_BRANCH" ]]; then
    fail "branch atual '$current_branch'; mude para '$MAIN_BRANCH' antes de publicar"
fi

if [[ -n "$(git status --porcelain --untracked-files=all)" ]]; then
    git status --short
    fail "ha modificacoes nao salvas; commit ou stash antes de gerar release"
fi

if [[ -n "$VERSION_ARG" ]]; then
    validate_version "$VERSION_ARG"
    VERSION="$VERSION_ARG"
else
    short_sha=$(git rev-parse --short HEAD)
    VERSION="snapshot-$short_sha"
fi

ARCHIVE="build/photonos-${VERSION}.tar.gz"

print_section "Aviso de Blindagem"
echo "Nunca adicione logs/net_test.pcap ou logs/net_test_serial.log ao indice do Git."
echo "Esses residuos de smoke test permanecem blindados pelo .gitignore."

mkdir -p "$LOG_DIR"

print_section "Clean Build"
make clean
if ! make 2>&1 | tee "$BUILD_LOG"; then
    fail "build limpo falhou; consulte $BUILD_LOG"
fi

if grep -Eiq '(^|[[:space:]])(warning|aviso):' "$BUILD_LOG"; then
    fail "build emitiu avisos; consulte $BUILD_LOG"
fi

for artifact in build/photon.img build/photon.elf build/photon.map; do
    [[ -f "$artifact" ]] || fail "artefato obrigatorio nao encontrado: $artifact"
done

print_section "Empacotamento"
rm -rf "$PAYLOAD_DIR"
mkdir -p "$PAYLOAD_DIR"
cp build/photon.img build/photon.elf build/photon.map "$PAYLOAD_DIR"/
tar -C "$PAYLOAD_DIR" -czf "$ARCHIVE" photon.img photon.elf photon.map

echo "Payload criado: $ARCHIVE"
echo "Conteudo:"
tar -tzf "$ARCHIVE"

if [[ -n "$VERSION_ARG" ]]; then
    print_section "Tag Git"
    if git rev-parse -q --verify "refs/tags/$VERSION" >/dev/null 2>&1; then
        fail "tag '$VERSION' ja existe"
    fi

    git tag -a "$VERSION" \
        -m "Release estavel: Kernel $VERSION com Stack de Rede e1000"
    echo "Tag local criada: $VERSION"
else
    print_section "Tag Git"
    echo "Nenhuma tag foi criada porque nenhum argumento de versao foi informado."
    echo "Exemplo: bash scripts/publish_release.sh v1.0.0"
fi

print_section "Proximos Passos"
echo "Revise o pacote gerado:"
echo "  tar -tzf $ARCHIVE"
echo
echo "Envie branch e tags para o GitHub:"
echo "  git push origin $MAIN_BRANCH --tags"
echo
echo "Na interface de Releases do GitHub, crie/anexe a release da versao:"
echo "  $VERSION"
echo "e envie o arquivo:"
echo "  $ARCHIVE"
echo
echo "Lembrete: logs/net_test.pcap e logs/net_test_serial.log nunca devem ser adicionados ao Git."
