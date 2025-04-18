SCRIPT_DIR=$(dirname "$(realpath "$0")")
sh "$SCRIPT_DIR/kill.sh" "$1" b
sh "$SCRIPT_DIR/kill.sh" "$1" s