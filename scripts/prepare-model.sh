#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd -- "${BASH_SOURCE[0]%/*}" && pwd)"

ssv_prepare_model_quote() {
    local value="$1"
    value="${value//\\/\\\\}"
    value="${value//\"/\\\"}"
    value="${value//$'\n'/\\n}"
    value="${value//$'\r'/\\r}"
    value="${value//$'\t'/\\t}"
    printf '"%s"' "$value"
}

ssv_prepare_model_fatal() {
    local exit_code="$1" stage="$2" error="$3"
    printf 'event=fatal_error exit_code=%s stage=%s source_id=prepare-model error=%s\n' \
        "$exit_code" "$stage" "$(ssv_prepare_model_quote "$error")" >&2
}

if ! command -v uv >/dev/null 2>&1; then
    ssv_prepare_model_fatal 3 dependency "uv is required to prepare ONNX models"
    exit 3
fi

stderr_file="$(mktemp "${TMPDIR:-/tmp}/ssv-prepare-model-stderr.XXXXXX")" || {
    ssv_prepare_model_fatal 4 model_prepare "cannot create temporary stderr capture"
    exit 4
}
trap 'rm -f -- "$stderr_file"' EXIT

uv run --script "$SCRIPT_DIR/model/prepare_wrapper.py" "$@" 2>"$stderr_file"
status=$?
fatal_seen=false
while IFS= read -r line || [ -n "$line" ]; do
    printf '%s\n' "$line" >&2
    case "$line" in
        event=fatal_error\ exit_code=[24]\ stage=*\ source_id=prepare-model\ error=*)
            fatal_seen=true
            ;;
    esac
done <"$stderr_file"
rm -f -- "$stderr_file"
trap - EXIT

[ "$status" -eq 0 ] && exit 0
if [ "$fatal_seen" = true ] && { [ "$status" -eq 2 ] || [ "$status" -eq 4 ]; }; then
    exit "$status"
fi

ssv_prepare_model_fatal 4 model_prepare "uv failed while running the model preparation tool: status=$status"
exit 4
