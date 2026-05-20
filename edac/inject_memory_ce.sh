#!/bin/bash
#
# inject_memory_ce.sh - 向指定内存条注入 CE (Correctable Error)
#
# 用法: ./inject_memory_ce.sh [mc_id] [dimm_label] [count]
#   mc_id       - 内存控制器 ID (默认: 0)
#   dimm_label  - 内存条标签 (默认: 列出所有 DIMM 供选择)
#   count       - 注入错误数量 (默认: 2001)
#

set -e

# 默认参数
MC_ID="${1:-0}"
DIMM_LABEL="${2:-}"
COUNT="${3:-2001}"
EDAC_DEBUG="/sys/kernel/debug/edac/mc${MC_ID}"
EDAC_SYS="/sys/devices/system/edac/mc/mc${MC_ID}"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }

# 检查 root 权限
check_root() {
    if [[ $EUID -ne 0 ]]; then
        log_error "此脚本需要 root 权限运行"
        echo "请使用: sudo $0 $*"
        exit 1
    fi
}

# 检查 EDAC 是否可用
check_edac() {
    if ! lsmod | grep -q edac; then
        log_error "EDAC 模块未加载"
        echo "尝试加载: modprobe edac_mce_amd 或 modprobe sb_edac"
        exit 1
    fi

    if [[ ! -d "$EDAC_DEBUG" ]]; then
        log_error "EDAC debug 目录不存在: $EDAC_DEBUG"
        echo "可用的内存控制器:"
        ls /sys/kernel/debug/edac/ 2>/dev/null || echo "  (无)"
        exit 1
    fi

    if [[ ! -f "${EDAC_DEBUG}/fake_inject_count" ]]; then
        log_error "当前 EDAC 驱动不支持错误注入"
        exit 1
    fi
}

# 列出可用的 DIMM
list_dimms() {
    log_info "内存控制器 mc${MC_ID} 上的 DIMM 列表:"
    echo "----------------------------------------"

    local dimm_dir="${EDAC_SYS}/dimm0"
    local index=0

    while [[ -d "${EDAC_SYS}/dimm${index}" ]]; do
        local label=$(cat "${EDAC_SYS}/dimm${index}/dimm_label" 2>/dev/null || echo "unknown")
        local size=$(cat "${EDAC_SYS}/dimm${index}/size" 2>/dev/null || echo "0")
        local ce=$(cat "${EDAC_SYS}/dimm${index}/dimm_ce_count" 2>/dev/null || echo "0")
        local ue=$(cat "${EDAC_SYS}/dimm${index}/dimm_ue_count" 2>/dev/null || echo "0")

        printf "  dimm%-2d  %-20s  %6d MB  CE: %4d  UE: %4d\n" \
               "$index" "$label" "$size" "$ce" "$ue"
        index=$((index + 1))
    done

    echo "----------------------------------------"
}

# 获取 DIMM 索引
get_dimm_index() {
    local target_label="$1"
    local index=0

    while [[ -d "${EDAC_SYS}/dimm${index}" ]]; do
        local label=$(cat "${EDAC_SYS}/dimm${index}/dimm_label" 2>/dev/null || echo "")
        if [[ "$label" == "$target_label" ]]; then
            echo "$index"
            return 0
        fi
        index=$((index + 1))
    done

    return 1
}

# 记录注入前的错误计数
record_before() {
    local dimm_index="$1"
    cat "${EDAC_SYS}/dimm${dimm_index}/dimm_ce_count" 2>/dev/null || echo "0"
}

# 注入 CE 错误
inject_ce() {
    local count="$1"

    log_info "开始注入 ${count} 条 CE 错误..."

    # 启用 CE 注入
    echo 1 > "${EDAC_DEBUG}/fake_inject_ce" 2>/dev/null || true

    # 逐条注入
    local injected=0
    while [[ $injected -lt $count ]]; do
        echo 1 > "${EDAC_DEBUG}/fake_inject_count"
        injected=$((injected + 1))

        # 每 100 条输出进度
        if [[ $((injected % 100)) -eq 0 ]]; then
            echo -ne "\r已注入: ${injected}/${count}"
        fi

        # 短暂延迟，避免过快
        usleep 10000 2>/dev/null || sleep 0.01
    done

    echo ""
    log_info "注入完成: ${injected} 条"
}

# 检查注入结果
check_result() {
    local dimm_index="$1"
    local before="$2"

    sleep 2

    local after=$(cat "${EDAC_SYS}/dimm${dimm_index}/dimm_ce_count" 2>/dev/null || echo "0")
    local diff=$((after - before))

    echo "========================================"
    log_info "注入结果:"
    echo "  DIMM:        $(cat ${EDAC_SYS}/dimm${dimm_index}/dimm_label 2>/dev/null)"
    echo "  注入前 CE:   ${before}"
    echo "  注入后 CE:   ${after}"
    echo "  新增 CE:     ${diff}"
    echo "========================================"

    # 检查内核日志
    log_info "最近的 EDAC 内核日志:"
    dmesg | grep -i edac | tail -5

    # 检查 rasdaemon
    if command -v ras-mc-ctl &>/dev/null; then
        log_info "rasdaemon 记录的错误:"
        ras-mc-ctl --errors 2>/dev/null | tail -5
    fi

    # 检查 abrtd
    if command -v abrt-cli &>/dev/null; then
        log_info "abrtd 问题列表:"
        abrt-cli list 2>/dev/null | tail -5
    fi
}

# 主流程
main() {
    echo "========================================"
    echo "  EDAC CE 错误注入工具"
    echo "========================================"
    echo ""

    check_root
    check_edac

    # 如果未指定 DIMM，列出所有 DIMM 供选择
    if [[ -z "$DIMM_LABEL" ]]; then
        list_dimms
        echo ""
        read -p "请输入要注入的 DIMM 标签 (如 DIMM_A1): " DIMM_LABEL
    fi

    # 获取 DIMM 索引
    DIMM_INDEX=$(get_dimm_index "$DIMM_LABEL")
    if [[ $? -ne 0 ]]; then
        log_error "未找到 DIMM: $DIMM_LABEL"
        list_dimms
        exit 1
    fi

    log_info "目标 DIMM: $DIMM_LABEL (索引: $DIMM_INDEX)"
    log_info "注入数量: $COUNT"
    echo ""

    # 确认操作
    read -p "确认注入? (yes/no): " confirm
    if [[ "$confirm" != "yes" ]]; then
        log_warn "操作已取消"
        exit 0
    fi

    # 记录注入前计数
    BEFORE=$(record_before "$DIMM_INDEX")

    # 注入错误
    inject_ce "$COUNT"

    # 检查结果
    check_result "$DIMM_INDEX" "$BEFORE"

    log_info "完成"
}

main "$@"
