#!/bin/bash
set -e

# Color definitions
GREEN="\e[32m"
YELLOW="\e[33m"
RED="\e[31m"
BLUE="\e[34m"
CYAN="\e[36m"
MAGENTA="\e[35m"
BOLD="\e[1m"
NC="\e[0m"

# Error handler
trap 'echo -e "${RED}Error on line $LINENO. Press Enter to exit...${NC}"; read' ERR

clear
echo -e "${BLUE}╔══════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN} MeMeDo Kernel • sweet_k6a ${NC}"
echo -e "${GREEN} Redmi Note 12 Pro ${NC}"
echo -e "${BLUE}╚══════════════════════════════════════════════════╝${NC}"

START_TIME=$(date +%s)

# Store original panel dimensions for restoration
PANEL_FILES=(
    "arch/arm64/boot/dts/qcom/xiaomi/sweet/dsi-panel-k6-38-0e-0b-fhd-dsc-video.dtsi"
    "arch/arm64/boot/dts/qcom/xiaomi/sweet/dsi-panel-k6-38-0c-0a-fhd-dsc-video.dtsi"
)

# Function to save git state
save_git_state() {
    echo -e "${CYAN}Saving git state...${NC}"
    # Create a temporary stash of any uncommitted changes
    git stash push -u -m "build_script_temp_$(date +%s)" >/dev/null 2>&1 || true
}

# Function to restore git state
restore_git_state() {
    echo -e "${CYAN}Restoring git state...${NC}"
    # Reset any build script changes
    for f in "${PANEL_FILES[@]}"; do
        if [ -f "$f" ]; then
            git checkout -- "$f" 2>/dev/null || true
        fi
    done
    # Restore original stash if exists
    git stash pop >/dev/null 2>&1 || true
}

# Cleanup function
cleanup_on_exit() {
    echo -e "${YELLOW}\nCleaning up modifications...${NC}"
    restore_git_state
    
    if [[ "$ksu_enabled" == true ]]; then
        echo -e "${YELLOW}Cleaning up RKSU (susfs) integration...${NC}"
        DRIVER_DIR="drivers"
        
        # Remove symlink
        if [ -L "$DRIVER_DIR/kernelsu" ]; then
            rm -f "$DRIVER_DIR/kernelsu"
            echo -e "${CYAN}[-] Symlink removed${NC}"
        fi
        
        # Clean Makefile
        if [ -f "$DRIVER_DIR/Makefile" ]; then
            sed -i '/obj-$(CONFIG_KSU) += kernelsu/d' "$DRIVER_DIR/Makefile"
            git checkout -- "$DRIVER_DIR/Makefile" 2>/dev/null || true
            echo -e "${CYAN}[-] Makefile cleaned${NC}"
        fi
        
        # Clean Kconfig
        if [ -f "$DRIVER_DIR/Kconfig" ]; then
            sed -i '/source "drivers\/kernelsu\/Kconfig"/d' "$DRIVER_DIR/Kconfig"
            git checkout -- "$DRIVER_DIR/Kconfig" 2>/dev/null || true
            echo -e "${CYAN}[-] Kconfig cleaned${NC}"
        fi
        
        # Remove KernelSU directory
        if [[ -n "$ksu_repo_dir" && -d "$ksu_repo_dir" ]]; then
            rm -rf "$ksu_repo_dir"
            echo -e "${CYAN}[-] $ksu_repo_dir directory deleted${NC}"
        else
            rm -rf KernelSU KernelSU-Next 2>/dev/null || true
            echo -e "${CYAN}[-] KernelSU directories deleted (fallback)${NC}"
        fi
        
        echo -e "${GREEN}RKSU cleanup complete${NC}"
    fi
    
    echo -e "${GREEN}Git tree restored to original state${NC}"
}

# Register cleanup on script exit
trap cleanup_on_exit EXIT

echo -e "${YELLOW}Do you want a clean build? (highly recommended)${NC}"
select clean_choice in "Yes (clean build)" "No (incremental)"; do
    case $clean_choice in
        "Yes (clean build)" )
            echo -e "${YELLOW}Performing full clean...${NC}"
            [ -d "out" ] && rm -rf out
            make distclean >/dev/null 2>&1 || true
            mkdir -p out
            CLEAN_BUILD=true
            break
            ;;
        "No (incremental)" )
            echo -e "${CYAN}Incremental build — keeping existing objects${NC}"
            [ ! -d "out" ] && mkdir -p out
            CLEAN_BUILD=false
            break
            ;;
    esac
done

