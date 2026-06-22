#!/bin/bash
# run.sh - Lanza el servidor, estaciones y naves del juego CosmiKernel.
#
# Uso:
#   ./run.sh                   # 1 estacion, 1 nave
#   ./run.sh 2                 # 1 estacion, 2 naves
#   ./run.sh 2 2               # 2 estaciones, 2 naves
#   ./run.sh stop              # mata todos los procesos del juego

DIR="$(cd "$(dirname "$0")" && pwd)"
SERVIDOR="$DIR/bin/servidor"
ESTACION="$DIR/bin/estacion"
NAVE="$DIR/bin/nave"

# ─── stop ──────────────────────────────────────────────────────────────────
if [[ "$1" == "stop" ]]; then
    echo "Deteniendo todos los procesos del juego..."
    pkill -SIGINT -f "$SERVIDOR" 2>/dev/null
    sleep 1
    pkill -9 -f "$SERVIDOR|$ESTACION|$NAVE" 2>/dev/null
    echo "Listo."
    exit 0
fi

NUM_ESTACIONES="${1:-1}"
NUM_NAVES="${2:-1}"

# ─── Verificar binarios ─────────────────────────────────────────────────────
for bin in "$SERVIDOR" "$ESTACION" "$NAVE"; do
    if [[ ! -x "$bin" ]]; then
        echo "ERROR: '$bin' no existe o no es ejecutable. Ejecuta 'make' primero."
        exit 1
    fi
done

# ─── Limpiar IPC obsoleto de ejecuciones anteriores ────────────────────────
echo "Limpiando IPC obsoleto..."
pkill -9 -f "$SERVIDOR|$ESTACION|$NAVE" 2>/dev/null
sleep 0.3
# Eliminar SHM y colas POSIX que puedan haber quedado colgadas
rm -f /dev/shm/cosmikernel_mapa 2>/dev/null
for f in /dev/mqueue/cosmikernel_*; do
    rm -f "$f" 2>/dev/null
done
# Eliminar semaforos de celda (si no los limpio el servidor anterior)
for f in /dev/shm/cosmikernel_cell_*; do
    rm -f "$f" 2>/dev/null
done
for f in /dev/shm/hangar_estacion_*; do
    rm -f "$f" 2>/dev/null
done

# ─── Lanzar servidor ────────────────────────────────────────────────────────
echo "Lanzando servidor..."
gnome-terminal --geometry=100x10 --title="CosmiKernel - Servidor" \
    -- bash -c "cd '$DIR' && ./bin/servidor; echo '--- Servidor terminó (Enter para cerrar) ---'; read" &

sleep 1.5

# ─── Lanzar estaciones ──────────────────────────────────────────────────────
for ((i=1; i<=NUM_ESTACIONES; i++)); do
    echo "Lanzando estación $i..."
    gnome-terminal --geometry=60x20 --title="CosmiKernel - Estación $i" \
        -- bash -c "cd '$DIR' && ./bin/estacion; echo '--- Estación terminó (Enter para cerrar) ---'; read" &
    sleep 0.5
done

sleep 0.5

# ─── Lanzar naves ───────────────────────────────────────────────────────────
for ((i=1; i<=NUM_NAVES; i++)); do
    echo "Lanzando nave $i..."
    gnome-terminal --geometry=82x26 --title="CosmiKernel - Nave $i" \
        -- bash -c "cd '$DIR' && ./bin/nave; echo '--- Nave terminó (Enter para cerrar) ---'; read" &
    sleep 0.3
done

echo ""
echo "Juego iniciado:"
echo "  - 1 servidor"
echo "  - $NUM_ESTACIONES estacion(es)"
echo "  - $NUM_NAVES nave(s)"
echo ""
echo "Para detener todo: ./run.sh stop"
