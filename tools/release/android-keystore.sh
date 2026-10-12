#!/usr/bin/env bash
# The Android app's release signing key (platform/android, CI: client.yml job `android`).
#
# Usage: tools/release/android-keystore.sh [dir]     (default ~/.config/kn-secrets/android)
#
# Creates <dir>/krunknative-release.jks (RSA 4096, 10000 days, alias krunknative) and
# <dir>/keystore.env (KN_ANDROID_KEYSTORE, KN_ANDROID_KEYSTORE_PASSWORD, KN_ANDROID_KEY_ALIAS,
# KN_ANDROID_KEY_PASSWORD: what app/build.gradle.kts reads) unless the keystore exists, then
# prints the `gh secret set` commands that hand them to CI. Never commit either file: every
# later APK must be signed with this same key, or Android refuses to install it over the old one.
#
# Local signed build:  set -a; . <dir>/keystore.env; set +a
#                      (cd platform/android && ./gradlew assembleRelease -Pkrunknative.serverUrl=https://krunk.io/)
set -euo pipefail

dir="${1:-$HOME/.config/kn-secrets/android}"
jks="$dir/krunknative-release.jks"
envf="$dir/keystore.env"
alias_name=krunknative

if [ ! -f "$jks" ]; then
    command -v keytool > /dev/null || { echo "android-keystore.sh: keytool (a JDK) is required" >&2; exit 1; }
    install -d -m 700 "$dir"
    pass="$(head -c 32 /dev/urandom | base64 | tr -dc 'A-Za-z0-9' | head -c 32)"
    # PKCS12 keystores use one password for the store and the key
    keytool -genkeypair -keystore "$jks" -storetype PKCS12 -alias "$alias_name" -keyalg RSA -keysize 4096 \
        -validity 10000 -storepass "$pass" -keypass "$pass" -dname "CN=KrunkNative, O=KrunkNative" > /dev/null
    umask 077
    {
        echo "KN_ANDROID_KEYSTORE=$jks"
        echo "KN_ANDROID_KEYSTORE_PASSWORD=$pass"
        echo "KN_ANDROID_KEY_ALIAS=$alias_name"
        echo "KN_ANDROID_KEY_PASSWORD=$pass"
    } > "$envf"
    chmod 600 "$jks" "$envf"
    echo "created $jks and $envf"
else
    echo "$jks exists (not replaced)"
fi

cat << EOF

Hand the key to CI (repository secrets; values are read from the files, never typed):
  base64 -w0 "$jks" | gh secret set ANDROID_KEYSTORE_BASE64
  printf %s "\$(sed -n 's/^KN_ANDROID_KEYSTORE_PASSWORD=//p' "$envf")" | gh secret set ANDROID_KEYSTORE_PASSWORD
  printf %s "$alias_name" | gh secret set ANDROID_KEY_ALIAS
  printf %s "\$(sed -n 's/^KN_ANDROID_KEY_PASSWORD=//p' "$envf")" | gh secret set ANDROID_KEY_PASSWORD
EOF