if [[ "$CLEAN_BUILD" == true ]]; then
    echo -e "${YELLOW}Installing/updating required packages...${NC}"
    sudo apt-get update -qq
    sudo apt-get install -y bc bison build-essential ccache cpio curl flex git libelf-dev libssl-dev \
        libncurses5-dev lld lzma python3 unzip wget xz-utils zip jq zstd libxml2-dev \
        gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu >/dev/null 2>&1
fi

echo -e "${YELLOW}\nSelect build type:${NC}"
select buildtype in "AOSP" "MIUI/OEM"; do
    case $buildtype in
        AOSP ) zip_prefix="AOSP"; break;;
        "MIUI/OEM" ) zip_prefix="MIUI"; break;;
    esac
done

ksu_enabled=false
ksu_repo_dir=""

echo -e "${YELLOW}\nInclude KernelSU (RKSU susfs-rksu-master)?${NC}"
select ksu in "Yes" "No"; do
    case $ksu in
        Yes )
            echo -e "${GREEN}Adding RKSU (rsuntk fork • susfs-rksu-master)...${NC}"
            KSU_SCRIPT="/tmp/ksu_setup_$(date +%s).sh"
            curl -LSs "https://raw.githubusercontent.com/rsuntk/KernelSU/main/kernel/setup.sh" -o "$KSU_SCRIPT"
            bash "$KSU_SCRIPT" susfs-rksu-master || {
                echo -e "${RED}KernelSU setup failed. Press Enter to continue...${NC}"
                read
                exit 1
            }
            rm -f "$KSU_SCRIPT"
            
            if [[ -d "KernelSU" ]]; then
                ksu_repo_dir="KernelSU"
            elif [[ -d "KernelSU-Next" ]]; then
                ksu_repo_dir="KernelSU-Next"
            fi
            
            zip_prefix="${zip_prefix}_KSU"
            ksu_enabled=true
            echo -e "${GREEN}RKSU (susfs) integrated successfully${NC}"
            break
            ;;
        No )
            echo -e "${CYAN}Skipping KernelSU${NC}"
            break
            ;;
    esac
done

mkdir -p toolchain

# Neutron Clang setup with better error handling
if [ ! -d "clang" ] || [ ! -f "clang/bin/clang" ]; then
    if [ -d "clang" ] && [ ! -f "clang/bin/clang" ]; then
        echo -e "${YELLOW}Incomplete Neutron Clang installation detected, removing...${NC}"
        rm -rf clang
    fi
    
    echo -e "${YELLOW}Downloading and setting up latest Neutron Clang...${NC}"
    mkdir -p clang && cd clang
    
    echo -e "${CYAN}Downloading antman...${NC}"
    curl -LO "https://raw.githubusercontent.com/Neutron-Toolchains/antman/main/antman" || {
        echo -e "${RED}Failed to download antman. Press Enter to exit...${NC}"
        read
        exit 1
    }
    
    chmod a+x antman
    
    echo -e "${CYAN}Setting up Neutron Clang (this may take a while)...${NC}"
    ./antman -S || {
        echo -e "${RED}antman -S failed. Press Enter to exit...${NC}"
        read
        exit 1
    }
    
    echo -e "${CYAN}Patching glibc...${NC}"
    ./antman --patch=glibc || {
        echo -e "${YELLOW}Warning: glibc patch failed, continuing anyway...${NC}"
    }
    
    cd ..
    echo -e "${GREEN}Neutron Clang setup complete${NC}"
else
    echo -e "${GREEN}Neutron Clang ready${NC}"
fi

# GCC toolchains
for dir in gcc64 gcc32; do
    repo=$([ "$dir" = "gcc64" ] && echo "gcc-arm64" || echo "gcc-arm")
    if [ ! -d "$dir" ]; then
        echo -e "${YELLOW}Cloning GreenForce $dir toolchain...${NC}"
        git clone https://github.com/greenforce-project/$repo -b main --depth=1 "$dir" || {
            echo -e "${RED}Failed to clone $dir. Press Enter to exit...${NC}"
            read
            exit 1
        }
    else
        echo -e "${GREEN}$dir ready${NC}"
    fi
done

# Verify toolchains exist
if [ ! -f "clang/bin/clang" ]; then
    echo -e "${RED}Clang binary not found! Press Enter to exit...${NC}"
    read
    exit 1
fi

if [ ! -f "gcc64/bin/aarch64-linux-gnu-gcc" ]; then
    echo -e "${RED}GCC64 binary not found! Press Enter to exit...${NC}"
    read
    exit 1
fi

echo -e "${GREEN}All toolchains verified successfully${NC}"

