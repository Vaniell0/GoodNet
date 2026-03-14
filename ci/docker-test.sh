#!/usr/bin/env bash
# ci/docker-test.sh — Build Docker image and run integration tests.
#
# Usage:
#   ./ci/docker-test.sh [tcp|ice|all]
#
# Defaults to "all" if no argument given.

set -euo pipefail

MODE="${1:-all}"
COMPOSE="docker compose"
COMPOSE_BASE="-f docker-compose.yml"
COMPOSE_ICE="-f docker-compose.yml -f docker-compose.ice.yml"

cleanup() {
    echo ">>> Cleaning up containers..."
    $COMPOSE $COMPOSE_BASE down -v --remove-orphans 2>/dev/null || true
}
trap cleanup EXIT

# Build and load Docker image
echo ">>> Building Docker image..."
nix build .#docker
nix run .#docker-load

run_tcp() {
    echo ""
    echo "════════════════════════════════════════"
    echo "  TCP Integration Test"
    echo "════════════════════════════════════════"
    $COMPOSE $COMPOSE_BASE up --abort-on-container-exit --exit-code-from node-b
    $COMPOSE $COMPOSE_BASE down -v --remove-orphans
}

run_ice() {
    echo ""
    echo "════════════════════════════════════════"
    echo "  ICE Integration Test"
    echo "════════════════════════════════════════"
    $COMPOSE $COMPOSE_ICE up --abort-on-container-exit --exit-code-from node-b
    $COMPOSE $COMPOSE_ICE down -v --remove-orphans
}

case "$MODE" in
    tcp) run_tcp ;;
    ice) run_ice ;;
    all) run_tcp; run_ice ;;
    *)
        echo "Usage: $0 [tcp|ice|all]"
        exit 1
        ;;
esac

echo ""
echo ">>> All tests passed!"
