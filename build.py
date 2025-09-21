import os
import sys
import subprocess
import string
import random
import urllib.request

# Generate random bash filename
bashfile = ''.join(random.choice(string.ascii_uppercase + string.digits) for _ in range(10))
bashfile = '/tmp/' + bashfile + '.sh'

# Write Bash content
with open(bashfile, 'w') as f:
    s = """#!/bin/bash
# Telegram Config
TOKEN=$(/usr/bin/env python -c "import os; print(os.environ.get('TOKEN'))")
CHATID=$(/usr/bin/env python -c "import os; print(os.environ.get('CHATID'))")

BOT_MSG_URL="https://api.telegram.org/bot${TOKEN}/sendMessage"
BOT_BUILD_URL="https://api.telegram.org/bot${TOKEN}/sendDocument"
BOT_STICKER_URL="https://api.telegram.org/bot${TOKEN}/sendSticker"

# Build Machine details
cores=$(nproc --all)
os=$(cat /etc/issue | awk '{print $1, $2, $3}' | awk '{$1=$1};1' | sed 's/\\n//g')
time=$(TZ="Asia/Kolkata" date "+%a %b %d %r")

tg_post_msg() {
  curl -s -X POST "$BOT_MSG_URL" -d chat_id="$CHATID" \\
    -d "disable_web_page_preview=true" \\
    -d "parse_mode=html" \\
    -d text="$1"
}

tg_post_build(){
  MD5CHECK=$(md5sum "$1" | cut -d' ' -f1)
  curl --progress-bar -F document=@"$1" "$BOT_BUILD_URL" \\
    -F chat_id="$CHATID" \\
    -F "disable_web_page_preview=true" \\
    -F "parse_mode=Markdown" \\
    -F caption="$2 | *MD5 Checksum : *\\`$MD5CHECK\\`"
}

tg_post_sticker() {
  curl -s -X POST "$BOT_STICKER_URL" -d chat_id="$CHATID" \\
    -d sticker="CAACAgUAAxkBAAECHIJgXlYR8K8bYvyYIpHaFTJXYULy4QACtgIAAs328FYI4H9L7GpWgR4E"
}

kernel_dir="${PWD}"
CCACHE=$(command -v ccache)
objdir="${kernel_dir}/out"
anykernel=$HOME/anykernel
builddir="${kernel_dir}/build"
ZIMAGE=$kernel_dir/out/arch/arm64/boot/Image.gz
kernel_name="xcalibur-v5.2-violet-dynamic"
variant="Retrofit Dynamic"
support="Android 15.0-16.0"
commit_head=$(git log --oneline -1)
zip_name="$kernel_name-$(date +"%d%m%Y-%H%M").zip"
TC_DIR=$HOME/tc/
CLANG_DIR=$TC_DIR

export CONFIG_FILE="vendor/violet-perf_defconfig"
export ARCH="arm64"
export KBUILD_BUILD_HOST=SuperiorOS
export KBUILD_BUILD_USER=Joker-V2
export PATH="$CLANG_DIR/bin:$PATH"

tg_post_sticker
tg_post_msg "<b>Build Triggered ⌛</b>%0A<b>Kernel : </b><code>$kernel_name</code>%0A<b>Support : </b><code>$support</code>%0A<b>Variant : </b><code>$variant</code>%0A<b>Machine : </b><code>$os</code>%0A<b>Cores : </b><code>$cores</code>%0A<b>Time : </b><code>$time</code>%0A<b>Top Commit : </b><code>$commit_head</code>"

if ! [ -d "$TC_DIR" ]; then
  echo "Toolchain not found! Cloning to $TC_DIR..."
  tg_post_msg "<code>Toolchain not found! Cloning ZyC-Clang</code>"
  wget -q https://github.com/ZyCromerZ/Clang/releases/download/22.0.0git-20250920-release/Clang-22.0.0git-20250920.tar.gz
  mkdir $TC_DIR && tar -xvf Clang-22.0.0git-20250920.tar.gz -C $TC_DIR && rm -rf Clang-22.0.0git-20250920.tar.gz
fi

NC='\\033[0m'
RED='\\033[0;31m'
LRD='\\033[1;31m'
LGR='\\033[1;32m'

make_defconfig(){
  START=$(date +"%s")
  echo -e ${LGR} "########### Generating Defconfig ############${NC}"
  make -s ARCH=${ARCH} O=${objdir} ${CONFIG_FILE} -j$(nproc --all)
}

compile(){
  cd ${kernel_dir}
  echo -e ${LGR} "######### Compiling kernel #########${NC}"
  make -j$(nproc --all) \\
    O=out \\
    ARCH=${ARCH} \\
    CC="ccache clang" \\
    CLANG_TRIPLE="aarch64-linux-gnu-" \\
    CROSS_COMPILE="aarch64-linux-gnu-" \\
    CROSS_COMPILE_ARM32="arm-linux-gnueabi-" \\
    LLVM=1 \\
    LLVM_IAS=1 \\
    2>&1 | tee error.log
}

completion() {
  cd ${objdir}
  COMPILED_IMAGE=arch/arm64/boot/Image.gz
  COMPILED_DTBO=arch/arm64/boot/dtbo.img

  if [[ -f ${COMPILED_IMAGE} && ${COMPILED_DTBO} ]]; then
    git clone -q https://github.com/Joker-V2/AnyKernel3 -b sixteen $anykernel
    mv -f $ZIMAGE ${COMPILED_DTBO} $anykernel
    cd $anykernel
    find . -name "*.zip" -type f -delete
    zip -r AnyKernel.zip *
    mv AnyKernel.zip $zip_name
    mv $anykernel/$zip_name $HOME/$zip_name
    rm -rf $anykernel

    # Sign the zip unconditionally
    tg_post_msg "<code>Signing build with AOSP keys</code>"
    cd $HOME
    if [ ! -f zipsigner-3.0.jar ]; then
      wget -O zipsigner-3.0.jar https://github.com/Magisk-Modules-Repo/zipsigner/raw/master/bin/zipsigner-3.0-dexed.jar
    fi
    java -jar zipsigner-3.0.jar $zip_name ${zip_name/.zip/-signed.zip}
    signed_zip=${zip_name/.zip/-signed.zip}

    END=$(date +"%s")
    DIFF=$(($END - $START))
    tg_post_build "$HOME/$signed_zip" "Build took : $((DIFF / 60)) minute(s) and $((DIFF % 60)) second(s)"
    tg_post_msg "<code>Compiled & Signed successfully ✅</code>"
    curl -T $HOME/$signed_zip bashupload.com

    echo
    echo -e ${LGR} "############################################"
    echo -e ${LGR} "############# OkThisIsEpic! ###############"
    echo -e ${LGR} "############################################${NC}"
  else
    tg_post_build "$kernel_dir/error.log" "$CHATID" "Debug Mode Logs"
    tg_post_msg "<code>Compilation failed ❎</code>"
    echo -e ${RED} "############################################"
    echo -e ${RED} "##         This Is Not Epic :'("         ##"
    echo -e ${RED} "############################################${NC}"
  fi
}

make_defconfig
if [ $? -eq 0 ]; then
  tg_post_msg "<code>Defconfig generated successfully ✅</code>"
fi

compile
completion
cd ${kernel_dir}
"""
    f.write(s)

# Make script executable
os.chmod(bashfile, 0o755)

# Run script with args
bashcmd = bashfile
for arg in sys.argv[1:]:
    bashcmd += ' ' + arg

subprocess.call(bashcmd, shell=True)