# Export variables
export ARCH=arm64
export SUBARCH=arm64
export PATH="${PWD}/clang/bin:${PWD}/gcc64/bin:${PWD}/gcc32/bin:${PATH}"
export KBUILD_BUILD_USER="MiDoNaSR"
export KBUILD_BUILD_HOST="sweet_k6a"
export LLVM=1 LLVM_IAS=1
export CLANG_TRIPLE=aarch64-linux-gnu-
export CROSS_COMPILE=aarch64-linux-gnu-
export CROSS_COMPILE_ARM32=arm-linux-gnueabi-
export USE_CCACHE=1
ccache -M 50G >/dev/null 2>&1 || true
export KCFLAGS="-O3"
export LTO=thin

apply_panel_dimensions() {
    local w=$1 h=$2
    echo -e "${CYAN}Setting panel size → ${w} × ${h} mm${NC}"
    for f in "${PANEL_FILES[@]}"; do
        if [ -f "$f" ]; then
            sed -i "s/\(qcom,mdss-pan-physical-width-dimension[[:space:]]*=[[:space:]]*< *\)[0-9]\+/\1${w}/" "$f"
            sed -i "s/\(qcom,mdss-pan-physical-height-dimension[[:space:]]*=[[:space:]]*< *\)[0-9]\+/\1${h}/" "$f"
        fi
    done
}

# Save git state before modifications
save_git_state

echo -e "${YELLOW}\nApplying panel dimensions...${NC}"
if [[ "$buildtype" == "AOSP" ]]; then
    apply_panel_dimensions 70 155
else
    apply_panel_dimensions 695 1546
fi
echo -e "${GREEN}Panel dimensions applied${NC}"

echo -e "${MAGENTA}\nStarting compilation with Neutron Clang + ThinLTO...${NC}"
echo -e "${CYAN}Running defconfig...${NC}"
make O=out sweet_defconfig || {
    echo -e "${RED}defconfig failed. Press Enter to exit...${NC}"
    read
    exit 1
}

echo -e "${CYAN}Building kernel...${NC}"
make -j$(nproc --all) O=out \
    CC=clang LD=ld.lld NM=llvm-nm OBJCOPY=llvm-objcopy \
    2>&1 | tee build.log

KERNEL_IMG="out/arch/arm64/boot/Image.gz"
DTBO_IMG="out/arch/arm64/boot/dtbo.img"

[[ ! -f "$KERNEL_IMG" || ! -f "$DTBO_IMG" ]] && {
    echo -e "${RED}BUILD FAILED — missing Image.gz or dtbo.img${NC}"
    echo -e "${YELLOW}Check build.log for errors. Press Enter to exit...${NC}"
    read
    exit 1
}

cp out/.config out/sweet_defconfig.txt

ZIPNAME="${zip_prefix}-MeMeDo-sweet_k6a-$(date '+%Y%m%d-%H%M').zip"
echo -e "${YELLOW}\nPackaging → $ZIPNAME${NC}"

rm -rf AnyKernel3
git clone --depth=1 https://github.com/MiDoNaSR545/AnyKernel3 || git clone --depth=1 https://github.com/osm0sis/AnyKernel3 AnyKernel3

cp "$KERNEL_IMG" "$DTBO_IMG" out/arch/arm64/boot/dtb.img AnyKernel3/ 2>/dev/null || true

cd AnyKernel3
zip -r9 "../$ZIPNAME" . -x ".git/*" "README.md" "*.zip" >/dev/null
cd ..

echo -e "${GREEN}Zip created: $ZIPNAME${NC}"

if [[ -z "$PIXELDRAIN_API_KEY" ]]; then
    echo -e "${YELLOW}PixelDrain API key not set. Please enter it now (or press Enter to skip upload):${NC}"
    read -s -r PIXELDRAIN_API_KEY
    echo
fi

if [[ -n "$PIXELDRAIN_API_KEY" ]]; then
    echo -e "${YELLOW}Uploading to PixelDrain...${NC}"
    RES=$(curl -s -u ":$PIXELDRAIN_API_KEY" -F "file=@$ZIPNAME" https://pixeldrain.com/api/file)
    ID=$(echo "$RES" | jq -r .id 2>/dev/null || echo "$RES" | grep -o '"id":"[^"]*' | cut -d'"' -f4)
    [[ -n "$ID" && "$ID" != "null" ]] && echo -e "${GREEN}https://pixeldrain.com/u/$ID${NC}"
fi

END_TIME=$(date +%s)

echo -e "${MAGENTA}${BOLD}"
echo "╔══════════════════════════════════════════════════╗"
echo " BUILD SUCCESSFUL in $((END_TIME - START_TIME)) seconds! "
echo " $ZIPNAME "
[[ -n "$ID" && "$ID" != "null" ]] && echo " https://pixeldrain.com/u/$ID "
echo "╚══════════════════════════════════════════════════╝"
echo -e "${NC}"

echo -e "${GREEN}Script completed. Press Enter to exit...${NC}"
read
