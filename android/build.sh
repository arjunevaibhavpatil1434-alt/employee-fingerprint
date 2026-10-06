#!/usr/bin/env bash
# Builds employees-access.apk without Gradle, using the Android SDK
# build tools and Android Studio's bundled JDK.
#
#     ./build.sh            -> build/employees-access.apk
set -euo pipefail
cd "$(dirname "$0")"

SDK="${ANDROID_HOME:-$HOME/Android/Sdk}"
BT="$SDK/build-tools/$(ls "$SDK/build-tools" | sort -V | tail -1)"
PLATFORM="$SDK/platforms/$(ls "$SDK/platforms" | sort -V | tail -1)/android.jar"
JDK="${JAVA_HOME:-/snap/android-studio/current/jbr}"
export PATH="$JDK/bin:$PATH"

VERSION_CODE=3
VERSION_NAME=1.2

# Signing key: created once, then reused so updates install over the old app
KEYSTORE=release.keystore
# Password from the environment or signing.env (not in git): KEY_PASS=...
if [ -z "${KEY_PASS:-}" ] && [ -f signing.env ]; then
    . ./signing.env
fi
: "${KEY_PASS:?Set KEY_PASS or create android/signing.env with KEY_PASS=...}"
if [ ! -f "$KEYSTORE" ]; then
    keytool -genkeypair -keystore "$KEYSTORE" -alias app -keyalg RSA -keysize 2048 \
        -validity 10000 -storepass "$KEY_PASS" -keypass "$KEY_PASS" \
        -dname "CN=Employees Access, O=Employees Access" >/dev/null
fi

rm -rf build && mkdir -p build/gen build/classes build/dex

echo "Resources..."
"$BT/aapt2" compile --dir res -o build/res.zip
"$BT/aapt2" link -o build/base.apk -I "$PLATFORM" --manifest AndroidManifest.xml \
    -R build/res.zip --java build/gen --auto-add-overlay \
    --min-sdk-version 26 --target-sdk-version 34 \
    --version-code "$VERSION_CODE" --version-name "$VERSION_NAME"

echo "Java..."
javac --release 11 -classpath "$PLATFORM" -d build/classes -Xlint:-options \
    $(find src build/gen -name '*.java')

echo "Dex..."
"$BT/d8" --release --min-api 26 --lib "$PLATFORM" --output build/dex \
    $(find build/classes -name '*.class')

echo "Package..."
cp build/base.apk build/unaligned.apk
(cd build/dex && zip -q ../unaligned.apk classes.dex)
"$BT/zipalign" -p -f 4 build/unaligned.apk build/aligned.apk
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass "pass:$KEY_PASS" \
    --out build/employees-access.apk build/aligned.apk

"$BT/apksigner" verify build/employees-access.apk
echo "Built $(pwd)/build/employees-access.apk ($(du -h build/employees-access.apk | cut -f1))"
