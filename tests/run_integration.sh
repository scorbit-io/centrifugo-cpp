#!/usr/bin/env bash
# Bring up Centrifugo + jwt-generator in a private compose project on free localhost ports,
# run the integration tests, tear down. Usage: tests/run_integration.sh [build-dir]
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$(cd "${1:-$root/build}" && pwd)"
test_bin="$build_dir/recovery_test"

if [[ ! -x "$test_bin" ]]; then
    echo "missing $test_bin; configure with -DCENTRIFUGO_CPP_BUILD_TESTS=ON and build first" >&2
    exit 2
fi

free_port() {
    python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])'
}
export CENTRIFUGO_PORT="$(free_port)"
export JWT_PORT="$(free_port)"

# Own project name so a developer's running stack is never touched or torn down.
compose=(docker compose -p "centrifugo-cpp-it-$$" --project-directory "$root"
         -f "$root/docker-compose.yml")
trap '"${compose[@]}" down --remove-orphans >/dev/null 2>&1 || true' EXIT
"${compose[@]}" up -d --build

echo "waiting for services (centrifugo 127.0.0.1:$CENTRIFUGO_PORT, jwt 127.0.0.1:$JWT_PORT)..."
for _ in $(seq 1 60); do
    if curl -sf -o /dev/null "http://127.0.0.1:$JWT_PORT/" \
        && curl -sf -o /dev/null -X POST -H "X-API-Key: api-key" -d '{}' \
            "http://127.0.0.1:$CENTRIFUGO_PORT/api/info"; then
        ready=1
        break
    fi
    sleep 1
done
if [[ -z "${ready:-}" ]]; then
    echo "services did not become healthy" >&2
    "${compose[@]}" logs >&2
    exit 1
fi

"$test_bin"
